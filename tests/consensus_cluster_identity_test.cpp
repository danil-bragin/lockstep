// consensus_cluster_identity_test.cpp — the CLUSTER-IDENTITY split-brain guard
// (plan P2 restore-new-cluster). Every peer message carries a cluster token; a node
// DROPS any message whose token differs from its own. This is what makes a
// snapshot-restored cluster SAFE: a stale node from the old/decommissioned cluster
// (same ids + ports) can neither vote nor replicate into the new one.
//
// Proves: (1) a uniform NON-ZERO token cluster still reaches consensus (the guard
// never blocks same-cluster traffic); (2) with one node given a FOREIGN token, the
// cluster EXCLUDES it — it never becomes leader and never commits an entry, while the
// same-token majority elects a leader and keeps committing. Deterministic sim; every host.
#include <cstdio>
#include <string>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::Role;
using lockstep::consensus::run_cluster;
using lockstep::consensus::raft_a::make_raft_a_factory;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

bool ever_leader(const ObservedRun& run, std::uint64_t id) {
    for (const auto& snap : run.snapshots)
        for (const auto& n : snap.nodes)
            if (n.node_id == id && n.live && n.role == Role::Leader) return true;
    return false;
}
std::uint64_t max_commit(const ObservedRun& run, std::uint64_t id) {
    std::uint64_t mx = 0;
    for (const auto& snap : run.snapshots)
        for (const auto& n : snap.nodes)
            if (n.node_id == id && n.commit_index > mx) mx = n.commit_index;
    return mx;
}
std::uint64_t max_commit_in(const ObservedRun& run, std::uint64_t lo, std::uint64_t hi) {
    std::uint64_t mx = 0;
    for (std::uint64_t id = lo; id <= hi; ++id) mx = std::max(mx, max_commit(run, id));
    return mx;
}
}  // namespace

int main() {
    // (1) A uniform NON-ZERO token: the guard must not block same-cluster consensus.
    {
        int progressed = 0;
        for (std::uint64_t seed = 0x10; seed <= 0x1F; ++seed) {
            ClusterConfig cfg;
            cfg.cluster_token_fn = [](std::uint64_t) { return 0x51DEu; };
            const ObservedRun run = run_cluster(seed, make_raft_a_factory(), cfg);
            if (max_commit_in(run, 0, cfg.n_nodes - 1) > 0) ++progressed;
        }
        check(progressed >= 8, "uniform non-zero token: cluster still reaches consensus (guard never self-blocks)");
    }

    // (2) GUARD: node 4 gets a FOREIGN token; nodes 0..3 share the cluster token. The
    //     foreign node is EXCLUDED — never leader, never commits — while the same-token
    //     majority (quorum 3 of 5) still elects + commits.
    {
        constexpr std::uint64_t kForeign = 4;
        int majority_progressed = 0;
        for (std::uint64_t seed = 0x20; seed <= 0x33; ++seed) {
            ClusterConfig cfg;
            cfg.n_nodes = 5;
            cfg.cluster_token_fn = [](std::uint64_t id) {
                return id == kForeign ? 0xBAD1'D000u : 0x600D'C000u;
            };
            const ObservedRun run = run_cluster(seed, make_raft_a_factory(), cfg);

            check(!ever_leader(run, kForeign), "foreign-token node NEVER becomes leader");
            check(max_commit(run, kForeign) == 0, "foreign-token node NEVER commits an entry (excluded)");
            if (max_commit_in(run, 0, 3) > 0) ++majority_progressed;
        }
        check(majority_progressed >= 8,
              "the same-token majority still elects + commits despite the foreign node (non-vacuous)");
    }

    if (g_fail) { std::printf("consensus_cluster_identity_test: FAILED\n"); return 1; }
    std::printf("consensus_cluster_identity_test: OK (non-zero token works + foreign-token node excluded)\n");
    return 0;
}
