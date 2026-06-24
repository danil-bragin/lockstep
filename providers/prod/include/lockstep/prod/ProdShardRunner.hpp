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

}  // namespace lockstep::prod

#endif  // __linux__
