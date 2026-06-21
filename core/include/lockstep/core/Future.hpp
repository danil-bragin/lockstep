#pragma once

// Future.hpp — C1.1. Future<T>/Promise<T> on C++20 coroutines.
//
//   Promise<T>  = the WRITE end: set_value(T) / set_error(Error). Move-only.
//   Future<T>   = the READ end: awaitable via co_await. Supports Future<void>
//                 and error completion.
//
// LOCKED SEMANTICS enforced here:
//   L1: fulfilling a Promise (set_value/set_error) SCHEDULES the waiting
//       coroutine onto the scheduler ready queue — it NEVER resumes it inline.
//       See SharedState::complete(): it calls sink->schedule(), never h.resume().
//   L2: co_await on a not-ready Future suspends the awaiting coroutine and yields
//       to the scheduler. See FutureAwaiter::await_suspend(): it parks the
//       handle in the shared state and returns control (true) to the scheduler.
//
// No wall-clock, no threads, no atomics, no locks: the shared state is a plain
// heap object reachable only from the single scheduler thread (L6). Lifetime is
// managed with std::shared_ptr so the Future and Promise can outlive each other
// in either order without a dangling waiter.

#include <cassert>
#include <coroutine>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Trace.hpp>
#include <lockstep/core/detail/SchedulerSink.hpp>

namespace lockstep::core {

namespace detail {

// The result of a future: either a value (for Future<T>), nothing (Future<void>
// success), or an Error. Templated on the value type; specialized for void.
template <class T>
struct ResultStorage {
    std::optional<T> value{};
    Error error{};
    [[nodiscard]] bool has_error() const noexcept { return !error.ok(); }
};

template <>
struct ResultStorage<void> {
    Error error{};
    [[nodiscard]] bool has_error() const noexcept { return !error.ok(); }
};

// The shared state behind a Future/Promise pair. Single-threaded: no locks, no
// atomics. Holds the (pending) result, the parked waiter, and a non-owning
// pointer to the scheduler sink used to SCHEDULE that waiter on completion (L1).
template <class T>
class SharedState {
public:
    // Bound to the scheduler that created this future. Never null in practice;
    // a future is always produced by a Promise minted from a Scheduler.
    explicit SharedState(SchedulerSink* sink) noexcept : sink_(sink) {}

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] SchedulerSink* sink() const noexcept { return sink_; }

    // Mark complete and SCHEDULE the parked waiter (if any) onto the ready queue.
    // This is L1's single enforcement point: completion never resumes inline.
    void complete() {
        assert(!ready_ && "future completed twice");
        ready_ = true;
        if (sink_ != nullptr) {
            sink_->trace(TraceAction::PromiseSet,
                         std::string("err=") + (result_.has_error() ? "1" : "0"));
        }
        if (waiter_ != nullptr) {
            std::coroutine_handle<> w = waiter_;
            waiter_ = nullptr;
            // L1: SCHEDULE, never resume inline.
            sink_->schedule(w, TraceAction::Schedule, "wake=future");
        }
    }

    // Park a waiting coroutine to be scheduled when this state completes. Called
    // from await_suspend on a not-ready future. At most one waiter (each future
    // is awaited by exactly one coroutine).
    void set_waiter(std::coroutine_handle<> h) noexcept {
        assert(waiter_ == nullptr && "future awaited by two coroutines");
        waiter_ = h;
    }

    ResultStorage<T>& result() noexcept { return result_; }
    [[nodiscard]] const ResultStorage<T>& result() const noexcept { return result_; }

private:
    SchedulerSink* sink_ = nullptr;
    bool ready_ = false;
    std::coroutine_handle<> waiter_{};
    ResultStorage<T> result_{};
};

} // namespace detail

template <class T> class Future;

// ---------------------------------------------------------------------------
// Promise<T> — the write end. Move-only. set_value / set_error each complete the
// shared state exactly once, which (L1) schedules the waiter rather than resuming
// it. NOTE: this is the user-facing async Promise, distinct from a coroutine's
// std::coroutine_traits promise_type (that lives in Task).
// ---------------------------------------------------------------------------
template <class T>
class Promise {
public:
    Promise() = default;
    explicit Promise(std::shared_ptr<detail::SharedState<T>> state) noexcept
        : state_(std::move(state)) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;
    ~Promise() = default;

    // Fulfill with a value. Schedules the waiter (L1). One-shot.
    void set_value(T v) {
        assert(state_ && "set_value on empty Promise");
        state_->result().value = std::move(v);
        state_->complete();
    }

    // Fulfill with an error. Schedules the waiter (L1). One-shot.
    void set_error(Error e) {
        assert(state_ && "set_error on empty Promise");
        state_->result().error = e;
        state_->complete();
    }

    // Mint the read end. Call once.
    [[nodiscard]] Future<T> get_future() noexcept { return Future<T>(state_); }

private:
    std::shared_ptr<detail::SharedState<T>> state_{};
};

// void specialization of the write end.
template <>
class Promise<void> {
public:
    Promise() = default;
    explicit Promise(std::shared_ptr<detail::SharedState<void>> state) noexcept
        : state_(std::move(state)) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) noexcept = default;
    ~Promise() = default;

    void set_value() {
        assert(state_ && "set_value on empty Promise");
        state_->complete();
    }
    void set_error(Error e) {
        assert(state_ && "set_error on empty Promise");
        state_->result().error = e;
        state_->complete();
    }
    [[nodiscard]] Future<void> get_future() noexcept;

private:
    std::shared_ptr<detail::SharedState<void>> state_{};
};

// ---------------------------------------------------------------------------
// Future<T> — the read end. Awaitable. co_await yields the value (or throws via
// the error path surfaced as a checked accessor). Move-only.
// ---------------------------------------------------------------------------
template <class T>
class Future {
public:
    using value_type = T;

    Future() = default;
    explicit Future(std::shared_ptr<detail::SharedState<T>> state) noexcept
        : state_(std::move(state)) {}

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;
    ~Future() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }
    [[nodiscard]] bool ready() const noexcept { return state_ && state_->ready(); }
    [[nodiscard]] bool has_error() const noexcept {
        return state_ && state_->result().has_error();
    }
    [[nodiscard]] Error error() const noexcept {
        return state_ ? state_->result().error : Error{};
    }

    // The awaiter (C1.1/L2). On a not-ready future, await_suspend parks the
    // awaiting coroutine and returns control to the scheduler (suspends + yields).
    struct Awaiter {
        std::shared_ptr<detail::SharedState<T>> state;

        // L2: only "ready" (skip suspension) when the result already exists.
        [[nodiscard]] bool await_ready() const noexcept {
            return state && state->ready();
        }

        // L2: suspend the awaiting coroutine; park its handle so completion
        // (which SCHEDULES, per L1) will re-enqueue it. Returning void here means
        // "stay suspended" — control returns to whoever resumed us (the
        // scheduler loop), i.e. we yield.
        void await_suspend(std::coroutine_handle<> awaiting) noexcept {
            state->set_waiter(awaiting);
        }

        // Deliver the value once resumed. The error, if any, is observable via
        // Future::has_error()/error() on the original future; here we return the
        // value (default-constructed-from-optional on the error path).
        T await_resume() {
            if (state->result().value.has_value()) {
                return std::move(*state->result().value);
            }
            return T{};
        }
    };

    Awaiter operator co_await() const noexcept { return Awaiter{state_}; }

private:
    std::shared_ptr<detail::SharedState<T>> state_{};
};

// void specialization of the read end.
template <>
class Future<void> {
public:
    using value_type = void;

    Future() = default;
    explicit Future(std::shared_ptr<detail::SharedState<void>> state) noexcept
        : state_(std::move(state)) {}

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;
    ~Future() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }
    [[nodiscard]] bool ready() const noexcept { return state_ && state_->ready(); }
    [[nodiscard]] bool has_error() const noexcept {
        return state_ && state_->result().has_error();
    }
    [[nodiscard]] Error error() const noexcept {
        return state_ ? state_->result().error : Error{};
    }

    struct Awaiter {
        std::shared_ptr<detail::SharedState<void>> state;
        [[nodiscard]] bool await_ready() const noexcept {
            return state && state->ready();
        }
        void await_suspend(std::coroutine_handle<> awaiting) noexcept {
            state->set_waiter(awaiting);
        }
        void await_resume() const noexcept {}
    };

    Awaiter operator co_await() const noexcept { return Awaiter{state_}; }

private:
    std::shared_ptr<detail::SharedState<void>> state_{};
};

inline Future<void> Promise<void>::get_future() noexcept { return Future<void>(state_); }

// Helpers to mint a fresh Promise/Future pair bound to a scheduler sink.
template <class T>
[[nodiscard]] Promise<T> make_promise(detail::SchedulerSink* sink) {
    return Promise<T>(std::make_shared<detail::SharedState<T>>(sink));
}

} // namespace lockstep::core
