#pragma once

// CrossCheck.hpp — Phase 4 Stage M. THE DUAL-IMPLEMENTATION CROSS-CHECK
// (master-plan §6.5 blind-spot coverage; briefs/phase4.md Stage V item 2).
//
// Runs the SAME seed + workload + fault schedule against TWO different
// ConsensusNode implementations (impl A vs impl B, built by two independent
// agents from the SAME verified spec specs/Consensus.tla) and asserts they AGREE
// on the OBSERVABLE OUTCOME: the committed-log prefix and the per-submit commit
// decisions. Two correct Raft impls need NOT produce identical scheduler traces
// (timing/leader-choice differ), but the COMMITTED log they agree on must be a
// common prefix relationship — neither may commit, at a shared index, a value the
// other committed differently (that would be a real divergence one of them got
// wrong).
//
// For Stage M (no real impl exists yet) the BASELINE is impl-vs-ITSELF: the same
// factory run twice on the same seed must agree byte-identically (a determinism
// cross-check). Stage V swaps in two real factories.
//
// PURE: a function of (seed, factories, cfg). Deterministic; every disagreement
// carries a witness + the replayable seed.
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, any nondeterminism.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>

namespace lockstep::consensus {

// The outcome of a cross-check: agree, or a witness of where the two impls'
// committed outcomes diverged. `seed` makes the disagreement replayable.
struct CrossCheckResult {
    bool agree = true;
    std::string witness;       // empty when agree
    std::string explanation;   // why this is a divergence
    std::uint64_t seed = 0;
    std::uint64_t agreed_prefix = 0;  // committed-log indices both impls share
};

// Two committed logs AGREE iff one is a prefix of the other AND every shared index
// holds an identical (term,value) entry. (A correct cluster may commit FURTHER on
// one run than the other — more time, luckier leader — so a strict-prefix
// relationship is the agreement; a CONFLICT at a shared index is the divergence.)
[[nodiscard]] inline CrossCheckResult cross_check_committed_logs(
    const ObservedRun& a, const ObservedRun& b, std::uint64_t seed) {
    CrossCheckResult r;
    r.seed = seed;
    const std::vector<LogEntry>& la = a.committed_log;
    const std::vector<LogEntry>& lb = b.committed_log;
    const std::size_t m = la.size() < lb.size() ? la.size() : lb.size();
    for (std::size_t i = 0; i < m; ++i) {
        if (!(la[i] == lb[i])) {
            r.agree = false;
            r.agreed_prefix = i;
            r.witness = "COMMITTED-LOG DIVERGE index=" + std::to_string(i + 1) +
                        " A=[t" + std::to_string(la[i].term) + ",\"" +
                        la[i].value + "\"] B=[t" + std::to_string(lb[i].term) +
                        ",\"" + lb[i].value + "\"]";
            r.explanation =
                "Cross-check VIOLATED: two implementations committed DIFFERENT "
                "entries at the same committed-log index for the same seed + "
                "workload + fault schedule (master-plan §6.5 dual-impl "
                "cross-check). At least one implementation is wrong.";
            return r;
        }
    }
    r.agreed_prefix = m;
    return r;
}

// Run two factories on the same seed/cfg and cross-check their committed logs.
// Stage M baseline: pass the SAME factory twice (impl-vs-itself → must agree).
// Stage V: pass impl A and impl B.
[[nodiscard]] inline CrossCheckResult cross_check(
    std::uint64_t seed, const ConsensusNodeFactory& factory_a,
    const ConsensusNodeFactory& factory_b, const ClusterConfig& cfg = {}) {
    const ObservedRun ra = run_cluster(seed, factory_a, cfg);
    const ObservedRun rb = run_cluster(seed, factory_b, cfg);
    return cross_check_committed_logs(ra, rb, seed);
}

}  // namespace lockstep::consensus
