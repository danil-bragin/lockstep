#pragma once

// ProdClock.hpp — Phase 7 S2. The PRODUCTION IClock provider (now() only; timer
// firing lands in S4 with the epoll reactor).
//
// V-PROD-CONTRACT: now() must satisfy the SAME universal IClock contract as
// SimClock — specifically "never decreases across calls on the same clock". We
// back now() with std::chrono::steady_clock (a MONOTONIC source by C++ guarantee:
// it cannot run backwards), mapped to core::Tick as NANOSECONDS elapsed since the
// clock's construction origin. Steady-clock monotonicity gives the conformance
// `clock/now-monotonic` check for free, and a per-instance origin keeps Tick
// values small and origin-relative (a Tick is "opaque ticks since an unspecified
// origin" per IClock.hpp — we pick construction time as that origin).
//
// DELAY IS DEFERRED TO S4. A correct prod delay() needs the prod REACTOR (epoll
// event loop) to wake the coroutine when wall-time passes the deadline WITHOUT a
// busy-wait or a rogue background thread (a threaded timer here would (a) violate
// the single-threaded determinism model the Scheduler assumes and (b) be a hack
// the brief explicitly forbids). So delay() returns an immediately-completing
// Future carrying ErrorCode::Unavailable with a clear "deferred to S4" detail,
// and we expose the minimal registration surface (pending_timers()) the S4
// reactor will drain. The universal `clock/delay-*` and `timers-fire-in-order`
// checks are therefore DEFERRED to S4 for prod; only `now-monotonic` runs now.
//
// providers/prod/ is the lint-exempt boundary zone: std::chrono::steady_clock is
// permitted HERE (and only here).

#include <chrono> // std::chrono::steady_clock — ALLOWED only under providers/ (rule 1)
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>

namespace lockstep::prod {

// Production clock. now() is real monotonic wall-elapsed time in ticks (ns);
// delay() is a documented S4-deferred stub (see header note).
class ProdClock final : public core::IClock {
public:
    // The Tick unit for this clock: 1 tick == 1 nanosecond of steady_clock
    // elapsed time. Exposed so callers / the recorder agree on the unit.
    static constexpr core::Tick kTicksPerSecond = 1'000'000'000;

    // Origin = construction instant. now() reports nanoseconds since this origin.
    ProdClock() noexcept : origin_(std::chrono::steady_clock::now()) {}

    // Real monotonic time as a Tick (ns since origin). steady_clock guarantees the
    // underlying count never decreases, so successive now() calls are monotonic
    // non-decreasing — satisfying the universal contract. We also clamp against a
    // remembered max so the *observable* value can never regress even if a future
    // libc quirk were to violate the steady guarantee (defence in depth; the
    // clamp is a max(), it never moves time backward and never invents advance).
    [[nodiscard]] core::Tick now() const noexcept override {
        auto d = std::chrono::steady_clock::now() - origin_;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        core::Tick t = static_cast<core::Tick>(ns);
        if (t < last_) {
            t = last_; // never let an observation decrease (belt-and-suspenders)
        }
        last_ = t;
        return t;
    }

    // DEFERRED TO S4 (prod reactor). Returns an already-complete Future carrying
    // Unavailable; it does NOT block, spawn a thread, or busy-wait. The S4 reactor
    // will replace this with a real epoll-driven timer that drains pending_timers().
    [[nodiscard]] core::Future<void> delay(core::Duration d) override {
        // Record the request so the S4 reactor (or a test) can see what was armed.
        pending_.push_back(d);
        core::Promise<void> p;
        core::Future<void> f = p.get_future();
        p.set_error(core::Error{core::ErrorCode::Unavailable,
                                "ProdClock::delay deferred to S4 (epoll reactor)"});
        return f;
    }

    // Minimal registration surface the S4 reactor will drive: the durations armed
    // via delay() since construction, in arm order. Empty until S4 wires the loop.
    [[nodiscard]] const std::vector<core::Duration>& pending_timers() const noexcept {
        return pending_;
    }

private:
    std::chrono::steady_clock::time_point origin_;
    mutable core::Tick last_ = 0;            // last observed now() (monotonic clamp)
    std::vector<core::Duration> pending_{};  // S4 reactor input (armed delays)
};

} // namespace lockstep::prod
