// lockstepd.cpp — Phase 7 S5b-1. THE PROD SERVER DAEMON running a REAL Raft
// ConsensusNode. A thin main(): parse a tiny config (node id, data dir, seed,
// optional run-duration), assemble a prod::ProdConsensusNode on the PROD providers
// (ProdReactor epoll loop + the reactor's ONE shared ProdClock + ProdRandom(seed) +
// ProdNetwork with cluster addressing + ProdDisk over the data dir) as a 1-NODE
// cluster, construct the node via make_raft_a_factory() (impl A), start() it (arms
// the election/heartbeat timers on the reactor's real clock, spawns the admin
// serve-loop), and run the reactor's BOUNDED loop. The consensus LOG is durable on
// the ProdDisk; a restart rebuilds it from those bytes.
//
// Multi-PROCESS Raft (3 nodes, election, replication) is S5b-2 — this is the 1-node
// consensus precursor. The ProdNetwork cluster addressing (id -> loopback port via
// add_node / port_of) is built NOW so S5b-2 just add_node's each peer + lists them
// in the cluster vector; this main() is otherwise unchanged.
//
// LINUX-ONLY (epoll/sockets). cli/CMakeLists.txt guards the target with
// if(UNIX AND NOT APPLE); the macOS host never builds it and stays green.
//
// This TU is NOT in the providers/ lint-exempt zone: the forbidden-call lint scans
// it. It touches NO raw socket/epoll/clock/file syscall of its own — only the
// provider surfaces (ProdConsensusNode) + plain argv parsing. The run loop is BOUNDED
// by an absolute reactor deadline (a hard wall guard), so a 1-node election timer
// can never spin forever in a constrained / CI context.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/prod/ProdConsensusNode.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>

namespace {

namespace prod = lockstep::prod;
namespace core = lockstep::core;
namespace consensus = lockstep::consensus;

struct Args {
    std::uint64_t node_id = 1;
    std::uint64_t seed = 0;
    std::string data_dir = ".";
    // Bounded run: serve at most this many real seconds, then exit cleanly. A 1-node
    // node with a live listen socket does not quiesce, so a small positive bound
    // ensures a no-arg run terminates; pass --run-seconds N to extend.
    std::uint64_t run_seconds = 2;
};

std::uint64_t parse_u64(const char* s, std::uint64_t fallback) {
    if (s == nullptr || s[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(v) : fallback;
}

// Parse a tiny --key value config. Unknown flags are ignored (forward-compatible).
Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = argv[i];
        const char* v = argv[i + 1];
        if (std::strcmp(k, "--node-id") == 0) {
            a.node_id = parse_u64(v, a.node_id);
        } else if (std::strcmp(k, "--seed") == 0) {
            a.seed = parse_u64(v, a.seed);
        } else if (std::strcmp(k, "--data-dir") == 0) {
            a.data_dir = v;
        } else if (std::strcmp(k, "--run-seconds") == 0) {
            a.run_seconds = parse_u64(v, a.run_seconds);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // --- assemble on the prod providers (ProdConsensusNode does the wiring) ----
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "lockstepd: failed to create epoll reactor\n");
        return 1;
    }

    // The admin listen endpoint id is the consensus node id + a fixed offset, so it
    // never collides with a (future) peer id; the admin client dials this endpoint.
    const std::uint64_t admin_id = args.node_id + 1'000'000;

    // ProdNetwork cluster addressing: bind a listen socket (-> ephemeral loopback
    // port recorded in the bus's id->port map) for the consensus node AND its admin
    // endpoint. S5b-2 add_node's each PEER here too; the map then routes peer RPC.
    prod::ProdNetworkBus bus(reactor);
    if (!bus.add_node(args.node_id) || !bus.add_node(admin_id)) {
        std::fprintf(stderr, "lockstepd: failed to bind listen sockets for node %llu\n",
                     static_cast<unsigned long long>(args.node_id));
        return 1;
    }

    // 1-node cluster: the cluster view is just this node (quorum=1). S5b-2 passes the
    // full peer list here (and add_node's each above).
    prod::ProdConsensusNode node(reactor, bus, args.node_id, admin_id, args.data_dir,
                                 args.seed,
                                 std::vector<std::uint64_t>{args.node_id});
    if (!node.valid()) {
        std::fprintf(stderr, "lockstepd: failed to assemble consensus node\n");
        return 1;
    }

    const core::Endpoint ep = node.endpoint();
    const core::Endpoint aep = node.admin_endpoint();
    std::printf("lockstepd: consensus node %llu listening (consensus ep=%llu admin "
                "ep=%llu) data-dir=%s seed=%llu disk=%s\n",
                static_cast<unsigned long long>(ep.id),
                static_cast<unsigned long long>(ep.id),
                static_cast<unsigned long long>(aep.id), args.data_dir.c_str(),
                static_cast<unsigned long long>(args.seed),
                node.disk_valid() ? "ok" : "UNAVAILABLE");

    // --- start the node + admin serve-loop, run the BOUNDED reactor loop ------
    // A generous bounded admin recv budget (NEVER an unbounded loop). The reactor run
    // is BOUNDED by an absolute now()-deadline (run_seconds), a hard wall guard so the
    // daemon always terminates in a constrained context (a 1-node election timer fires
    // perpetually, so without the deadline the loop would never return).
    constexpr int kAdminBudget = 1 << 20;
    node.start(kAdminBudget);

    const core::Tick deadline_ns =
        reactor.now() + static_cast<core::Tick>(args.run_seconds) * 1'000'000'000;
    node.run_with_deadline(deadline_ns);

    std::printf("lockstepd: consensus node %llu shutting down — role=%s term=%llu "
                "commit_index=%llu\n",
                static_cast<unsigned long long>(ep.id),
                consensus::role_name(node.role()),
                static_cast<unsigned long long>(node.term()),
                static_cast<unsigned long long>(node.commit_index()));
    return 0;
}
