#pragma once

// BuggyKvSystems.hpp — Phase 2 batch 2 (stage §6, the harness-has-teeth GATE).
// Author = a THIRD independent agent (≠ system author, ≠ checker author —
// spec §9 DECISION-D). PURPOSE: prove the §4 checker set has TEETH by feeding it
// DELIBERATELY-BROKEN KV systems through the SAME run_kv_sim_with driver and
// asserting the checkers FLAG each (spec §6 V-TEETH1). A checker set that passes
// a known-buggy system IS the bug.
//
// WHAT THIS IS NOT: this does NOT touch the honest ReplicatedKvSystem, the
// checkers, or the sim providers. Each variant here is a SELF-CONTAINED
// IKvSystem with ONE clear injected bug, plugged in via the SystemFactory seam
// (detail::SystemFactory in KvSim.hpp). The honest system stays untouched.
//
// DESIGN — a minimal honest-baseline KV the bugs perturb:
//   The variants share BaseBuggyKvSystem: a small single-leader-ish KV that
//   serializes mutating ops through a per-key sorted-vector store, persists to a
//   durable WAL (SimDisk, like the honest system) so the CRASH/RECOVER lifecycle
//   is real, and acks the client over an in-process response cell on a bounded
//   virtual-time deadline (the same no-livelock pattern the honest system uses).
//   The baseline alone is honest-ish (it would pass C-INT/C-DUR); each subclass
//   overrides exactly ONE seam (apply_mutation / read_value / recover) to inject
//   a single bug. This keeps each fixture's defect ISOLATED and obvious.
//
// DETERMINISM (binding, same contract as the honest system): the ONLY randomness
// is the injected core::IRandom (shared with providers + Buggify). All time is
// virtual (core::Tick). No wall-clock, no std::thread/atomics, no
// std::*_distribution, no unordered iteration on any ordering path. Same seed ⇒
// byte-identical History. Every run terminates (bounded client deadline).
//
// THE VARIANTS + WHICH CHECKER CATCHES EACH (the teeth table):
//   DROP_WRITE_ON_PARTITION : silently drops a committing write but STILL acks it
//                             and never persists/serves it → the ack'd write is
//                             lost. Caught by C-INT/INT-2 (lost ack'd write).
//   STALE_READ              : a read serves the PREVIOUS committed value, ignoring
//                             the latest write → a stale value surfaces in a
//                             session that already wrote newer. Caught by C-MONO
//                             (read-your-writes) and/or C-LIN (non-linearizable).
//   SKIP_CAS_COMPARE        : cas ALWAYS commits, ignoring expected-old → a cas
//                             that should have mismatched commits its value, so a
//                             value the register could not legally hold surfaces.
//                             Caught by C-LIN (no register-legal order).
//   FABRICATE_VALUE         : a read occasionally returns a value NO client ever
//                             wrote to that key → a fabricated/torn value. Caught
//                             by C-INT/INT-1 AND C-DUR/DUR-2 (the durability /
//                             fabrication class that §6.1 says MUST be caught).
//   LOSE_DURABLE_ON_RECOVER : on recover, the node DROPS its truly-durable WAL
//                             (returns ∅ for keys it had committed+synced) → an
//                             ack'd, durable write vanishes across crash+recover.
//                             Caught by C-INT/INT-2 (lost ack'd write).
//
// Each variant exposes a static factory() returning a detail::SystemFactory so
// the teeth test plugs it straight into run_kv_sim_with(seed, factory, ...).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/kv/Buggify.hpp>
#include <lockstep/harness/kv/KvSim.hpp>
#include <lockstep/harness/kv/KvSystem.hpp>

#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

namespace lockstep::harness::kv::buggy {

using core::IClock;
using core::IRandom;
using core::Scheduler;
using core::Task;
using core::Tick;

// ---------------------------------------------------------------------------
// BaseBuggyKvSystem — the honest-ish baseline the variants perturb. It is a
// deliberately SIMPLE single-store KV with a durable WAL + crash/recover, so the
// run driver's full fault envelope (crash/partition/disk faults) genuinely
// exercises it. Mutating ops serialize through apply_mutation(); reads go through
// read_value(); recover goes through replay_durable(). Subclasses override ONE
// of those seams to inject a single bug. Everything else is honest.
// ---------------------------------------------------------------------------
class BaseBuggyKvSystem : public IKvSystem {
public:
    BaseBuggyKvSystem(Scheduler& sched, IClock& clock, IRandom& rng,
                      sim::SimNetworkBus& bus, Buggify& buggify,
                      std::uint64_t n_nodes,
                      const sim::DiskFaultConfig& disk_cfg)
        : sched_(&sched),
          clock_(&clock),
          rng_(&rng),
          bus_(&bus),
          n_nodes_(n_nodes == 0 ? 1 : n_nodes) {
        // Buggify is part of the factory signature (shared with the honest
        // system) but these fixtures inject their bug at the store seam, not via
        // sim buggify — so it is intentionally unused here.
        (void)buggify;
        // One durable WAL backs the (logical leader's) store. The lifecycle ops
        // crash/recover it like the honest system. We register the node ids with
        // the bus so the bus's partition logic is well-formed (we do not route
        // server traffic — the bug surfaces at the store, not on the wire — but
        // the envelope still crashes/recovers/partitions us for real).
        disk_ = std::make_unique<sim::SimDisk>(sched, clock, rng, disk_cfg);
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            bus_->add_node(i);
        }
    }

    BaseBuggyKvSystem(const BaseBuggyKvSystem&) = delete;
    BaseBuggyKvSystem& operator=(const BaseBuggyKvSystem&) = delete;

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
        // The leader is the lowest live id; crashing it loses the volatile store
        // (durable WAL survives) — exactly the honest crash semantics.
        if (node_id == leader_id()) {
            alive_[idx(node_id)] = false;
            disk_->crash();
            store_.clear();
        } else {
            alive_[idx(node_id)] = false;
        }
    }

    void recover_node(std::uint64_t node_id) override {
        if (node_id >= n_nodes_) {
            return;
        }
        if (killed_[idx(node_id)] || alive_[idx(node_id)]) {
            return;
        }
        const bool was_no_leader = (leader_id() >= n_nodes_);
        alive_[idx(node_id)] = true;
        // If this recover restores the (lowest-id) leader and the store is empty
        // because it crashed, rebuild the store from the durable WAL.
        if (node_id == leader_id() && (was_no_leader || store_.empty())) {
            disk_->recover();
            replay_durable();  // SEAM: LOSE_DURABLE_ON_RECOVER overrides this.
        }
    }

    void kill_node(std::uint64_t node_id) override {
        if (node_id >= n_nodes_) {
            return;
        }
        alive_[idx(node_id)] = false;
        killed_[idx(node_id)] = true;
        if (node_id == leader_id_after_kill(node_id)) {
            // (no-op: handled by leader_id() scanning live+!killed)
        }
    }

    [[nodiscard]] std::uint64_t node_count() const override { return n_nodes_; }

    // Call once before submitting work (mirrors the honest system's start()).
    void start() {
        alive_.assign(n_nodes_, true);
        killed_.assign(n_nodes_, false);
    }

protected:
    // A single committed register value.
    struct Entry {
        std::string key;
        std::string value;
        std::uint64_t commit_seq = 0;
        bool present = false;
    };

    // ---- SEAMS the variants override (exactly one each) -------------------

    // Apply a committing mutation to the store + durable WAL. Honest baseline
    // commits, persists, and reports do_ack=true so the client gets the ack.
    // DROP_WRITE_ON_PARTITION overrides this to skip the commit but still ack.
    virtual Task apply_mutation(std::string key, std::string value,
                                std::uint64_t seq, bool& do_ack) {
        commit_and_persist(key, value, seq);
        do_ack = true;
        co_return;
    }

    // Read the current value for key (∅ ⇒ empty). Honest baseline returns the
    // latest committed value. STALE_READ / FABRICATE_VALUE override this.
    [[nodiscard]] virtual std::string read_value(const std::string& key) {
        const Entry* e = find(key);
        return (e != nullptr && e->present) ? e->value : std::string();
    }

    // Decide whether a cas commits. Honest baseline: commit iff current==old.
    // SKIP_CAS_COMPARE overrides this to ALWAYS commit.
    [[nodiscard]] virtual bool cas_should_commit(const std::string& key,
                                                 const std::string& cas_old) {
        const Entry* e = find(key);
        const std::string cur = (e != nullptr && e->present) ? e->value : "";
        return cur == cas_old;
    }

    // Rebuild the store from the durable WAL after recover. Honest baseline
    // replays every durable record. LOSE_DURABLE_ON_RECOVER overrides this to
    // drop the durable state.
    virtual void replay_durable() {
        std::vector<std::byte> durable = disk_->durable_snapshot();
        store_.clear();
        decode_durable_into_store(durable);
    }

    // ---- store helpers (sorted-vector map; deterministic) ----------------

    [[nodiscard]] const Entry* find(const std::string& key) const {
        for (const Entry& e : store_) {
            if (e.key == key) {
                return &e;
            }
        }
        return nullptr;
    }

    void commit_in_memory(const std::string& key, const std::string& value,
                          std::uint64_t seq) {
        for (Entry& e : store_) {
            if (e.key == key) {
                if (seq >= e.commit_seq) {
                    e.value = value;
                    e.commit_seq = seq;
                    e.present = true;
                }
                return;
            }
        }
        Entry ne;
        ne.key = key;
        ne.value = value;
        ne.commit_seq = seq;
        ne.present = true;
        std::size_t pos = 0;
        while (pos < store_.size() && store_[pos].key < key) {
            ++pos;
        }
        store_.insert(store_.begin() + static_cast<std::ptrdiff_t>(pos),
                      std::move(ne));
    }

    // Commit to RAM + append a record to the durable WAL and sync (durability
    // barrier). The WAL frame is a tiny self-describing record: a length-prefixed
    // key, value, and seq — no hashing, no std::*_distribution.
    void commit_and_persist(const std::string& key, const std::string& value,
                            std::uint64_t seq) {
        commit_in_memory(key, value, seq);
        sched_->spawn(persist_record(this, key, value, seq));
    }

    std::vector<Entry> store_;  // the (leader's) committed register set

private:
    // ---- WAL codec (self-contained; deterministic) -----------------------
    static void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFFU));
        }
    }
    static void put_str(std::vector<std::byte>& out, const std::string& s) {
        put_u32(out, static_cast<std::uint32_t>(s.size()));
        for (char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    }
    static bool get_u32(const std::vector<std::byte>& in, std::size_t& pos,
                        std::uint32_t& v) {
        if (pos + 4 > in.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(in[pos + static_cast<std::size_t>(i)])) << (i * 8);
        }
        pos += 4;
        return true;
    }
    static bool get_str(const std::vector<std::byte>& in, std::size_t& pos,
                        std::string& s) {
        std::uint32_t len = 0;
        if (!get_u32(in, pos, len)) {
            return false;
        }
        if (pos + len > in.size()) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(in.data() + pos), len);
        pos += len;
        return true;
    }

    static std::vector<std::byte> encode_record(const std::string& key,
                                                const std::string& value,
                                                std::uint64_t seq) {
        std::vector<std::byte> out;
        put_str(out, key);
        put_str(out, value);
        put_u32(out, static_cast<std::uint32_t>(seq & 0xFFFFFFFFU));
        return out;
    }

    void decode_durable_into_store(const std::vector<std::byte>& durable) {
        std::size_t pos = 0;
        while (pos < durable.size()) {
            std::string key;
            std::string value;
            std::uint32_t seq = 0;
            if (!get_str(durable, pos, key) || !get_str(durable, pos, value) ||
                !get_u32(durable, pos, seq)) {
                // short/torn tail → stop; only the valid prefix was applied.
                break;
            }
            commit_in_memory(key, value, seq);
        }
    }

    static Task persist_record(BaseBuggyKvSystem* self, std::string key,
                               std::string value, std::uint64_t seq) {
        std::vector<std::byte> frame = encode_record(key, value, seq);
        core::Offset off = 0;
        core::Error ae = co_await self->disk_->append(
            std::span<const std::byte>(frame.data(), frame.size()), off);
        (void)ae;
        core::Error se = co_await self->disk_->sync();
        (void)se;
        co_return;
    }

    // ---- leader selection (lowest live, non-killed id) -------------------
    [[nodiscard]] std::size_t idx(std::uint64_t id) const {
        return static_cast<std::size_t>(id);
    }
    [[nodiscard]] std::uint64_t leader_id() const {
        for (std::uint64_t i = 0; i < n_nodes_; ++i) {
            if (alive_[idx(i)] && !killed_[idx(i)]) {
                return i;
            }
        }
        return n_nodes_;
    }
    [[nodiscard]] std::uint64_t leader_id_after_kill(std::uint64_t /*k*/) const {
        return leader_id();
    }

    // ---- the client op: serialize through the (logical) leader -----------
    // Bounded + deadline-driven so the run always quiesces. The op applies at the
    // leader's store; the ONLY randomness in scheduling is the seeded clock grid.
    static Task client_op(BaseBuggyKvSystem* self, std::uint64_t /*client_id*/,
                          KvRequest req, core::Promise<KvResult> p) {
        // A small seeded service delay so client ops interleave with faults and
        // each other (gives the envelope a window). Bounded → terminates.
        const Tick svc = self->rng_->uniform_range(1, 4);
        co_await self->clock_->delay(svc);

        KvResult res;
        const std::uint64_t leader = self->leader_id();
        if (leader >= self->n_nodes_) {
            // No leader up right now: wait a bounded grid for one to return, then
            // error if still none (the chaos driver always recovers eventually).
            Tick waited = 0;
            while (waited < kNoLeaderDeadline && self->leader_id() >= self->n_nodes_) {
                co_await self->clock_->delay(kPollGrain);
                waited += kPollGrain;
            }
            if (self->leader_id() >= self->n_nodes_) {
                res.ok = false;
                res.error = "unavailable";
                p.set_value(std::move(res));
                co_return;
            }
        }

        switch (req.kind) {
            case OpKind::Read: {
                res.ok = true;
                res.result = self->read_value(req.key);  // SEAM
                break;
            }
            case OpKind::Write: {
                const std::uint64_t seq = ++self->commit_seq_;
                bool do_ack = false;
                co_await self->apply_mutation(req.key, req.value, seq, do_ack);  // SEAM
                if (do_ack) {
                    res.ok = true;
                    res.result = "ack";
                } else {
                    res.ok = false;
                    res.error = "timeout";
                }
                break;
            }
            case OpKind::Cas: {
                if (self->cas_should_commit(req.key, req.cas_old)) {  // SEAM
                    const std::uint64_t seq = ++self->commit_seq_;
                    bool do_ack = false;
                    co_await self->apply_mutation(req.key, req.value, seq, do_ack);
                    if (do_ack) {
                        res.ok = true;
                        res.result = "ack";
                    } else {
                        res.ok = false;
                        res.error = "timeout";
                    }
                } else {
                    res.ok = false;
                    res.error = "cas_mismatch";
                }
                break;
            }
        }
        p.set_value(std::move(res));
        co_return;
    }

    static constexpr Tick kPollGrain = 2;
    static constexpr Tick kNoLeaderDeadline = 80;

    Scheduler* sched_;
    IClock* clock_;
    IRandom* rng_;
    sim::SimNetworkBus* bus_;
    std::uint64_t n_nodes_;
    std::uint64_t commit_seq_ = 0;
    std::unique_ptr<sim::SimDisk> disk_;
    std::vector<bool> alive_;
    std::vector<bool> killed_;
};

// ===========================================================================
// THE BUGGY VARIANTS — each overrides exactly ONE seam.
// ===========================================================================

// DROP_WRITE_ON_PARTITION — silently drops a committing write but STILL acks it.
// INJECTED BUG: the leader pretends to commit (acks "ack") but never applies or
// persists the value, so the ack'd write is never observable. This is the
// fabrication-of-success / lost-ack'd-write failure mode.
// CHECKER THAT CATCHES IT: C-INT/INT-2 (every ack'd last write must be
// observable; here every later read of the key returns ∅ → lost ack).
class DropWriteOnPartition final : public BaseBuggyKvSystem {
public:
    using BaseBuggyKvSystem::BaseBuggyKvSystem;

    [[nodiscard]] static detail::SystemFactory factory() {
        return [](Scheduler& sched, SimClock& clock, IRandom& rng,
                  sim::SimNetworkBus& bus, Buggify& buggify,
                  const KvConfig& cfg) -> std::unique_ptr<IKvSystem> {
            auto sys = std::make_unique<DropWriteOnPartition>(
                sched, clock, rng, bus, buggify, cfg.n_nodes, cfg.disk_faults);
            sys->start();
            return sys;
        };
    }

protected:
    Task apply_mutation(std::string /*key*/, std::string /*value*/,
                        std::uint64_t /*seq*/, bool& do_ack) override {
        // THE BUG: drop the write entirely (no commit, no persist) but ACK it.
        do_ack = true;
        co_return;
    }
};

// STALE_READ — a read serves the PREVIOUS committed value, ignoring the latest
// write to the same key.
// INJECTED BUG: read_value returns the value the key held BEFORE its most-recent
// committing write, so a session that just wrote v reads back its prior value.
// CHECKER THAT CATCHES IT: C-MONO (read-your-writes / monotonic-reads) and
// C-LIN (a stale read after a strictly-later write has no register-legal order).
class StaleRead final : public BaseBuggyKvSystem {
public:
    using BaseBuggyKvSystem::BaseBuggyKvSystem;

    [[nodiscard]] static detail::SystemFactory factory() {
        return [](Scheduler& sched, SimClock& clock, IRandom& rng,
                  sim::SimNetworkBus& bus, Buggify& buggify,
                  const KvConfig& cfg) -> std::unique_ptr<IKvSystem> {
            auto sys = std::make_unique<StaleRead>(sched, clock, rng, bus,
                                                   buggify, cfg.n_nodes,
                                                   cfg.disk_faults);
            sys->start();
            return sys;
        };
    }

protected:
    // Track the prior committed value per key so reads can serve the stale one.
    Task apply_mutation(std::string key, std::string value, std::uint64_t seq,
                        bool& do_ack) override {
        // Record the value-before-this-commit for the stale read to serve.
        const Entry* e = find(key);
        const std::string prev = (e != nullptr && e->present) ? e->value : "";
        set_prev(key, prev);
        commit_and_persist(key, value, seq);
        do_ack = true;
        co_return;
    }

    [[nodiscard]] std::string read_value(const std::string& key) override {
        // THE BUG: serve the PREVIOUS value, not the latest committed one. If
        // there is no recorded prior, fall through to ∅ (also stale vs a write).
        for (const Prev& p : prev_) {
            if (p.key == key) {
                return p.value;
            }
        }
        return std::string();
    }

private:
    struct Prev {
        std::string key;
        std::string value;
    };
    void set_prev(const std::string& key, const std::string& value) {
        for (Prev& p : prev_) {
            if (p.key == key) {
                p.value = value;
                return;
            }
        }
        Prev np;
        np.key = key;
        np.value = value;
        std::size_t pos = 0;
        while (pos < prev_.size() && prev_[pos].key < key) {
            ++pos;
        }
        prev_.insert(prev_.begin() + static_cast<std::ptrdiff_t>(pos),
                     std::move(np));
    }
    std::vector<Prev> prev_;
};

// SKIP_CAS_COMPARE — cas ALWAYS commits, ignoring expected-old.
// INJECTED BUG: cas_should_commit returns true unconditionally, so a cas whose
// expected-old does NOT match the current value commits its new-value anyway.
// CHECKER THAT CATCHES IT: C-LIN — a committing cas requires current==cas_old in
// every register-legal order; a cas that commits against a non-matching value
// has no legal placement → the key is not linearizable.
class SkipCasCompare final : public BaseBuggyKvSystem {
public:
    using BaseBuggyKvSystem::BaseBuggyKvSystem;

    [[nodiscard]] static detail::SystemFactory factory() {
        return [](Scheduler& sched, SimClock& clock, IRandom& rng,
                  sim::SimNetworkBus& bus, Buggify& buggify,
                  const KvConfig& cfg) -> std::unique_ptr<IKvSystem> {
            auto sys = std::make_unique<SkipCasCompare>(
                sched, clock, rng, bus, buggify, cfg.n_nodes, cfg.disk_faults);
            sys->start();
            return sys;
        };
    }

protected:
    [[nodiscard]] bool cas_should_commit(const std::string& /*key*/,
                                         const std::string& /*cas_old*/) override {
        return true;  // THE BUG: commit regardless of expected-old.
    }
};

// FABRICATE_VALUE — a read occasionally returns a value NO client ever wrote.
// INJECTED BUG: read_value sometimes (seeded) returns a manufactured token never
// offered to the key — a torn / bit-rot / phantom value.
// CHECKER THAT CATCHES IT: C-INT/INT-1 (fabricated read) AND C-DUR/DUR-2
// (fabricated durable value) — the durability/fabrication class that §6.1 says
// MUST be caught (no false negatives here).
class FabricateValue final : public BaseBuggyKvSystem {
public:
    FabricateValue(Scheduler& sched, IClock& clock, IRandom& rng,
                   sim::SimNetworkBus& bus, Buggify& buggify,
                   std::uint64_t n_nodes, const sim::DiskFaultConfig& disk_cfg)
        : BaseBuggyKvSystem(sched, clock, rng, bus, buggify, n_nodes, disk_cfg),
          rng_(&rng) {}

    [[nodiscard]] static detail::SystemFactory factory() {
        return [](Scheduler& sched, SimClock& clock, IRandom& rng,
                  sim::SimNetworkBus& bus, Buggify& buggify,
                  const KvConfig& cfg) -> std::unique_ptr<IKvSystem> {
            auto sys = std::make_unique<FabricateValue>(
                sched, clock, rng, bus, buggify, cfg.n_nodes, cfg.disk_faults);
            sys->start();
            return sys;
        };
    }

protected:
    [[nodiscard]] std::string read_value(const std::string& key) override {
        const Entry* e = find(key);
        const std::string honest = (e != nullptr && e->present) ? e->value : "";
        // THE BUG: with a seeded chance, fabricate a value never written here.
        // A monotonically-unique token guarantees no mutation ever offered it,
        // so C-INT/INT-1 + C-DUR/DUR-2 must fire. (Deterministic: pure PRNG.)
        if (rng_->chance(0.30)) {
            return "PHANTOM_" + std::to_string(++fab_seq_);
        }
        return honest;
    }

private:
    IRandom* rng_;
    std::uint64_t fab_seq_ = 0;
};

// LOSE_DURABLE_ON_RECOVER — drops truly-durable data on recover.
// INJECTED BUG: replay_durable() clears the store and replays NOTHING, so a
// write that was ack'd, committed, AND synced to the durable WAL vanishes after
// crash+recover.
// CHECKER THAT CATCHES IT: C-INT/INT-2 (the ack'd, durable write is no longer
// observable by any later read → lost ack'd write). This is the C-DUR class of
// failure (durable data lost) surfaced through the client-op history.
class LoseDurableOnRecover final : public BaseBuggyKvSystem {
public:
    using BaseBuggyKvSystem::BaseBuggyKvSystem;

    [[nodiscard]] static detail::SystemFactory factory() {
        return [](Scheduler& sched, SimClock& clock, IRandom& rng,
                  sim::SimNetworkBus& bus, Buggify& buggify,
                  const KvConfig& cfg) -> std::unique_ptr<IKvSystem> {
            auto sys = std::make_unique<LoseDurableOnRecover>(
                sched, clock, rng, bus, buggify, cfg.n_nodes, cfg.disk_faults);
            sys->start();
            return sys;
        };
    }

protected:
    void replay_durable() override {
        // THE BUG: discard ALL durable state on recover — the synced WAL is
        // ignored, so every committed+ack'd write that crashed is lost.
        store_.clear();
    }
};

}  // namespace lockstep::harness::kv::buggy
