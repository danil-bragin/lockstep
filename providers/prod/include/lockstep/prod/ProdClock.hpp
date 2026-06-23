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
// DELAY IS WIRED TO THE REACTOR (S4a). A correct prod delay() needs the prod
// REACTOR (epoll event loop) to wake the coroutine when wall-time passes the
// deadline WITHOUT a busy-wait or a rogue background thread (a threaded timer here
// would (a) violate the single-threaded determinism model the Scheduler assumes
// and (b) be a hack the brief forbids). S4a resolves the S2 deferral: a ProdClock
// constructed WITH an ITimerRegistrar* (the ProdReactor implements it) forwards
// delay(d) to reactor->arm_timer(d), returning a Future<void> the reactor's loop
// completes when real time passes the deadline. A ProdClock constructed WITHOUT a
// registrar (the now()-only S2 path) keeps the documented Unavailable stub so the
// macOS / no-reactor build still compiles and the now()-monotonic check still runs.
//
// ITimerRegistrar is the narrow seam between ProdClock (core IClock impl) and
// ProdReactor (the epoll loop) — it lives HERE in prod, NOT in core, so the core
// IClock interface is untouched (no core change). ProdReactor::delay() is the
// implementation; ProdClock just forwards to it.
//
// providers/prod/ is the lint-exempt boundary zone: std::chrono::steady_clock is
// permitted HERE (and only here).

#include <chrono> // std::chrono::steady_clock — ALLOWED only under providers/ (rule 1)
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>

namespace lockstep::prod {

// The narrow seam ProdClock uses to arm a real timer on the reactor. ProdReactor
// implements it (arm_timer == ProdReactor::delay). It lives in prod, NOT in core,
// so wiring delay() to the reactor needs NO change to the core IClock interface.
class ITimerRegistrar {
public:
    virtual ~ITimerRegistrar() = default;
    // Arm a real timer `d` ticks (ns) ahead of the reactor's now() and return a
    // Future<void> the reactor's loop completes once real time passes the deadline.
    // d <= 0 is permitted and completes promptly (next loop turn), never hangs.
    [[nodiscard]] virtual core::Future<void> arm_timer(core::Duration d) = 0;
};

// Production clock. now() is real monotonic wall-elapsed time in ticks (ns);
// delay() forwards to a bound reactor (ITimerRegistrar) when present, else the
// documented S2 Unavailable stub (see header note).
class ProdClock final : public core::IClock {
public:
    // The Tick unit for this clock: 1 tick == 1 nanosecond of steady_clock
    // elapsed time. Exposed so callers / the recorder agree on the unit.
    static constexpr core::Tick kTicksPerSecond = 1'000'000'000;

    // Origin = construction instant. now() reports nanoseconds since this origin.
    // No reactor bound: delay() is the now()-only S2 stub (Unavailable).
    ProdClock() noexcept : origin_(std::chrono::steady_clock::now()) {}

    // S4a: bind a reactor (ITimerRegistrar) so delay() arms a real epoll timer.
    // Explicit single-arg ctor (no implicit conversion from a registrar pointer).
    explicit ProdClock(ITimerRegistrar* reactor) noexcept
        : origin_(std::chrono::steady_clock::now()), reactor_(reactor) {}

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

    // S4a: forward to the bound reactor's real epoll timer when present. With no
    // reactor (the S2 now()-only path) keep the documented Unavailable stub: it
    // does NOT block, spawn a thread, or busy-wait. The reactor path does not
    // block here either — arm_timer() only ENQUEUES a timer; the reactor's run()
    // loop is what later fires it (single-threaded, epoll-driven).
    [[nodiscard]] core::Future<void> delay(core::Duration d) override {
        // Record the request so a test / recorder can see what was armed.
        pending_.push_back(d);
        if (reactor_ != nullptr) {
            return reactor_->arm_timer(d);
        }
        core::Promise<void> p;
        core::Future<void> f = p.get_future();
        p.set_error(core::Error{core::ErrorCode::Unavailable,
                                "ProdClock::delay needs a reactor (bind via "
                                "ProdClock(ITimerRegistrar*) — S4a)"});
        return f;
    }

    // Minimal registration surface the S4 reactor will drive: the durations armed
    // via delay() since construction, in arm order. Empty until S4 wires the loop.
    [[nodiscard]] const std::vector<core::Duration>& pending_timers() const noexcept {
        return pending_;
    }

private:
    std::chrono::steady_clock::time_point origin_;
    ITimerRegistrar* reactor_ = nullptr;     // bound reactor (S4a); null = S2 stub
    mutable core::Tick last_ = 0;            // last observed now() (monotonic clamp)
    std::vector<core::Duration> pending_{};  // armed-delay log (test/recorder view)
};

} // namespace lockstep::prod
