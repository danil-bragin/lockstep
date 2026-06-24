#pragma once

// ProdShardRunner.hpp — Phase 9 S9.1. THE MULTI-SHARD ORCHESTRATOR. The order-of-
// magnitude throughput lever: run M FULLY-INDEPENDENT single-node Raft shards in ONE
// process, each on its OWN std::thread (the reactor IS that thread), so aggregate
// throughput scales ~linearly with shard count up to the core count.
//
// WHY THIS LIVES IN providers/prod (not in cli/lockstepd.cpp): real threads + the
// std::memory_order on the lifecycle atomics are FORBIDDEN outside the providers/
// boundary (the forbidden-call lint scans cli/). The reactor is already the ONE place
// real threads were sanctioned; this is the SAME boundary — providers/prod owns every
// real-thread + epoll + socket + disk syscall, and cli/ just calls this surface. So the
// multi-shard threading is correctly placed at the prod-provider boundary, exactly like
// the single-thread reactor it spawns M of.
//
// THE DESIGN (embarrassingly parallel — no shared mutable state on the data path):
//   * Each shard owns its OWN ProdReactor (own epoll fd) + ProdNetworkBus (own listen +
//     admin sockets/ports) + ProdDisk (own data_dir/shard_<i>/consensus.wal) +
//     ProdConsensusNode (single-node cluster {node_id}, self-commits via the gated N=1
//     path — UNCHANGED consensus surface). The shards share NOTHING mutable: separate
//     reactors, disks, sockets, logs. So there are NO locks on the data path.
//   * Shard i (0-based): node_id = i+1, admin port = base_port + i, consensus listen
//     port = base_port + shards + i (its own range, never colliding with an admin port),
//     data_dir = data_dir/shard_<i>. The CLIENT routes by key-hash to a shard's admin
//     port (hash(key) % M -> port base_port + shard), so a request reaches a shard on
//     its own port — NO in-process cross-thread request handoff.
//
// LIFECYCLE (clean join, no orphan threads): each shard's reactor SELF-DEADLINES
// (run_with_deadline(now + run_seconds)), so every thread terminates on its own within
// the budget even if the parent never signals. run_shards() JOINS all M threads before
// returning — UNCONDITIONALLY, on every path; NO detached/leaked threads, no orphaned
// shards. The ONLY cross-thread state is read-only startup config + a single atomic
// failure counter (no data-path sharing).
//
// CONSTRUCTION SERIALIZATION (TSan/concurrency-clean for the FIRST real threads):
// ProdReactor's ctor reads getenv() once (a diagnostic gate). getenv is not thread-safe
// against a concurrent setenv; nothing here calls setenv, so concurrent getenv-only is
// benign — but to be airtight, shard CONSTRUCTION is serialized under a mutex; only the
// per-shard RUN loops execute concurrently. Construction is one-time + cheap, so
// serializing it costs nothing measurable and removes any shadow of a startup data race.
// On the data path (the M reactor run loops) there is ZERO shared mutable state, so TSan
// has nothing to flag there either.
//
// LINUX-ONLY (epoll/sockets/disk via the prod providers): compiled only under __linux__.

#ifdef __linux__

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>   // mkdir — ALLOWED only under providers/ (rule 1)
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>

#include <lockstep/prod/ProdConsensusNode.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>

namespace lockstep::prod {

namespace core = ::lockstep::core;
namespace consensus = ::lockstep::consensus;

// The orchestrator's config (parsed by the daemon, passed in by value).
struct ShardRunConfig {
    std::uint64_t shards = 1;
    std::uint16_t base_port = 0;     // shard i admin port = base_port + i
    std::string data_dir = ".";      // per-shard dir = data_dir/shard_<i>
    std::uint64_t seed = 0;          // shard i seed = seed + i (distinct election jitter)
    std::uint64_t run_seconds = 10;  // each reactor's self-deadline
    // Real-time Raft timing (ms). For a lone single-node shard the election window can be
    // SHORT (it self-elects); defaults below are the 1-node window.
    std::uint64_t election_min_ms = 8;
    std::uint64_t election_max_ms = 20;
    std::uint64_t heartbeat_ms = 4;
};

namespace shard_detail {

constexpr core::Tick kMsToNs = 1'000'000;

// Per-shard derived addressing/ids.
struct ShardPlan {
    std::uint64_t index = 0;
    std::uint64_t node_id = 0;      // index + 1
    std::uint16_t admin_port = 0;   // base + index
    std::uint16_t listen_port = 0;  // base + shards + index
    std::string data_dir;           // data_dir/shard_<index>
};

inline ShardPlan plan_shard(const ShardRunConfig& c, std::uint64_t i) {
    ShardPlan p;
    p.index = i;
    p.node_id = i + 1;
    p.admin_port = static_cast<std::uint16_t>(c.base_port + i);
    p.listen_port = static_cast<std::uint16_t>(c.base_port + c.shards + i);
    p.data_dir = c.data_dir + "/shard_" + std::to_string(i);
    return p;
}

// Cross-thread startup sync: a build mutex (serialize assembly) + a failure counter.
struct ShardSync {
    std::mutex build_mu;
    std::atomic<std::uint64_t> failures{0};
};

// The thread body: assemble ONE independent single-node shard (under the build mutex),
// start it, run the BOUNDED reactor loop to the self-deadline, print a shutdown line.
// Every shard-owned object lives on THIS thread's stack — no other thread touches it.
// cfg/plan are passed by const-ref: the std::thread constructor decay-COPIES them into
// the thread's own storage, which outlives this call, so the thread owns the data and a
// const-ref param here neither dangles nor copies twice (clang-tidy clean).
inline void run_one_shard(const ShardRunConfig& cfg, const ShardPlan& plan,
                          ShardSync* sync) {
    ProdReactor reactor;
    ProdConsensusNode* node_ptr = nullptr;
    std::unique_ptr<ProdNetworkBus> bus;
    std::unique_ptr<ProdConsensusNode> node;

    {
        std::scoped_lock lk(sync->build_mu);
        if (!reactor.valid()) {
            std::fprintf(stderr, "shard %llu: epoll reactor create failed\n",
                         static_cast<unsigned long long>(plan.index));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Create the shard's data dir (data_dir is assumed to exist; mkdir the subdir).
        // EEXIST is fine (a restart reuses the same dir). mkdir is a provider-zone syscall.
        if (::mkdir(plan.data_dir.c_str(), 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "shard %llu: mkdir(%s) failed\n",
                         static_cast<unsigned long long>(plan.index),
                         plan.data_dir.c_str());
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t admin_id = plan.node_id + 1'000'000;
        bus = std::make_unique<ProdNetworkBus>(reactor);
        if (!bus->add_node_on_port(plan.node_id, plan.listen_port) ||
            !bus->add_node_on_port(admin_id, plan.admin_port)) {
            std::fprintf(stderr, "shard %llu: port bind failed (listen=%u admin=%u)\n",
                         static_cast<unsigned long long>(plan.index),
                         static_cast<unsigned>(plan.listen_port),
                         static_cast<unsigned>(plan.admin_port));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::uint64_t> cluster{plan.node_id};  // single-node: N=1 self-commit
        ProdConsensusNode::Timing timing;
        timing.election_min = static_cast<core::Tick>(cfg.election_min_ms) * kMsToNs;
        timing.election_max = static_cast<core::Tick>(cfg.election_max_ms) * kMsToNs;
        timing.heartbeat = static_cast<core::Tick>(cfg.heartbeat_ms) * kMsToNs;
        timing.request_deadline = 2'000 * kMsToNs;
        node = std::make_unique<ProdConsensusNode>(reactor, *bus, plan.node_id, admin_id,
                                                   plan.data_dir, cfg.seed + plan.index,
                                                   std::move(cluster), timing);
        if (!node->valid()) {
            std::fprintf(stderr, "shard %llu: consensus node assembly failed\n",
                         static_cast<unsigned long long>(plan.index));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        node_ptr = node.get();
    }

    const core::Endpoint ep = node_ptr->endpoint();
    std::printf("lockstepd: shard %llu UP — node %llu admin port=%u listen port=%u "
                "data-dir=%s disk=%s\n",
                static_cast<unsigned long long>(plan.index),
                static_cast<unsigned long long>(ep.id),
                static_cast<unsigned>(plan.admin_port),
                static_cast<unsigned>(plan.listen_port), plan.data_dir.c_str(),
                node_ptr->disk_valid() ? "ok" : "UNAVAILABLE");
    std::fflush(stdout);

    constexpr int kAdminBudget = 1 << 20;
    node_ptr->start(kAdminBudget);
    const core::Tick deadline_ns =
        reactor.now() + static_cast<core::Tick>(cfg.run_seconds) * 1'000'000'000;
    node_ptr->run_with_deadline(deadline_ns);

    std::printf("lockstepd: shard %llu shutting down — node %llu role=%s term=%llu "
                "commit_index=%llu fdatasync_calls=%llu\n",
                static_cast<unsigned long long>(plan.index),
                static_cast<unsigned long long>(ep.id),
                consensus::role_name(node_ptr->role()),
                static_cast<unsigned long long>(node_ptr->term()),
                static_cast<unsigned long long>(node_ptr->commit_index()),
                static_cast<unsigned long long>(node_ptr->disk_sync_calls()));
    std::fflush(stdout);
    // bus/node/reactor RAII-destruct on THIS thread (no cross-thread teardown).
}

}  // namespace shard_detail

// Spawn M shard threads, JOIN all of them, return the process exit code (nonzero if any
// shard failed to assemble). No detached/leaked threads — the join loop is unconditional.
inline int run_shards(const ShardRunConfig& cfg) {
    if (cfg.base_port == 0 || cfg.shards == 0) {
        std::fprintf(stderr, "run_shards: shards>=1 and base_port>0 required\n");
        return 2;
    }
    std::printf("lockstepd: MULTI-SHARD start — shards=%llu base-port=%u run-seconds=%llu "
                "data-dir=%s\n",
                static_cast<unsigned long long>(cfg.shards),
                static_cast<unsigned>(cfg.base_port),
                static_cast<unsigned long long>(cfg.run_seconds), cfg.data_dir.c_str());
    std::fflush(stdout);

    shard_detail::ShardSync sync;
    std::vector<std::thread> threads;
    threads.reserve(cfg.shards);
    for (std::uint64_t i = 0; i < cfg.shards; ++i) {
        threads.emplace_back(shard_detail::run_one_shard, cfg,
                             shard_detail::plan_shard(cfg, i), &sync);
    }
    for (std::thread& t : threads) {
        t.join();  // UNCONDITIONAL join — no detach, no orphan threads.
    }
    const std::uint64_t fails = sync.failures.load(std::memory_order_relaxed);
    std::printf("lockstepd: MULTI-SHARD shutdown complete — shards=%llu failures=%llu "
                "(all threads joined)\n",
                static_cast<unsigned long long>(cfg.shards),
                static_cast<unsigned long long>(fails));
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

// ============================================================================
// Phase 9 S9.4 — REPLICATED SHARDS (3-node Raft groups per shard for HA).
// ============================================================================
// S9.1 above runs M SINGLE-NODE shards in ONE process. S9.4 makes each shard a
// REPLICATED N-node Raft group spread across N PROCESSES, so a shard survives a node
// (process) failure while keeping the multi-shard scaling. It COMBINES the S9.1
// thread-per-shard orchestration with the S5b-2 multi-process Raft cluster — the
// VERIFIED consensus core is UNCHANGED; this is pure prod orchestration.
//
// TOPOLOGY: N lockstepd PROCESSES, each hosting M shard-REPLICAS (M threads). Shard s's
// Raft group = the s-th replica across the N processes -> an N-node group (N=3 => 3-node
// HA group). Replica (process p, shard s) peers with (process q, shard s) for q != p,
// over real TCP. The Raft cluster id-set for EVERY shard is {1..N} (the PROCESS ids); the
// replica on process p uses node_id = p. So NodeConfig.cluster = {1..N} verbatim — the
// same id-set the S5b-2 multi-process cluster uses (zero consensus change).
//
// DETERMINISTIC PORT SCHEME (every peer computes the same ports a priori — ephemeral
// ports cannot be agreed cross-process). Given a global `base_port` and a per-process
// `stride = 2*M`:
//   consensus listen port (process p in 1..N, shard s in 0..M-1) =
//       base_port + (p-1)*stride + s
//   admin port = base_port + (p-1)*stride + M + s
// Process p's shard s replica LISTENS on consensus_port(p,s); to reach shard s's replica
// on process q it RECORDS peer (node_id=q -> consensus_port(q,s)). Every process computes
// consensus_port(q,s) identically, so the dial map is consistent without negotiation.
// Admin ports are local-only (the client routes key->shard s->find shard s's leader by
// trying every process's admin_port(*,s)); they never collide with a consensus port
// because the +M offset separates the two ranges within each process's stride.
//
// LIFECYCLE (same discipline as S9.1): each shard-replica's reactor SELF-DEADLINES, so
// every thread terminates within the budget even if the parent never signals;
// run_repl_shards() JOINS all M threads UNCONDITIONALLY before returning — no orphans.
// Construction is serialized under the build mutex (same getenv-safety argument as S9.1);
// the M run loops execute concurrently with ZERO shared mutable state on the data path.
// Cross-PROCESS Raft RPC is the ONLY inter-replica traffic, exactly as in S5b-2.

struct ReplShardRunConfig {
    std::uint64_t shards = 1;          // M shard-replicas this process hosts (threads)
    std::uint64_t proc_id = 1;         // THIS process's id, 1..cluster_size (Raft node id)
    std::uint64_t cluster_size = 3;    // N processes => each shard is an N-node group
    std::uint16_t base_port = 0;       // global base for the deterministic port scheme
    std::string data_dir = ".";        // per-shard dir = data_dir/shard_<s>
    std::uint64_t seed = 0;            // shard s seed = seed + s (distinct election jitter)
    std::uint64_t run_seconds = 30;    // each reactor's self-deadline
    // Real-time Raft timing (ms). Sized for an N>=2 group over real cross-process TCP:
    // the election window must exceed a few heartbeat intervals + connect/RTT latency so
    // followers don't time out before the leader's heartbeat lands (else perpetual
    // elections). Same defaults as the S5b-2 multi-process cluster.
    std::uint64_t election_min_ms = 150;
    std::uint64_t election_max_ms = 300;
    std::uint64_t heartbeat_ms = 30;
};

namespace shard_detail {

// Deterministic port scheme (see the header comment above). Process ids are 1-based.
inline std::uint16_t repl_stride(std::uint64_t shards) {
    return static_cast<std::uint16_t>(2 * shards);
}
inline std::uint16_t repl_consensus_port(std::uint16_t base, std::uint64_t shards,
                                         std::uint64_t proc_id, std::uint64_t shard) {
    return static_cast<std::uint16_t>(base + (proc_id - 1) * repl_stride(shards) + shard);
}
inline std::uint16_t repl_admin_port(std::uint16_t base, std::uint64_t shards,
                                      std::uint64_t proc_id, std::uint64_t shard) {
    return static_cast<std::uint16_t>(base + (proc_id - 1) * repl_stride(shards) + shards +
                                      shard);
}

// The thread body for ONE replicated shard-replica on THIS process. Assemble the replica
// (under the build mutex): bind its own consensus + admin ports, RECORD every OTHER
// process's (node_id=q -> consensus_port(q,shard)) as a peer to dial, build the N-node
// cluster {1..N}, construct its own ProdConsensusNode over data_dir/shard_<s>. Start,
// run the BOUNDED reactor loop to the self-deadline, print a shutdown line. Every replica-
// owned object lives on THIS thread's stack — no other thread touches it. The ONLY
// cross-replica traffic is cross-PROCESS Raft RPC over TCP (S5b-2), not in-process.
inline void run_one_repl_shard(const ReplShardRunConfig& cfg, std::uint64_t shard,
                               ShardSync* sync) {
    ProdReactor reactor;
    ProdConsensusNode* node_ptr = nullptr;
    std::unique_ptr<ProdNetworkBus> bus;
    std::unique_ptr<ProdConsensusNode> node;
    const std::uint64_t node_id = cfg.proc_id;  // this replica's Raft id == process id
    const std::string sdir = cfg.data_dir + "/shard_" + std::to_string(shard);
    const std::uint16_t my_listen =
        repl_consensus_port(cfg.base_port, cfg.shards, cfg.proc_id, shard);
    const std::uint16_t my_admin =
        repl_admin_port(cfg.base_port, cfg.shards, cfg.proc_id, shard);

    {
        std::scoped_lock lk(sync->build_mu);
        if (!reactor.valid()) {
            std::fprintf(stderr, "repl-shard %llu (proc %llu): epoll create failed\n",
                         static_cast<unsigned long long>(shard),
                         static_cast<unsigned long long>(cfg.proc_id));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (::mkdir(sdir.c_str(), 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "repl-shard %llu (proc %llu): mkdir(%s) failed\n",
                         static_cast<unsigned long long>(shard),
                         static_cast<unsigned long long>(cfg.proc_id), sdir.c_str());
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t admin_id = node_id + 1'000'000;
        bus = std::make_unique<ProdNetworkBus>(reactor);
        // LISTEN on this replica's own consensus + admin ports.
        if (!bus->add_node_on_port(node_id, my_listen) ||
            !bus->add_node_on_port(admin_id, my_admin)) {
            std::fprintf(stderr,
                         "repl-shard %llu (proc %llu): port bind failed (listen=%u "
                         "admin=%u)\n",
                         static_cast<unsigned long long>(shard),
                         static_cast<unsigned long long>(cfg.proc_id),
                         static_cast<unsigned>(my_listen),
                         static_cast<unsigned>(my_admin));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // RECORD every OTHER process's replica of THIS shard as a peer to dial, and build
        // the N-node cluster id-set {1..N} (the process ids).
        std::vector<std::uint64_t> cluster;
        cluster.reserve(cfg.cluster_size);
        for (std::uint64_t q = 1; q <= cfg.cluster_size; ++q) {
            cluster.push_back(q);
            if (q != cfg.proc_id) {
                bus->add_peer(q, repl_consensus_port(cfg.base_port, cfg.shards, q, shard));
            }
        }
        ProdConsensusNode::Timing timing;
        timing.election_min = static_cast<core::Tick>(cfg.election_min_ms) * kMsToNs;
        timing.election_max = static_cast<core::Tick>(cfg.election_max_ms) * kMsToNs;
        timing.heartbeat = static_cast<core::Tick>(cfg.heartbeat_ms) * kMsToNs;
        timing.request_deadline = 2'000 * kMsToNs;
        node = std::make_unique<ProdConsensusNode>(reactor, *bus, node_id, admin_id, sdir,
                                                   cfg.seed + shard, std::move(cluster),
                                                   timing);
        if (!node->valid()) {
            std::fprintf(stderr, "repl-shard %llu (proc %llu): node assembly failed\n",
                         static_cast<unsigned long long>(shard),
                         static_cast<unsigned long long>(cfg.proc_id));
            sync->failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        node_ptr = node.get();
    }

    const core::Endpoint ep = node_ptr->endpoint();
    std::printf("lockstepd: repl-shard %llu proc %llu UP — node %llu admin port=%u listen "
                "port=%u cluster=%llu data-dir=%s disk=%s\n",
                static_cast<unsigned long long>(shard),
                static_cast<unsigned long long>(cfg.proc_id),
                static_cast<unsigned long long>(ep.id), static_cast<unsigned>(my_admin),
                static_cast<unsigned>(my_listen),
                static_cast<unsigned long long>(cfg.cluster_size), sdir.c_str(),
                node_ptr->disk_valid() ? "ok" : "UNAVAILABLE");
    std::fflush(stdout);

    constexpr int kAdminBudget = 1 << 20;
    node_ptr->start(kAdminBudget);
    const core::Tick deadline_ns =
        reactor.now() + static_cast<core::Tick>(cfg.run_seconds) * 1'000'000'000;
    node_ptr->run_with_deadline(deadline_ns);

    std::printf("lockstepd: repl-shard %llu proc %llu shutting down — node %llu role=%s "
                "term=%llu commit_index=%llu fdatasync_calls=%llu\n",
                static_cast<unsigned long long>(shard),
                static_cast<unsigned long long>(cfg.proc_id),
                static_cast<unsigned long long>(ep.id),
                consensus::role_name(node_ptr->role()),
                static_cast<unsigned long long>(node_ptr->term()),
                static_cast<unsigned long long>(node_ptr->commit_index()),
                static_cast<unsigned long long>(node_ptr->disk_sync_calls()));
    std::fflush(stdout);
    // bus/node/reactor RAII-destruct on THIS thread (no cross-thread teardown).
}

}  // namespace shard_detail

// Spawn M replicated-shard-replica threads on THIS process, JOIN all of them, return the
// process exit code (nonzero if any replica failed to assemble). The join loop is
// UNCONDITIONAL — no detached/leaked threads, no orphan replicas.
inline int run_repl_shards(const ReplShardRunConfig& cfg) {
    if (cfg.base_port == 0 || cfg.shards == 0 || cfg.cluster_size == 0 ||
        cfg.proc_id == 0 || cfg.proc_id > cfg.cluster_size) {
        std::fprintf(stderr,
                     "run_repl_shards: shards>=1, base_port>0, cluster_size>=1, "
                     "1<=proc_id<=cluster_size required\n");
        return 2;
    }
    std::printf("lockstepd: REPLICATED-SHARD start — proc=%llu/%llu shards=%llu "
                "base-port=%u run-seconds=%llu data-dir=%s\n",
                static_cast<unsigned long long>(cfg.proc_id),
                static_cast<unsigned long long>(cfg.cluster_size),
                static_cast<unsigned long long>(cfg.shards),
                static_cast<unsigned>(cfg.base_port),
                static_cast<unsigned long long>(cfg.run_seconds), cfg.data_dir.c_str());
    std::fflush(stdout);

    shard_detail::ShardSync sync;
    std::vector<std::thread> threads;
    threads.reserve(cfg.shards);
    for (std::uint64_t s = 0; s < cfg.shards; ++s) {
        threads.emplace_back(shard_detail::run_one_repl_shard, cfg, s, &sync);
    }
    for (std::thread& t : threads) {
        t.join();  // UNCONDITIONAL join — no detach, no orphan threads.
    }
    const std::uint64_t fails = sync.failures.load(std::memory_order_relaxed);
    std::printf("lockstepd: REPLICATED-SHARD shutdown complete — proc=%llu shards=%llu "
                "failures=%llu (all threads joined)\n",
                static_cast<unsigned long long>(cfg.proc_id),
                static_cast<unsigned long long>(cfg.shards),
                static_cast<unsigned long long>(fails));
    std::fflush(stdout);
    return fails == 0 ? 0 : 1;
}

}  // namespace lockstep::prod

#endif  // __linux__
