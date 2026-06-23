#pragma once

// Database.hpp — Phase 6 Stage F. THE DEVELOPER-FACING CLIENT SURFACE.
//
// Source of truth: lockstep-phase-specs-all.md Phase 6 C6.1 (transaction-function
// model) + C6.2 (read/query language); master-plan D1 (ONE-SHOT, no interactive
// sessions) / D3 (NEW non-SQL model) / D5 (the four read levels, call-site-
// visible). This is the foundation the wire protocol / drivers / CLI (Stage B)
// build on. It WRAPS the LANDED seams — txn::Executor (via deterministic_factory)
// + storage::Engine MVCC — and reinvents NOTHING.
//
// ============================================================================
// (1) THE TRANSACTION-FUNCTION MODEL (C6.1 / D1 / D3 / V-DET-USER).
// A user authors a ONE-SHOT transaction as a DETERMINISTIC C++ function over a
// typed handle `TxnContext` that exposes ONLY deterministic read/write operations
// (no clock, no random, no IO). They DECLARE the read footprint (the keys the body
// reads) and SUBMIT the function; they get a typed result back. There is NO
// interactive begin/commit session and NO client-held cursor (D1): the whole
// function is submitted, sequenced, and executed deterministically by the
// underlying txn::Executor.
//
//   user writes:                          maps onto:
//   ----------------------------------    ------------------------------------
//   db.submit(TxnFn{ declared, body })  -> txn::Txn (declared_reads + pure body)
//   ctx.read(key)        (in body)      -> the value txn::ReadView gave for key
//   ctx.write(key, val)  (in body)      -> txn::Txn::Outcome::writes[key]
//   ctx.also_read(key)   (in body)      -> txn::Txn::Outcome::extra_reads (OLLP)
//   ctx.result(token)    (in body)      -> txn::Txn::Outcome::result
//
// V-DET-USER (compile-enforced purity): the body receives ONLY a `TxnContext&`.
// TxnContext exposes read/write/also_read/result and NOTHING ELSE — no clock,
// no rng, no IO handle, not even a non-const ambient. The body therefore CANNOT
// reach a nondeterministic source; it is a pure function of its declared reads.
// This is enforced by the TYPE (the body signature), so a nondeterministic txn
// does not compile — exactly the V-DET-USER guarantee.
//
// ============================================================================
// (2) THE READ/QUERY SURFACE (C6.2 / D5 / V-D5-SAFE).
// `db.run(Query<L>)` executes a typed, composable, NON-SQL read (Query.hpp:
// get/scan/range) over storage::Engine MVCC AT the call-site-visible D5 level L.
// The planner (Query.hpp::plan_query) resolves L to ONE committed snapshot; this
// file drives the per-step engine reads at that snapshot and records the
// per-read served-prefix diagnostics so the SAME txn D5 checkers judge the result.
//
// ============================================================================
// FORBIDDEN (query/ is NOT lint-exempt): wall-clock, threads, std::*_distribution,
// unordered iteration affecting output, any nondeterminism. The Database is a pure
// function of its submitted txns + queries (ultimately of the seed). No pointer
// into a growable container across a co_await (V-RKV1): the query path consumes
// each engine read into a value before the next.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/WalEngine.hpp>

#include <lockstep/txn/DeterministicExecutor.hpp>
#include <lockstep/txn/Transaction.hpp>

#include <lockstep/query/Query.hpp>

namespace lockstep::query {

// ----------------------------------------------------------------------------
// TxnContext — the DETERMINISTIC handle a user txn body operates over. It exposes
// ONLY deterministic read/write/footprint/result operations. There is NO clock,
// NO rng, NO IO on this type — so a user body, which receives only this handle,
// is PURE BY CONSTRUCTION (V-DET-USER, compile-enforced).
//
// A body reads the values the executor presented for its declared reads via
// read(key); writes via write(key, value); surfaces a value-dependent footprint
// expansion (the OLLP trigger) via also_read(key); and sets its observable result
// via result(token). The context accumulates the writes/extra_reads/result, which
// the Database packs into the txn::Txn::Outcome.
// ----------------------------------------------------------------------------
class TxnContext {
public:
    explicit TxnContext(const txn::ReadView& reads) : reads_(reads) {}

    // Read a declared key's value at this txn's serialization snapshot. Returns ∅
    // for an absent / tombstoned key. PURE: just looks up what the executor gave.
    [[nodiscard]] ReadResult read(const Key& key) const {
        const auto it = reads_.find(key);
        if (it == reads_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Stage a write of key->value to commit. A later write of the same key wins
    // (last-writer in body order), matching txn::WriteSet semantics.
    void write(Key key, Value value) { writes_[std::move(key)] = std::move(value); }

    // Surface a value-dependent footprint expansion (OLLP trigger / Extra): a key
    // the body decided it ALSO needs based on what it read. If it is outside the
    // declared footprint the executor RE-SEQUENCES (then aborts past MaxRetry) —
    // this is the honest OLLP path, judged by check_ollp_sound.
    void also_read(Key key) { extra_reads_.push_back(std::move(key)); }

    // Set the observable result token the client receives.
    void result(std::string token) { result_ = std::move(token); }

    // --- the Database packs these into the txn::Txn::Outcome (not user-facing) --
    [[nodiscard]] txn::Txn::Outcome into_outcome() && {
        txn::Txn::Outcome oc;
        oc.writes = std::move(writes_);
        oc.result = std::move(result_);
        oc.extra_reads = std::move(extra_reads_);
        return oc;
    }

private:
    const txn::ReadView& reads_;  // borrowed for the body's lifetime only (no parking)
    txn::WriteSet writes_;
    std::vector<Key> extra_reads_;
    std::string result_;
};

// ----------------------------------------------------------------------------
// A user-authored ONE-SHOT transaction function. `declared` is the OLLP-predicted
// read footprint (the reads, each carrying its D5 level via Query.hpp/txn helpers);
// `body` is the deterministic command over a TxnContext. The body signature
// (TxnContext& only) is what enforces V-DET-USER: nothing nondeterministic is
// reachable from inside it.
// ----------------------------------------------------------------------------
struct TxnFn {
    std::uint64_t id = 0;                 // stable client tag (txn::Txn::id)
    std::vector<txn::Read> declared;      // declared read footprint (RSet[t])
    std::function<void(TxnContext&)> body;  // the deterministic command body
};

// A declared-read helper namespace so authoring reads at a level reads naturally:
//   reads(declare::strict("acct:a"), declare::strict("acct:b"))
namespace declare {
[[nodiscard]] inline txn::Read strict(Key key) { return txn::read_strict(std::move(key)); }
[[nodiscard]] inline txn::Read snapshot(Key key, Seq version = kNoSeq) {
    return txn::read_snapshot(std::move(key), version);
}
[[nodiscard]] inline txn::Read bounded(Key key, Seq max_lag) {
    return txn::read_bounded(std::move(key), max_lag);
}
[[nodiscard]] inline txn::Read ryw(Key key, SessionId session) {
    return txn::read_ryw(std::move(key), session);
}
}  // namespace declare

// Pack a list of declared reads into a vector (variadic sugar for authoring).
template <typename... Rs>
[[nodiscard]] std::vector<txn::Read> reads(Rs... rs) {
    return std::vector<txn::Read>{std::move(rs)...};
}

// What a submitted batch reports back to the client: the per-txn observable
// outcome, in serialization order. This is exactly txn::RunResult — we surface it
// directly so the conformance test can hand it to the txn checkers unchanged.
using SubmitResult = txn::RunResult;

// ----------------------------------------------------------------------------
// THE DATABASE / CLIENT. The single developer-facing object. It owns the txn
// Executor (the LANDED deterministic_factory by default) and a storage::Engine
// for the standalone read/query path. ONE-SHOT only: submit(batch) executes a
// whole batch of TxnFns; run(Query) executes a typed read at its D5 level.
// ----------------------------------------------------------------------------
class Database {
public:
    // Default: the real verified deterministic executor (deterministic_factory).
    // A different factory (e.g. the oracle, for conformance) can be injected.
    Database() : exec_factory_(txn::deterministic_factory()) {}
    explicit Database(txn::ExecutorFactory factory)
        : exec_factory_(std::move(factory)) {}

    // ---- (1) submit a batch of one-shot txn functions (C6.1) ----------------
    // The TxnFns are ALREADY in their agreed global order (the seqLog the
    // consensus layer would produce; the conformance harness supplies a seeded
    // order to stand in for it). Each TxnFn is wrapped into a txn::Txn whose body
    // runs the user body over a TxnContext; the batch is handed to the Executor.
    // Returns the per-txn observable outcome in serialization order.
    [[nodiscard]] SubmitResult submit(const std::vector<TxnFn>& ordered_fns,
                                      const txn::ExecConfig& cfg = {}) const {
        std::vector<txn::Txn> txns;
        txns.reserve(ordered_fns.size());
        for (const TxnFn& fn : ordered_fns) {
            txns.push_back(to_txn(fn));
        }
        auto executor = exec_factory_();
        return executor->submit_batch(txns, cfg);
    }

    // Submit a SINGLE one-shot txn function (the common client call).
    [[nodiscard]] SubmitResult submit(const TxnFn& fn,
                                      const txn::ExecConfig& cfg = {}) const {
        return submit(std::vector<TxnFn>{fn}, cfg);
    }

    // ---- (2) run a typed read/query at its call-site-visible D5 level (C6.2) -
    // Executes a Query<L> over the standalone storage::Engine MVCC. The level L is
    // part of the query's TYPE (V-D5-SAFE), so it is impossible to omit. The
    // planner resolves L to ONE committed snapshot; we drive the per-step engine
    // reads at that snapshot and record served-prefix diagnostics for the checkers.
    //
    // `replica_lag` models the BoundedStaleness local-replica lag for this read;
    // `session_last_write` is the highest committed prefix the RYW session wrote.
    template <typename L>
    [[nodiscard]] QueryResult run(const Query<L>& q, Seq replica_lag = 0,
                                  Seq session_last_write = 0) {
        QueryResult out;
        core::Scheduler sched;
        core::SimClock clock(sched);
        sim::SeededRandom rng(kStorageSeed);
        sim::DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 0;
        sim::SimDisk disk(sched, clock, rng, dc);
        storage::WalEngine engine(sched, disk);

        // run_query deterministically replays the primed history_ into this engine
        // (building the versioned MVCC store), then reads the planned query AS-OF
        // the committed prefix the call-site D5 level resolves to.
        sched.spawn(run_query(engine, q, replica_lag, session_last_write, out));
        sched.run();
        return out;
    }

    // Prime the standalone read-path with a committed write-set HISTORY IN ORDER:
    // `history[p-1]` is the write-set committed by the p-th commit (commit_version
    // p), so each entry advances the version by one — building a versioned MVCC
    // history the D5 read path can read AS-OF a chosen committed prefix. The
    // scheduler-local engine is rebuilt deterministically from this history on each
    // run() (run_query replays it), so prime() just records the history + tip. A
    // pure function of `history`. Returns the tip (== history.size()).
    [[nodiscard]] Seq prime(const std::vector<txn::WriteSet>& history) {
        history_ = history;
        tip_ = static_cast<Seq>(history.size());
        return tip_;
    }

    [[nodiscard]] Seq tip() const noexcept { return tip_; }

private:
    // A fixed seed for the standalone storage sim. The OBSERVABLE result must NOT
    // depend on it (we map commit prefixes onto engine snapshot Seqs), so a
    // constant keeps the read path a pure function of (history, query).
    static constexpr std::uint64_t kStorageSeed = 0x5713'5713'5713'5713ULL;

    txn::ExecutorFactory exec_factory_;

    // The standalone read-path state: the committed write-set history (replayed
    // into a scheduler-local engine on each run) + the live tip prefix.
    std::vector<txn::WriteSet> history_;
    Seq tip_ = 0;

    // Wrap a user TxnFn into a txn::Txn: the body runs the user body over a
    // TxnContext built from the executor-presented ReadView, then yields the
    // accumulated Outcome. This is the ONLY place the user body is invoked, and it
    // is invoked with ONLY a TxnContext& — so V-DET-USER holds by construction.
    [[nodiscard]] static txn::Txn to_txn(const TxnFn& fn) {
        txn::Txn t;
        t.id = fn.id;
        t.declared_reads = fn.declared;
        const std::function<void(TxnContext&)> body = fn.body;
        t.body = [body](const txn::ReadView& reads) -> txn::Txn::Outcome {
            TxnContext ctx(reads);
            if (body) {
                body(ctx);
            }
            return std::move(ctx).into_outcome();
        };
        return t;
    }

    // Build the engine from history_ deterministically, then run the planned query
    // at its resolved committed prefix and record results + served diagnostics.
    template <typename L>
    core::Task run_query(storage::Engine& engine, const Query<L>& q, Seq replica_lag,
                         Seq session_last_write, QueryResult& out) {
        // Rebuild the versioned MVCC history into THIS scheduler-local engine.
        std::vector<storage::Seq> snap{storage::kNoSeq};
        for (const txn::WriteSet& ws : history_) {
            for (const auto& [k, v] : ws) {
                (void)co_await engine.put(k, v);
            }
            const storage::Snapshot tip = co_await engine.snapshot();
            snap.push_back(tip.at);
        }
        const Seq tip_prefix = static_cast<Seq>(history_.size());

        // Resolve the level to ONE committed prefix (the planner's job).
        const Plan plan = plan_query(q, tip_prefix, replica_lag, session_last_write);
        out.level = plan.level;
        out.served_version = plan.read_prefix;

        const storage::Seq snap_seq =
            (plan.read_prefix < snap.size()) ? snap[plan.read_prefix]
                                             : snap.back();

        for (const Step& st : plan.steps) {
            if (st.kind == StepKind::Point) {
                const std::optional<Value> v =
                    co_await engine.get(st.key, storage::Snapshot{snap_seq});
                out.points.push_back(PointResult{st.key, v});

                txn::CommitInfo::ServedRead sr;
                sr.key = st.key;
                sr.level = plan.level;
                sr.served_version = plan.read_prefix;
                sr.session = session_last_write_session(q);
                sr.max_lag = level_max_lag(q);
                sr.value = v;
                out.served_reads.push_back(std::move(sr));
            } else {
                storage::Range r;
                r.lo = st.key;
                r.hi = st.hi;
                r.hi_unbounded = st.hi_unbounded;
                const std::vector<storage::KeyValue> rows =
                    co_await engine.scan(r, storage::Snapshot{snap_seq});
                RangeResult rr;
                rr.lo = st.key;
                rr.hi = st.hi;
                rr.hi_unbounded = st.hi_unbounded;
                rr.rows = rows;
                out.ranges.push_back(std::move(rr));
            }
        }
        co_return;
    }

    // The session a query's level carries (0 unless RYW). Used for the served-read
    // diagnostic so the RYW checker can attribute the read.
    template <typename L>
    [[nodiscard]] static SessionId session_last_write_session(const Query<L>& q) {
        if constexpr (std::is_same_v<L, RYW>) {
            return q.tag().session;
        } else {
            (void)q;
            return 0;
        }
    }

    // The max_lag a query's level carries (0 unless Bounded). Used for the served-
    // read diagnostic so the BoundedStaleness checker can judge the lag.
    template <typename L>
    [[nodiscard]] static Seq level_max_lag(const Query<L>& q) {
        if constexpr (std::is_same_v<L, Bounded>) {
            return q.tag().max_lag;
        } else {
            (void)q;
            return 0;
        }
    }
};

}  // namespace lockstep::query
