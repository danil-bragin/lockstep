#pragma once

// Sequencer.hpp — Phase 4 C4.4. THE MULTI-SHARD GLOBAL TOTAL ORDER.
//
// Source of truth: specs/Sequencer.tla (model-checked clean + committed) +
// specs/Sequencer.check.md. This is the deterministic CROSS-shard merge that
// sits ON TOP of the verified Phase-4 per-shard consensus logs (each shard's
// committed log = one consensus order, ConsensusNode::log() / commit_index()):
// it combines N such per-shard logs into ONE global deterministic total order
// (Calvin-style epoch batching, D2 pure log-order). That global order IS the
// linearization point the distributed-txn layer (Phase 5, CommitOrdering.tla)
// consumes as its `seqLog`: Transaction.hpp's Executor::submit_batch takes the
// txns ALREADY in their agreed global order — the order this sequencer produces.
//
// ============================================================================
// MAPS DIRECTLY onto specs/Sequencer.tla.
// ============================================================================
//   TLA concept                                →  this header
//   ------------------------------------------    ------------------------------
//   Shard ids                                  →  ShardId (a small index)
//   ShardRank : [Shard -> Nat] (injective)     →  ShardRank vector (rank[shard])
//   inputLog[s] : Seq([txn, epoch])            →  ShardLog (per-shard input log)
//   one entry [txn, epoch] at 1-based idx i    →  InputEntry { txn_id; epoch }
//   sealed : Nat (sealed boundary)             →  the `sealed` arg to merge()
//   globalLog : Seq([txn, shard, idx, epoch])  →  std::vector<GlobalEntry>
//   EpochEntries(e) sorted by LessEntry        →  the per-epoch batch in merge()
//   GlobalLogUpto(sealed) (PURE oracle)        →  merge() itself (it IS the pure fn)
//
// ============================================================================
// THE DETERMINISTIC MERGE (specs/Sequencer.tla SealEpoch + GlobalLogUpto).
// ============================================================================
// merge(shard_logs, ranks, sealed) is a PURE FUNCTION of (shard logs, ranks,
// sealed boundary): same inputs ⇒ byte-identical global log. It:
//   1. Seals epochs 1..sealed IN ORDER (the outer loop, ascending epoch).
//   2. For each sealed epoch e, gathers EVERY shard's epoch-e entries
//      (EpochEntries(e)) and emits them sorted by the FIXED total order
//      LessEntry = (ShardRank[shard], per-shard idx). ShardRank is injective so
//      cross-shard ties never occur; within a shard idx is strictly increasing,
//      so the order is TOTAL and the sorted sequence is UNIQUE — no
//      nondeterministic choice (Sequencer.tla SortEntries).
//   3. Per-shard order is preserved because within one shard the sort key idx is
//      strictly increasing (Sequencer.tla PerShardOrderPreserved).
// Each committed input entry whose epoch <= sealed appears EXACTLY ONCE; nothing
// is fabricated (Sequencer.tla ExactlyOnceGlobal / NoLossSealed). Entries appear
// in non-decreasing epoch order because epochs are sealed ascending and a whole
// epoch's batch is emitted before the next (Sequencer.tla EpochMonotone).
//
// SEAL SAFETY (Sequencer.tla SealEpoch guard `sealed + 1 < epoch`): only a CLOSED
// epoch may be sealed — one the open epoch has moved past, so no late commit can
// land in it. The caller owns the open/sealed boundary (it comes from the
// Phase-4 commit progress); merge() is given the already-chosen `sealed` and only
// reads input entries whose epoch <= sealed. A caller that passes a `sealed`
// covering a still-open epoch would violate the spec's seal guard upstream — see
// max_sealable() which computes the largest spec-legal sealed boundary.
//
// ============================================================================
// INVARIANTS (binding; consensus/ is NOT lint-exempt):
//   * Pure function of its inputs (ultimately of the seed): NO wall-clock, NO
//     threads, NO std::*_distribution, NO unordered iteration affecting output.
//     std::stable_sort with the total comparator LessEntry; ordered containers.
//   * NO pointer into a growable container held across anything: merge() builds
//     and returns a value vector; callers copy what they need.
//   * Conforms to specs/Sequencer.tla.
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lockstep::consensus::sequencer {

// A shard id == an index into the per-shard log array and the ShardRank vector.
// (Sequencer.tla Shard; here a dense 0-based index for cheap array lookup.)
using ShardId = std::uint32_t;

// An epoch number. Epochs are 1-based (epoch 0 means "before the first epoch",
// matching the spec where Init sets epoch = 1, sealed = 0). (Sequencer.tla Nat.)
using Epoch = std::uint64_t;

// A 1-based per-shard index (the entry's position in its shard's input log).
// (Sequencer.tla idx; index i refers to inputLog[s][i].)
using Index = std::uint64_t;

// The opaque txn id being ordered. A string so callers can mint unique ids and
// match an input entry to its global-log appearance. (Sequencer.tla Txn.)
using TxnId = std::string;

// One committed input entry in a shard's log: a txn id tagged with the epoch it
// was committed in. Append-only; per-shard order is fixed by Phase-4 consensus.
// (Sequencer.tla inputLog[s][i] == [txn |-> ..., epoch |-> ...].)
struct InputEntry {
    TxnId txn_id;
    Epoch epoch = 0;
};

// A shard's committed input log: txn ids in that shard's fixed consensus order,
// each epoch-tagged. This is what Phase-4 consensus produces per shard (the
// committed prefix of ConsensusNode::log()). (Sequencer.tla inputLog[s].)
using ShardLog = std::vector<InputEntry>;

// One entry in the produced global total order. Carries its source identity
// (shard, idx, txn, epoch) so ExactlyOnce / NoLoss can be pinned one-to-one
// against the input logs. (Sequencer.tla globalLog[p] == [txn, shard, idx, epoch].)
struct GlobalEntry {
    TxnId txn_id;
    ShardId shard = 0;
    Index idx = 0;  // the entry's 1-based per-shard index
    Epoch epoch = 0;
};

// The produced global total order. (Sequencer.tla globalLog.)
using GlobalLog = std::vector<GlobalEntry>;

// The injective rank over shards used as the fixed merge tie-break key:
// ranks[shard] gives that shard's total-order rank. Any injective assignment
// works; determinism only needs it total + fixed. (Sequencer.tla ShardRank.)
using ShardRank = std::vector<std::uint64_t>;

// ----------------------------------------------------------------------------
// max_sealable — the largest spec-legal `sealed` boundary for these shard logs.
// ----------------------------------------------------------------------------
// Sequencer.tla seals only a CLOSED epoch (guard `sealed + 1 < epoch`): an epoch
// is sealable once the OPEN epoch has moved strictly past it. Modeling the
// Phase-4 commit progress as "the open epoch is one past the highest epoch any
// shard has committed into" (no further commits can land below it), the highest
// SEALABLE epoch is (max committed epoch) - 1: the top committed epoch may still
// be open, so we never seal it. Returns 0 when nothing is yet closed.
[[nodiscard]] inline Epoch max_sealable(const std::vector<ShardLog>& shard_logs) noexcept {
    Epoch max_committed = 0;
    for (const auto& log : shard_logs) {
        for (const auto& e : log) {
            if (e.epoch > max_committed) {
                max_committed = e.epoch;
            }
        }
    }
    return max_committed == 0 ? 0 : max_committed - 1;
}

// ----------------------------------------------------------------------------
// merge — THE deterministic cross-shard merge (Sequencer.tla GlobalLogUpto).
// ----------------------------------------------------------------------------
// Pure function of (shard_logs, ranks, sealed). Seals epochs 1..sealed in order;
// for each, gathers every shard's epoch-e entries and emits them sorted by the
// FIXED total order (ShardRank[shard], idx). Same inputs ⇒ byte-identical output.
//
// `ranks` must have one rank per shard (ranks.size() == shard_logs.size()) and be
// injective (a total order over shards). Entries whose epoch > sealed (or epoch 0
// / unsealed) are NOT emitted — they belong to a not-yet-sealed batch.
[[nodiscard]] inline GlobalLog merge(const std::vector<ShardLog>& shard_logs,
                                     const ShardRank& ranks,
                                     Epoch sealed) {
    GlobalLog out;

    // Seal epochs 1..sealed strictly IN ORDER (the GlobalLogUpto recursion's
    // ascending epoch boundary). Each epoch's whole batch precedes the next's, so
    // the global log is in non-decreasing epoch order (EpochMonotone).
    for (Epoch e = 1; e <= sealed; ++e) {
        // EpochEntries(e): every shard's epoch-e entries, carrying (shard, idx).
        GlobalLog batch;
        for (ShardId s = 0; s < shard_logs.size(); ++s) {
            const ShardLog& log = shard_logs[s];
            for (std::size_t i = 0; i < log.size(); ++i) {
                if (log[i].epoch == e) {
                    // idx is the 1-based per-shard index (spec idx == i+1).
                    batch.push_back(GlobalEntry{log[i].txn_id, s,
                                                static_cast<Index>(i) + 1, e});
                }
            }
        }

        // SortEntries(batch): the FIXED total order LessEntry =
        // (ShardRank[shard], idx). ShardRank injective ⇒ cross-shard ties never
        // occur; within a shard idx strictly increases ⇒ total order ⇒ a UNIQUE
        // sorted sequence (no nondeterministic choice). stable_sort is overkill
        // for a total order but makes the determinism explicit and tie-proof.
        std::stable_sort(batch.begin(), batch.end(),
                         [&ranks](const GlobalEntry& x, const GlobalEntry& y) {
                             const std::uint64_t rx = ranks[x.shard];
                             const std::uint64_t ry = ranks[y.shard];
                             if (rx != ry) {
                                 return rx < ry;
                             }
                             return x.idx < y.idx;
                         });

        out.insert(out.end(), batch.begin(), batch.end());
    }
    return out;
}

// ----------------------------------------------------------------------------
// The Sequencer — a thin stateful wrapper over the pure merge(), so a caller can
// hold per-shard logs + a sealed boundary and re-derive the global order. State
// is JUST the inputs (shard logs, ranks, sealed); global() recomputes from
// scratch every call (the GlobalLogUpto-is-the-truth modeling decision: the
// produced log can never diverge from the pure from-scratch merge).
// ----------------------------------------------------------------------------
class Sequencer {
public:
    // Construct with the fixed injective shard ranks. The number of shards is
    // fixed at construction (ranks.size()); a shard's rank never changes.
    explicit Sequencer(ShardRank ranks)
        : ranks_(std::move(ranks)), shard_logs_(ranks_.size()), sealed_(0) {}

    // Append a committed input entry to shard `s`'s log (the Phase-4 consensus
    // hand-off: a shard's consensus committed a txn into the open epoch). Mirrors
    // Sequencer.tla Commit — append-only, per-shard order is the call order.
    void commit(ShardId s, TxnId txn_id, Epoch epoch) {
        shard_logs_[s].push_back(InputEntry{std::move(txn_id), epoch});
    }

    // Advance the sealed boundary to `sealed` (must be >= the current boundary —
    // epochs seal monotonically, Sequencer.tla EpochMonotone `sealed <= epoch`).
    // The caller chooses a spec-legal boundary (see max_sealable). No-op if it
    // would move backward.
    void seal_to(Epoch sealed) {
        if (sealed > sealed_) {
            sealed_ = sealed;
        }
    }

    // The current sealed boundary.
    [[nodiscard]] Epoch sealed() const noexcept { return sealed_; }

    // The largest spec-legal sealed boundary for the current shard logs.
    [[nodiscard]] Epoch max_sealable() const noexcept {
        return sequencer::max_sealable(shard_logs_);
    }

    // The produced global total order for the current (shard logs, sealed). PURE:
    // recomputed from scratch via merge() — equals GlobalLogUpto(sealed). Two
    // Sequencers with the same shard logs + sealed return byte-identical logs.
    [[nodiscard]] GlobalLog global() const { return merge(shard_logs_, ranks_, sealed_); }

    // The per-shard input logs (read-only) — for checkers that pin global entries
    // back against the input (ExactlyOnce / NoLoss).
    [[nodiscard]] const std::vector<ShardLog>& shard_logs() const noexcept {
        return shard_logs_;
    }

private:
    ShardRank ranks_;
    std::vector<ShardLog> shard_logs_;
    Epoch sealed_;
};

// ----------------------------------------------------------------------------
// to_txn_seqlog — feed the global order to the Phase-5 txn executor's seqLog.
// ----------------------------------------------------------------------------
// Transaction.hpp's Executor::submit_batch takes the txns ALREADY in their agreed
// global order. The sequencer's global log IS that order; this extracts the txn
// ids in global-order so the caller can resolve each to a txn::Txn and submit the
// batch. The mapping is order-preserving and exactly-once (one global entry ⇒ one
// seqLog position), so the txn layer's seqLog == the sequencer's global log.
[[nodiscard]] inline std::vector<TxnId> to_txn_seqlog(const GlobalLog& global) {
    std::vector<TxnId> order;
    order.reserve(global.size());
    for (const auto& e : global) {
        order.push_back(e.txn_id);
    }
    return order;
}

}  // namespace lockstep::consensus::sequencer
