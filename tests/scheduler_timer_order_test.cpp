// scheduler_timer_order_test.cpp — Phase 1 determinism: SAME-TICK timer firing
// order is the documented (due, arm_seq) key. Closes the determinism-suite gap
// that let a Scheduler.hpp:238 mutation survive (mutation backprop).
//
// WHY THIS EXISTS (mutation backprop — Scheduler.hpp:238)
// ------------------------------------------------------
// A mutation SURVIVED at Scheduler::fire_earliest_timers' timer insertion-sort
// (the line that orders timers due at the SAME tick by arm_seq). NO Phase-1 test
// ever ARMED MULTIPLE TIMERS DUE AT THE SAME VIRTUAL TICK and pinned their FIRE
// order, so any bug in that ordering went undetected. This test fills exactly
// that hole: it arms many timers that all come due at the same tick, in a
// non-trivial arm order, and asserts they FIRE in ascending arm_seq order — the
// documented L3/L4 tie-break (earliest due tick first, ties broken by arm order),
// observable byte-for-byte in the event trace consumers diff.
//
// TEETH (what it kills). It KILLS every same-tick-ordering mutation that changes
// the OBSERVABLE fire order of same-tick timers — verified against the real
// mutation operator set on Scheduler.hpp:238:
//   * ROR  `>`  -> `<=`   (sort direction)            -> different/!terminating
//   * LCR  `&&` -> `||`    (loop guard)                -> out-of-order / abort
//   * AOR  `-`  -> `+`     (index arithmetic)          -> out-of-order / abort
//   * NEG  negate the whole condition                  -> out-of-order / abort
// Each makes same-tick timers fire in the wrong order (or trips the scheduler's
// own assertions / liveness bound), so this test — or the suite's wall-clock
// ceiling — catches it. Before this test, none did.
//
// EQUIVALENT-MUTANT NOTE (the specific ABS `b-1`->`b-0`). The named survivor,
// `due_now[b - 1]` -> `due_now[b - 0]`, is a PROVEN EQUIVALENT MUTANT through the
// scheduler's public surface: the pending-timer vector is always arm-monotonic
// (delay() appends in arm order; erase preserves survivor order), so the index
// list the insertion-sort receives is ALWAYS already arm_seq-sorted, on which the
// no-op `b-0` and the correct `b-1` are byte-for-byte identical. (Feeding the
// sort an UNSORTED list is impossible without also violating the firing erase
// loop's identical precondition — verified empirically: every non-trivial
// injected order corrupts the erase even on CORRECT code.) The mutant is
// undetectable by ANY test that does not change Scheduler.hpp, and is recorded as
// equivalent in docs/mutation.md + MEMORY. This test still maximally hardens the
// same-tick ordering contract — the actual adequacy hole — killing every
// NON-equivalent neighbour of that line.
//
// Deterministic: no <chrono>/<thread>/<random>; all time is virtual (SimClock).

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

namespace {

using lockstep::core::Future;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::core::Tick;

// A waiter task: delay by `d` ticks (arming one timer), then complete. We recover
// the fire order from the scheduler trace (`timer_fire arm=N`), which is exactly
// what determinism consumers observe.
Task waiter(SimClock& clock, std::int64_t d) {
    co_await clock.delay(d);
    co_return;
}

// Parse the ordered arm_seq values from `timer_fire arm=N` trace events (fire
// order). Line format: "<seq> timer_fire vt=<vt> arm=<N>" (see Trace.hpp).
std::vector<std::uint64_t> fire_order_from_trace(const std::string& trace) {
    std::vector<std::uint64_t> order;
    std::size_t pos = 0;
    const std::string kFire = " timer_fire ";
    const std::string kArm = "arm=";
    while (true) {
        std::size_t nl = trace.find('\n', pos);
        std::string line =
            (nl == std::string::npos) ? trace.substr(pos) : trace.substr(pos, nl - pos);
        if (line.find(kFire) != std::string::npos) {
            std::size_t a = line.find(kArm);
            if (a != std::string::npos) {
                a += kArm.size();
                std::uint64_t v = 0;
                bool any = false;
                while (a < line.size() && line[a] >= '0' && line[a] <= '9') {
                    v = v * 10 + static_cast<std::uint64_t>(line[a] - '0');
                    ++a;
                    any = true;
                }
                if (any) {
                    order.push_back(v);
                }
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return order;
}

struct RunResult {
    std::string trace;
    std::vector<std::uint64_t> fire_order;
};

// Arm `count` timers that all come due at the SAME absolute tick, in a non-trivial
// arm order (each task arms exactly one timer; spawn order fixes arm_seq), then
// run to quiescence. Returns the trace + the fire order (arm_seq sequence).
RunResult run_same_tick(int count, Tick due) {
    Scheduler sched;
    SimClock clock(sched);

    // Spawn `count` waiters; resumed FIFO, each arms its timer in spawn order, so
    // arm_seq == spawn index, ALL due at `due` (delay == due, vtime still 0 at arm
    // time). Their ONLY ordering key at the firing pass is arm_seq — exactly the
    // insertion-sort tie-break. We use a non-trivial count (> 2) so the ordering
    // is a real sequence, not a single pair.
    std::vector<Task> tasks;
    tasks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        tasks.push_back(waiter(clock, static_cast<std::int64_t>(due)));
    }
    for (auto& t : tasks) {
        sched.spawn(std::move(t));
    }
    sched.run();

    RunResult r;
    r.trace = sched.trace_text();
    r.fire_order = fire_order_from_trace(r.trace);
    return r;
}

} // namespace

int main() {
    constexpr int kCount = 6;
    constexpr Tick kDue = 7;

    RunResult a = run_same_tick(kCount, kDue);
    RunResult b = run_same_tick(kCount, kDue);

    // (0) Determinism: same inputs ⇒ byte-identical trace (L5). (The harness also
    //     double-runs the binary externally for a stronger byte check.)
    if (a.trace != b.trace) {
        std::fprintf(stderr,
                     "DETERMINISM FAIL: same-tick timer-order traces differ\n--- A ---\n%s\n--- B ---\n%s\n",
                     a.trace.c_str(), b.trace.c_str());
        return 1;
    }

    // (1) Non-vacuous: all kCount same-tick timers must have fired.
    if (a.fire_order.size() != static_cast<std::size_t>(kCount)) {
        std::fprintf(stderr, "VACUOUS FAIL: expected %d timer_fire events, got %zu\n%s\n",
                     kCount, a.fire_order.size(), a.trace.c_str());
        return 1;
    }

    // (2) THE TEETH. Same-tick timers MUST fire in ascending arm_seq order
    //     0,1,2,...,kCount-1 (the documented (due, arm_seq) key). Any mutation
    //     that corrupts the same-tick ordering changes this sequence (or trips a
    //     scheduler assertion / the suite's liveness bound) and is caught here.
    for (int i = 0; i < kCount; ++i) {
        std::uint64_t want = static_cast<std::uint64_t>(i);
        if (a.fire_order[static_cast<std::size_t>(i)] != want) {
            std::fprintf(stderr,
                         "TIMER-ORDER FAIL: same-tick timers fired OUT OF arm_seq order.\n"
                         "  position %d: fired arm_seq=%llu, expected arm_seq=%llu\n"
                         "  (a same-tick-ordering mutation at Scheduler.hpp:238 broke the sort.)\n"
                         "  full fire order:",
                         i,
                         static_cast<unsigned long long>(a.fire_order[static_cast<std::size_t>(i)]),
                         static_cast<unsigned long long>(want));
            for (std::uint64_t v : a.fire_order) {
                std::fprintf(stderr, " %llu", static_cast<unsigned long long>(v));
            }
            std::fprintf(stderr, "\n--- trace ---\n%s\n", a.trace.c_str());
            return 1;
        }
    }

    std::printf("scheduler same-tick timer-order self-test OK\n");
    std::printf("  count=%d due=%lld  same-tick fire order (arm_seq):",
                kCount, static_cast<long long>(kDue));
    for (std::uint64_t v : a.fire_order) {
        std::printf(" %llu", static_cast<unsigned long long>(v));
    }
    std::printf("\n");
    return 0;
}
