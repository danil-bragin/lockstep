#pragma once

// DeterministicExecutor.hpp — Phase 5 Stage I. THE REAL distributed-txn executor.
//
// Source of truth: specs/CommitOrdering.tla (model-checked clean + human-
// approved) + briefs/phase5.md C5.1-C5.6 + master-plan D1/D2/D5. It implements
// the binding seam txn/Transaction.hpp (Executor::submit_batch) and is judged by
// the Stage-M battery (Oracle differential + the 8 checkers + linearizability +
// OLLP soundness) through the IDENTICAL factory surface as the oracle / teeth.
//
// ============================================================================
// WHAT IT IS — Calvin-style deterministic, sequential commit ordering (D1/D2),
// NO 2PC, NO concurrency at apply, backed by the REAL Phase-3 MVCC storage
// engine (storage::WalEngine), driven on the deterministic core::Scheduler.
//
//   CommitOrdering.tla action      ->  DeterministicExecutor mechanism
//   -----------------------------     --------------------------------------
//   seqLog (consensus total order) ->  submit_batch's `ordered_txns` (the
//                                      Phase-4 consensus output; here the agreed
//                                      global order is supplied to us).
//   Sequence(t)                    ->  appending t to the work queue (initial
//                                      pass) / re-appending on re-sequence.
//   Execute (commit branch)        ->  run body on the serialization-point
//                                      snapshot, OLLP recon, APPLY writes to the
//                                      MVCC engine at a fresh monotone commit
//                                      version, RECORD reads_observed.
//   Execute (re-sequence branch)   ->  recon mismatch -> discard the slot with
//                                      NO store/history effect, retries+1,
//                                      re-append to the queue back (fresh recon).
//   Execute (terminal-abort branch)->  retries == max_retry -> deterministic
//                                      Status::Aborted (C5.6 starvation bound).
//   Snapshot(t) / history[i].reads ->  CommitInfo::reads_observed, taken from
//                                      the engine at THIS txn's serialization
//                                      snapshot (stable under later writes).
//   store / ValueAfterPrefix       ->  the MVCC engine: serial prefix p maps to
//                                      the engine Seq snapshot captured right
//                                      after the p-th committed txn applied.
//   FootprintValid(t)              ->  CommitInfo::footprint_valid (true iff the
//                                      body asked for no key outside its
//                                      declared OLLP footprint at its own snap).
//
// ============================================================================
// D5 READ PATH (C5.4/C5.5) — each read served at EXACTLY its declared Level, the
// served prefix recorded in CommitInfo::ServedRead so the D5 checkers can judge:
//   StrictSerializable  : served from this txn's own serialization snapshot (the
//                         committed prefix strictly before it) — linearizable.
//   Snapshot(version)   : ALL snapshot reads in a txn share ONE chosen committed
//                         version (no torn read); kNoSeq selects this txn's own
//                         serialization snapshot (the strict default snapshot).
//   BoundedStaleness(K) : a modeled local replica that lags the tip by
//                         ExecConfig.replica_lag committed entries; served from
//                         max(tip - replica_lag, 0), which honors max_lag when
//                         replica_lag <= max_lag.
//   ReadYourWrites(sid) : served from a prefix >= the session's last own commit,
//                         so the session always observes its own prior writes.
//
// ============================================================================
// DETERMINISM (binding; txn/ is NOT lint-exempt): the ONLY state is
// (ordered_txns, cfg). The internal Scheduler + SimDisk are seeded with a FIXED
// constant (no wall-clock, no ambient randomness), faults OFF on the in-gate
// path. std::map / ordered iteration throughout; no std::*_distribution, no
// threads, no clock. Same (ordered_txns, cfg) => byte-identical RunResult.
//
// V-RKV1: no pointer/reference into a growable container is held across a
// co_await. Each await result is consumed into a value before the next await;
// the queue/maps are re-indexed by value, never by parked iterator.
//
// V-DET (storage seed): the engine's SimDisk runs with faults OFF and a fixed
// seed so a put's assigned Seq stream is a pure function of the apply order. We
// NEVER depend on the absolute engine Seq for the OBSERVABLE result — we map our
// own contiguous commit_version (1,2,3,...) onto the engine snapshot taken right
// after each commit (commit_snap_), so a multi-key txn (which consumes several
// engine Seqs) still presents ONE monotone commit_version to the checkers.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/WalEngine.hpp>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// The real deterministic, sequential, MVCC-backed one-shot transaction executor.
class DeterministicExecutor final : public Executor {
public:
    [[nodiscard]] RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                         const ExecConfig& cfg) override {
        RunResult out;
        // The whole batch runs inside ONE deterministic scheduler turn. The
        // engine's disk is fault-free + fixed-seed (the in-gate correctness
        // path); a real deployment would inject the sim's fault envelope (see
        // run_batch_with_disk for the fault-injection entry point).
        core::Scheduler sched;
        core::SimClock clock(sched);
        sim::SeededRandom rng(kStorageSeed);
        sim::DiskFaultConfig dc;  // faults OFF by default
        dc.latency_min = 0;
        dc.latency_max = 0;
        sim::SimDisk disk(sched, clock, rng, dc);
        storage::WalEngine engine(sched, disk);

        sched.spawn(run_batch(engine, ordered_txns, cfg, out));
        sched.run();
        return out;
    }

    [[nodiscard]] std::string name() const override { return "DeterministicExecutor"; }

private:
    // A fixed seed for the internal storage sim. The OBSERVABLE result must NOT
    // depend on this value (we map engine Seqs onto our own commit_version), so a
    // constant keeps the executor a pure function of (ordered_txns, cfg).
    static constexpr std::uint64_t kStorageSeed = 0x5713'5713'5713'5713ULL;

    // ----------------------------------------------------------------------
    // The serialization-history sidecar the D5 read path + checkers need. We map
    // our contiguous commit_version (1,2,...) onto the engine snapshot Seq taken
    // right AFTER that commit applied. commit_snap_[p] == engine Seq such that
    // get(k, {commit_snap_[p]}) == ValueAfterPrefix(k, p). commit_snap_[0] == 0.
    // ----------------------------------------------------------------------
    struct Apply {
        // (session, key) -> the commit_version at which the session last wrote k.
        std::map<std::pair<SessionId, Key>, Seq> session_last_write;
        // commit_version p -> engine snapshot Seq giving the state after p commits.
        std::vector<Seq> commit_snap{kNoSeq};  // index 0 == empty prefix
    };

    // Run the whole seqLog-ordered batch deterministically, one txn at a time.
    static core::Task run_batch(storage::Engine& engine,
                                const std::vector<Txn>& ordered_txns,
                                const ExecConfig& cfg, RunResult& out) {
        Apply ap;

        // Work queue == the seqLog order. A re-sequenced txn is re-appended to the
        // back with a bumped retry count (fresh recon) — exactly the spec's
        // Execute re-sequence path (status->pending, retries+1). The queue is
        // finite: each txn re-sequences at most max_retry times.
        struct Pending {
            std::size_t idx = 0;  // index into ordered_txns (NOT a pointer: V-RKV1)
            Seq retries = 0;
        };
        std::vector<Pending> queue;
        queue.reserve(ordered_txns.size());
        for (std::size_t i = 0; i < ordered_txns.size(); ++i) {
            queue.push_back(Pending{i, 0});
        }

        Seq next_commit_version = 0;  // contiguous serialization index 1,2,3,...
        std::size_t pos = 0;
        while (pos < queue.size()) {
            const Pending p = queue[pos];  // by value (V-RKV1)
            ++pos;
            const Txn& t = ordered_txns[p.idx];

            // committed entries strictly before this txn (its serialization point).
            const Seq serial_prefix = next_commit_version;

            // --- read path: serve each declared read at its D5 Level ----------
            ReadView reads;
            std::vector<CommitInfo::ServedRead> served;

            // A strict read on a key supplies the body's FUNCTIONAL value; a
            // relaxed observational read on the same key must NOT clobber it.
            std::map<Key, bool> has_strict;
            for (const Read& r : t.declared_reads) {
                if (r.level == Level::StrictSerializable) {
                    has_strict[r.key] = true;
                }
            }

            // The single Snapshot version shared by all Snapshot reads in this txn
            // (no torn read): kNoSeq on the read => own serialization prefix.
            Seq snapshot_prefix = serial_prefix;
            bool snapshot_prefix_set = false;

            for (const Read& r : t.declared_reads) {
                // Default = the own serialization prefix; the StrictSerializable
                // case keeps it. Every other arm overrides it. This init is the
                // value read for strict reads (not a dead store).
                Seq read_prefix = serial_prefix;
                switch (r.level) {
                    case Level::StrictSerializable:
                        // strict read serves at the own serialization prefix
                        // (the default above) — no override needed.
                        break;
                    case Level::Snapshot: {
                        // All Snapshot reads share ONE prefix. A non-kNoSeq request
                        // names an explicit committed version (clamped to tip); the
                        // first such request fixes the shared prefix.
                        if (!snapshot_prefix_set) {
                            if (r.snapshot_version != kNoSeq &&
                                r.snapshot_version <= serial_prefix) {
                                snapshot_prefix = r.snapshot_version;
                            } else {
                                snapshot_prefix = serial_prefix;
                            }
                            snapshot_prefix_set = true;
                        }
                        read_prefix = snapshot_prefix;
                        break;
                    }
                    case Level::BoundedStaleness: {
                        // Modeled local replica lags the tip by replica_lag entries.
                        const Seq lag = cfg.replica_lag;
                        read_prefix = (serial_prefix > lag) ? (serial_prefix - lag) : 0;
                        break;
                    }
                    case Level::ReadYourWrites: {
                        // Serve from a prefix >= the session's last own write so the
                        // session observes its own prior committed write. Default to
                        // the own serialization prefix (the strongest, which always
                        // includes prior writes); never serve BEFORE the own write.
                        read_prefix = serial_prefix;
                        const auto it = ap.session_last_write.find({r.session, r.key});
                        if (it != ap.session_last_write.end() && read_prefix < it->second) {
                            read_prefix = it->second;
                        }
                        break;
                    }
                }

                const Seq snap_seq = ap.commit_snap[read_prefix];
                // MVCC read on the REAL engine at the chosen snapshot.
                const std::optional<Value> v =
                    co_await engine.get(r.key, storage::Snapshot{snap_seq});

                CommitInfo::ServedRead s;
                s.key = r.key;
                s.level = r.level;
                s.session = r.session;
                s.max_lag = r.max_lag;
                s.served_version = read_prefix;  // checker compares this prefix
                s.value = v;

                // Feed the body: a strict read always sets the functional value; a
                // relaxed read sets it only when no strict read covers the key.
                if (r.level == Level::StrictSerializable || !has_strict[r.key]) {
                    reads[r.key] = v;
                }
                served.push_back(std::move(s));
            }

            // --- run the deterministic body -----------------------------------
            Txn::Outcome oc = t.body ? t.body(reads) : Txn::Outcome{};

            // --- OLLP reconnaissance (FootprintValid) -------------------------
            // The body surfaced extra_reads it actually needed (value-dependent
            // footprint expansion). Any not in the declared read set => mismatch.
            bool footprint_ok = true;
            for (const Key& ek : oc.extra_reads) {
                if (!declared_contains(t, ek)) {
                    footprint_ok = false;
                    break;
                }
            }

            if (!footprint_ok) {
                // Re-sequence (fresh recon) bounded by max_retry. NO store/history
                // effect — the consumed slot is discarded.
                if (p.retries < cfg.max_retry) {
                    queue.push_back(Pending{p.idx, p.retries + 1});
                    continue;
                }
                // Bound exhausted -> deterministic terminal abort (C5.6).
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

            // --- valid footprint: commit exactly once, in seqLog order --------
            // Apply the writes to the MVCC engine. Each put consumes a fresh engine
            // Seq; we capture the engine tip AFTER all writes landed as this commit
            // version's snapshot. (A del would map to engine.del; the Stage-M
            // workload is puts-only, but tombstones are handled identically.)
            for (const auto& [k, val] : oc.writes) {
                (void)co_await engine.put(k, val);
            }
            const storage::Snapshot tip = co_await engine.snapshot();

            ++next_commit_version;
            ap.commit_snap.push_back(tip.at);  // commit_snap[next_commit_version]

            CommitInfo ci;
            ci.txn_id = t.id;
            ci.status = Status::Committed;
            ci.seq_index = next_commit_version;
            ci.commit_version = next_commit_version;
            ci.reads_observed = reads;
            ci.writes_committed = oc.writes;
            ci.result = std::move(oc.result);
            ci.footprint_valid = true;
            ci.retries = p.retries;
            ci.served_reads = std::move(served);

            // Record this txn's session writes for ReadYourWrites: any RYW read in
            // the txn names its session; its writes are that session's writes.
            for (const Read& r : t.declared_reads) {
                if (r.level == Level::ReadYourWrites && r.session != 0) {
                    for (const auto& [k, val] : oc.writes) {
                        (void)val;
                        ap.session_last_write[{r.session, k}] = next_commit_version;
                    }
                }
            }

            out.commits.push_back(std::move(ci));
        }

        out.tip_version = next_commit_version;
        co_return;
    }

    [[nodiscard]] static bool declared_contains(const Txn& t, const Key& k) {
        for (const Read& r : t.declared_reads) {
            if (r.key == k) {
                return true;
            }
        }
        return false;
    }
};

// THE FACTORY. The Stage-I real impl is constructed through THIS (the binding
// seam shape) — the harness swaps impl <-> oracle <-> teeth by swapping it.
[[nodiscard]] inline ExecutorFactory deterministic_factory() {
    return [] {
        return std::unique_ptr<Executor>(std::make_unique<DeterministicExecutor>());
    };
}

}  // namespace lockstep::txn
