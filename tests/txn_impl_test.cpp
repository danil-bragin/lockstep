// txn_impl_test.cpp — Phase 5 Stage I GATE for P5-IMPL.
//
// Drives the REAL DeterministicExecutor (txn/DeterministicExecutor.hpp) through
// the Stage-M DiffHarness + ALL 8 checkers over a seed sweep and asserts:
//   (A) DIFFERENTIAL + ALL 8 CHECKERS, 0 violations — the SUT matches the
//       strict-serializable oracle, is linearizable (strict reads), each D5 level
//       honors EXACTLY its contract (Snapshot no-torn / BoundedStaleness within
//       lag / ReadYourWrites sees own writes), OLLP is sound + retry terminates,
//       and ExactlyOnce holds (CommitOrdering.tla SerializedBySeqLog /
//       ReadsMatchSerialPrefix / StoreReflectsHistory / OLLPSound / ExactlyOnce +
//       D5Snapshot / D5BoundedStale / D5ReadYourWrites).
//   (B) PROGRESS — across the sweep the executor COMMITS txns (not vacuous;
//       a NoProgressExecutor would pass every safety checker yet commit nothing).
//   (C) DETERMINISM — same seed => byte-identical rendered outcome (in-test
//       double run; the external double-run diff is in the verify receipt).
//
// The sweep also varies the D5 knobs (replica_lag, max_retry) so every level +
// the OLLP re-sequence/abort branches are exercised across the seeds.
//
// Determinism (binding; txn/ is NOT lint-exempt): the only entropy is the seed,
// consumed by the harness's inlined SplitMix; the executor is a pure fn of
// (ordered_txns, cfg). Bounded: <=64 seeds in-gate (LOCKSTEP_TXN_SEEDS overrides),
// CTest TIMEOUT 90.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/DeterministicExecutor.hpp>
#include <lockstep/txn/DiffHarness.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

using namespace lockstep::txn;

int g_failures = 0;

#define IMPL_CHECK(cond, msg)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "txn_impl_test FAIL [%s:%d]: %s\n", __FILE__, \
                         __LINE__, (msg));                                     \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// The in-gate sweep size. Bounded; env override for a heavier soak.
std::uint64_t sweep_count() {
    const char* env = std::getenv("LOCKSTEP_TXN_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 64;
}

// A small matrix of workload/exec configs so every D5 level + the OLLP
// re-sequence/abort branches get exercised across the sweep.
std::vector<WorkloadConfig> config_matrix() {
    std::vector<WorkloadConfig> cfgs;
    for (std::size_t ntxns : {std::size_t{4}, std::size_t{8}, std::size_t{12}}) {
        for (std::size_t nkeys : {std::size_t{3}, std::size_t{5}}) {
            for (Seq lag : {Seq{0}, Seq{1}, Seq{2}}) {
                for (Seq retry : {Seq{1}, Seq{2}, Seq{3}}) {
                    WorkloadConfig wc;
                    wc.num_txns = ntxns;
                    wc.num_keys = nkeys;
                    wc.exec.replica_lag = lag;
                    wc.exec.max_retry = retry;
                    cfgs.push_back(wc);
                }
            }
        }
    }
    return cfgs;
}

}  // namespace

int main() {
    const ExecutorFactory sut = deterministic_factory();
    const std::uint64_t seeds = sweep_count();
    const std::vector<WorkloadConfig> cfgs = config_matrix();

    std::uint64_t total_runs = 0;
    std::uint64_t total_commits = 0;
    std::uint64_t total_aborts = 0;
    std::uint64_t total_resequenced = 0;  // commits/aborts with retries > 0
    std::uint64_t runs_with_progress = 0;

    for (std::uint64_t s = 0; s < seeds; ++s) {
        for (const WorkloadConfig& wc : cfgs) {
            const DiffOutcome o = run_diff_seed(s, sut, wc);
            ++total_runs;

            // (A) every checker passes with 0 violations.
            for (const Verdict& v : o.verdicts) {
                if (!v.ok) {
                    std::fprintf(stderr,
                                 "txn_impl_test FAIL: checker[%s] VIOLATION "
                                 "(spec=%s) seed=%llu witness=%s : %s\n",
                                 v.checker.c_str(), v.spec_ref.c_str(),
                                 static_cast<unsigned long long>(v.seed),
                                 v.witness.c_str(), v.explanation.c_str());
                    ++g_failures;
                }
            }
            IMPL_CHECK(o.all_ok, "a checker reported a violation");

            // (B) progress accounting.
            if (o.sut_made_progress) {
                ++runs_with_progress;
            }
            for (const CommitInfo& c : o.sut.commits) {
                if (c.status == Status::Committed) {
                    ++total_commits;
                } else if (c.status == Status::Aborted) {
                    ++total_aborts;
                }
                if (c.retries > 0) {
                    ++total_resequenced;
                }
            }

            // (C) determinism: same (seed, cfg) => byte-identical rendered outcome.
            const DiffOutcome o2 = run_diff_seed(s, sut, wc);
            const std::string r1 = render_outcome(o);
            const std::string r2 = render_outcome(o2);
            IMPL_CHECK(r1 == r2, "non-deterministic: rendered outcome differs on "
                                 "a re-run of the same seed+config");
        }
    }

    // PROGRESS is non-vacuous: the executor must actually commit txns and most
    // runs must make progress (a dead/aborting executor is caught here).
    IMPL_CHECK(total_commits > 0, "no txn ever committed (vacuous)");
    IMPL_CHECK(runs_with_progress > total_runs / 2,
               "too few runs made progress (executor near-vacuous)");

    std::fprintf(stderr,
                 "txn_impl_test: runs=%llu seeds=%llu configs=%zu commits=%llu "
                 "aborts=%llu resequenced=%llu progress_runs=%llu\n",
                 static_cast<unsigned long long>(total_runs),
                 static_cast<unsigned long long>(seeds), cfgs.size(),
                 static_cast<unsigned long long>(total_commits),
                 static_cast<unsigned long long>(total_aborts),
                 static_cast<unsigned long long>(total_resequenced),
                 static_cast<unsigned long long>(runs_with_progress));

    if (g_failures != 0) {
        std::fprintf(stderr, "txn_impl_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "txn_impl_test: ALL PASS (8/8 checkers, 0 violations)\n");
    return EXIT_SUCCESS;
}
