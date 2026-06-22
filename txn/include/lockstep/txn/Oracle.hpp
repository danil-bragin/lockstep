#pragma once

// Oracle.hpp — Phase 5 Stage M. THE STRICT-SERIALIZABLE TXN ORACLE.
//
// Source of truth: specs/CommitOrdering.tla. This is the TRIVIAL, obviously-
// correct reference executor — the ground truth the differential harness judges
// every real txn executor against (briefs/phase5.md Stage M ORACLE).
//
// WHAT THIS IS: apply the submitted txns in seqLog order, ONE AT A TIME, each
// reading the committed prefix (the live store after all earlier committed txns)
// and writing its outputs. NO concurrency, NO 2PC, NO faults, NO background work.
// It is a direct executable transcription of CommitOrdering.tla's Execute action:
//
//   * store : Key -> latest committed Value (∅ default). == the spec's `store`.
//   * For each txn t in order:
//       - take its serialization snapshot over declared_reads + the trigger keys
//         from the live store (Snapshot(t) — the values it reads).
//       - run the body on that snapshot to learn its writes + extra_reads
//         (the value-dependent footprint expansion: Trigger/Extra).
//       - OLLP recon (FootprintValid): if the body asked for extra_reads NOT in
//         the declared footprint, the recon MISMATCHED -> RE-SEQUENCE (discard
//         the slot with NO store/history effect, retries+1, re-run at the END of
//         the remaining order with a FRESH snapshot). Bounded by max_retry; once
//         exhausted -> deterministic terminal ABORT (C5.6 starvation avoidance).
//       - on a valid footprint: APPLY the writes to the store at a fresh
//         monotonic commit version, RECORD the observed reads into history
//         (snapshot-stable: later txns mutating the store never change what THIS
//         txn read — OLLPSound is a statement about each txn's own commit point).
//
// Because it executes the literal serial order, the oracle's per-txn results ARE
// the strict-serializable ground truth: SerializedBySeqLog, ReadsMatchSerial
// Prefix, StoreReflectsHistory, OLLPSound, ExactlyOnce all hold by construction.
// If the oracle were complicated it could not be trusted as truth; every step is
// straight-line logic over ordered maps.
//
// DETERMINISM (binding): std::map throughout (ordered, no hash iteration); a
// monotonic version counter; no clock, no randomness. Same (ordered_txns, cfg) ⇒
// byte-identical RunResult. txn/ is NOT lint-exempt.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// The strict-serializable reference executor. The default (StrictSerializable)
// path is what the oracle realizes: every read sees exactly the committed prefix
// strictly before this txn's serialization point.
class StrictSerialOracle final : public Executor {
public:
    [[nodiscard]] RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                         const ExecConfig& cfg) override {
        store_.clear();
        Seq version = kNoSeq;
        RunResult out;

        // Work queue = the seqLog order. A re-sequenced txn is re-appended to the
        // back with a bumped retry count (fresh recon), exactly as the spec's
        // Execute re-sequence path sets status->pending and retries+1. The queue
        // is finite: each txn re-sequences at most max_retry times.
        struct Pending {
            const Txn* txn = nullptr;
            Seq retries = 0;
        };
        std::vector<Pending> queue;
        queue.reserve(ordered_txns.size());
        for (const Txn& t : ordered_txns) {
            queue.push_back(Pending{&t, 0});
        }

        std::size_t pos = 0;
        Seq next_seq_index = 0;
        while (pos < queue.size()) {
            const Pending p = queue[pos];
            ++pos;
            const Txn& t = *p.txn;

            // --- serialization snapshot over the PREDICTED footprint -----------
            // Read each declared key from the live store (the committed prefix).
            // The level on the read is honored by the oracle's strict semantics:
            // EVERY read is served from THIS txn's own serialization snapshot (the
            // live tip just before it executes), which is the strongest contract
            // and therefore satisfies every D5 level (a stronger read trivially
            // honors a weaker promise). Served-prefix diagnostics record that.
            ReadView reads;
            std::vector<CommitInfo::ServedRead> served;
            const Seq serial_prefix = version;  // committed entries before this txn
            for (const Read& r : t.declared_reads) {
                const ReadResult v = lookup(r.key);
                reads[r.key] = v;
                served.push_back(make_served(r, serial_prefix, v));
            }

            // --- run the deterministic body -----------------------------------
            Txn::Outcome oc = t.body ? t.body(reads) : Txn::Outcome{};

            // --- OLLP reconnaissance (FootprintValid) -------------------------
            // The body surfaced extra_reads it actually needed (value-dependent
            // footprint expansion). If any are NOT already in the declared read
            // set, the recon MISMATCHED at this serialization point.
            bool footprint_ok = true;
            for (const Key& ek : oc.extra_reads) {
                if (!declared_contains(t, ek)) {
                    footprint_ok = false;
                    break;
                }
            }

            if (!footprint_ok) {
                // Re-sequence with fresh recon, bounded by max_retry. NO store/
                // history effect (the consumed slot is discarded).
                if (p.retries < cfg.max_retry) {
                    queue.push_back(Pending{p.txn, p.retries + 1});
                    continue;
                }
                // Bound exhausted -> deterministic terminal abort (C5.6).
                CommitInfo ci;
                ci.txn_id = t.id;
                ci.status = Status::Aborted;
                ci.footprint_valid = false;
                ci.retries = p.retries;
                ci.served_reads = served;
                ci.reads_observed = reads;
                out.commits.push_back(std::move(ci));
                continue;
            }

            // --- valid footprint: commit exactly once, in seqLog order --------
            ++version;
            ++next_seq_index;
            for (const auto& [k, val] : oc.writes) {
                store_[k] = val;
            }
            CommitInfo ci;
            ci.txn_id = t.id;
            ci.status = Status::Committed;
            ci.seq_index = next_seq_index;
            ci.commit_version = version;
            ci.reads_observed = reads;          // snapshot-stable history record
            ci.writes_committed = oc.writes;
            ci.result = std::move(oc.result);
            ci.footprint_valid = true;
            ci.retries = p.retries;
            ci.served_reads = std::move(served);

            // The oracle always serves the live committed tip, so a ReadYourWrites
            // session ALWAYS observes its own prior writes — the strongest read
            // trivially honors the weakest promise. No extra session bookkeeping
            // is needed in the ground truth.
            out.commits.push_back(std::move(ci));
        }

        out.tip_version = version;
        return out;
    }

    [[nodiscard]] std::string name() const override { return "StrictSerialOracle"; }

private:
    [[nodiscard]] ReadResult lookup(const Key& k) const {
        const auto it = store_.find(k);
        if (it == store_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] static bool declared_contains(const Txn& t, const Key& k) {
        for (const Read& r : t.declared_reads) {
            if (r.key == k) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static CommitInfo::ServedRead make_served(const Read& r,
                                                            Seq serial_prefix,
                                                            const ReadResult& v) {
        CommitInfo::ServedRead s;
        s.key = r.key;
        s.level = r.level;
        // The oracle serves EVERY read from this txn's own serialization prefix
        // (the strongest snapshot), so served_version == the serial prefix for
        // all levels. A relaxed-level executor MAY serve an older prefix; the D5
        // checkers test the served prefix against each level's contract.
        s.served_version = serial_prefix;
        s.session = r.session;
        s.max_lag = r.max_lag;
        s.value = v;
        return s;
    }

    std::map<Key, Value> store_;
};

}  // namespace lockstep::txn
