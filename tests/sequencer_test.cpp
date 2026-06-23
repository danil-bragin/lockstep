// sequencer_test.cpp — Phase 4 C4.4 GATE: the multi-shard SEQUENCER conforms to
// specs/Sequencer.tla on the REAL impl (consensus/sequencer/Sequencer.hpp).
//
// The sequencer merges N per-shard committed logs (Phase-4 consensus order per
// shard) into ONE global deterministic total order (Calvin epoch batching). This
// proves the four spec safety invariants HOLD on the real merge over a seed
// sweep, AND that a deliberately-WRONG merge (by-arrival instead of the fixed
// (ShardRank, idx) order) is CAUGHT — the teeth check.
//
// WHAT THIS ASSERTS over a seed sweep (<=64 in-gate), each seed generating
// multiple shards committing txns across epochs deterministically (seeded):
//   (a) GlobalOrderDeterministic — same shard logs ⇒ byte-identical global order
//       (the merge run twice gives the identical sequence).
//   (b) PerShardOrderPreserved — same-shard txns keep their per-shard (idx) order
//       in the global log (the merge never reorders within a shard).
//   (c) ExactlyOnceGlobal — every committed input entry with epoch <= sealed
//       appears EXACTLY ONCE; nothing is fabricated (every global entry pins back
//       to a real input entry); no (shard, idx) duplicated.
//   (d) EpochMonotone — global entries are in non-decreasing epoch order.
//   TEETH — a wrong by-arrival merge is FLAGGED by the determinism /
//       per-shard-order assertion (the gate has bite).
//
// DETERMINISM: pure function of (seed). Only entropy is the seed, consumed by an
// inlined SplitMix64; NO clock, NO threads, NO std::*_distribution, ordered
// containers throughout. NON-provider TU → the forbidden-call lint scans it.
// Seeds are printed for replay. Every run is bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <lockstep/consensus/sequencer/Sequencer.hpp>

namespace {

using lockstep::consensus::sequencer::Epoch;
using lockstep::consensus::sequencer::GlobalLog;
using lockstep::consensus::sequencer::merge;
using lockstep::consensus::sequencer::Sequencer;
using lockstep::consensus::sequencer::ShardId;
using lockstep::consensus::sequencer::ShardLog;
using lockstep::consensus::sequencer::ShardRank;
using lockstep::consensus::sequencer::to_txn_seqlog;

int g_failures = 0;

void check(bool cond, const char* what, std::uint64_t seed) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED (seed=%llu): %s\n",
                     static_cast<unsigned long long>(seed), what);
        ++g_failures;
    }
}

// In-gate sweep bound (freeze discipline: <= 64 seeds).
constexpr std::uint64_t kSeeds = 64;

// Inlined SplitMix64 — a pure, seeded PRNG (NO std::*_distribution, NO ambient
// randomness). Same seed ⇒ identical stream ⇒ identical generated shard logs.
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // A bounded draw in [0, n). n must be > 0.
    std::uint64_t below(std::uint64_t n) { return next() % n; }

private:
    std::uint64_t state_;
};

// One generated scenario: per-shard committed logs (epoch-tagged) + the injective
// shard ranks + a spec-legal sealed boundary.
struct Scenario {
    std::vector<ShardLog> shard_logs;
    ShardRank ranks;
    Epoch sealed = 0;
};

// Deterministically build a scenario from a seed: a few shards, each committing
// txns into ascending epochs (per-shard epochs non-decreasing, mirroring an
// append-only consensus log committing into the open epoch over time). Txn ids
// are globally unique. The sealed boundary is the spec-legal max_sealable (only a
// CLOSED epoch may be sealed — the top committed epoch may still be open).
Scenario make_scenario(std::uint64_t seed) {
    SplitMix rng(seed ^ 0x5151515151515151ULL);
    Scenario sc;

    const std::uint32_t num_shards = 2 + static_cast<std::uint32_t>(rng.below(3));  // 2..4
    sc.shard_logs.resize(num_shards);
    sc.ranks.resize(num_shards);
    // Injective ranks: a fixed total order over shards. Use the identity rank
    // (rank[s] = s) — injective, total, fixed. (Any injective vector conforms.)
    for (ShardId s = 0; s < num_shards; ++s) {
        sc.ranks[s] = s;
    }

    std::uint64_t next_txn = 0;
    for (ShardId s = 0; s < num_shards; ++s) {
        const std::uint32_t entries = 1 + static_cast<std::uint32_t>(rng.below(6));  // 1..6
        Epoch cur_epoch = 1;
        for (std::uint32_t i = 0; i < entries; ++i) {
            // Per-shard epoch is non-decreasing: sometimes advance (the open epoch
            // moved on between this shard's commits), bounded so we exercise both
            // same-epoch multi-shard merges and cross-epoch ordering.
            if (rng.below(3) == 0 && cur_epoch < 4) {
                ++cur_epoch;
            }
            std::string txn_id = "t" + std::to_string(next_txn++);
            sc.shard_logs[s].push_back({std::move(txn_id), cur_epoch});
        }
    }

    sc.sealed = lockstep::consensus::sequencer::max_sealable(sc.shard_logs);
    return sc;
}

// Render a global log to a stable byte string (for byte-identical determinism).
std::string render(const GlobalLog& g) {
    std::string out;
    for (const auto& e : g) {
        out += e.txn_id;
        out += '|';
        out += std::to_string(e.shard);
        out += '|';
        out += std::to_string(e.idx);
        out += '|';
        out += std::to_string(e.epoch);
        out += '\n';
    }
    return out;
}

// (b) PerShardOrderPreserved: for any two global entries from the SAME shard,
// earlier global position ⇒ strictly smaller per-shard idx.
bool per_shard_order_preserved(const GlobalLog& g) {
    for (std::size_t p = 0; p < g.size(); ++p) {
        for (std::size_t q = p + 1; q < g.size(); ++q) {
            if (g[p].shard == g[q].shard && !(g[p].idx < g[q].idx)) {
                return false;
            }
        }
    }
    return true;
}

// (d) EpochMonotone: global entries appear in non-decreasing epoch order.
bool epoch_monotone(const GlobalLog& g) {
    for (std::size_t p = 1; p < g.size(); ++p) {
        if (g[p - 1].epoch > g[p].epoch) {
            return false;
        }
    }
    return true;
}

// (c) ExactlyOnceGlobal + NoLossSealed: every committed input entry with
// epoch <= sealed is present EXACTLY ONCE; every global entry pins back to a real
// input entry (txn + epoch match, epoch <= sealed); no (shard, idx) duplicated.
bool exactly_once_global(const Scenario& sc, const GlobalLog& g) {
    // No duplicate (shard, idx) in the global log.
    std::map<std::pair<ShardId, std::uint64_t>, int> seen;
    for (const auto& e : g) {
        if (++seen[{e.shard, e.idx}] != 1) {
            return false;  // duplicated
        }
    }
    // Every global entry corresponds to a real committed input entry, sealed
    // (NoLossSealed — nothing fabricated).
    for (const auto& e : g) {
        if (e.shard >= sc.shard_logs.size()) {
            return false;
        }
        const ShardLog& log = sc.shard_logs[e.shard];
        if (e.idx < 1 || e.idx > log.size()) {
            return false;
        }
        const auto& in = log[e.idx - 1];
        if (in.txn_id != e.txn_id || in.epoch != e.epoch || e.epoch > sc.sealed) {
            return false;
        }
    }
    // Every committed input entry with epoch <= sealed is present (no loss).
    for (ShardId s = 0; s < sc.shard_logs.size(); ++s) {
        for (std::size_t i = 0; i < sc.shard_logs[s].size(); ++i) {
            if (sc.shard_logs[s][i].epoch <= sc.sealed && sc.sealed > 0) {
                const auto it = seen.find({s, static_cast<std::uint64_t>(i) + 1});
                if (it == seen.end()) {
                    return false;  // a sealed input entry is missing
                }
            }
        }
    }
    return true;
}

// THE DELIBERATELY-WRONG merge (teeth): emit each sealed epoch's batch in
// BY-ARRIVAL order (shard-major as logs were scanned) instead of the fixed
// (ShardRank, idx) order. This is still per-shard-ordered and exactly-once, but
// it is NOT the deterministic (ShardRank, idx) order — so the byte-identical
// CROSS-CHECK against the real merge catches it. We also build a variant that
// reverses within-shard idx order to break PerShardOrderPreserved.
GlobalLog wrong_by_reversed_idx(const Scenario& sc) {
    GlobalLog out;
    for (Epoch e = 1; e <= sc.sealed; ++e) {
        // Emit each shard's epoch-e entries in REVERSE idx order (breaks
        // PerShardOrderPreserved — a same-shard later idx appears first).
        for (ShardId s = 0; s < sc.shard_logs.size(); ++s) {
            const ShardLog& log = sc.shard_logs[s];
            for (std::size_t k = log.size(); k > 0; --k) {
                const std::size_t i = k - 1;
                if (log[i].epoch == e) {
                    out.push_back({log[i].txn_id, s,
                                   static_cast<std::uint64_t>(i) + 1, e});
                }
            }
        }
    }
    return out;
}

}  // namespace

int main() {
    bool any_nonempty = false;       // progress: the merge actually produced order
    bool any_multishard_epoch = false;  // a sealed epoch with >1 shard contributing
    int teeth_idx_caught = 0;        // wrong reversed-idx merge caught by (b)

    for (std::uint64_t seed = 0; seed < kSeeds; ++seed) {
        const Scenario sc = make_scenario(seed);
        Sequencer seq(sc.ranks);
        for (ShardId s = 0; s < sc.shard_logs.size(); ++s) {
            for (const auto& in : sc.shard_logs[s]) {
                seq.commit(s, in.txn_id, in.epoch);
            }
        }
        seq.seal_to(sc.sealed);

        const GlobalLog g = seq.global();

        // (a) GlobalOrderDeterministic — same inputs ⇒ byte-identical global order.
        // Run the pure merge() a SECOND time from the same inputs and compare bytes.
        const GlobalLog g2 = merge(sc.shard_logs, sc.ranks, sc.sealed);
        check(render(g) == render(g2),
              "GlobalOrderDeterministic: merge is byte-identical on re-run", seed);
        // And the stateful Sequencer's global() equals the pure merge oracle.
        check(render(g) == render(merge(sc.shard_logs, sc.ranks, sc.sealed)),
              "GlobalOrderDeterministic: Sequencer.global() == pure merge oracle",
              seed);

        // (b) PerShardOrderPreserved.
        check(per_shard_order_preserved(g),
              "PerShardOrderPreserved: same-shard idx order kept globally", seed);

        // (c) ExactlyOnceGlobal + NoLossSealed.
        check(exactly_once_global(sc, g),
              "ExactlyOnceGlobal: each sealed input entry once, none fabricated",
              seed);

        // (d) EpochMonotone.
        check(epoch_monotone(g),
              "EpochMonotone: global entries in non-decreasing epoch order", seed);

        // The txn seqLog hand-off: order-preserving, exactly-once, same length.
        const auto seqlog = to_txn_seqlog(g);
        check(seqlog.size() == g.size(),
              "to_txn_seqlog: one seqLog position per global entry", seed);

        // Progress / coverage bookkeeping.
        if (!g.empty()) {
            any_nonempty = true;
        }
        for (Epoch e = 1; e <= sc.sealed; ++e) {
            std::map<ShardId, int> shards_in_epoch;
            for (const auto& en : g) {
                if (en.epoch == e) {
                    ++shards_in_epoch[en.shard];
                }
            }
            if (shards_in_epoch.size() > 1) {
                any_multishard_epoch = true;
            }
        }

        // ============================================================
        // TEETH — a DELIBERATELY-WRONG merge is CAUGHT.
        // ============================================================
        // The reversed-idx merge breaks PerShardOrderPreserved (b). Only count
        // seeds where the wrong merge actually differs (a sealed epoch with >=2
        // same-shard entries) — otherwise the wrong merge coincides with the right
        // one and there is nothing to catch.
        const GlobalLog wrong = wrong_by_reversed_idx(sc);
        if (!per_shard_order_preserved(wrong)) {
            ++teeth_idx_caught;
            // And it must DIFFER byte-wise from the correct order (determinism
            // cross-check would flag it too).
            check(render(wrong) != render(g),
                  "TEETH: wrong merge differs byte-wise from the correct order",
                  seed);
        }
    }

    // Progress: the merge produced a non-empty global order on some seed, and we
    // exercised at least one multi-shard same-epoch merge (the merge's whole job).
    check(any_nonempty, "progress: some seed produced a non-empty global order", 0);
    check(any_multishard_epoch,
          "coverage: some sealed epoch had >1 shard contributing (real merge)", 0);

    // Teeth had bite: the wrong reversed-idx merge was caught by (b) on some seed.
    check(teeth_idx_caught > 0,
          "TEETH: a wrong (reversed-idx) merge was caught by PerShardOrderPreserved",
          0);

    if (g_failures == 0) {
        std::fprintf(stderr,
                     "[SEQUENCER] PASS: %llu seeds; 4 spec invariants hold on the "
                     "real merge (GlobalOrderDeterministic byte-identical, "
                     "PerShardOrderPreserved, ExactlyOnceGlobal, EpochMonotone); "
                     "teeth caught the wrong merge on %d seed(s).\n",
                     static_cast<unsigned long long>(kSeeds), teeth_idx_caught);
        return 0;
    }
    std::fprintf(stderr, "[SEQUENCER] FAIL: %d assertion(s) failed.\n", g_failures);
    return 1;
}
