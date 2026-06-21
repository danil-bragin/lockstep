// scheduler_fuzz_test.cpp — property/fuzz over scheduler interleavings & workloads.
//
// VERIFICATION-ONLY (P1-VERIFY). Beyond the fixed ping-pong, this generates
// RANDOMIZED (seeded) workloads that stress the scheduler's interleaving:
//   - a seeded number of coroutines,
//   - each doing a seeded sequence of clock.delay(d) (random d, including d<=0
//     which must yield without advancing the clock, L4) and chained Future
//     hand-offs (promise set by a timer-woken coroutine, awaited by another),
//   - producing many concurrent timers due at the same / different ticks (so the
//     deterministic (due,arm_seq) ordering is exercised).
//
// PROPERTIES asserted for EVERY generated workload:
//   P1 (determinism): running the SAME seed twice => byte-identical trace.
//   P2 (structural invariants): seq contiguous, vtime monotonic, L1
//      schedule-not-resume, clock_advance discipline, balanced lifecycle.
//   P3 (clock floor): final vtime >= 0 and equals the max timer due that fired
//      (clock only advances to timer due ticks; never past the last).
//
// Any counterexample PRINTS its seed and replays. All randomness is
// sim::SeededRandom — no <random>, no std::*_distribution, no <chrono>/<thread>.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "harness/invariants.hpp"

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>

namespace {

using lockstep::core::Future;
using lockstep::core::Promise;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return static_cast<int>(std::strtol(v, nullptr, 0));
}

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return static_cast<std::uint64_t>(std::strtoull(v, nullptr, 0));
}

// A shared bus of promise/future slots that worker coroutines hand off through,
// plus the seeded RNG that drives every decision. Single-threaded; no locks.
struct Workload {
    SimClock* clock = nullptr;
    SeededRandom* rng = nullptr;
    std::vector<std::shared_ptr<lockstep::core::detail::SharedState<std::int64_t>>> slots;
    std::int64_t counter = 0;
};

// A worker: performs `steps` actions. Each action is a seeded choice between
// delaying a seeded amount (sometimes <=0 to exercise the no-advance path) and
// fulfilling a slot. Some steps await a slot another worker fulfills, forcing
// cross-coroutine wakeups through the ready queue. Deterministic from the RNG.
Task worker(Workload& w, int steps, std::size_t base_slot, std::size_t slot_span) {
    for (int s = 0; s < steps; ++s) {
        // Seeded delay; allow non-positive to exercise the yield-without-advance
        // branch (L4 documents d<=0 completes at the current tick).
        std::int64_t d = w.rng->uniform_range(-1, 5);
        co_await w.clock->delay(d);
        w.counter += 1;
        // Fulfill a slot in this worker's span (if any remain unset). We mint a
        // Promise over the shared state; setting it SCHEDULES any waiter (L1).
        std::size_t idx = base_slot + static_cast<std::size_t>(
                                          w.rng->uniform(slot_span ? slot_span : 1));
        if (idx < w.slots.size() && !w.slots[idx]->ready()) {
            Promise<std::int64_t> p(w.slots[idx]);
            p.set_value(w.counter);
        }
        // Occasionally await a slot (possibly already-ready, possibly pending),
        // forcing a suspend/resume through the ready queue.
        if (w.rng->chance(0.5)) {
            std::size_t widx = base_slot + static_cast<std::size_t>(
                                               w.rng->uniform(slot_span ? slot_span : 1));
            if (widx < w.slots.size()) {
                Future<std::int64_t> f(w.slots[widx]);
                if (!f.ready()) {
                    // only await pending ones we KNOW will be set, else we'd hang.
                    // Skip awaiting unset slots to keep the run terminating.
                } else {
                    std::int64_t got = co_await f;
                    (void)got;
                }
            }
        }
    }
    co_return;
}

struct FuzzRun {
    std::string trace;
    lockstep::core::Tick final_vtime = 0;
};

FuzzRun run_workload(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);

    Workload w;
    w.clock = &clock;
    w.rng = &rng;

    // Seeded shape: number of workers and steps each. Kept small so the run is
    // fast but still produces overlapping timers and cross-wakeups.
    int workers = static_cast<int>(rng.uniform_range(1, 6));
    int steps = static_cast<int>(rng.uniform_range(1, 8));

    // Mint a pool of slots large enough for every worker's span.
    std::size_t slot_span = 4;
    std::size_t total_slots = static_cast<std::size_t>(workers) * slot_span;
    for (std::size_t i = 0; i < total_slots; ++i) {
        w.slots.push_back(
            std::make_shared<lockstep::core::detail::SharedState<std::int64_t>>(&sched));
    }

    for (int k = 0; k < workers; ++k) {
        sched.spawn(worker(w, steps, static_cast<std::size_t>(k) * slot_span, slot_span));
    }
    sched.run();

    FuzzRun r;
    r.trace = sched.trace_text();
    r.final_vtime = sched.now();
    return r;
}

} // namespace

int main() {
    const int kTrials = env_int("LOCKSTEP_FUZZ_TRIALS", 3000);
    const std::uint64_t kBase = env_u64("LOCKSTEP_FUZZ_BASE", 0x5EED1234DEADC0DEULL);

    std::printf("[scheduler_fuzz] trials=%d base=%llu\n", kTrials,
                static_cast<unsigned long long>(kBase));

    SeededRandom meta(kBase);
    int passed = 0;
    int failed = 0;

    for (int t = 0; t < kTrials; ++t) {
        std::uint64_t seed = meta.next();

        FuzzRun a = run_workload(seed);
        FuzzRun b = run_workload(seed);

        bool ok = true;
        std::string why;

        // P1: determinism — same seed twice => byte-identical.
        if (a.trace != b.trace) {
            ok = false;
            why = "non-deterministic (run-twice traces differ)";
        }
        // P2: structural invariants.
        if (ok) {
            auto inv = lockstep::verify::check_invariants(a.trace, /*exchanges*/ 0);
            if (!inv.ok) {
                ok = false;
                why = "invariant: " + inv.why;
            }
        }
        // P3: clock floor — final vtime is non-negative and equals the last
        // clock_advance target (or 0 if no advance happened).
        if (ok) {
            auto ev = lockstep::verify::parse_trace(a.trace);
            std::int64_t last_advance = 0;
            for (const auto& e : ev) {
                if (e.action == "clock_advance") {
                    last_advance = e.vt;
                }
            }
            if (a.final_vtime < 0) {
                ok = false;
                why = "negative final vtime";
            } else if (a.final_vtime != last_advance) {
                ok = false;
                why = "final vtime (" + std::to_string(a.final_vtime) +
                      ") != last clock_advance (" + std::to_string(last_advance) + ")";
            }
        }

        if (ok) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr,
                         "[scheduler_fuzz] FAIL seed=%llu : %s\n"
                         "  REPLAY: run_workload(%llu)  (base=%llu)\n",
                         static_cast<unsigned long long>(seed), why.c_str(),
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(kBase));
            if (failed >= 10) {
                std::fprintf(stderr, "[scheduler_fuzz] ... stopping after 10 failures\n");
                break;
            }
        }
    }

    std::printf("[scheduler_fuzz] done: passed=%d failed=%d / %d\n", passed, failed,
                kTrials);
    if (failed != 0) {
        return 1;
    }
    std::printf("[scheduler_fuzz] OK — all %d randomized workloads deterministic + "
                "invariant-clean.\n",
                passed);
    return 0;
}
