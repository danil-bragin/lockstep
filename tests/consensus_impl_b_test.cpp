// consensus_impl_b_test.cpp — Phase 4 Stage I, IMPLEMENTATION B GATE.
//
// Drives the REAL Raft impl B (raft_b::RaftNodeB) through the Stage-M
// ClusterDriver + the 5 conformance checkers (the SAME harness that judges impl
// A + the teeth stubs), over a SEED SWEEP under fault storms (partition/heal,
// crash/restart, net reorder/drop/dup). Asserts:
//   (1) CONFORMANCE — across every seed, ALL FIVE invariants hold:
//         ElectionSafety, LogMatching, StateMachineSafety, LeaderAppendOnly
//         (the four model-checked Consensus.tla safety properties) + Linearizability.
//   (2) PROGRESS — commits accumulate across the sweep (a quorum-up cluster makes
//         progress; a vacuously-safe-but-dead impl would be caught here).
//   (3) NO-FAULT LIVENESS SANITY — a stable, fault-free cluster elects EXACTLY one
//         leader and commits the submitted values.
//   (4) DETERMINISM — same seed ⇒ byte-identical ObservedRun render (in-test +
//         the external double-run diff under a stable marker).
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time is virtual; all RPC/durability via the
// injected boundary. NON-provider code → forbidden-call lint scans it. Seeds are
// printed for replay. Every run is bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConformanceCheckers.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_b/RaftNodeB.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::SubmitObservation;
using lockstep::consensus::run_all_conformance;
using lockstep::consensus::run_cluster;
using lockstep::consensus::raft_b::RaftNodeB;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

const char* ck_name(std::size_t i) {
    switch (i) {
        case 0: return "ElectionSafety";
        case 1: return "LogMatching";
        case 2: return "StateMachineSafety";
        case 3: return "LeaderAppendOnly";
        case 4: return "Linearizability";
    }
    return "?";
}

std::size_t count_committed(const ObservedRun& run) {
    std::size_t n = 0;
    for (const SubmitObservation& s : run.submits) {
        if (s.committed) {
            ++n;
        }
    }
    return n;
}

// Render an ObservedRun to stable line-oriented text (the byte-repro surface).
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
            out += " n" + std::to_string(n.node_id) + "{" + (n.live ? "L" : "D") +
                   "," + lockstep::consensus::role_name(n.role) + ",t" +
                   std::to_string(n.term) + ",ci" + std::to_string(n.commit_index) +
                   ",log" + std::to_string(n.log.size()) + "}";
        }
        out += "\n";
    }
    for (const auto& s : run.submits) {
        out += "OP" + std::to_string(s.op_id) + " c" + std::to_string(s.client_id) +
               " \"" + s.value + "\" [" +
               std::to_string(static_cast<long long>(s.invoke_vt)) + "," +
               std::to_string(static_cast<long long>(s.return_vt)) + "] acc" +
               std::to_string(s.accepted ? 1 : 0) + " com" +
               std::to_string(s.committed ? 1 : 0) + " idx" + std::to_string(s.index) +
               "\n";
    }
    out += "committed_log=" + std::to_string(run.committed_log.size()) + ":";
    for (const auto& e : run.committed_log) {
        out += " [t" + std::to_string(e.term) + ",\"" + e.value + "\"]";
    }
    out += "\n";
    return out;
}

// =====================================================================
// (1)+(2) CONFORMANCE SEED SWEEP under fault storms + progress.
// =====================================================================

void conformance_seed_sweep() {
    std::printf("CONFORMANCE SEED SWEEP (impl B under fault storms):\n");
    ClusterConfig cfg;  // defaults: 3 nodes, full fault envelope, 2 clients x 8 submits
    // In-gate sweep is bounded to 64 seeds (resource discipline). The 0xB10C00..
    // base keeps the former witness seed 0xB10C0031 (offset 0x31 = 49) covered.
    // CONSENSUS_B_SEEDS env override raises it for bounded out-of-gate stress.
    int kSeeds = 64;
    if (const char* env = std::getenv("CONSENSUS_B_SEEDS")) {
        const int n = std::atoi(env);
        if (n > 0 && n <= 5000) {
            kSeeds = n;
        }
    }

    int violations[5] = {0, 0, 0, 0, 0};
    std::size_t total_committed = 0;
    int seeds_with_progress = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad_witness;

    for (int s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0xB10C'0000ULL + static_cast<std::uint64_t>(s);
        const ObservedRun run = run_cluster(seed, RaftNodeB::factory(), cfg);
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (!vs[i].verdict.ok) {
                ++violations[i];
                if (first_bad_witness.empty()) {
                    first_bad_seed = seed;
                    first_bad_witness = vs[i].checker + ": " + vs[i].verdict.witness;
                }
            }
        }
        const std::size_t c = count_committed(run);
        total_committed += c;
        if (c > 0) {
            ++seeds_with_progress;
        }
    }

    std::printf("  seeds=%d  committed_total=%zu  seeds_with_progress=%d/%d\n", kSeeds,
                total_committed, seeds_with_progress, kSeeds);
    for (std::size_t i = 0; i < 5; ++i) {
        std::printf("    %-20s violations=%d\n", ck_name(i), violations[i]);
    }
    if (!first_bad_witness.empty()) {
        std::fprintf(stderr, "  !! FIRST VIOLATION seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed),
                     first_bad_witness.c_str());
    }

    for (std::size_t i = 0; i < 5; ++i) {
        std::string m = std::string(ck_name(i)) + " holds across the seed sweep";
        check(violations[i] == 0, m.c_str());
    }
    // PROGRESS: a quorum-up cluster must commit. Demand commits accumulate AND a
    // large majority of seeds make at least some progress.
    check(total_committed > 0, "PROGRESS: commits accumulate across the sweep");
    check(seeds_with_progress >= (kSeeds * 3) / 4,
          "PROGRESS: most seeds make progress (commit at least one value)");
}

// =====================================================================
// (3) NO-FAULT LIVENESS SANITY — stable cluster: one leader, commits.
// =====================================================================

void no_fault_liveness() {
    std::printf("NO-FAULT LIVENESS (stable cluster: one leader, commits):\n");
    ClusterConfig cfg;
    cfg.full_envelope = false;     // pristine bus + honest disk
    cfg.partition_episodes = 0;
    cfg.crash_episodes = 0;

    const int kSeeds = 24;
    int seeds_committed_all = 0;
    int two_leader_snaps = 0;
    std::size_t total_committed = 0;
    std::size_t total_submits = 0;

    for (int s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0x11FE'0000ULL + static_cast<std::uint64_t>(s);
        const ObservedRun run = run_cluster(seed, RaftNodeB::factory(), cfg);

        // Exactly-one-leader: no snapshot may show two live leaders in a term
        // (ElectionSafety checker proves this rigorously; we also count directly).
        for (const auto& snap : run.snapshots) {
            for (std::size_t a = 0; a < snap.nodes.size(); ++a) {
                if (!snap.nodes[a].live || snap.nodes[a].role !=
                                               lockstep::consensus::Role::Leader) {
                    continue;
                }
                for (std::size_t b = a + 1; b < snap.nodes.size(); ++b) {
                    if (snap.nodes[b].live &&
                        snap.nodes[b].role == lockstep::consensus::Role::Leader &&
                        snap.nodes[b].term == snap.nodes[a].term) {
                        ++two_leader_snaps;
                    }
                }
            }
        }

        const std::size_t c = count_committed(run);
        total_committed += c;
        total_submits += run.submits.size();
        if (c == run.submits.size() && c > 0) {
            ++seeds_committed_all;
        }

        // Conformance must still hold on the calm baseline.
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (!vs[i].verdict.ok) {
                std::fprintf(stderr,
                             "  !! NO-FAULT VIOLATION %s seed=0x%llX wit=%s\n",
                             vs[i].checker.c_str(),
                             static_cast<unsigned long long>(seed),
                             vs[i].verdict.witness.c_str());
                ++g_failures;
            }
        }
    }

    std::printf("  seeds=%d  submits=%zu  committed=%zu  all-committed-seeds=%d/%d  "
                "two-leader-snaps=%d\n",
                kSeeds, total_submits, total_committed, seeds_committed_all, kSeeds,
                two_leader_snaps);

    check(two_leader_snaps == 0,
          "NO-FAULT: never two leaders in a term (exactly one leader)");
    check(total_committed > 0, "NO-FAULT: stable cluster commits submitted values");
    // A stable, fault-free cluster should commit ALL submitted values on the vast
    // majority of seeds.
    check(seeds_committed_all >= (kSeeds * 3) / 4,
          "NO-FAULT: most seeds commit EVERY submitted value");
}

// =====================================================================
// (4) DETERMINISM — same seed ⇒ byte-identical ObservedRun render.
// =====================================================================

std::string determinism_render() {
    const std::uint64_t seed = 0xB0B0'2026ULL;
    ClusterConfig cfg;  // full fault envelope
    const ObservedRun run = run_cluster(seed, RaftNodeB::factory(), cfg);
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
    std::printf("consensus_impl_b_test: Phase 4 Stage I — RaftNodeB conformance\n");

    conformance_seed_sweep();
    no_fault_liveness();
    determinism_run();

    // === EXTERNAL DIFF PROOF ==============================================
    // Emit a rendered run under a stable marker. The gate runs this binary twice
    // and diffs the captured blocks → must be byte-identical.
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(determinism_render().c_str(), stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_impl_b_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("consensus_impl_b_test: OK\n");
    return 0;
}
