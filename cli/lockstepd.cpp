// lockstepd.cpp — Phase 7 S5b-2. THE PROD SERVER DAEMON running a REAL Raft
// ConsensusNode as ONE member of a MULTI-PROCESS cluster. A thin main(): parse the
// cluster config (this node's id + listen/admin ports, the FULL peer map id->port for
// every node incl self, data dir, seed, timing), assemble a prod::ProdConsensusNode on
// the PROD providers (ProdReactor epoll loop + the reactor's ONE shared ProdClock +
// ProdRandom(seed) + ProdNetwork bound to FIXED loopback ports + every peer recorded +
// ProdDisk over the data dir), construct the node via make_raft_a_factory() (impl A),
// start() it (arms election/heartbeat timers, spawns the admin serve-loop + Raft
// recv/timer loops), and run the reactor's BOUNDED loop.
//
// CROSS-PROCESS PEER CONNECT (the S5b-2 milestone):
//   * Each process LISTENS on its OWN fixed loopback port (add_node_on_port) so peers
//     can dial it a priori — ephemeral ports cannot be agreed across processes.
//   * Each process RECORDS every PEER's (id -> fixed port) (add_peer) so its outbound
//     RequestVote/AppendEntries send(Endpoint{peer}) dials 127.0.0.1:peer_port.
//   * NodeConfig.cluster lists EVERY member id (incl self); quorum = floor(N/2)+1.
//   * STARTUP ORDER is handled by ProdNetwork's reconnect: a peer not yet up makes
//     connect() fail (its port is bound but maybe not listening yet, or the process
//     isn't up) — the connection drops, and the NEXT heartbeat/election send re-dials.
//     Raft re-sends on its timers, so the cluster converges once all processes are up;
//     no startup deadlock. N=1 self-commit stays gated on quorum()==1; for N>=2 commit
//     is ack-driven over real peer AppendEntries.
//
// PROCESS SELF-DEADLINE: the reactor run is BOUNDED by an ABSOLUTE now()-deadline
// (--run-seconds). A networked node never quiesces (its listen fd is always armed), so
// this hard wall guard guarantees the daemon ALWAYS terminates even if the test crashes
// before killing it — a runaway lockstepd can never outlive its budget.
//
// LINUX-ONLY (epoll/sockets). cli/CMakeLists.txt guards the target with
// if(UNIX AND NOT APPLE); the macOS host never builds it and stays green.
//
// This TU is NOT in the providers/ lint-exempt zone: the forbidden-call lint scans it.
// It touches NO raw socket/epoll/clock/file syscall of its own — only the provider
// surfaces (ProdConsensusNode / ProdNetworkBus) + plain argv parsing.

#include <algorithm>
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

struct Peer {
    std::uint64_t id = 0;
    std::uint16_t port = 0;
};

struct Args {
    std::uint64_t node_id = 1;
    std::uint16_t listen_port = 0;  // this node's consensus listen port (REQUIRED >0)
    std::uint16_t admin_port = 0;   // this node's admin listen port (REQUIRED >0)
    std::uint64_t seed = 0;
    std::string data_dir = ".";
    std::vector<Peer> peers;  // EVERY cluster member incl self (id -> consensus port)

    // Bounded run: serve at most this many real seconds, then exit cleanly. A node
    // with a live listen socket does not quiesce, so a positive bound ensures the
    // daemon always terminates; pass --run-seconds N to extend.
    std::uint64_t run_seconds = 10;

    // Real-time Raft timing (ms). Defaults sized for N>=2 over real cross-process TCP:
    // the election window must exceed a few heartbeat intervals + connect/RTT latency
    // so followers don't time out before the leader's heartbeat lands (which would
    // spin perpetual elections and never settle on ONE leader).
    std::uint64_t election_min_ms = 150;
    std::uint64_t election_max_ms = 300;
    std::uint64_t heartbeat_ms = 30;
};

std::uint64_t parse_u64(const char* s, std::uint64_t fallback) {
    if (s == nullptr || s[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(v) : fallback;
}

// Parse "id:port" into a Peer. Returns false on a malformed token.
bool parse_peer(const char* s, Peer& out) {
    if (s == nullptr) {
        return false;
    }
    const char* colon = std::strchr(s, ':');
    if (colon == nullptr || colon == s || colon[1] == '\0') {
        return false;
    }
    const std::string id_str(s, colon);
    const std::uint64_t id = parse_u64(id_str.c_str(), 0);
    const std::uint64_t port = parse_u64(colon + 1, 0);
    if (id == 0 || port == 0 || port > 65535) {
        return false;
    }
    out = Peer{id, static_cast<std::uint16_t>(port)};
    return true;
}

// Parse a tiny --key value config. --peer id:port may repeat (the full cluster map).
// Unknown flags are ignored (forward-compatible).
Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = argv[i];
        const char* v = argv[i + 1];
        if (std::strcmp(k, "--node-id") == 0) {
            a.node_id = parse_u64(v, a.node_id);
        } else if (std::strcmp(k, "--listen-port") == 0) {
            a.listen_port = static_cast<std::uint16_t>(parse_u64(v, a.listen_port));
        } else if (std::strcmp(k, "--admin-port") == 0) {
            a.admin_port = static_cast<std::uint16_t>(parse_u64(v, a.admin_port));
        } else if (std::strcmp(k, "--seed") == 0) {
            a.seed = parse_u64(v, a.seed);
        } else if (std::strcmp(k, "--data-dir") == 0) {
            a.data_dir = v;
        } else if (std::strcmp(k, "--run-seconds") == 0) {
            a.run_seconds = parse_u64(v, a.run_seconds);
        } else if (std::strcmp(k, "--peer") == 0) {
            Peer p;
            if (parse_peer(v, p)) {
                a.peers.push_back(p);
            }
        } else if (std::strcmp(k, "--election-min-ms") == 0) {
            a.election_min_ms = parse_u64(v, a.election_min_ms);
        } else if (std::strcmp(k, "--election-max-ms") == 0) {
            a.election_max_ms = parse_u64(v, a.election_max_ms);
        } else if (std::strcmp(k, "--heartbeat-ms") == 0) {
            a.heartbeat_ms = parse_u64(v, a.heartbeat_ms);
        }
    }
    return a;
}

constexpr core::Tick kMsToNs = 1'000'000;

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (args.listen_port == 0 || args.admin_port == 0 || args.peers.empty()) {
        std::fprintf(stderr,
                     "lockstepd: usage: --node-id N --listen-port P --admin-port A "
                     "--peer id:port [--peer id:port ...] --data-dir DIR [--seed S] "
                     "[--run-seconds T]\n");
        return 2;
    }

    // --- assemble on the prod providers --------------------------------------
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "lockstepd: failed to create epoll reactor\n");
        return 1;
    }

    // The admin listen endpoint id is the consensus node id + a fixed offset, so it
    // never collides with any peer id; the admin client dials this endpoint's port.
    const std::uint64_t admin_id = args.node_id + 1'000'000;

    prod::ProdNetworkBus bus(reactor);

    // This node LISTENS on its OWN fixed consensus + admin ports so peers / the admin
    // client can dial it a priori (cross-process: ephemeral ports cannot be agreed).
    if (!bus.add_node_on_port(args.node_id, args.listen_port)) {
        std::fprintf(stderr,
                     "lockstepd: node %llu failed to bind consensus listen port %u "
                     "(already taken?)\n",
                     static_cast<unsigned long long>(args.node_id),
                     static_cast<unsigned>(args.listen_port));
        return 1;
    }
    if (!bus.add_node_on_port(admin_id, args.admin_port)) {
        std::fprintf(stderr, "lockstepd: node %llu failed to bind admin port %u\n",
                     static_cast<unsigned long long>(args.node_id),
                     static_cast<unsigned>(args.admin_port));
        return 1;
    }

    // RECORD every PEER's (id -> consensus port) so outbound Raft RPC dials it, and
    // build the cluster id vector (sorted; the seam contract). Self is in the peer map
    // too (its port is already this node's bound listen port — recording it is a no-op
    // overwrite with the same value).
    std::vector<std::uint64_t> cluster;
    cluster.reserve(args.peers.size());
    for (const Peer& p : args.peers) {
        if (p.id != args.node_id) {
            bus.add_peer(p.id, p.port);
        }
        cluster.push_back(p.id);
    }
    // Keep the cluster vector sorted (NodeConfig.cluster contract) + dedup self.
    std::sort(cluster.begin(), cluster.end());
    cluster.erase(std::unique(cluster.begin(), cluster.end()), cluster.end());

    prod::ProdConsensusNode::Timing timing;
    timing.election_min = static_cast<core::Tick>(args.election_min_ms) * kMsToNs;
    timing.election_max = static_cast<core::Tick>(args.election_max_ms) * kMsToNs;
    timing.heartbeat = static_cast<core::Tick>(args.heartbeat_ms) * kMsToNs;
    timing.request_deadline = 2'000 * kMsToNs;  // 2s generous client-side patience

    prod::ProdConsensusNode node(reactor, bus, args.node_id, admin_id, args.data_dir,
                                 args.seed, cluster, timing);
    if (!node.valid()) {
        std::fprintf(stderr, "lockstepd: failed to assemble consensus node\n");
        return 1;
    }

    const core::Endpoint ep = node.endpoint();
    const core::Endpoint aep = node.admin_endpoint();
    std::printf("lockstepd: node %llu UP — consensus ep=%llu port=%u admin ep=%llu "
                "port=%u cluster-size=%zu data-dir=%s seed=%llu disk=%s\n",
                static_cast<unsigned long long>(args.node_id),
                static_cast<unsigned long long>(ep.id),
                static_cast<unsigned>(args.listen_port),
                static_cast<unsigned long long>(aep.id),
                static_cast<unsigned>(args.admin_port), cluster.size(),
                args.data_dir.c_str(), static_cast<unsigned long long>(args.seed),
                node.disk_valid() ? "ok" : "UNAVAILABLE");
    std::fflush(stdout);

    // --- start the node + admin serve-loop, run the BOUNDED reactor loop ------
    // A generous bounded admin recv budget (NEVER an unbounded loop). The reactor run
    // is BOUNDED by an absolute now()-deadline (run_seconds), a hard wall guard so the
    // daemon always terminates even if the test crashes before killing it.
    constexpr int kAdminBudget = 1 << 20;
    node.start(kAdminBudget);

    const core::Tick deadline_ns =
        reactor.now() + static_cast<core::Tick>(args.run_seconds) * 1'000'000'000;
    node.run_with_deadline(deadline_ns);

    std::printf("lockstepd: node %llu shutting down — role=%s term=%llu "
                "commit_index=%llu\n",
                static_cast<unsigned long long>(ep.id),
                consensus::role_name(node.role()),
                static_cast<unsigned long long>(node.term()),
                static_cast<unsigned long long>(node.commit_index()));
    return 0;
}
