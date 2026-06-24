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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// PERF PROFILER (S8.6) — a global operator new counter to measure HEAP ALLOCATIONS PER
// COMMITTED OP on the single reactor thread, a LOAD-INDEPENDENT metric (host CPU
// contention changes wall time, NOT the allocation COUNT). It is the receipt behind the
// S8.6 "where does the per-op churn go" profile. COMPILE-GATED behind LOCKSTEP_PROFILE_ALLOC
// so the SHIPPING daemon is byte-identical to before — a global operator new replacement
// must NOT exist in the production binary (it can defeat the system allocator). Build the
// profiling variant with -DLOCKSTEP_PROFILE_ALLOC and arm it at run time with
// LOCKSTEP_ALLOC_PROFILE=1. Single-threaded (one reactor) so the counters need no atomics.
#ifdef LOCKSTEP_PROFILE_ALLOC
namespace {
bool g_alloc_profile = false;
std::uint64_t g_alloc_count = 0;
std::uint64_t g_alloc_bytes = 0;
// Size histogram buckets: <=32, <=64, <=128, <=512, <=4K, <=64K, >64K.
std::uint64_t g_alloc_hist[7] = {0, 0, 0, 0, 0, 0, 0};
std::uint64_t g_alloc_hbytes[7] = {0, 0, 0, 0, 0, 0, 0};
inline int alloc_bucket(std::size_t n) {
    if (n <= 32) return 0;
    if (n <= 64) return 1;
    if (n <= 128) return 2;
    if (n <= 512) return 3;
    if (n <= 4096) return 4;
    if (n <= 65536) return 5;
    return 6;
}
}  // namespace

void* operator new(std::size_t n) {
    if (g_alloc_profile) {
        ++g_alloc_count;
        g_alloc_bytes += n;
        const int b = alloc_bucket(n);
        ++g_alloc_hist[b];
        g_alloc_hbytes[b] += n;
    }
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif  // LOCKSTEP_PROFILE_ALLOC

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

#ifdef LOCKSTEP_PROFILE_ALLOC
    // PERF PROFILER (S8.6): turn on the alloc counter AFTER startup allocations so the
    // count reflects the STEADY-STATE per-op path, not one-time setup. Armed below right
    // before run_with_deadline; the env read here only decides whether to.
    {
        const char* ap = std::getenv("LOCKSTEP_ALLOC_PROFILE");  // NOLINT(concurrency-mt-unsafe)
        g_alloc_profile = (ap != nullptr && ap[0] == '1');
    }
#endif

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
#ifdef LOCKSTEP_PROFILE_ALLOC
    // PERF PROFILER (S8.6): zero the alloc counters AT the steady-state boundary (node
    // started, admin loop spawned, leader elected shortly after) so the reported count is
    // the LOAD PHASE only, divided by committed entries below for allocs/committed-op.
    const std::uint64_t commit_before = static_cast<std::uint64_t>(node.commit_index());
    g_alloc_count = 0;
    g_alloc_bytes = 0;
    for (int b = 0; b < 7; ++b) {
        g_alloc_hist[b] = 0;
        g_alloc_hbytes[b] = 0;
    }
#endif
    node.run_with_deadline(deadline_ns);
#ifdef LOCKSTEP_PROFILE_ALLOC
    const std::uint64_t prof_allocs = g_alloc_count;
    const std::uint64_t prof_bytes = g_alloc_bytes;
    g_alloc_profile = false;  // stop counting during shutdown reporting
#endif

    const unsigned long long ci =
        static_cast<unsigned long long>(node.commit_index());
    const unsigned long long syncs = node.disk_sync_calls();
    const unsigned long long appends = node.disk_append_calls();
    const unsigned long long sync_ns = node.disk_sync_total_ns();
    // S8.5 PROFILE line: fdatasync count + avg latency + fsyncs-per-committed-op.
    // commit_index is the count of committed entries on this node (1-based dense),
    // so syncs/commit answers the bottleneck question directly.
    const double avg_sync_us = syncs ? (static_cast<double>(sync_ns) / 1000.0 / static_cast<double>(syncs)) : 0.0;
    const double syncs_per_commit = ci ? (static_cast<double>(syncs) / static_cast<double>(ci)) : 0.0;
    const double appends_per_sync = syncs ? (static_cast<double>(appends) / static_cast<double>(syncs)) : 0.0;
    std::printf("lockstepd: node %llu shutting down — role=%s term=%llu "
                "commit_index=%llu\n",
                static_cast<unsigned long long>(ep.id),
                consensus::role_name(node.role()),
                static_cast<unsigned long long>(node.term()),
                ci);
    std::printf("DISKSTATS node=%llu commit_index=%llu fdatasync_calls=%llu "
                "append_calls=%llu fsync_total_ms=%.2f fsync_avg_us=%.2f "
                "fsyncs_per_commit=%.3f appends_per_fsync=%.3f bytes_appended=%llu\n",
                static_cast<unsigned long long>(ep.id), ci, syncs, appends,
                static_cast<double>(sync_ns) / 1e6, avg_sync_us,
                syncs_per_commit, appends_per_sync,
                static_cast<unsigned long long>(node.disk_bytes_appended()));
#ifdef LOCKSTEP_PROFILE_ALLOC
    if (prof_allocs != 0 || prof_bytes != 0) {
        const std::uint64_t committed_in_load =
            (ci > commit_before) ? (ci - commit_before) : 0;
        const double allocs_per_op =
            committed_in_load ? (static_cast<double>(prof_allocs) /
                                 static_cast<double>(committed_in_load))
                              : 0.0;
        const double bytes_per_op =
            committed_in_load ? (static_cast<double>(prof_bytes) /
                                 static_cast<double>(committed_in_load))
                              : 0.0;
        std::printf("ALLOCSTATS node=%llu committed_in_load=%llu heap_allocs=%llu "
                    "heap_bytes=%llu allocs_per_op=%.2f bytes_per_op=%.1f\n",
                    static_cast<unsigned long long>(ep.id),
                    static_cast<unsigned long long>(committed_in_load),
                    static_cast<unsigned long long>(prof_allocs),
                    static_cast<unsigned long long>(prof_bytes), allocs_per_op,
                    bytes_per_op);
        static const char* kBucketName[7] = {"<=32", "<=64", "<=128",
                                             "<=512", "<=4K", "<=64K", ">64K"};
        for (int b = 0; b < 7; ++b) {
            std::printf("ALLOCHIST node=%llu bucket=%-5s count=%llu bytes=%llu\n",
                        static_cast<unsigned long long>(ep.id), kBucketName[b],
                        static_cast<unsigned long long>(g_alloc_hist[b]),
                        static_cast<unsigned long long>(g_alloc_hbytes[b]));
        }
    }
#endif  // LOCKSTEP_PROFILE_ALLOC
    return 0;
}
