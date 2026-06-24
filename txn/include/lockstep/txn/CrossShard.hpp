#pragma once

// CrossShard.hpp — Phase 9 S9.3. THE CROSS-SHARD TXN WIRING (Calvin-style, NO 2PC).
//
// Source of truth: specs/XShardCommit.tla (model-checked clean + committed) +
// specs/XShardCommit.check.md. This is the INTEGRATION layer that wires a
// transaction touching keys on MULTIPLE shards through the ALREADY-VERIFIED
// building blocks — it adds NO new verified-core semantics, it only COMPOSES them:
//
//   consensus/sequencer/Sequencer.hpp  (C4.4, TLC-verified)  — the deterministic
//       cross-shard merge of the per-shard committed logs into one global order.
//   txn/DeterministicExecutor.hpp      (Phase 5, TLC-verified) — executes a global
//       seqLog deterministically + atomically (NO 2PC, NO concurrency at apply).
//   txn/{Oracle,Checkers}.hpp          (Phase 5, TLC-verified) — the strict-
//       serializable oracle + the 8-checker battery that JUDGE the result.
//
// THE CALVIN INSIGHT (specs/XShardCommit.tla): a cross-shard txn is replicated into
// the committed Raft log of EVERY shard it touches (so each shard agrees it
// happened). The deterministic Sequencer merge then sees it on every shard; this
// layer COLLAPSES those per-shard appearances to ONE global position and feeds the
// deduped order to the DeterministicExecutor, which applies ALL of the txn's writes
// together at that one position. Because the merge + execution are pure
// deterministic functions of the agreed per-shard logs, every replica computes the
// SAME single position + the SAME atomic apply — so a cross-shard txn commits on
// ALL its shards or NONE. No 2PC, no prepare, no distributed lock; atomicity is a
// consequence of the deterministic global order (XShardCommit.tla AtomicAllOrNone).
//
// ============================================================================
// MAPS DIRECTLY onto specs/XShardCommit.tla.
// ============================================================================
//   TLA concept                              →  this header
//   --------------------------------------     ------------------------------
//   ShardsOf[t] (shards a txn touches)       →  shards_of(txn, router)
//   inputLog[s] (per-shard committed log)    →  std::vector<ShardLog> (sequencer)
//   the dedup to ONE global position         →  dedup_global_order() — keep the
//       (OrderShard = lowest-rank shard)        FIRST appearance (Sequencer already
//                                               emits shards in ShardRank order, so
//                                               the first is the lowest-rank shard).
//   Sealable(t) = fully committed everywhere →  fully_committed() gate
//   Apply(t) atomic over ShardsOf[t]         →  one Txn → DeterministicExecutor
//                                               applies its whole write set at once.
//
// ============================================================================
// FORBIDDEN (txn/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output. Pure deterministic
// functions of their inputs throughout; std::map / ordered iteration only.
// ============================================================================

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <lockstep/consensus/sequencer/Sequencer.hpp>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

namespace seq = ::lockstep::consensus::sequencer;

// ----------------------------------------------------------------------------
// key_shard — the deterministic key→shard router. A pure function of (key,
// num_shards): hash the key bytes (FNV-1a, deterministic, no clock/rng) and take
// it mod the shard count. Same key always routes to the same shard. (The router IS
// the data-partitioning rule; any total deterministic key→shard map conforms.)
// ----------------------------------------------------------------------------
[[nodiscard]] inline seq::ShardId key_shard(const Key& key, std::uint32_t num_shards) {
    std::uint64_t h = 0xCBF29CE484222325ULL;  // FNV-1a offset basis
    for (const unsigned char ch : key) {
        h ^= static_cast<std::uint64_t>(ch);
        h *= 0x0000'0100'0000'01B3ULL;  // FNV-1a prime
    }
    return num_shards == 0 ? 0 : static_cast<seq::ShardId>(h % num_shards);
}

// ----------------------------------------------------------------------------
// shards_of — the set of shards a txn TOUCHES (XShardCommit.tla ShardsOf[t]): the
// union of the shards owning each key in its declared reads + writes. A cross-shard
// txn is one whose result spans >= 2 shards. Ordered std::set => deterministic.
// ----------------------------------------------------------------------------
[[nodiscard]] inline std::set<seq::ShardId> shards_of(const Txn& t,
                                                      std::uint32_t num_shards) {
    std::set<seq::ShardId> shards;
    // A Calvin one-shot txn declares its full footprint up front (its declared
    // reads name every key it touches, including the keys it writes — the workload
    // declares each write key as a strict read). So the declared read set already
    // covers every touched key, and the shards it spans are exactly those owners.
    for (const Read& r : t.declared_reads) {
        shards.insert(key_shard(r.key, num_shards));
    }
    return shards;
}

// ----------------------------------------------------------------------------
// dedup_global_order — collapse the Sequencer's global log (which emits a cross-
// shard txn ONCE PER shard it landed on) to ONE position per txn, gated on the txn
// being FULLY COMMITTED on every shard it touches (XShardCommit.tla Sealable +
// OneGlobalPosition + the all-or-nothing seal gate).
//
// The Sequencer emits each epoch's batch in (ShardRank, idx) order, so the FIRST
// appearance of a txn is the one ordered by its LOWEST-ranked involved shard —
// exactly XShardCommit.tla's OrderShard. We keep that first appearance and drop the
// rest. A txn missing on ANY of its shards is DROPPED entirely (none of its writes
// apply): all-or-nothing. PURE fn of (global, shards_by_txn).
// ----------------------------------------------------------------------------
[[nodiscard]] inline std::vector<seq::TxnId> dedup_global_order(
    const seq::GlobalLog& global,
    const std::map<seq::TxnId, std::set<seq::ShardId>>& shards_by_txn) {
    // How many DISTINCT shards has each txn appeared on in the sealed global log?
    std::map<seq::TxnId, std::set<seq::ShardId>> seen_on;
    for (const seq::GlobalEntry& e : global) {
        seen_on[e.txn_id].insert(e.shard);
    }

    std::vector<seq::TxnId> order;
    std::set<seq::TxnId> emitted;
    order.reserve(global.size());
    for (const seq::GlobalEntry& e : global) {
        if (emitted.count(e.txn_id) != 0) {
            continue;  // already placed at its lowest-rank-shard position (dedup)
        }
        // The seal gate (FAIL-CLOSED — XShardCommit.tla Sealable / all-or-nothing):
        // a txn is placed ONLY when its required shard set is KNOWN and it is fully
        // committed on EVERY shard it touches. A txn whose footprint is unknown
        // (absent from shards_by_txn) cannot be PROVEN fully committed, so it is
        // DROPPED — never emitted unproven. A txn not yet present on all its shards
        // is likewise dropped. Either way NONE of its writes apply: never a partial
        // cross-shard commit. (Dropping the unknown closes the atomicity bypass a
        // fall-through would open if a caller ever passed a phantom global entry.)
        const auto sit = shards_by_txn.find(e.txn_id);
        if (sit == shards_by_txn.end()) {
            continue;  // unknown footprint => cannot prove all-committed => drop
        }
        const std::set<seq::ShardId>& need = sit->second;
        const std::set<seq::ShardId>& have = seen_on[e.txn_id];
        bool complete = true;
        for (const seq::ShardId s : need) {
            if (have.count(s) == 0) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            continue;  // not sealed on all its shards yet => not committed
        }
        emitted.insert(e.txn_id);
        order.push_back(e.txn_id);
    }
    return order;
}

}  // namespace lockstep::txn
