// txn_crosscheck_test.cpp — Phase 5 Stage V. THE §6.5 DUAL-IMPL CROSS-CHECK GATE.
//
// Source of truth: master-plan §6.5 (the commit protocol is critical -> DUAL
// independent implementations) + specs/CommitOrdering.tla + briefs/phase5.md.
//
// Runs BOTH independently-built one-shot transaction executors —
//   impl A: DeterministicExecutor (deterministic_factory)  — MVCC engine + sched
//   impl B: IndependentExecutor   (independent_factory)    — pure value model
// — on the SAME seed + SAME workload + SAME ExecConfig over a seed sweep, and
// asserts they AGREE:
//   (1) CROSS-CHECK — A and B produce the SAME committed effects (per txn id:
//       same terminal status; for commits: same writes_committed, same result,
//       same seq_index/commit_version, same StrictSerializable read values). A
//       divergence at a committed effect means at LEAST one impl is wrong; we
//       report the seed + the witnessing txn (replayable).
//   (2) CONFORM — both impls individually match the strict-serializable ORACLE
//       (check_differential == ok) and pass the full 8-checker battery (belt and
//       suspenders: agreeing on a SHARED wrong answer is caught by the oracle).
//       A and B both matching the oracle implies they match each other; we assert
//       the direct A==B agreement ANYWAY so a divergence is localized to the pair.
//   (3) PROGRESS — commits accumulate across the sweep (agreement is non-vacuous;
//       two dead executors trivially "agree").
//   (4) DETERMINISM — same seed => byte-identical rendered outcome for each impl.
//
// Determinism (binding; txn/ is NOT lint-exempt): the only entropy is the seed.
// Both impls are pure fns of (ordered_txns, cfg). Bounded: <=64 seeds in-gate
// (LOCKSTEP_TXN_SEEDS overrides); CTest TIMEOUT inherited (sanitizer-scaled).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/DeterministicExecutor.hpp>
#include <lockstep/txn/DiffHarness.hpp>
#include <lockstep/txn/IndependentExecutor.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

using namespace lockstep::txn;

int g_failures = 0;

#define XCHK_FAIL(seedv, msg)                                              \
    do {                                                                   \
        std::fprintf(stderr, "txn_crosscheck_test FAIL [%s:%d] seed=%llu: %s\n", \
                     __FILE__, __LINE__,                                   \
                     static_cast<unsigned long long>(seedv), (msg));       \
        ++g_failures;                                                      \
    } while (0)

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

// The strict-read values of a CommitInfo, keyed by read key (relaxed reads are
// excluded: they may legitimately differ between impls and are judged by their
// own D5 checker, not the cross-check on committed effects).
std::map<Key, ReadResult> strict_reads(const CommitInfo& c) {
    std::map<Key, ReadResult> m;
    for (const CommitInfo::ServedRead& s : c.served_reads) {
        if (s.level == Level::StrictSerializable) {
            m[s.key] = s.value;
        }
    }
    return m;
}

// Index a RunResult's commits by txn id.
std::map<std::uint64_t, const CommitInfo*> by_id(const RunResult& r) {
    std::map<std::uint64_t, const CommitInfo*> m;
    for (const CommitInfo& c : r.commits) {
        m[c.txn_id] = &c;
    }
    return m;
}

// Assert impl A and impl B AGREE on every txn's committed effect. Returns true on
// full agreement; on the first divergence reports the seed + the witnessing txn.
bool agree(std::uint64_t seed, const std::vector<Txn>& submitted,
           const RunResult& a, const RunResult& b) {
    const auto am = by_id(a);
    const auto bm = by_id(b);

    for (const Txn& t : submitted) {
        const auto ai = am.find(t.id);
        const auto bi = bm.find(t.id);
        if (ai == am.end() || bi == bm.end()) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) +
                             " missing from one impl's output")
                                .c_str());
            return false;
        }
        const CommitInfo& ca = *ai->second;
        const CommitInfo& cb = *bi->second;

        if (ca.status != cb.status) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) + " status A=" +
                             status_name(ca.status) + " B=" + status_name(cb.status))
                                .c_str());
            return false;
        }
        if (ca.status != Status::Committed) {
            continue;  // both aborted/pending agree on status; no effect to compare
        }
        if (ca.seq_index != cb.seq_index || ca.commit_version != cb.commit_version) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) + " seq/version A=(" +
                             std::to_string(ca.seq_index) + "," +
                             std::to_string(ca.commit_version) + ") B=(" +
                             std::to_string(cb.seq_index) + "," +
                             std::to_string(cb.commit_version) + ")")
                                .c_str());
            return false;
        }
        if (ca.writes_committed != cb.writes_committed) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) +
                             " committed DIFFERENT writes (a non-serializable "
                             "divergence between impl A and impl B)")
                                .c_str());
            return false;
        }
        if (ca.result != cb.result) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) + " result A=\"" +
                             ca.result + "\" B=\"" + cb.result + "\"")
                                .c_str());
            return false;
        }
        if (strict_reads(ca) != strict_reads(cb)) {
            XCHK_FAIL(seed, ("txn_id=" + std::to_string(t.id) +
                             " strict-read values diverge between impl A and impl B")
                                .c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    const ExecutorFactory impl_a = deterministic_factory();
    const ExecutorFactory impl_b = independent_factory();
    const std::uint64_t seeds = sweep_count();
    const std::vector<WorkloadConfig> cfgs = config_matrix();

    std::uint64_t total_runs = 0;
    std::uint64_t agree_runs = 0;
    std::uint64_t total_commits = 0;  // counted on impl A (== impl B by agreement)

    for (std::uint64_t s = 0; s < seeds; ++s) {
        for (const WorkloadConfig& wc : cfgs) {
            ++total_runs;

            // Both impls + the oracle, SAME seed + workload + cfg. run_diff_seed
            // builds the workload from the seed and runs the SUT vs the oracle +
            // the full 8-checker battery.
            const DiffOutcome oa = run_diff_seed(s, impl_a, wc);
            const DiffOutcome ob = run_diff_seed(s, impl_b, wc);

            // (2) CONFORM — both impls pass every checker vs the oracle.
            if (!oa.all_ok) {
                for (const Verdict& v : oa.verdicts) {
                    if (!v.ok) {
                        XCHK_FAIL(s, ("impl A checker[" + v.checker +
                                      "] VIOLATION: " + v.witness)
                                         .c_str());
                    }
                }
            }
            if (!ob.all_ok) {
                for (const Verdict& v : ob.verdicts) {
                    if (!v.ok) {
                        XCHK_FAIL(s, ("impl B checker[" + v.checker +
                                      "] VIOLATION: " + v.witness)
                                         .c_str());
                    }
                }
            }

            // (1) CROSS-CHECK — A and B agree on every committed effect. A and B
            // both matching the oracle already implies this; we assert it directly
            // so a divergence is localized to the A/B pair with a witness.
            if (agree(s, oa.submitted, oa.sut, ob.sut)) {
                ++agree_runs;
            }

            for (const CommitInfo& c : oa.sut.commits) {
                if (c.status == Status::Committed) {
                    ++total_commits;
                }
            }

            // (4) DETERMINISM — same seed => byte-identical rendered outcome.
            const DiffOutcome oa2 = run_diff_seed(s, impl_a, wc);
            const DiffOutcome ob2 = run_diff_seed(s, impl_b, wc);
            if (render_outcome(oa) != render_outcome(oa2)) {
                XCHK_FAIL(s, "impl A non-deterministic across re-runs");
            }
            if (render_outcome(ob) != render_outcome(ob2)) {
                XCHK_FAIL(s, "impl B non-deterministic across re-runs");
            }
        }
    }

    // (3) PROGRESS — agreement is non-vacuous (two dead executors trivially agree).
    if (total_commits == 0) {
        std::fprintf(stderr,
                     "txn_crosscheck_test FAIL: no commits across the sweep "
                     "(agreement is vacuous)\n");
        ++g_failures;
    }
    if (agree_runs != total_runs) {
        std::fprintf(stderr,
                     "txn_crosscheck_test FAIL: A and B agreed on only %llu/%llu "
                     "runs (a committed-effect divergence above)\n",
                     static_cast<unsigned long long>(agree_runs),
                     static_cast<unsigned long long>(total_runs));
        // agree() already incremented g_failures at the divergence; guard anyway.
        if (g_failures == 0) {
            ++g_failures;
        }
    }

    std::fprintf(stderr,
                 "txn_crosscheck_test: runs=%llu agree=%llu commits=%llu "
                 "(A=DeterministicExecutor B=IndependentExecutor)\n",
                 static_cast<unsigned long long>(total_runs),
                 static_cast<unsigned long long>(agree_runs),
                 static_cast<unsigned long long>(total_commits));

    if (g_failures != 0) {
        std::fprintf(stderr, "txn_crosscheck_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr,
                 "txn_crosscheck_test: ALL PASS (A and B AGREE on all runs; both "
                 "match the oracle)\n");
    return EXIT_SUCCESS;
}
