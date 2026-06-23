// prod_reactor_test.cpp — Phase 7 S4a driver. Proves the PROD REACTOR (a real
// epoll-driven event loop) implements the SAME two core surfaces the sim Scheduler
// does, so the verified async core (Future/Promise/Task) runs on it UNCHANGED, and
// that ProdClock::delay() — wired to the reactor — now satisfies the S1 clock
// checks that were DEFERRED in S2 (resolving the S2 flag), ADAPTED for real time.
//
// LINUX-ONLY: this TU is only built on Linux (tests/CMakeLists.txt guards the
// target with if(UNIX AND NOT APPLE)); ProdReactor.hpp is #ifdef __linux__. So the
// macOS host build never sees it and stays green.
//
// EXACT-d -> REAL-ELAPSED ADAPTATION (the S2 resolution). In sim, delay(d) advances
// VIRTUAL time by EXACTLY d, so the harness asserts now()-advance == d exactly. On
// a REAL monotonic clock that is impossible — real time advances continuously and
// scheduling/epoll add slack. So here:
//   * `timers-fire-in-time-order` is UNIVERSAL and runs verbatim in spirit: arm two
//     timers, the nearer DEADLINE fires first. (T1)
//   * the prod analogue of `delay-advances-exactly-d` is `delay completes after
//     >= d REAL elapsed` WITH A TOLERANCE (never exact). We assert real elapsed
//     (measured via reactor.now(), the same monotonic ns source) is >= d. The
//     sim's EXACT-d assertion is documented sim-specific (exact virtual time), NOT
//     universal. (T2)
//   * `delay(<=0)` completes PROMPTLY (next loop turn) and does not hang. (T3)
//   * the core actually RUNS on the reactor: a coroutine co_awaits ProdClock::
//     delay() twice + a Promise resolved by another scheduled task; ProdReactor::
//     run() drives it to quiescence; it completes in real-time order. (T4)
//
// This is NON-provider code (a test) -> the forbidden-call lint scans it; it
// touches NO <chrono>/<thread>/<sys/epoll.h>. All real time + epoll stay inside
// providers/prod/ProdReactor.hpp; the test reads time only via reactor.now().

#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdClock.hpp>
#include <lockstep/prod/ProdReactor.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;

// 1 ms in ProdClock ns ticks. Small durations keep the test BOUNDED.
constexpr core::Tick kMs = 1'000'000;

// ---------------------------------------------------------------------------
// (T1) timers-fire-in-time-order — UNIVERSAL. Arm two timers at the same instant,
// 3ms and 9ms; the nearer DEADLINE must fire first. Drives ProdReactor::run() to
// quiescence; the coroutines run on the reactor UNCHANGED via ProdClock::delay().
// ---------------------------------------------------------------------------
struct OrderState {
    std::vector<int> fire_order{};
};

core::Task timer_task(core::IClock& clock, core::Duration d, int id, OrderState& s) {
    co_await clock.delay(d);
    s.fire_order.push_back(id);
    co_return;
}

bool check_timers_fire_in_order() {
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "T1: epoll_create1 failed\n");
        return false;
    }
    prod::ProdClock clock(&reactor); // bound -> delay() arms real reactor timers
    OrderState s;
    reactor.spawn(timer_task(clock, 9 * kMs, /*id=*/9, s));
    reactor.spawn(timer_task(clock, 3 * kMs, /*id=*/3, s));
    reactor.run(); // to quiescence
    const bool ok =
        s.fire_order.size() == 2 && s.fire_order[0] == 3 && s.fire_order[1] == 9;
    std::string got;
    for (int id : s.fire_order) {
        got += std::to_string(id) + " ";
    }
    std::printf("%s T1 clock/timers-fire-in-time-order  expected [3 9] got [%s]\n",
                ok ? "PASS" : "FAIL", got.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// (T2) delay completes after >= d REAL elapsed (the prod analogue of sim's exact-d
// advance, with tolerance). Measure now() before/after a 5ms delay on the reactor;
// the real elapsed must be >= the requested span (monotonic; no exactness). Also
// re-checks monotonicity across the await.
// ---------------------------------------------------------------------------
struct ElapsedState {
    bool ran = false;
    core::Tick before = 0;
    core::Tick after = 0;
};

core::Task elapsed_task(prod::ProdReactor& reactor, core::IClock& clock,
                        core::Duration d, ElapsedState& s) {
    s.before = reactor.now();
    co_await clock.delay(d);
    s.after = reactor.now();
    s.ran = true;
    co_return;
}

bool check_delay_real_elapsed() {
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "T2: epoll_create1 failed\n");
        return false;
    }
    prod::ProdClock clock(&reactor);
    ElapsedState s;
    constexpr core::Duration kSpan = 5 * kMs;
    reactor.spawn(elapsed_task(reactor, clock, kSpan, s));
    reactor.run();
    const core::Tick elapsed = s.after - s.before;
    // >= d real elapsed, monotonic (after >= before). NOT exact (real time slack).
    const bool ok = s.ran && elapsed >= kSpan && s.after >= s.before;
    std::printf("%s T2 clock/delay-completes-after-real-elapsed-%lldms  "
                "(requested %lld ns, real elapsed %lld ns)\n",
                ok ? "PASS" : "FAIL", static_cast<long long>(kSpan / kMs),
                static_cast<long long>(kSpan), static_cast<long long>(elapsed));
    return ok;
}

// ---------------------------------------------------------------------------
// (T3) delay(<=0) completes PROMPTLY (next loop turn) and does not hang. A timer
// due at now() fires on the first wait turn (epoll timeout clamped to 0); the
// coroutine resumes. run() returns (no wedge). We also assert it actually ran.
// ---------------------------------------------------------------------------
struct NonPosState {
    bool ran = false;
    int resumes = 0;
};

core::Task nonpos_task(core::IClock& clock, NonPosState& s) {
    co_await clock.delay(0);
    ++s.resumes;
    co_await clock.delay(-5);
    ++s.resumes;
    s.ran = true;
    co_return;
}

bool check_delay_nonpositive_no_hang() {
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "T3: epoll_create1 failed\n");
        return false;
    }
    prod::ProdClock clock(&reactor);
    NonPosState s;
    reactor.spawn(nonpos_task(clock, s));
    reactor.run(); // must TERMINATE (no hang) — the wall guard around the binary
                   // also catches a regression, but to quiescence is the contract.
    const bool ok = s.ran && s.resumes == 2;
    std::printf("%s T3 clock/delay-nonpositive-completes-no-hang  (resumes=%d)\n",
                ok ? "PASS" : "FAIL", s.resumes);
    return ok;
}

// ---------------------------------------------------------------------------
// (T4) THE CORE RUNS ON THE REACTOR. A coroutine co_awaits ProdClock::delay()
// twice AND a Promise resolved by ANOTHER scheduled task (a "resolver" coroutine
// that the same reactor runs), all driven by ProdReactor::run() to quiescence. It
// completes in real-time order (monotonic timestamps at each checkpoint). This is
// the verified Future/Promise/Task core running on real epoll + real timers with
// no core change — the whole point of the seam.
// ---------------------------------------------------------------------------
struct CoreRunState {
    bool ran = false;
    bool resolved = false;
    bool monotonic = true;
    core::Tick t0 = 0;
    core::Tick t1 = 0;
    core::Tick t2 = 0;
    core::Tick t3 = 0;
};

// The resolver: scheduled on the reactor, it fulfils `p` (which SCHEDULES the
// awaiting main coroutine, L1). It just sets the value — no inline resume.
core::Task resolver_task(core::Promise<void> p, CoreRunState& s) {
    s.resolved = true;
    p.set_value();
    co_return;
}

core::Task core_driver(prod::ProdReactor& reactor, core::IClock& clock,
                       core::Future<void> gate, CoreRunState& s) {
    s.t0 = reactor.now();
    co_await clock.delay(2 * kMs); // first real timer
    s.t1 = reactor.now();
    co_await gate;                 // Promise resolved by resolver_task (L1 wake)
    s.t2 = reactor.now();
    co_await clock.delay(2 * kMs); // second real timer
    s.t3 = reactor.now();
    s.monotonic = (s.t0 <= s.t1) && (s.t1 <= s.t2) && (s.t2 <= s.t3);
    s.ran = true;
    co_return;
}

bool check_core_runs_on_reactor() {
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "T4: epoll_create1 failed\n");
        return false;
    }
    prod::ProdClock clock(&reactor);
    CoreRunState s;
    // Mint a Promise/Future pair bound to the reactor-as-SchedulerSink, so the
    // waiter routes through L1 exactly like in sim.
    core::Promise<void> p = core::make_promise<void>(&reactor);
    core::Future<void> gate = p.get_future();
    reactor.spawn(core_driver(reactor, clock, std::move(gate), s));
    reactor.spawn(resolver_task(std::move(p), s));
    reactor.run(); // to quiescence
    const bool ok = s.ran && s.resolved && s.monotonic;
    std::printf("%s T4 core-runs-on-reactor (delay x2 + Promise wake)  "
                "ran=%d resolved=%d monotonic=%d  ts=[%lld %lld %lld %lld]\n",
                ok ? "PASS" : "FAIL", static_cast<int>(s.ran),
                static_cast<int>(s.resolved), static_cast<int>(s.monotonic),
                static_cast<long long>(s.t0), static_cast<long long>(s.t1),
                static_cast<long long>(s.t2), static_cast<long long>(s.t3));
    return ok;
}

} // namespace

int main() {
    std::printf("[prod_reactor_test] Phase 7 S4a — PROD REACTOR (epoll loop + real "
                "timers); core runs UNCHANGED\n\n");
    bool all = true;
    all = check_timers_fire_in_order() && all;
    all = check_delay_real_elapsed() && all;
    all = check_delay_nonpositive_no_hang() && all;
    all = check_core_runs_on_reactor() && all;
    std::printf("\n[prod_reactor_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
