// txn_harness_test.cpp — Phase 5 Stage M GATE: the distributed-txn verification
// machinery has TEETH (briefs/phase5.md Stage M; specs/CommitOrdering.tla the
// source of truth). Authored VERIFICATION-FIRST — before the real txn impl — so
// the differential + per-D5-level + OLLP + linearizability checker battery is
// proven to JUDGE correctly first. A harness that passes a known-broken txn
// executor IS the bug.
//
// WHAT THIS ASSERTS (binding):
//   (A) TEETH — each DELIBERATELY-WRONG executor is FLAGGED by the checker that
//       maps to the CommitOrdering.tla invariant / D5 rule it breaks, with a
//       non-empty witness + the replayable seed:
//         StaleFootprintExecutor  → ollp_sound          (OLLPSound; skips recon)
//         OutOfOrderExecutor      → serialized_by_seqlog (SerializedBySeqLog) /
//                                   strict_serializable  (real-time)
//         StaleStrictReadExecutor → strict_serializable  (ReadsMatchSerialPrefix)
//         RywLosesWriteExecutor   → read_your_writes_level (D5ReadYourWrites)
//   (B) BASELINES —
//         HonestExecutor      passes ALL checkers across a seed sweep AND makes
//                             progress (the clean baseline; no false positives).
//         NoProgressExecutor  passes every SAFETY checker (vacuously) BUT makes
//                             NO progress — explicitly NOT mistaken for correct.
//   (C) DETERMINISM — same seed ⇒ byte-identical rendered outcome (in-test diff;
//       the external double-run diff is the gate's separate check).
//
// DETERMINISM: pure function of (seed). The only entropy is the seed, consumed by
// the inlined SplitMix in the harness; no clock, no ambient randomness, ordered
// maps throughout. NON-provider TU → the forbidden-call lint scans it. Seeds are
// printed for replay (V-CHK2). Every run is bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/DiffHarness.hpp>
#include <lockstep/txn/Oracle.hpp>
#include <lockstep/txn/TeethExecutors.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

using lockstep::txn::DiffOutcome;
using lockstep::txn::ExecutorFactory;
using lockstep::txn::honest_factory;
using lockstep::txn::no_progress_factory;
using lockstep::txn::out_of_order_factory;
using lockstep::txn::render_outcome;
using lockstep::txn::run_diff_seed;
using lockstep::txn::ryw_loses_write_factory;
using lockstep::txn::stale_footprint_factory;
using lockstep::txn::stale_strict_read_factory;
using lockstep::txn::Verdict;
using lockstep::txn::WorkloadConfig;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

// In-gate sweep bound (kept small; freeze discipline: <= 64 seeds).
constexpr std::uint64_t kSeeds = 48;

WorkloadConfig make_wc() {
    WorkloadConfig wc;
    wc.num_txns = 8;
    wc.num_keys = 4;
    wc.exec.max_retry = 2;
    wc.exec.replica_lag = 1;  // a 1-entry-behind local replica (within bounds)
    return wc;
}

// Did `checker` produce a VIOLATION (with a witness) for this outcome?
bool flagged_by(const DiffOutcome& o, const char* checker) {
    for (const Verdict& v : o.verdicts) {
        if (v.checker == checker && !v.ok) {
            return !v.witness.empty();
        }
    }
    return false;
}

// Find the first seed in [0,kSeeds) for which `factory` is flagged by `checker`.
// Returns true + the witnessing outcome (so we can print witness + seed), false
// if NO seed flags it (the teeth has no bite → a checker gap).
bool find_flag(const ExecutorFactory& factory, const char* checker,
               const WorkloadConfig& wc, DiffOutcome& out) {
    for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
        DiffOutcome o = run_diff_seed(seed, factory, wc);
        if (flagged_by(o, checker)) {
            out = std::move(o);
            return true;
        }
    }
    return false;
}

void print_witness(const char* label, const char* checker, const DiffOutcome& o) {
    for (const Verdict& v : o.verdicts) {
        if (v.checker == checker && !v.ok) {
            std::fprintf(stderr,
                         "[TEETH] %s flagged by %s (seed=%llu)\n"
                         "        spec_ref: %s\n"
                         "        witness : %s\n",
                         label, checker,
                         static_cast<unsigned long long>(v.seed), v.spec_ref.c_str(),
                         v.witness.c_str());
            return;
        }
    }
}

}  // namespace

int main() {
    const WorkloadConfig wc = make_wc();

    // ===================================================================
    // (B) BASELINE 1 — the HONEST executor passes ALL checkers + progresses.
    // ===================================================================
    {
        bool any_progress = false;
        for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
            const DiffOutcome o = run_diff_seed(seed, honest_factory(), wc);
            if (!o.all_ok) {
                for (const Verdict& v : o.verdicts) {
                    if (!v.ok) {
                        std::fprintf(stderr,
                                     "HONEST FALSE-POSITIVE seed=%llu checker=%s "
                                     "witness=%s\n",
                                     static_cast<unsigned long long>(seed),
                                     v.checker.c_str(), v.witness.c_str());
                    }
                }
            }
            check(o.all_ok, "honest executor passes every checker (no false positive)");
            if (o.sut_made_progress) {
                any_progress = true;
            }
        }
        check(any_progress, "honest executor makes progress on some seed");
        std::fprintf(stderr, "[BASELINE] HonestExecutor: clean across %llu seeds.\n",
                     static_cast<unsigned long long>(kSeeds));
    }

    // ===================================================================
    // (B) BASELINE 2 — NoProgress: vacuously safe BUT no progress (NOT correct).
    // ===================================================================
    {
        bool any_safety_false_positive = false;
        bool any_progress = false;
        for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
            const DiffOutcome o = run_diff_seed(seed, no_progress_factory(), wc);
            // The SAFETY checkers (everything except differential) should NOT flag
            // a system that never commits — vacuously safe.
            for (const Verdict& v : o.verdicts) {
                if (v.checker != "differential_vs_oracle" && !v.ok) {
                    any_safety_false_positive = true;
                }
            }
            if (o.sut_made_progress) {
                any_progress = true;
            }
        }
        check(!any_safety_false_positive,
              "no-progress executor is vacuously SAFE (safety checkers don't "
              "false-positive a dead system)");
        check(!any_progress,
              "no-progress executor makes NO progress (vacuous safety is NOT "
              "mistaken for correctness)");
        std::fprintf(stderr,
                     "[BASELINE] NoProgressExecutor: vacuously safe, ZERO progress "
                     "(correctly NOT counted as correct).\n");
    }

    // ===================================================================
    // (A) TEETH — each wrong executor flagged by the RIGHT checker, w/ witness+seed.
    // ===================================================================
    {
        DiffOutcome o;
        // (a) skips OLLP recon → commits on a stale footprint → ollp_sound.
        check(find_flag(stale_footprint_factory(), "ollp_sound", wc, o),
              "StaleFootprintExecutor FLAGGED by ollp_sound (OLLPSound)");
        print_witness("StaleFootprintExecutor", "ollp_sound", o);

        // (b) applies out of seqLog order → serialized_by_seqlog (the commit
        //     order must be a subsequence of the agreed seqLog) AND it diverges
        //     from the serial oracle (differential).
        check(find_flag(out_of_order_factory(), "serialized_by_seqlog", wc, o),
              "OutOfOrderExecutor FLAGGED by serialized_by_seqlog (SerializedBySeqLog)");
        print_witness("OutOfOrderExecutor", "serialized_by_seqlog", o);
        check(find_flag(out_of_order_factory(), "differential_vs_oracle", wc, o),
              "OutOfOrderExecutor FLAGGED by differential_vs_oracle (≠ serial oracle)");
        print_witness("OutOfOrderExecutor", "differential_vs_oracle", o);

        // (c) serves a strict read a stale/snapshot value → strict_serializable.
        check(find_flag(stale_strict_read_factory(), "strict_serializable", wc, o),
              "StaleStrictReadExecutor FLAGGED by strict_serializable "
              "(ReadsMatchSerialPrefix / linearizability)");
        print_witness("StaleStrictReadExecutor", "strict_serializable", o);

        // (d) a RYW session loses its own write → read_your_writes_level.
        check(find_flag(ryw_loses_write_factory(), "read_your_writes_level", wc, o),
              "RywLosesWriteExecutor FLAGGED by read_your_writes_level "
              "(D5ReadYourWrites)");
        print_witness("RywLosesWriteExecutor", "read_your_writes_level", o);
    }

    // ===================================================================
    // (C) DETERMINISM — same seed ⇒ byte-identical rendered outcome (in-test).
    // ===================================================================
    {
        bool all_identical = true;
        for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
            const DiffOutcome a = run_diff_seed(seed, honest_factory(), wc);
            const DiffOutcome b = run_diff_seed(seed, honest_factory(), wc);
            if (render_outcome(a) != render_outcome(b)) {
                std::fprintf(stderr, "NONDETERMINISM at seed=%llu\n",
                             static_cast<unsigned long long>(seed));
                all_identical = false;
            }
        }
        check(all_identical, "harness is a pure function of seed (byte-identical "
                             "render on replay)");
    }

    // ===================================================================
    // (C') DETERMINISM EXTERNAL SURFACE — print a stable marker block the gate's
    // double-run external diff compares. Honest run, fixed seed.
    // ===================================================================
    {
        const DiffOutcome o = run_diff_seed(7, honest_factory(), wc);
        std::printf("===TXN-HARNESS-DETERMINISM-BLOCK-BEGIN===\n");
        std::printf("%s", render_outcome(o).c_str());
        std::printf("===TXN-HARNESS-DETERMINISM-BLOCK-END===\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\nTXN HARNESS GATE: FAIL (%d assertion(s)).\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nTXN HARNESS GATE: PASS — machinery has teeth, "
                         "baselines clean.\n");
    return 0;
}
