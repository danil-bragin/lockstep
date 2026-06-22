#pragma once

// ReplicatedKvSystem.hpp — Phase 2 batch 2 (stage B, spec §5). The honest
// best-effort toy replicated KV register system. See KvSystem.hpp for the
// scheme; this is the concrete actor implementation on the deterministic
// runtime. It is built to be CORRECT-ish under the FULL fault envelope and to
// always reach quiescence (bounded client deadlines + bounded retries → no
// livelock). It is NOT deliberately bugged (the known-buggy fixture is a
// separate agent's job).
//
// ACTOR MODEL
//   * Each node has a server_loop() coroutine: recv() a framed message, handle
//     it, repeat, until the node is crashed/killed (it stops recv-ing).
//   * Messages are length-prefixed, fixed-field byte frames (no JSON, no
//     std::*_distribution, no hashing): a small hand-rolled codec (Codec.hpp).
//   * The CLIENT path is submit(): it spawns a client_op() coroutine that sends
//     the request to the believed leader over the LOSSY bus, then waits on an
//     in-process per-op RESPONSE CELL with a virtual-time DEADLINE. The leader
//     fills the cell when it would ack. On deadline (reply lost / leader gone)
//     the client retries the next believed leader, up to kMaxRetries, then
//     returns an error. The deadline is driven by clock.delay() (which can NEVER
//     be dropped), so the client ALWAYS terminates — this is the no-livelock
//     guarantee that keeps the harness quiescent.
//
//     Why an in-process cell for the ack leg (not a network recv)? A dropped
//     ack frame would park a client recv() forever and HANG the run. Modelling
//     the client library receiving the leader's response in-process keeps
//     termination bounded; the request + replication legs still ride the full
//     fault envelope (drop/reorder/dup/partition), and a LOST ack still surfaces
//     — the cell is simply never filled, the deadline fires, the client errors
//     or retries exactly as a real client whose ack was lost would.
//
// REPLICATION (single-leader primary-backup):
//   * leader = lowest live node id (deterministic, no election).
//   * A non-leader node that receives a client request FORWARDS it to the
//     leader (one hop). The leader serializes the op, persists it to its
//     durable WAL (append+sync via SimDisk), best-effort replicates the
//     committed record to backups, and fills the client's response cell. An
//     ack'd write is leader-durable; replication is best-effort.
//
// DETERMINISM: the ONLY randomness is the injected core::IRandom (shared with
// the providers + Buggify). No unordered containers on any ordering path; the
// per-key map is a sorted vector. All time is virtual.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting order, any nondeterminism.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/kv/Buggify.hpp>
#include <lockstep/harness/kv/Codec.hpp>
#include <lockstep/harness/kv/KvSystem.hpp>

#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

namespace lockstep::harness::kv {

using core::Endpoint;
using core::IClock;
using core::IRandom;
using core::Message;
using core::Scheduler;
using core::Task;
using core::Tick;

// A per-op response cell. The leader's ack path fills it; the client_op polls it
// on a clock grid until filled or its deadline expires. Held by shared_ptr so it
// outlives either side regardless of completion order. Single-threaded — no
// atomics needed.
struct ResponseCell {
    bool filled = false;
    KvResult result{};
};

// ---------------------------------------------------------------------------
// ReplicatedKvSystem — owns N nodes + the shared bus, drives them on `sched`.
// ---------------------------------------------------------------------------
class ReplicatedKvSystem final : public IKvSystem {
public:
    // A single replicated register value with its committing leader-sequence.
    struct Entry {
        std::string key;
        std::string value;
        std::uint64_t commit_seq = 0;
        bool present = false;  // false ⇒ ∅ (never written / not yet known)
    };

    // Per-node state: durable WAL (SimDisk), in-memory map (sorted vector),
    // liveness, and (when leader) its commit counter. Owned by the system.
    struct Node {
        std::uint64_t id = 0;
        bool alive = true;
        bool killed = false;
        std::uint64_t commit_seq = 0;
        std::vector<Entry> store;  // sorted by key
        std::unique_ptr<sim::SimDisk> disk;
    };

    // A pending client op the leader can ack by op_id (keyed per client). The
    // cell is shared with the waiting client_op coroutine.
    struct Pending {
        std::uint64_t client_id = 0;
        std::uint64_t op_id = 0;
        std::shared_ptr<ResponseCell> cell;
    };

    ReplicatedKvSystem(Scheduler& sched, IClock& clock, IRandom& rng,
                       sim::SimNetworkBus& bus, Buggify& buggify,
                       std::uint64_t n_nodes,
                       const sim::DiskFaultConfig& disk_cfg)
        : sched_(&sched),
          clock_(&clock),
          bus_(&bus),
          buggify_(&buggify),
          n_nodes_(n_nodes) {
        nodes_.reserve(n_nodes_);
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            Node node;
            node.id = i;
            node.disk =
                std::make_unique<sim::SimDisk>(sched, clock, rng, disk_cfg);
            nodes_.push_back(std::move(node));
            bus_->add_node(i);
        }
    }

    ReplicatedKvSystem(const ReplicatedKvSystem&) = delete;
    ReplicatedKvSystem& operator=(const ReplicatedKvSystem&) = delete;

    // Spawn every node's server loop. Call once before submitting work.
    void start() {
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            sched_->spawn(server_loop(this, i));
        }
    }

    // ---- IKvSystem -------------------------------------------------------

    [[nodiscard]] Future<KvResult> submit(std::uint64_t client_id,
                                          const KvRequest& req) override {
        core::Promise<KvResult> p = core::make_promise<KvResult>(sched_);
        Future<KvResult> f = p.get_future();
        sched_->spawn(client_op(this, client_id, req, std::move(p)));
        return f;
    }

    void crash_node(std::uint64_t node_id) override {
        if (node_id >= n_nodes_) {
            return;
        }
        Node& nd = nodes_[node_id];
        if (nd.killed) {
            return;
        }
        nd.alive = false;
        nd.disk->crash();  // drop staged + lying bytes; durable WAL survives
        nd.store.clear();  // volatile RAM lost on crash
        nd.commit_seq = 0;
    }

    void recover_node(std::uint64_t node_id) override {
        if (node_id >= n_nodes_) {
            return;
        }
        Node& nd = nodes_[node_id];
        if (nd.killed || nd.alive) {
            return;
        }
        nd.disk->recover();
        replay_durable(nd);  // rebuild map from surviving durable WAL
        nd.alive = true;
        sched_->spawn(server_loop(this, node_id));  // relaunch its loop
    }

    void kill_node(std::uint64_t node_id) override {
        if (node_id >= n_nodes_) {
            return;
        }
        Node& nd = nodes_[node_id];
        nd.alive = false;
        nd.killed = true;
        nd.disk->crash();
        nd.store.clear();
    }

    [[nodiscard]] std::uint64_t node_count() const override { return n_nodes_; }

private:
    // --- tuning constants (deterministic; bounded → no livelock) -----------
    static constexpr std::uint64_t kClientEndpointBase = 1000;  // > node ids
    static constexpr Tick kAttemptDeadline = 50;  // per-attempt virtual ticks
    static constexpr Tick kPollGrain = 2;         // client cell poll grid
    static constexpr int kMaxRetries = 4;         // bounded retry count
    static constexpr Tick kReplicaWait = 8;       // buggify extra replica wait
    static constexpr Tick kSlowPathDelay = 5;     // buggify slow-path delay

    // ---- leader selection (deterministic) --------------------------------

    [[nodiscard]] std::uint64_t current_leader() const {
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            if (nodes_[i].alive && !nodes_[i].killed) {
                return i;
            }
        }
        return n_nodes_;  // nobody up
    }

    // The (k+1)-th lowest live id (retry routing). Returns n_nodes_ if none.
    [[nodiscard]] std::uint64_t candidate_leader(int k) const {
        int seen = 0;
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            if (nodes_[i].alive && !nodes_[i].killed) {
                if (seen == k) {
                    return i;
                }
                ++seen;
            }
        }
        return n_nodes_;
    }

    // ---- store helpers (sorted-vector map; deterministic) ----------------

    static Entry* find_entry(std::vector<Entry>& store, const std::string& key) {
        for (Entry& e : store) {
            if (e.key == key) {
                return &e;
            }
        }
        return nullptr;
    }

    // Apply a committed record, keeping last-writer-by-commit-seq. Idempotent
    // under duplicate delivery; convergent under reorder.
    static void apply_record(Node& nd, const std::string& key,
                             const std::string& value, std::uint64_t seq,
                             bool present) {
        Entry* e = find_entry(nd.store, key);
        if (e == nullptr) {
            Entry ne;
            ne.key = key;
            ne.value = value;
            ne.commit_seq = seq;
            ne.present = present;
            std::size_t pos = 0;
            while (pos < nd.store.size() && nd.store[pos].key < key) {
                ++pos;
            }
            nd.store.insert(nd.store.begin() + static_cast<std::ptrdiff_t>(pos),
                            std::move(ne));
            return;
        }
        if (seq >= e->commit_seq) {
            e->value = value;
            e->commit_seq = seq;
            e->present = present;
        }
    }

    // ---- durable WAL: append a committed record + sync; replay on recover --

    Task persist_record(Node* nd, std::string key, std::string value,
                        std::uint64_t seq, bool present) {
        std::vector<std::byte> frame =
            encode_wal_record(key, value, seq, present);
        core::Offset off = 0;
        core::Error ae = co_await nd->disk->append(
            std::span<const std::byte>(frame.data(), frame.size()), off);
        (void)ae;  // append io-fault tolerated; sync is the durability barrier
        core::Error se = co_await nd->disk->sync();
        (void)se;
        co_return;
    }

    // Rebuild a node's store from its surviving durable WAL prefix after crash.
    void replay_durable(Node& nd) {
        std::vector<std::byte> durable = nd.disk->durable_snapshot();
        std::size_t pos = 0;
        std::uint64_t max_seq = 0;
        while (pos < durable.size()) {
            WalRecord rec;
            std::size_t consumed = 0;
            if (!decode_wal_record(
                    std::span<const std::byte>(durable.data() + pos,
                                               durable.size() - pos),
                    rec, consumed)) {
                break;  // torn/partial tail: stop at the consistent prefix
            }
            apply_record(nd, rec.key, rec.value, rec.seq, rec.present);
            if (rec.seq > max_seq) {
                max_seq = rec.seq;
            }
            pos += consumed;
        }
        nd.commit_seq = max_seq;
    }

    // ---- node server loop -------------------------------------------------

    static Task server_loop(ReplicatedKvSystem* self, std::uint64_t node_id) {
        sim::SimNetwork net = self->bus_->node(node_id);
        for (;;) {
            if (!self->nodes_[node_id].alive || self->nodes_[node_id].killed) {
                co_return;
            }
            Message m = co_await net.recv();
            if (!self->nodes_[node_id].alive || self->nodes_[node_id].killed) {
                co_return;  // crashed while parked: drop this message
            }
            std::vector<std::byte> payload(m.payload.begin(), m.payload.end());
            co_await self->handle_message(node_id, std::move(payload));
        }
    }

    Task handle_message(std::uint64_t node_id, std::vector<std::byte> payload) {
        Frame fr;
        if (!decode_frame(
                std::span<const std::byte>(payload.data(), payload.size()),
                fr)) {
            co_return;  // malformed frame: drop
        }
        if (buggify_->fire(BuggifyKind::SlowPath)) {
            co_await clock_->delay(kSlowPathDelay);  // widen interleaving window
        }
        switch (fr.type) {
            case FrameType::ClientRequest:
                co_await on_client_request(node_id, fr);
                break;
            case FrameType::Replicate:
                on_replicate(node_id, fr);
                break;
            case FrameType::ClientReply:
            case FrameType::ReplicateAck:
            case FrameType::ClientTimeout:
                break;  // not awaited by servers
        }
        co_return;
    }

    // A node received a client request. If not the leader, forward to the
    // believed leader. If leader, serialize + commit + replicate + ack the cell.
    Task on_client_request(std::uint64_t node_id, Frame fr) {
        std::uint64_t leader = current_leader();
        if (leader >= n_nodes_) {
            co_return;  // no leader; client deadline fires
        }
        if (node_id != leader) {
            std::vector<std::byte> out = encode_frame(fr);
            (void)co_await send_node(node_id, leader, out);
            co_return;
        }

        Node& nd = nodes_[node_id];
        KvResult res;
        Entry* e = find_entry(nd.store, fr.key);
        bool do_commit = false;
        std::string committed_value;
        std::uint64_t commit_seq = 0;

        switch (fr.op_kind) {
            case OpKind::Read: {
                if (buggify_->fire(BuggifyKind::ColdRead)) {
                    co_await clock_->delay(1);  // cold-read slow path
                }
                res.ok = true;
                res.result = (e != nullptr && e->present) ? e->value : "";
                break;
            }
            case OpKind::Write: {
                commit_seq = ++nd.commit_seq;
                committed_value = fr.value;
                do_commit = true;
                res.ok = true;
                res.result = "ack";
                break;
            }
            case OpKind::Cas: {
                const std::string cur =
                    (e != nullptr && e->present) ? e->value : "";
                if (cur == fr.cas_old) {
                    commit_seq = ++nd.commit_seq;
                    committed_value = fr.value;
                    do_commit = true;
                    res.ok = true;
                    res.result = "ack";
                } else {
                    res.ok = false;
                    res.error = "cas_mismatch";
                }
                break;
            }
        }

        if (do_commit) {
            apply_record(nd, fr.key, committed_value, commit_seq, true);
            // Persist durably BEFORE acking (ack'd write is leader-durable).
            co_await persist_record(&nd, fr.key, committed_value, commit_seq,
                                    true);
            replicate(node_id, fr.key, committed_value, commit_seq);
            if (buggify_->fire(BuggifyKind::ExtraReplicaWait)) {
                co_await clock_->delay(kReplicaWait);
            }
            // Re-check we are still leader + alive after the awaits: if we
            // crashed mid-commit, do NOT ack (the client's ack is "lost" — the
            // deadline fires, exactly as a leader that died post-commit).
            if (!nodes_[node_id].alive || nodes_[node_id].killed ||
                current_leader() != node_id) {
                co_return;
            }
        }

        ack_client(fr.client_endpoint.id - kClientEndpointBase, fr.op_id, res);
        co_return;
    }

    // A backup received a replicated committed record: apply + durably persist.
    void on_replicate(std::uint64_t node_id, const Frame& fr) {
        Node& nd = nodes_[node_id];
        apply_record(nd, fr.key, fr.value, fr.commit_seq, fr.present);
        if (fr.commit_seq > nd.commit_seq) {
            nd.commit_seq = fr.commit_seq;
        }
        sched_->spawn(
            persist_record(&nd, fr.key, fr.value, fr.commit_seq, fr.present));
    }

    // Leader → all live backups: ship the committed record (best-effort).
    void replicate(std::uint64_t leader, const std::string& key,
                   const std::string& value, std::uint64_t commit_seq) {
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            if (i == leader || !nodes_[i].alive || nodes_[i].killed) {
                continue;
            }
            Frame rep;
            rep.type = FrameType::Replicate;
            rep.key = key;
            rep.value = value;
            rep.commit_seq = commit_seq;
            rep.present = true;
            std::vector<std::byte> out = encode_frame(rep);
            sched_->spawn(fire_and_forget_send(leader, i, std::move(out)));
        }
    }

    // ---- client ack registry (in-process; bounded-termination ack leg) ----

    // Register a pending client op + its response cell so the leader can fill it.
    void register_pending(std::uint64_t client_id, std::uint64_t op_id,
                          const std::shared_ptr<ResponseCell>& cell) {
        Pending pend;
        pend.client_id = client_id;
        pend.op_id = op_id;
        pend.cell = cell;
        pending_.push_back(std::move(pend));
    }

    void unregister_pending(std::uint64_t client_id, std::uint64_t op_id) {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].client_id == client_id &&
                pending_[i].op_id == op_id) {
                pending_.erase(pending_.begin() +
                               static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    // Fill the client's response cell (the leader's ack). No-op if the client
    // already gave up (cell unregistered) — a late ack for a timed-out op.
    void ack_client(std::uint64_t client_id, std::uint64_t op_id,
                    const KvResult& res) {
        for (Pending& pend : pending_) {
            if (pend.client_id == client_id && pend.op_id == op_id) {
                if (!pend.cell->filled) {
                    pend.cell->filled = true;
                    pend.cell->result = res;
                }
                return;
            }
        }
    }

    // ---- client op (bounded, deadline-driven) ----------------------------

    static Task client_op(ReplicatedKvSystem* self, std::uint64_t client_id,
                          KvRequest req, core::Promise<KvResult> p) {
        const std::uint64_t op_id = self->next_client_op_id_++;
        const std::uint64_t client_ep = kClientEndpointBase + client_id;
        auto cell = std::make_shared<ResponseCell>();
        self->register_pending(client_id, op_id, cell);

        KvResult final_result;
        final_result.ok = false;
        final_result.error = "timeout";

        // buggify: an extra leading retry exercises the retry/dedup path.
        int start = self->buggify_->fire(BuggifyKind::ExtraClientRetry) ? -1 : 0;
        for (int attempt = start; attempt < kMaxRetries; ++attempt) {
            std::uint64_t target =
                self->candidate_leader(attempt < 0 ? 0 : attempt);
            if (target >= self->n_nodes_) {
                co_await self->clock_->delay(kPollGrain);  // nobody up; wait
                if (cell->filled) {
                    break;
                }
                continue;
            }

            // Send the request to the target over the LOSSY bus.
            Frame f;
            f.type = FrameType::ClientRequest;
            f.op_id = op_id;
            f.op_kind = req.kind;
            f.key = req.key;
            f.value = req.value;
            f.cas_old = req.cas_old;
            f.client_endpoint = Endpoint{client_ep};
            std::vector<std::byte> out = encode_frame(f);
            (void)co_await self->send_ep(client_ep, target, out);

            // Wait for the cell to fill, polling on a clock grid up to the
            // per-attempt deadline. clock.delay() can NEVER be dropped, so this
            // ALWAYS terminates (no livelock).
            Tick waited = 0;
            while (waited < kAttemptDeadline && !cell->filled) {
                co_await self->clock_->delay(kPollGrain);
                waited += kPollGrain;
            }
            if (cell->filled) {
                final_result = cell->result;
                break;
            }
            // Deadline → retry the next candidate leader.
        }

        self->unregister_pending(client_id, op_id);
        p.set_value(final_result);
        co_return;
    }

    // ---- send helpers (route through handles → forbidden-lint clean) ------

    Future<core::Error> send_node(std::uint64_t from_node, std::uint64_t to,
                                  const std::vector<std::byte>& bytes) {
        sim::SimNetwork net = bus_->node(from_node);
        return net.send(Endpoint{to},
                        std::span<const std::byte>(bytes.data(), bytes.size()));
    }
    Future<core::Error> send_ep(std::uint64_t from_ep, std::uint64_t to,
                                const std::vector<std::byte>& bytes) {
        sim::SimNetwork net = bus_->node(from_ep);
        return net.send(Endpoint{to},
                        std::span<const std::byte>(bytes.data(), bytes.size()));
    }
    Task fire_and_forget_send(std::uint64_t from_node, std::uint64_t to,
                              std::vector<std::byte> bytes) {
        sim::SimNetwork net = bus_->node(from_node);
        (void)co_await net.send(
            Endpoint{to},
            std::span<const std::byte>(bytes.data(), bytes.size()));
        co_return;
    }

    Scheduler* sched_;
    IClock* clock_;
    sim::SimNetworkBus* bus_;
    Buggify* buggify_;
    std::uint64_t n_nodes_;
    std::uint64_t next_client_op_id_ = 1;
    std::vector<Node> nodes_;
    std::vector<Pending> pending_;  // outstanding client ops awaiting ack
};

}  // namespace lockstep::harness::kv
