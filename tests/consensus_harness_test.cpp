// consensus_harness_test.cpp — Phase 4 Stage M GATE: the consensus verification
// machinery has TEETH (briefs/phase4.md Stage M; specs/Consensus.tla the source
// of truth). Authored VERIFICATION-FIRST — before either real Raft impl exists —
// so the conformance / linearizability / cross-check harness is proven to JUDGE
// correctly first. A harness that passes a known-broken consensus node IS the bug.
//
// WHAT THIS ASSERTS (binding):
//   (A) TEETH — each DELIBERATELY-WRONG ConsensusNode stub is FLAGGED by the
//       conformance checker that maps to the Consensus.tla invariant it breaks,
//       with a non-empty witness + the replayable seed:
//         TwoLeadersSameTerm    → ElectionSafety       (Consensus.tla ElectionSafety)
//         LogMatchingViolation  → LogMatching          (Consensus.tla LogMatching)
//         DropCommittedEntry    → StateMachineSafety   (Consensus.tla StateMachineSafety)
//                                  AND Linearizability
//         LeaderTruncatesOwnLog → LeaderAppendOnly     (Consensus.tla LeaderAppendOnly)
//   (B) BASELINE — the trivial "always-follower / no-op" node does NOT
//       false-positive ANY safety checker (vacuously safe: no leader, empty logs),
//       BUT makes NO PROGRESS (no submit commits) — a vacuously-safe-but-dead
//       system is explicitly NOT mistaken for correct.
//   (C) CROSS-CHECK — impl-vs-itself (no real impl yet) AGREES on the committed
//       log for the same seed (the §6.5 dual-impl cross-check baseline). A
//       deliberately-DIVERGENT pair (two different teeth factories) is caught.
//   (D) DETERMINISM — same seed ⇒ byte-identical ObservedRun render (in-test) AND
//       the external double-run diff (a rendered block under a stable marker).
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time is virtual. NON-provider code → the
// forbidden-call lint scans it. Seeds are printed for replay (V-CHK2). Every run
// is bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConformanceCheckers.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/CrossCheck.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/TrivialNodes.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::CrossCheckResult;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::NoOpFollowerNode;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::run_all_conformance;
using lockstep::consensus::run_cluster;
using lockstep::consensus::ScriptedNode;
using lockstep::consensus::SubmitObservation;
using lockstep::consensus::TeethFault;
using lockstep::consensus::cross_check;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

// Which conformance checker fired (by fixed index in run_all_conformance order:
// ElectionSafety, LogMatching, StateMachineSafety, LeaderAppendOnly,
// Linearizability).
enum CkIdx {
    CK_ELECT = 0,
    CK_MATCH = 1,
    CK_SMS = 2,
    CK_LAO = 3,
    CK_LIN = 4,
};

struct Fired {
    bool any = false;
    bool by[5] = {false, false, false, false, false};
    std::string sample_witness;
    std::uint64_t sample_seed = 0;
};

// Run one (factory, seed) through the driver + all conformance checkers; return
// which fired. cfg lets the caller use 3 or 5 nodes.
Fired run_and_classify(const ConsensusNodeFactory& factory, std::uint64_t seed,
                       const ClusterConfig& cfg) {
    const ObservedRun run = run_cluster(seed, factory, cfg);
    const std::vector<NamedVerdict> vs = run_all_conformance(run);
    Fired f;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (vs[i].verdict.ok) {
            continue;
        }
        f.by[i] = true;
        if (!f.any) {
            f.any = true;
            f.sample_witness = vs[i].verdict.witness;
            f.sample_seed = vs[i].verdict.seed;
        }
    }
    return f;
}

// Count how many submits committed in a run (progress measure).
std::size_t count_committed(const ObservedRun& run) {
    std::size_t n = 0;
    for (const SubmitObservation& s : run.submits) {
        if (s.committed) {
            ++n;
        }
    }
    return n;
}

const char* ck_name(int i) {
    switch (i) {
        case CK_ELECT: return "ElectionSafety";
        case CK_MATCH: return "LogMatching";
        case CK_SMS: return "StateMachineSafety";
        case CK_LAO: return "LeaderAppendOnly";
        case CK_LIN: return "Linearizability";
    }
    return "?";
}

// =====================================================================
// (A) TEETH — each wrong node MUST be flagged by its mapped checker.
// =====================================================================

void teeth_wrong_nodes() {
    std::printf("TEETH (each deliberately-wrong node MUST be flagged):\n");
    ClusterConfig cfg;  // defaults: 3 nodes, full envelope
    const int kSeeds = 6;

    struct Variant {
        const char* name;
        TeethFault fault;
        int must_fire;        // the checker index that MUST flag it
        int also_fire;        // a second checker that should also flag (-1 = none)
    };
    const Variant variants[] = {
        {"TwoLeadersSameTerm", TeethFault::TwoLeadersSameTerm, CK_ELECT, -1},
        {"LogMatchingViolation", TeethFault::LogMatchingViolation, CK_MATCH, -1},
        {"DropCommittedEntry", TeethFault::DropCommittedEntry, CK_SMS, CK_LIN},
        {"LeaderTruncatesOwnLog", TeethFault::LeaderTruncatesOwnLog, CK_LAO, -1},
    };

    for (const Variant& v : variants) {
        bool ever_must = false;
        bool ever_also = (v.also_fire < 0);
        int flagged = 0;
        std::string wit;
        std::uint64_t wit_seed = 0;
        for (int s = 0; s < kSeeds; ++s) {
            const std::uint64_t seed =
                0x7EE7'0000ULL + static_cast<std::uint64_t>(s);
            const Fired f =
                run_and_classify(ScriptedNode::factory(v.fault), seed, cfg);
            if (f.any) {
                ++flagged;
                if (wit.empty()) {
                    wit = f.sample_witness;
                    wit_seed = f.sample_seed;
                }
            }
            if (f.by[v.must_fire]) {
                ever_must = true;
            }
            if (v.also_fire >= 0 && f.by[v.also_fire]) {
                ever_also = true;
            }
        }
        std::printf("  [%-22s] flagged=%d/%d  must=%s(%d) also=%s\n", v.name,
                    flagged, kSeeds, ck_name(v.must_fire), ever_must ? 1 : 0,
                    v.also_fire < 0 ? "-" : ck_name(v.also_fire));
        if (!wit.empty()) {
            std::printf("       sample seed=0x%llX witness=%s\n",
                        static_cast<unsigned long long>(wit_seed), wit.c_str());
        }
        std::string m1 = std::string(v.name) + " is FLAGGED by some checker";
        std::string m2 = std::string(v.name) + " caught by mapped checker " +
                         ck_name(v.must_fire);
        check(flagged == kSeeds, m1.c_str());
        check(ever_must, m2.c_str());
        if (v.also_fire >= 0) {
            std::string m3 = std::string(v.name) + " also caught by " +
                             ck_name(v.also_fire);
            check(ever_also, m3.c_str());
        }
    }
}

// =====================================================================
// (B) BASELINE — no-op follower: vacuously SAFE, no false positives, NO PROGRESS.
// =====================================================================

void baseline_noop_follower() {
    std::printf("BASELINE (no-op follower: vacuously safe + NO progress):\n");
    ClusterConfig cfg;
    const int kSeeds = 8;
    int safety_fp = 0;
    std::size_t total_committed = 0;
    for (int s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0xBA5E'0000ULL + static_cast<std::uint64_t>(s);
        const ObservedRun run = run_cluster(seed, NoOpFollowerNode::factory(), cfg);
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        // The four SAFETY checkers must NOT fire (vacuously safe). Linearizability
        // also holds vacuously (no committed submit). NONE may false-positive.
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (!vs[i].verdict.ok) {
                ++safety_fp;
                std::fprintf(stderr,
                             "  !! BASELINE FALSE POSITIVE %s seed=0x%llX wit=%s\n",
                             vs[i].checker.c_str(),
                             static_cast<unsigned long long>(seed),
                             vs[i].verdict.witness.c_str());
            }
        }
        total_committed += count_committed(run);
    }
    std::printf("  no-op baseline: %d seeds  safety_false_positives=%d  "
                "total_committed=%zu\n",
                kSeeds, safety_fp, total_committed);
    check(safety_fp == 0,
          "no-op follower: NO safety/linearizability false positive (vacuously "
          "safe)");
    // EXPLICIT no-progress assertion: a vacuously-safe-but-dead system commits
    // NOTHING — we must NOT mistake it for a correct, live consensus.
    check(total_committed == 0,
          "no-op follower: makes NO progress (no submit ever commits) — vacuous "
          "safety is NOT correctness");
}

// =====================================================================
// (C) CROSS-CHECK — impl-vs-itself agrees; a divergent pair is caught.
// =====================================================================

void cross_check_baseline() {
    std::printf("CROSS-CHECK (§6.5 dual-impl; impl-vs-itself baseline):\n");
    ClusterConfig cfg;
    // Impl-vs-itself on the no-op baseline: both commit nothing ⇒ identical
    // (empty) committed log ⇒ AGREE. (When real impls land, swap the factories.)
    for (std::uint64_t s = 0; s < 6; ++s) {
        const std::uint64_t seed = 0xC0DE'0000ULL + s;
        const CrossCheckResult r = cross_check(
            seed, NoOpFollowerNode::factory(), NoOpFollowerNode::factory(), cfg);
        check(r.agree,
              "cross-check: impl-vs-itself AGREES on committed log (baseline)");
    }
    // A DIVERGENT pair: two single-leader impls that each commit a non-empty log,
    // but tag their committed values differently (plain vs "ALT:") ⇒ their
    // committed logs conflict at index 1 for the SAME seed ⇒ the cross-check MUST
    // catch it (proving the cross-check itself has teeth, not just the conformance
    // checkers). This is a true committed-log conflict, not a prefix relationship:
    // for one seed + workload + fault schedule, two impls cannot legitimately
    // commit different entries at a shared committed index — at least one is wrong.
    {
        const std::uint64_t seed = 0xC0DE'BEEFULL;
        const CrossCheckResult r =
            cross_check(seed, ScriptedNode::factory(TeethFault::HonestSingleLeader),
                        ScriptedNode::factory(TeethFault::HonestSingleLeaderAlt), cfg);
        std::printf("  divergent pair: agree=%d witness=%s\n", r.agree ? 1 : 0,
                    r.witness.c_str());
        check(!r.agree,
              "cross-check: a divergent impl pair is FLAGGED (cross-check has "
              "teeth)");
    }
}

// =====================================================================
// (D) DETERMINISM — same seed ⇒ byte-identical ObservedRun render.
// =====================================================================

// Render an ObservedRun to a stable, line-oriented text (the byte-repro surface).
std::string render_run(const ObservedRun& run) {
    std::string out;
    out += "seed=" + std::to_string(run.seed) +
           " n_nodes=" + std::to_string(run.n_nodes) +
           " snapshots=" + std::to_string(run.snapshots.size()) +
           " submits=" + std::to_string(run.submits.size()) + "\n";
    for (const auto& snap : run.snapshots) {
        out += "S" + std::to_string(snap.step) + "@" +
               std::to_string(static_cast<long long>(snap.vt)) + ":";
        for (const auto& n : snap.nodes) {
            out += " n" + std::to_string(n.node_id) + "{" +
                   (n.live ? "L" : "D") + "," +
                   lockstep::consensus::role_name(n.role) + ",t" +
                   std::to_string(n.term) + ",ci" +
                   std::to_string(n.commit_index) + ",log" +
                   std::to_string(n.log.size()) + "}";
        }
        out += "\n";
    }
    for (const auto& s : run.submits) {
        out += "OP" + std::to_string(s.op_id) + " c" +
               std::to_string(s.client_id) + " \"" + s.value + "\" [" +
               std::to_string(static_cast<long long>(s.invoke_vt)) + "," +
               std::to_string(static_cast<long long>(s.return_vt)) + "] acc" +
               std::to_string(s.accepted ? 1 : 0) + " com" +
               std::to_string(s.committed ? 1 : 0) + " idx" +
               std::to_string(s.index) + "\n";
    }
    out += "committed_log=" + std::to_string(run.committed_log.size()) + ":";
    for (const auto& e : run.committed_log) {
        out += " [t" + std::to_string(e.term) + ",\"" + e.value + "\"]";
    }
    out += "\n";
    return out;
}

std::string determinism_render() {
    // Use a teeth variant that drives real stepping (DropCommittedEntry) so the
    // render is non-trivial (snapshots + submits + committed log all populated).
    const std::uint64_t seed = 0xDE7E'2026ULL;
    ClusterConfig cfg;
    const ObservedRun run =
        run_cluster(seed, ScriptedNode::factory(TeethFault::DropCommittedEntry), cfg);
    return render_run(run);
}

void determinism_run() {
    std::printf("DETERMINISM (same seed ⇒ byte-identical ObservedRun):\n");
    const std::string a = determinism_render();
    const std::string b = determinism_render();
    check(a == b, "same seed ⇒ byte-identical ObservedRun render (deterministic)");
}

}  // namespace

int main() {
    std::printf("consensus_harness_test: Phase 4 Stage M teeth gate\n");

    teeth_wrong_nodes();
    baseline_noop_follower();
    cross_check_baseline();
    determinism_run();

    // === EXTERNAL DIFF PROOF ==============================================
    // Emit a rendered run under a stable marker. The gate runs this binary twice
    // and diffs the captured blocks → must be byte-identical.
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(determinism_render().c_str(), stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_harness_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("consensus_harness_test: OK\n");
    return 0;
}
