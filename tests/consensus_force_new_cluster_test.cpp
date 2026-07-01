// consensus_force_new_cluster_test.cpp — QUORUM-LOSS RECOVERY (plan P5). After a
// permanent majority loss a survivor cannot commit (no quorum). RaftNodeA::
// force_new_cluster unilaterally reconfigures the survivor into a single-node cluster
// {self} under a FRESH cluster identity, keeping its log, so it self-elects (quorum 1)
// and resumes committing — the etcd force-new-cluster recovery, with the P2 identity
// token as the built-in split-brain guard.
//
// Proves: a lone node in a 3-node config CANNOT become leader / commit (no quorum);
// after force_new_cluster it self-elects, its config becomes {self}, and a subsequent
// submit COMMITS — the recovered ability to make progress. Deterministic sim.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

using lockstep::consensus::NodeConfig;
using lockstep::consensus::NodeDeps;
using lockstep::consensus::Role;
using lockstep::consensus::SubmitResult;
using RaftNodeA = lockstep::consensus::raft_a::RaftNodeA;

using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::sim::SimNetworkBus;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
DiskFaultConfig nofault() {
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 0;
    return dc;
}
}  // namespace

int main() {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(0x5150'0000u + seed);
        SimNetworkBus bus(sched, rng);
        bus.add_node(0);
        bus.add_node(1);
        bus.add_node(2);
        auto net = bus.node(0);
        SimDisk disk(sched, clock, rng, nofault());

        NodeDeps deps;
        deps.sched = &sched;
        deps.clock = &clock;
        deps.rng = &rng;
        deps.net = &net;
        deps.disk = &disk;
        NodeConfig nc;
        nc.self_id = 0;
        nc.cluster = {0, 1, 2};  // a 3-node cluster whose peers 1,2 are DEAD (never respond)
        nc.election_timeout_min = 5;
        nc.election_timeout_max = 10;
        nc.heartbeat_interval = 2;

        RaftNodeA node(deps, nc);
        node.start();
        sched.run_until(clock.now() + 300);  // node 0 needs 2 votes; peers dead → cannot win

        check(node.role() != Role::Leader, "quorum lost: the survivor CANNOT become leader alone");
        check(node.commit_index() == 0, "quorum lost: nothing commits");

        // Force-new-cluster: reconfigure to {self} under a fresh identity token.
        node.force_new_cluster(0xF00D'0001u + seed);
        sched.run_until(clock.now() + 300);  // self-elect in the {self} config

        check(node.role() == Role::Leader, "force_new_cluster: the survivor self-elects as leader");
        check(node.current_config() == std::vector<std::uint64_t>{0}, "config is now the single node {self}");

        // A submit must now COMMIT on the recovered 1-node cluster.
        const SubmitResult r = node.submit("recovered-write");
        sched.run_until(clock.now() + 300);
        check(r.accepted, "the recovered leader accepts a submit");
        check(node.commit_index() >= r.index && r.index >= 1,
              "the submit COMMITS on the recovered single-node cluster (progress restored)");
    }

    if (g_fail) { std::printf("consensus_force_new_cluster_test: FAILED\n"); return 1; }
    std::printf("consensus_force_new_cluster_test: OK (quorum-loss survivor recovers via force_new_cluster)\n");
    return 0;
}
