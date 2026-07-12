#pragma once

// Task.hpp — C1.2. Task is the coroutine return type for every runtime coroutine.
//
// Semantics enforced here:
//   - A Task starts SUSPENDED (initial_suspend = suspend_always): spawning, not
//     construction, is what hands it to the scheduler. The scheduler owns the
//     first resume, keeping run() the single driver (L5).
//   - On co_await of a not-ready Future the coroutine suspends and yields to the
//     scheduler (that mechanism lives in Future.hpp / L2); Task just forwards it.
//   - Task completion may fulfill a waiter (tasks compose): when a Task that was
//     co_await-ed finishes, final_suspend SCHEDULES its awaiting coroutine onto
//     the ready queue — never inline-resumes it (L1). The scheduler is told who
//     to schedule via a SchedulerSink set at spawn time.
//   - Tasks are move-only and own their coroutine frame (RAII; destroyed on
//     Task destruction unless ownership was transferred to the scheduler).
//
// No threads/atomics/wall-clock: a Task is a plain coroutine frame resumed only
// from the single scheduler thread (L6).

#include <cassert>
#include <coroutine>
#include <exception>
#include <string>
#include <utility>

#include <lockstep/core/Trace.hpp>
#include <lockstep/core/detail/FramePool.hpp>
#include <lockstep/core/detail/SchedulerSink.hpp>

namespace lockstep::core {

class Task {
public:
    struct promise_type {
        // FRAME RECYCLING (S8.7 follow-on): route this coroutine's frame allocation
        // through the thread_local FramePool. The compiler calls promise::operator new
        // to allocate the frame and the sized promise::operator delete to free it, so
        // these two functions recycle frame storage instead of hitting the OS allocator
        // per spawn. Pure memory reuse — invisible to output (no value/order depends on a
        // frame address), so the deterministic sim stays byte-identical (see FramePool.hpp).
        static void* operator new(std::size_t n) { return detail::frame_alloc(n); }
        static void operator delete(void* p, std::size_t n) noexcept { detail::frame_free(p, n); }

        // The scheduler sink this task runs under. Set by the scheduler at spawn
        // (and propagated when one task co_awaits another) so final_suspend can
        // SCHEDULE the continuation rather than resume it inline (L1).
        detail::SchedulerSink* sink = nullptr;

        // The coroutine awaiting THIS task (if it was co_await-ed). When the task
        // finishes, this continuation is scheduled onto the ready queue. Null for
        // a top-level spawned task (nobody is awaiting it).
        std::coroutine_handle<> continuation{};

        // Trace id assigned at spawn, echoed in task_done for cross-referencing.
        std::uint64_t task_id = 0;

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // Start suspended: the scheduler owns the first resume (via spawn()).
        std::suspend_always initial_suspend() noexcept { return {}; }

        // final_suspend: on completion, SCHEDULE the awaiting continuation (L1).
        // We use a custom awaiter whose await_suspend enqueues the continuation
        // and returns void (stays suspended), so control returns to the scheduler
        // loop — the continuation runs later, in FIFO order, never inline here.
        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                promise_type& p = h.promise();
                if (p.sink != nullptr && p.sink->trace_wanted()) {
                    p.sink->trace(TraceAction::TaskDone,
                                  std::string("id=") + std::to_string(p.task_id));
                }
                if (p.continuation != nullptr && p.sink != nullptr) {
                    // L1: schedule the awaiting coroutine, do not resume inline.
                    p.sink->schedule(p.continuation, TraceAction::Schedule, "wake=task");
                }
                // Stay suspended; the frame is destroyed when its owning Task
                // (or the scheduler, post-spawn) is destroyed.
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        // Coroutines in the deterministic runtime do not throw across the
        // boundary; an escaped exception is a programming error. Terminate so it
        // surfaces loudly under sanitizers rather than corrupting determinism.
        void unhandled_exception() noexcept { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(handle_type h) noexcept : handle_(h) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    ~Task() { destroy(); }

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }

    // Release ownership of the coroutine frame to the caller (the scheduler takes
    // it at spawn so the frame outlives this temporary Task).
    [[nodiscard]] handle_type release() noexcept { return std::exchange(handle_, {}); }

    // Make a Task awaitable so tasks compose (co_await another Task). Awaiting a
    // task wires the awaiting coroutine as the child's continuation, propagates
    // the scheduler sink from the parent, and SCHEDULES the child's first resume
    // onto the ready queue (never inline, L1). The parent stays suspended; when
    // the child finishes, its final_suspend SCHEDULES the parent back (L1).
    //
    // NOTE: await_suspend takes the TYPED parent handle so it can read the
    // parent's sink (a type-erased handle could not). The awaiter OWNS the child
    // frame (the temporary Task released its handle into it) and destroys it once
    // the parent resumes, so the child outlives the await. Both child and parent
    // are driven only by run().
    struct Awaiter {
        handle_type child{}; // owned child frame; destroyed in await_resume
        Awaiter() = default;
        explicit Awaiter(handle_type c) noexcept : child(c) {}
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&& o) noexcept : child(std::exchange(o.child, {})) {}
        Awaiter& operator=(Awaiter&&) = delete;
        ~Awaiter() {
            if (child) {
                child.destroy();
            }
        }
        [[nodiscard]] bool await_ready() const noexcept {
            return !child || child.done();
        }
        template <class ParentPromise>
        void await_suspend(std::coroutine_handle<ParentPromise> parent) noexcept {
            detail::SchedulerSink* sink = parent.promise().sink;
            child.promise().sink = sink;
            child.promise().continuation = parent;
            if (sink != nullptr) {
                // L1: schedule the child's first resume; do not resume inline.
                sink->schedule(child, TraceAction::Schedule, "wake=compose");
            }
        }
        void await_resume() const noexcept {}
    };
    // Take ownership of this Task's frame into the awaiter (move-from temporary).
    Awaiter operator co_await() && noexcept { return Awaiter{release()}; }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }
    handle_type handle_{};
};

} // namespace lockstep::core
