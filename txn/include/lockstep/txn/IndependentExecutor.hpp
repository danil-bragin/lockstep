#pragma once

// IndependentExecutor.hpp — Phase 5 Stage I, impl B. THE SECOND, INDEPENDENT
// deterministic one-shot transaction executor.
//
// Source of truth: specs/CommitOrdering.tla (model-checked clean + human-
// approved) + briefs/phase5.md C5.1-C5.6 + master-plan D1/D2/D5. It implements
// the SAME binding seam txn/Transaction.hpp (Executor::submit_batch) as impl A
// (DeterministicExecutor) and is judged by the SAME Stage-M battery through the
// SAME factory surface.
//
// WHY A SECOND IMPL (master-plan §6.5): the commit protocol is critical, so it
// gets a DUAL independent implementation. This executor is built FROM THE SPEC,
// NOT from impl A's algorithm — a deliberately DIFFERENT internal design so the
// two impls do not share a blind spot. Both are cross-checked against the
// strict-serializable oracle AND each other.
//
// ============================================================================
// HOW THIS DIFFERS FROM IMPL A (the independence that earns the dual gate).
// ============================================================================
// Impl A (DeterministicExecutor) drives the REAL Phase-3 MVCC storage::WalEngine
// on a deterministic core::Scheduler with coroutines: it MUTATES a live store,
// captures an engine snapshot Seq after each commit, and serves reads by
// awaiting engine.get(k, Snapshot{seq}). Its serialization-history is a sidecar
// (commit_snap_) mapping commit_version -> engine Seq.
//
// Impl B (this file) is a PURE VALUE MODEL with NO storage engine, NO scheduler,
// NO coroutines, NO co_await. It keeps the committed serialization history as an
// explicit list of (id, writes, commit_version) records and serves EVERY read by
// directly recomputing ValueAfterPrefix(k, p) — scanning the committed history up
// to the chosen prefix `p` — exactly as specs/CommitOrdering.tla defines it. There
// is no mutable store and no snapshot-Seq indirection: a read result is a pure
// function of (key, prefix length) over the history. The serialization point of a
// txn is simply "the number of commits so far" (an integer), and the prefix a
// relaxed level chooses is another integer; reads never touch a live store.
//
// The two impls therefore exercise DISJOINT machinery: A's correctness depends on
// the MVCC engine + snapshot bookkeeping being right; B's depends on the
// history-scan ValueAfterPrefix being right. A bug shared by both would have to be
// in the SPEC's read semantics itself (which TLC already model-checked), not in an
// engine or a scan — which is exactly the blind spot the dual impl removes.
//
//   CommitOrdering.tla action      ->  IndependentExecutor mechanism
//   -----------------------------     --------------------------------------
//   seqLog (consensus total order) ->  submit_batch's `ordered_txns`.
//   Sequence(t)                    ->  enqueue WorkItem{idx,retries=0} (initial)
//                                      / push_back on re-sequence.
//   Execute (commit branch)        ->  read history at the txn's prefix, run body,
//                                      OLLP recon, APPEND a HistoryEntry, bump the
//                                      commit counter.
//   Execute (re-sequence branch)   ->  recon mismatch -> NO history effect,
//                                      retries+1, push the WorkItem to the back.
//   Execute (terminal-abort branch)->  retries == max_retry -> Status::Aborted
//                                      (C5.6 starvation bound).
//   Snapshot(t)/history[i].reads   ->  CommitInfo::reads_observed, recomputed by
//                                      value_at_prefix at THIS txn's serial prefix
//                                      (stable: later commits never rewrite an
//                                      earlier prefix's value).
//   store / ValueAfterPrefix       ->  HistoryLog::value_at_prefix(k, p): scan the
//                                      first p committed entries, newest write wins.
//   FootprintValid(t)              ->  CommitInfo::footprint_valid.
//
// ============================================================================
// D5 READ PATH (C5.4/C5.5) — each read served at EXACTLY its declared Level, the
// chosen prefix recorded in CommitInfo::ServedRead so the D5 checkers can judge.
// `serial = number of commits before this txn` (its serialization point).
//   StrictSerializable  : prefix = serial (the committed prefix strictly before
//                         this txn) — linearizable.
//   Snapshot(version)   : ALL snapshot reads in a txn share ONE prefix (no torn
//                         read); a named committed version (clamped to serial)
//                         selects the prefix, else serial (own snapshot).
//   BoundedStaleness(K) : a modeled local replica that lags the tip by
//                         ExecConfig.replica_lag commits; prefix =
//                         max(serial - replica_lag, 0), honoring max_lag when
//                         replica_lag <= max_lag.
//   ReadYourWrites(sid) : prefix >= the session's last own commit, so the session
//                         always observes its own prior committed write.
//
// ============================================================================
// DETERMINISM (binding; txn/ is NOT lint-exempt): the ONLY state is
// (ordered_txns, cfg). No clock, no threads, no std::*_distribution, no ambient
// randomness, no unordered iteration affecting output — std::map / std::vector,
// integer prefixes throughout. Same (ordered_txns, cfg) => byte-identical
// RunResult. There is no co_await here at all, so V-RKV1 is trivially satisfied
// (no pointer parked across a suspension point); the work list is indexed by
// value (std::size_t), never by a parked iterator.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// The independent, pure-value-model one-shot transaction executor (impl B).
class IndependentExecutor final : public Executor {
public:
    [[nodiscard]] RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                         const ExecConfig& cfg) override {
        RunResult out;
        HistoryLog log;

        // The work list IS the seqLog order. A re-sequenced txn is pushed to the
        // back with a bumped retry count (fresh recon) — the spec's Execute
        // re-sequence path (status->pending, retries+1). Finite: each txn
        // re-sequences at most max_retry times, so the list has at most
        // |txns| * (max_retry + 1) items (the spec's StateConstraint bound).
        struct WorkItem {
            std::size_t idx = 0;  // index into ordered_txns (a value, never a ptr)
            Seq retries = 0;
        };
        std::vector<WorkItem> work;
        work.reserve(ordered_txns.size());
        for (std::size_t i = 0; i < ordered_txns.size(); ++i) {
            work.push_back(WorkItem{i, 0});
        }

        // Consume the work list front-to-back; re-sequenced items append to the
        // back. `cursor` is the read position (NOT a queue pop, so the vector is
        // never reallocated out from under an in-flight reference — we copy each
        // item by value before doing any work).
        std::size_t cursor = 0;
        while (cursor < work.size()) {
            const WorkItem item = work[cursor];  // by value
            ++cursor;
            const Txn& t = ordered_txns[item.idx];

            // This txn's serialization point: the number of commits before it.
            const Seq serial = log.commit_count();

            // --- D5 read path: serve every declared read at its Level ---------
            ReadView reads;
            std::vector<CommitInfo::ServedRead> served;
            served.reserve(t.declared_reads.size());

            // A relaxed observational read on a key a strict read also covers must
            // NOT clobber the body's functional (strict) value. Track which keys a
            // strict read owns.
            std::map<Key, bool> strict_owns;
            for (const Read& r : t.declared_reads) {
                if (r.level == Level::StrictSerializable) {
                    strict_owns[r.key] = true;
                }
            }

            // The ONE prefix shared by all Snapshot reads in this txn (no torn
            // read). Fixed by the first Snapshot read; default = serial.
            Seq snapshot_prefix = serial;
            bool snapshot_fixed = false;

            for (const Read& r : t.declared_reads) {
                const Seq prefix =
                    choose_prefix(r, serial, cfg, log, snapshot_prefix, snapshot_fixed);
                const ReadResult value = log.value_at_prefix(r.key, prefix);

                CommitInfo::ServedRead s;
                s.key = r.key;
                s.level = r.level;
                s.session = r.session;
                s.max_lag = r.max_lag;
                s.served_version = prefix;
                s.value = value;
                served.push_back(std::move(s));

                // Feed the body: a strict read always provides the functional
                // value; a relaxed read provides it only when no strict read on the
                // same key already did.
                const bool owned_by_strict =
                    strict_owns.find(r.key) != strict_owns.end();
                if (r.level == Level::StrictSerializable || !owned_by_strict) {
                    reads[r.key] = value;
                }
            }

            // --- run the deterministic body -----------------------------------
            Txn::Outcome oc = t.body ? t.body(reads) : Txn::Outcome{};

            // --- OLLP reconnaissance (FootprintValid) -------------------------
            // The body surfaced the keys it actually needed. If any was NOT in the
            // declared (OLLP-predicted) footprint, recon mismatched at this point.
            const bool footprint_ok = footprint_valid(t, oc.extra_reads);

            if (!footprint_ok) {
                if (item.retries < cfg.max_retry) {
                    // Re-sequence with fresh recon. NO history effect.
                    work.push_back(WorkItem{item.idx, item.retries + 1});
                    continue;
                }
                // Bound exhausted -> deterministic terminal abort (C5.6).
                CommitInfo ci;
                ci.txn_id = t.id;
                ci.status = Status::Aborted;
                ci.footprint_valid = false;
                ci.retries = item.retries;
                ci.reads_observed = std::move(reads);
                ci.served_reads = std::move(served);
                out.commits.push_back(std::move(ci));
                continue;
            }

            // --- valid footprint: commit exactly once, in seqLog order --------
            const Seq commit_version = log.append(t.id, oc.writes, t.declared_reads);

            CommitInfo ci;
            ci.txn_id = t.id;
            ci.status = Status::Committed;
            ci.seq_index = commit_version;     // contiguous 1,2,3,...
            ci.commit_version = commit_version;
            ci.reads_observed = reads;
            ci.writes_committed = oc.writes;
            ci.result = std::move(oc.result);
            ci.footprint_valid = true;
            ci.retries = item.retries;
            ci.served_reads = std::move(served);
            out.commits.push_back(std::move(ci));
        }

        out.tip_version = log.commit_count();
        return out;
    }

    [[nodiscard]] std::string name() const override { return "IndependentExecutor"; }

private:
    // ----------------------------------------------------------------------
    // The committed serialization history — the ONLY persistent state. A pure
    // value model: each commit appends a record; a read is recomputed from the
    // records (NO live store, NO snapshot Seq). value_at_prefix(k, p) is a direct
    // transcription of CommitOrdering.tla ValueAfterPrefix(k, p): scan the first p
    // committed entries, the newest write to k wins, ∅ if none.
    // ----------------------------------------------------------------------
    class HistoryLog {
    public:
        // Append a committed txn; returns its contiguous commit_version (1-based).
        // Also records, for each session naming a ReadYourWrites read in `reads`,
        // the version at which that session wrote each key (for the RYW prefix).
        Seq append(std::uint64_t txn_id, const WriteSet& writes,
                   const std::vector<Read>& reads) {
            const Seq version = static_cast<Seq>(entries_.size()) + 1;
            entries_.push_back(Entry{txn_id, writes});
            for (const Read& r : reads) {
                if (r.level == Level::ReadYourWrites && r.session != 0) {
                    for (const auto& [k, v] : writes) {
                        (void)v;
                        session_last_write_[{r.session, k}] = version;
                    }
                }
            }
            return version;
        }

        // Number of committed txns so far (a serialization point / prefix length).
        [[nodiscard]] Seq commit_count() const {
            return static_cast<Seq>(entries_.size());
        }

        // ValueAfterPrefix(k, p): the value of k after the first `p` commits. The
        // newest write to k at or before prefix p wins; ∅ if never written. `p` is
        // clamped to the live history length (a future prefix sees the live tip,
        // which never happens on the served paths but keeps the function total).
        [[nodiscard]] ReadResult value_at_prefix(const Key& k, Seq p) const {
            const std::size_t limit =
                (static_cast<std::size_t>(p) < entries_.size())
                    ? static_cast<std::size_t>(p)
                    : entries_.size();
            ReadResult v = std::nullopt;
            for (std::size_t i = 0; i < limit; ++i) {
                const auto it = entries_[i].writes.find(k);
                if (it != entries_[i].writes.end()) {
                    v = it->second;  // newest write at or before p wins
                }
            }
            return v;
        }

        // The commit_version at which `session` last wrote `key`, or kNoSeq.
        [[nodiscard]] Seq session_last_write(SessionId session, const Key& key) const {
            const auto it = session_last_write_.find({session, key});
            return it == session_last_write_.end() ? kNoSeq : it->second;
        }

    private:
        struct Entry {
            std::uint64_t txn_id = 0;
            WriteSet writes;
        };
        std::vector<Entry> entries_;
        std::map<std::pair<SessionId, Key>, Seq> session_last_write_;
    };

    // Pick the committed prefix to serve `r` from, per its D5 Level. `serial` is
    // this txn's own serialization point. Mutates the shared snapshot prefix the
    // first time a Snapshot read is seen (no torn read across versions).
    [[nodiscard]] static Seq choose_prefix(const Read& r, Seq serial,
                                           const ExecConfig& cfg,
                                           const HistoryLog& log,
                                           Seq& snapshot_prefix,
                                           bool& snapshot_fixed) {
        switch (r.level) {
            case Level::StrictSerializable:
                // Linearizable: the committed prefix strictly before this txn.
                return serial;
            case Level::Snapshot: {
                // All Snapshot reads share ONE prefix. A named version (clamped to
                // the serial tip) fixes it; else the own serialization prefix.
                if (!snapshot_fixed) {
                    if (r.snapshot_version != kNoSeq && r.snapshot_version <= serial) {
                        snapshot_prefix = r.snapshot_version;
                    } else {
                        snapshot_prefix = serial;
                    }
                    snapshot_fixed = true;
                }
                return snapshot_prefix;
            }
            case Level::BoundedStaleness: {
                // Modeled local replica lagging the tip by replica_lag commits.
                const Seq lag = cfg.replica_lag;
                return (serial > lag) ? (serial - lag) : 0;
            }
            case Level::ReadYourWrites: {
                // Serve from a prefix >= the session's last own write, so the
                // session observes its own prior committed write. The own
                // serialization prefix already includes every prior commit, so it
                // is the strongest valid choice; never serve before the own write.
                Seq prefix = serial;
                const Seq own = log.session_last_write(r.session, r.key);
                if (own != kNoSeq && prefix < own) {
                    prefix = own;
                }
                return prefix;
            }
        }
        return serial;  // unreachable; keeps the function total.
    }

    // OLLP recon: the footprint is valid iff every key the body actually needed
    // (extra_reads) was already in the declared (OLLP-predicted) read footprint.
    [[nodiscard]] static bool footprint_valid(const Txn& t,
                                              const std::vector<Key>& extra_reads) {
        for (const Key& ek : extra_reads) {
            bool declared = false;
            for (const Read& r : t.declared_reads) {
                if (r.key == ek) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                return false;
            }
        }
        return true;
    }
};

// THE FACTORY for impl B. Same binding seam shape as deterministic_factory(): the
// harness swaps impl A <-> impl B <-> oracle <-> teeth by swapping the factory.
[[nodiscard]] inline ExecutorFactory independent_factory() {
    return [] {
        return std::unique_ptr<Executor>(std::make_unique<IndependentExecutor>());
    };
}

}  // namespace lockstep::txn
