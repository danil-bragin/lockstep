#pragma once

// TeethExecutors.hpp — Phase 5 Stage M. The TEETH stubs + the trivial baselines.
//
// Source of truth: briefs/phase5.md Stage M (the TEETH test) + CommitOrdering.tla.
//
// Authored VERIFICATION-FIRST: there is NO real distributed-txn algorithm here.
// These are TRIVIAL executors that exist ONLY to prove the checkers + harness have
// TEETH — a harness that passes a known-wrong executor IS the bug.
//
//   HonestExecutor        — a trivial CORRECT executor: applies txns in seqLog
//                           order, sequentially, with honest OLLP recon + honest
//                           relaxed-level reads. The differential + every D5
//                           checker PASS on it (the clean baseline). It is NOT the
//                           real impl — it is the simplest thing that honors the
//                           spec, so a false positive in a checker shows up here.
//   NoProgressExecutor    — aborts (or never commits) every txn. Vacuously honors
//                           every SAFETY checker (no commit can violate a read
//                           rule) BUT makes NO PROGRESS — the harness must catch
//                           "vacuous ≠ correct".
//
//   --- the four deliberately-WRONG executors (each flagged by ONE checker) ---
//   StaleFootprintExecutor — (a) SKIPS OLLP recon: when a value-dependent footprint
//                           expands, it commits anyway on the stale predicted
//                           footprint -> a non-serializable result. FLAGGED by
//                           ollp_sound (+ shows as a strict_serializable /
//                           differential mismatch).
//   OutOfOrderExecutor     — (b) applies txns OUT OF seqLog order (reverses the
//                           batch) -> commit order is not the agreed serialization
//                           order. FLAGGED by serialized_by_seqlog /
//                           strict_serializable (real-time) / differential.
//   StaleStrictReadExecutor— (c) serves a StrictSerializable read from a STALE /
//                           snapshot value (the value one prefix behind) ->
//                           violates linearizability. FLAGGED by
//                           strict_serializable / differential.
//   RywLosesWriteExecutor  — (d) a ReadYourWrites session read that does NOT
//                           reflect the session's own prior write (serves an
//                           older prefix and the wrong value). FLAGGED by
//                           read_your_writes_level.
//
// All are PURE deterministic functions of (ordered_txns, cfg). std::map / ordered
// iteration only; no clock, no randomness. txn/ is NOT lint-exempt.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// ===========================================================================
// Shared serial-apply core. The honest executor + most teeth reuse this and
// tweak ONE knob, so each teeth's wrongness is isolated and obvious.
// ===========================================================================
//
// Behavior knobs (each teeth flips exactly one):
struct SerialKnobs {
    bool skip_ollp_recon = false;      // (a) commit on stale footprint, no re-seq
    bool reverse_order = false;        // (b) apply the batch back-to-front
    bool stale_strict_read = false;    // (c) serve strict reads one prefix behind
    bool ryw_loses_write = false;      // (d) RYW read ignores the session's writes
};

// A minimal serial executor parameterized by the knobs. Honest when all knobs
// are false. Models the spec's Execute action with the chosen defect injected.
class KnobbedSerialExecutor final : public Executor {
public:
    KnobbedSerialExecutor(SerialKnobs knobs, std::string nm)
        : knobs_(knobs), name_(std::move(nm)) {}

    [[nodiscard]] RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                         const ExecConfig& cfg) override {
        // The order this executor actually applies (honest = as given; teeth (b)
        // reverses it). We keep pointers into the caller's vector (it outlives the
        // call) — NO growable container is parked across an await (this is
        // synchronous), so V-RKV1 is not at risk.
        std::vector<const Txn*> order;
        order.reserve(ordered_txns.size());
        for (const Txn& t : ordered_txns) {
            order.push_back(&t);
        }
        if (knobs_.reverse_order) {
            std::vector<const Txn*> rev(order.rbegin(), order.rend());
            order.swap(rev);
        }

        store_.clear();
        history_versions_.clear();  // value of each key at each committed version
        session_last_write_.clear();
        Seq version = kNoSeq;
        RunResult out;

        struct Pending {
            const Txn* txn = nullptr;
            Seq retries = 0;
        };
        std::vector<Pending> queue;
        queue.reserve(order.size());
        for (const Txn* t : order) {
            queue.push_back(Pending{t, 0});
        }

        std::size_t pos = 0;
        Seq next_seq_index = 0;
        while (pos < queue.size()) {
            const Pending p = queue[pos];
            ++pos;
            const Txn& t = *p.txn;
            const Seq serial_prefix = version;  // committed entries before this txn

            // Which keys carry a StrictSerializable read? The body's FUNCTIONAL
            // value for such a key is the STRICT (own-prefix) value — a relaxed
            // observational read on the same key must NOT clobber it in the body's
            // ReadView (relaxed reads are observational overlays for the D5
            // checkers, not the serializable functional input).
            std::map<Key, bool> has_strict;
            for (const Read& r : t.declared_reads) {
                if (r.level == Level::StrictSerializable) {
                    has_strict[r.key] = true;
                }
            }

            // Build the read view + per-read served diagnostics, honoring each
            // read's D5 level (with the chosen defect for strict/ryw reads).
            ReadView reads;
            std::vector<CommitInfo::ServedRead> served;
            for (const Read& r : t.declared_reads) {
                CommitInfo::ServedRead s;
                s.key = r.key;
                s.level = r.level;
                s.session = r.session;
                s.max_lag = r.max_lag;

                Seq read_prefix = serial_prefix;  // default: own serialization pt
                if (r.level == Level::StrictSerializable && knobs_.stale_strict_read &&
                    serial_prefix > 0) {
                    // (c) DEFECT: serve the strict read one committed prefix behind
                    // (a stale / snapshot value) — violates linearizability.
                    read_prefix = serial_prefix - 1;
                } else if (r.level == Level::BoundedStaleness) {
                    // Honest local-replica read: served from (tip - replica_lag),
                    // clamped at 0, which honestly sits within max_lag when the
                    // replica lag is within the requested bound.
                    const Seq lag = cfg.replica_lag;
                    read_prefix = (serial_prefix > lag) ? (serial_prefix - lag) : 0;
                } else if (r.level == Level::ReadYourWrites && knobs_.ryw_loses_write) {
                    // (d) DEFECT: serve the RYW read from BEFORE the session's own
                    // last write — losing the session's own write.
                    const auto lw = session_last_write_.find({r.session, r.key});
                    if (lw != session_last_write_.end() && lw->second > 0) {
                        read_prefix = lw->second - 1;  // strictly before own write
                    }
                }
                // Snapshot reads: served from the txn's own serialization prefix
                // (internally consistent — honest D5Snapshot). read_prefix already
                // = serial_prefix for them.

                const ReadResult v = value_at(r.key, read_prefix);
                // Feed the body: a relaxed read does NOT overwrite a strict read on
                // the same key (the strict value is the functional input). A strict
                // read always sets the body's value.
                if (r.level == Level::StrictSerializable || !has_strict[r.key]) {
                    reads[r.key] = v;
                }
                s.served_version = read_prefix;
                s.value = v;
                served.push_back(std::move(s));
            }

            // Run the body.
            Txn::Outcome oc = t.body ? t.body(reads) : Txn::Outcome{};

            // OLLP recon: extra_reads outside the declared footprint = mismatch.
            bool footprint_ok = true;
            for (const Key& ek : oc.extra_reads) {
                bool declared = false;
                for (const Read& r : t.declared_reads) {
                    if (r.key == ek) {
                        declared = true;
                        break;
                    }
                }
                if (!declared) {
                    footprint_ok = false;
                    break;
                }
            }

            if (!footprint_ok && !knobs_.skip_ollp_recon) {
                // Honest: re-sequence with fresh recon, bounded by max_retry.
                if (p.retries < cfg.max_retry) {
                    queue.push_back(Pending{p.txn, p.retries + 1});
                    continue;
                }
                CommitInfo ci;
                ci.txn_id = t.id;
                ci.status = Status::Aborted;
                ci.footprint_valid = false;
                ci.retries = p.retries;
                ci.served_reads = std::move(served);
                ci.reads_observed = std::move(reads);
                out.commits.push_back(std::move(ci));
                continue;
            }
            // (a) DEFECT path: knobs_.skip_ollp_recon => commit anyway, recording
            // footprint_valid = footprint_ok (which is FALSE here) so the checker
            // can catch the stale-footprint commit.

            // Commit (in this executor's apply order).
            ++version;
            ++next_seq_index;
            for (const auto& [k, val] : oc.writes) {
                store_[k] = val;
            }
            history_versions_.push_back(store_);  // snapshot the store at this ver

            CommitInfo ci;
            ci.txn_id = t.id;
            ci.status = Status::Committed;
            ci.seq_index = next_seq_index;
            ci.commit_version = version;
            ci.reads_observed = reads;
            ci.writes_committed = oc.writes;
            ci.result = std::move(oc.result);
            ci.footprint_valid = footprint_ok;
            ci.retries = p.retries;
            ci.served_reads = std::move(served);

            // Track this txn's session writes for RYW.
            for (const Read& r : t.declared_reads) {
                if (r.level == Level::ReadYourWrites && r.session != 0) {
                    for (const auto& [k, val] : oc.writes) {
                        (void)val;
                        session_last_write_[{r.session, k}] = version;
                    }
                }
            }

            out.commits.push_back(std::move(ci));
        }

        out.tip_version = version;
        return out;
    }

    [[nodiscard]] std::string name() const override { return name_; }

private:
    // Value of key k as-of committed version `ver` (== the store snapshot after
    // `ver` commits). ver 0 ⇒ ∅. history_versions_[i] is the store after the
    // (i+1)-th commit (version i+1).
    [[nodiscard]] ReadResult value_at(const Key& k, Seq ver) const {
        if (ver == 0 || ver > history_versions_.size()) {
            // ver==0 -> nothing; ver beyond what we have -> use the live store.
            if (ver == 0) {
                return std::nullopt;
            }
            const auto it = store_.find(k);
            return it == store_.end() ? std::nullopt : ReadResult(it->second);
        }
        const std::map<Key, Value>& snap = history_versions_[ver - 1];
        const auto it = snap.find(k);
        return it == snap.end() ? std::nullopt : ReadResult(it->second);
    }

    SerialKnobs knobs_;
    std::string name_;
    std::map<Key, Value> store_;
    std::vector<std::map<Key, Value>> history_versions_;
    std::map<std::pair<SessionId, Key>, Seq> session_last_write_;
};

// ===========================================================================
// The no-progress baseline: aborts every txn. Vacuously safe, makes no progress.
// ===========================================================================
class NoProgressExecutor final : public Executor {
public:
    [[nodiscard]] RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                         const ExecConfig& /*cfg*/) override {
        RunResult out;
        for (const Txn& t : ordered_txns) {
            CommitInfo ci;
            ci.txn_id = t.id;
            ci.status = Status::Aborted;  // terminal, but never commits
            ci.footprint_valid = true;    // honest: it never claims a bad commit
            out.commits.push_back(std::move(ci));
        }
        out.tip_version = kNoSeq;
        return out;
    }
    [[nodiscard]] std::string name() const override { return "NoProgressExecutor"; }
};

// ===========================================================================
// Factories — swap the executor behind the identical harness + checkers.
// ===========================================================================

[[nodiscard]] inline ExecutorFactory honest_factory() {
    return [] {
        return std::unique_ptr<Executor>(
            std::make_unique<KnobbedSerialExecutor>(SerialKnobs{}, "HonestExecutor"));
    };
}

[[nodiscard]] inline ExecutorFactory no_progress_factory() {
    return [] { return std::unique_ptr<Executor>(std::make_unique<NoProgressExecutor>()); };
}

[[nodiscard]] inline ExecutorFactory stale_footprint_factory() {
    return [] {
        SerialKnobs k;
        k.skip_ollp_recon = true;
        return std::unique_ptr<Executor>(
            std::make_unique<KnobbedSerialExecutor>(k, "StaleFootprintExecutor"));
    };
}

[[nodiscard]] inline ExecutorFactory out_of_order_factory() {
    return [] {
        SerialKnobs k;
        k.reverse_order = true;
        return std::unique_ptr<Executor>(
            std::make_unique<KnobbedSerialExecutor>(k, "OutOfOrderExecutor"));
    };
}

[[nodiscard]] inline ExecutorFactory stale_strict_read_factory() {
    return [] {
        SerialKnobs k;
        k.stale_strict_read = true;
        return std::unique_ptr<Executor>(
            std::make_unique<KnobbedSerialExecutor>(k, "StaleStrictReadExecutor"));
    };
}

[[nodiscard]] inline ExecutorFactory ryw_loses_write_factory() {
    return [] {
        SerialKnobs k;
        k.ryw_loses_write = true;
        return std::unique_ptr<Executor>(
            std::make_unique<KnobbedSerialExecutor>(k, "RywLosesWriteExecutor"));
    };
}

}  // namespace lockstep::txn
