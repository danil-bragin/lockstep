// consensus_impl_a_test.cpp — Phase 4 Stage I, IMPLEMENTATION A self-test.
//
// Drives the Stage-M ClusterDriver + the five conformance checkers against
// raft_a::make_raft_a_factory() — the real Raft implementation A — under fault
// storms (partition/heal, crash/restart, net reorder/drop/dup) over a SEED SWEEP.
// Asserts ALL of: ElectionSafety, LogMatching, StateMachineSafety,
// LeaderAppendOnly, Linearizability hold on every seed; AND that the cluster makes
// PROGRESS (commits accumulate when a quorum is up — not vacuously dead). Plus a
// no-fault liveness sanity: a stable cluster elects exactly one leader and commits
// every submitted value. Every seed is printed for replay; failures replay
// byte-identically (the gate proves it with an external double-run diff).
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time is virtual. consensus/ is NOT lint-exempt
// → the forbidden-call lint scans this TU. Bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConformanceCheckers.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/CrossCheck.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::CrossCheckResult;
using lockstep::consensus::LogEntry;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::Role;
using lockstep::consensus::run_all_conformance;
using lockstep::consensus::run_cluster;
using lockstep::consensus::SubmitObservation;
using lockstep::consensus::cross_check;
using lockstep::consensus::raft_a::make_raft_a_factory;

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

// ---------------------------------------------------------------------------
// (1) CONFORMANCE SEED SWEEP under the full fault envelope. All five invariants
//     MUST hold on every seed; the cluster MUST make progress in aggregate.
// ---------------------------------------------------------------------------
void conformance_seed_sweep() {
    std::printf("CONFORMANCE SEED SWEEP (full fault envelope; all 5 invariants):\n");
    const ConsensusNodeFactory factory = make_raft_a_factory();
    ClusterConfig cfg;  // defaults: 3 nodes, full envelope (partition/crash/net)
    // Modest in-gate sweep (resource discipline; CTest TIMEOUT 90). A bigger
    // sweep is available via CONSENSUS_A_SEEDS for an out-of-gate stress run,
    // bounded to a sane ceiling so it can never run unbounded.
    std::uint64_t kSeeds = 64;
    if (const char* env = std::getenv("CONSENSUS_A_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0) {
            kSeeds = (v > 4096) ? 4096 : static_cast<std::uint64_t>(v);
        }
    }

    std::size_t fired[5] = {0, 0, 0, 0, 0};
    std::size_t total_committed = 0;
    std::size_t seeds_with_progress = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad_witness;

    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0xA1FA'0000ULL + s;
        const ObservedRun run = run_cluster(seed, factory, cfg);
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (!vs[i].verdict.ok) {
                ++fired[i];
                if (first_bad_witness.empty()) {
                    first_bad_seed = seed;
                    first_bad_witness =
                        std::string(ck_name(i)) + ": " + vs[i].verdict.witness;
                }
            }
        }
        const std::size_t c = count_committed(run);
        total_committed += c;
        if (c > 0) {
            ++seeds_with_progress;
        }
    }

    std::printf("  seeds=%llu  committed_total=%zu  seeds_with_progress=%zu\n",
                static_cast<unsigned long long>(kSeeds), total_committed,
                seeds_with_progress);
    std::printf("  violations: Elect=%zu Match=%zu SMS=%zu LAO=%zu Lin=%zu\n",
                fired[0], fired[1], fired[2], fired[3], fired[4]);
    if (!first_bad_witness.empty()) {
        std::fprintf(stderr, "  FIRST VIOLATION seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed),
                     first_bad_witness.c_str());
    }

    check(fired[0] == 0, "ElectionSafety holds on every seed");
    check(fired[1] == 0, "LogMatching holds on every seed");
    check(fired[2] == 0, "StateMachineSafety holds on every seed");
    check(fired[3] == 0, "LeaderAppendOnly holds on every seed");
    check(fired[4] == 0, "Linearizability holds on every seed");
    // PROGRESS: the cluster is not vacuously dead — commits accumulate across the
    // sweep (a quorum is kept up by the fault schedule, so real work commits).
    check(total_committed > 0,
          "cluster makes PROGRESS under faults (commits accumulate — not "
          "vacuously dead)");
    check(seeds_with_progress * 2 >= kSeeds,
          "a MAJORITY of seeds commit at least one value under the fault storm");
}

// ---------------------------------------------------------------------------
// (2) NO-FAULT LIVENESS SANITY. A stable cluster (pristine bus, honest disk, no
//     partition/crash) elects exactly one leader and commits every submit.
// ---------------------------------------------------------------------------
void no_fault_liveness() {
    std::printf("NO-FAULT LIVENESS (stable cluster: one leader, all commits):\n");
    const ConsensusNodeFactory factory = make_raft_a_factory();
    ClusterConfig cfg;
    cfg.full_envelope = false;     // pristine bus + honest disk
    cfg.partition_episodes = 0;
    cfg.crash_episodes = 0;

    const std::uint64_t kSeeds = 40;
    std::size_t all_committed_seeds = 0;
    std::size_t total_submits = 0;
    std::size_t total_committed = 0;
    bool ever_two_leaders = false;

    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0x11FE'0000ULL + s;
        const ObservedRun run = run_cluster(seed, factory, cfg);
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < vs.size(); ++i) {
            check(vs[i].verdict.ok,
                  "no-fault: all conformance invariants hold");
            if (i == 0 && !vs[i].verdict.ok) {
                ever_two_leaders = true;
            }
        }
        // Exactly one leader at the final (settled) snapshot.
        if (!run.snapshots.empty()) {
            const auto& last = run.snapshots.back();
            std::size_t leaders = 0;
            for (const auto& n : last.nodes) {
                if (n.live && n.role == Role::Leader) {
                    ++leaders;
                }
            }
            check(leaders == 1,
                  "no-fault: exactly one leader once the cluster has settled");
        }
        const std::size_t c = count_committed(run);
        total_submits += run.submits.size();
        total_committed += c;
        bool all = !run.submits.empty();
        for (const SubmitObservation& sub : run.submits) {
            if (!sub.committed) {
                all = false;
            }
        }
        if (all) {
            ++all_committed_seeds;
        }
    }
    std::printf("  seeds=%llu  submits=%zu committed=%zu  all-committed seeds=%zu\n",
                static_cast<unsigned long long>(kSeeds), total_submits,
                total_committed, all_committed_seeds);
    check(!ever_two_leaders, "no-fault: never two leaders in a term");
    // Every submitted value must commit in a stable, fault-free cluster.
    check(total_committed == total_submits && total_submits > 0,
          "no-fault: EVERY submitted value commits (full liveness)");
}

// ---------------------------------------------------------------------------
// (3) CROSS-CHECK impl-A-vs-itself: same seed ⇒ identical committed log (the
//     §6.5 determinism baseline; a real impl-A-vs-impl-B cross-check is Stage V).
// ---------------------------------------------------------------------------
void cross_check_self() {
    std::printf("CROSS-CHECK (impl A vs itself; deterministic committed log):\n");
    const ConsensusNodeFactory factory = make_raft_a_factory();
    ClusterConfig cfg;
    for (std::uint64_t s = 0; s < 20; ++s) {
        const std::uint64_t seed = 0xC0DE'A000ULL + s;
        const CrossCheckResult r = cross_check(seed, factory, factory, cfg);
        if (!r.agree) {
            std::fprintf(stderr, "  CROSS-CHECK DIVERGE seed=0x%llX %s\n",
                         static_cast<unsigned long long>(seed), r.witness.c_str());
        }
        check(r.agree, "impl A vs itself AGREES on committed log (deterministic)");
    }
}

// ---------------------------------------------------------------------------
// (4) DETERMINISM: same seed ⇒ byte-identical ObservedRun render.
// ---------------------------------------------------------------------------
std::string determinism_render() {
    const std::uint64_t seed = 0xDE7E'A007ULL;
    ClusterConfig cfg;
    const ObservedRun run = run_cluster(seed, make_raft_a_factory(), cfg);
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
    std::printf("consensus_impl_a_test: Phase 4 Stage I — Raft implementation A\n");

    conformance_seed_sweep();
    no_fault_liveness();
    cross_check_self();
    determinism_run();

    // === EXTERNAL DIFF PROOF =============================================
    // Emit a rendered run under a stable marker. The gate runs this binary twice
    // and diffs the captured blocks → must be byte-identical.
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(determinism_render().c_str(), stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_impl_a_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("consensus_impl_a_test: OK\n");
    return 0;
}
