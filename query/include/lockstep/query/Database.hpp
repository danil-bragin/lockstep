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
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
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
// PersistentStore — the DURABLE committed query state (Phase 7 S5a flag closure).
//
// THE GAP it closes: previously the query-visible committed state lived ONLY in
// memory (a vector<WriteSet> history replayed into an EPHEMERAL WalEngine per
// query) — a server restart lost it (only the consensus log was durable). This
// makes the committed query state DURABLE on an injected core::IDisk, REUSING the
// already-verified storage::WalEngine (WAL append + crash recovery): a committed
// write-set is applied to ONE persistent engine ONCE (WAL'd + synced); queries
// read the live engine (no per-query rebuild); on restart, reopen the SAME IDisk
// -> WalEngine::recover() rebuilds the store -> the committed query state is back
// WITHOUT replaying the consensus log.
//
// PREFIX->SEQ FIDELITY (the D5 load-bearing piece): a query at level L resolves to
// ONE committed PREFIX p in [0, tip] (Query.hpp::resolve_prefix); the read must be
// AS-OF that prefix's engine Snapshot Seq (e.g. a Snapshot read as-of version 1
// returns the value before any later transfer). The engine assigns one monotonic
// Seq per individual key-write and retains ALL MVCC versions (no GC here), so we
// keep snap_[p] = the engine Seq after applying the first p committed write-sets.
// snap_[0] = kNoSeq (empty prefix). This is EXACTLY the per-query snap[] the old
// ephemeral path built — now maintained INCREMENTALLY over the persistent engine,
// so query results are byte-identical to the re-execution model (the conformance
// gate is the proof).
//
// DETERMINISM: the engine runs on a Scheduler + a fixed-seed SimDisk-or-injected
// IDisk; the OBSERVABLE result maps committed prefixes onto engine snapshot Seqs
// and never depends on disk latency/seed (a constant keeps the read path a pure
// function of (applied history, query)). No pointer into a growable container
// across a co_await (V-RKV1): each engine read is consumed into a value before the
// next.
//
// SCHEDULER IDENTITY (the seam constraint): the engine + its IDisk must run on the
// SAME Scheduler — every IDisk (SimDisk, ProdDisk) is constructed over a Scheduler
// and drives its async completions on it. So the INJECTED backing borrows the
// caller's Scheduler (the one the disk was built over); only the DEFAULT in-memory
// backing owns its own Scheduler + SimDisk.
// ----------------------------------------------------------------------------
class PersistentStore {
public:
    // Default backing: an internally-owned Scheduler + fault-free SimDisk (the
    // in-memory backing existing callers + tests get when they inject nothing). The
    // committed state is durable across queries but the backing is volatile (matches
    // the pre-seam behaviour for default callers).
    PersistentStore()
        : owned_sched_(std::make_unique<core::Scheduler>()),
          sched_(owned_sched_.get()),
          owned_clock_(std::make_unique<core::SimClock>(*sched_)),
          owned_rng_(std::make_unique<sim::SeededRandom>(kStorageSeed)),
          owned_disk_(std::make_unique<sim::SimDisk>(*sched_, *owned_clock_,
                                                     *owned_rng_, fault_free_cfg())),
          disk_(owned_disk_.get()),
          owned_manifest_(std::make_unique<sim::SimDisk>(*sched_, *owned_clock_,
                                                         *owned_rng_, fault_free_cfg())),
          owned_factory_(std::make_unique<StoreDiskFactory>(*sched_, *owned_clock_,
                                                            *owned_rng_)),
          engine_(std::make_unique<storage::WalEngine>(
              *sched_, *disk_, *owned_manifest_, *owned_factory_, kFlushThreshold)) {
        // SELECTIVE-FLUSH LSM: bound the FLUSH-ELIGIBLE (row-mode / index) memtable
        // via SSTable flush + size-tiered compaction, while the SQL columnar
        // namespaces stay resident (never flushed; the columnar engine manages them).
        // This removes the std::map memtable's point-get cost on row tables without
        // the columnar/LSM flush churn. The injected backing stays WAL+memtable-only.
        engine_->set_compaction_trigger(kCompactionTrigger);
        engine_->set_keep_resident_prefixes(kColumnarResident);
    }

    // Injected backing: the committed query state is a durable WalEngine over the
    // caller's IDisk (a ProdDisk for real on-disk recovery, or a SimDisk for a
    // deterministic crash/recovery test), driven on the caller's Scheduler (the one
    // the disk runs on). The caller owns both lifetimes; they must outlive this
    // store. recover() rebuilds the store from the disk image.
    PersistentStore(core::Scheduler& sched, core::IDisk& disk)
        : sched_(&sched),
          disk_(&disk),
          engine_(std::make_unique<storage::WalEngine>(*sched_, *disk_)) {}

    PersistentStore(const PersistentStore&) = delete;
    PersistentStore& operator=(const PersistentStore&) = delete;

    // Apply ONE committed write-set (the p-th commit) to the persistent engine:
    // WAL-append every key-write, sync (the durability barrier), and record the new
    // prefix->Seq boundary. Applied EXACTLY ONCE per committed write-set (the caller
    // dedups). Advances the live tip by one.
    void apply_committed(const txn::WriteSet& ws) {
        sched_->spawn(apply_task(ws));
        sched_->run();
    }

    // GROUP-COMMIT seam: apply a committed write-set to the memtable + advance the prefix
    // boundary but DEFER the fsync (no durability barrier here). The caller MUST call
    // sync_durable() before acking any of the batched writes — until then the writes are buffered,
    // NOT durable. Lets the wire server amortize ONE fsync over many writes (drain N ready frames
    // -> apply each nosync -> sync_durable() once -> ack all N). acked==durable is preserved
    // because the ack is withheld until the shared sync completes.
    void apply_committed_nosync(const txn::WriteSet& ws) {
        sched_->spawn(apply_task_nosync(ws));
        sched_->run();
    }

    // Durability barrier for all writes applied via apply_committed_nosync() since the last
    // sync: fsync the WAL once. After this returns, every buffered write is durable.
    void sync_durable() {
        sched_->spawn(sync_task());
        sched_->run();
    }

    // The live committed tip (number of committed write-sets applied).
    [[nodiscard]] Seq tip() const noexcept { return static_cast<Seq>(snap_.size()) - 1; }

    // The engine Snapshot Seq for a committed prefix p (clamped to the live tip).
    [[nodiscard]] storage::Seq snap_for(Seq prefix) const {
        const std::size_t idx = (prefix < snap_.size())
                                    ? static_cast<std::size_t>(prefix)
                                    : (snap_.size() - 1);
        return snap_[idx];
    }

    // Read access to the live persistent engine (queries drive reads against it).
    [[nodiscard]] storage::Engine& engine() noexcept { return *engine_; }
    [[nodiscard]] core::Scheduler& scheduler() noexcept { return *sched_; }

    // True iff this store owns its (default in-memory) backing — i.e. NO IDisk was
    // injected. prime() uses this to decide whether a rebuild starts a fresh default
    // backing or reopens the SAME injected disk.
    [[nodiscard]] bool owns_default_backing() const noexcept {
        return owned_disk_ != nullptr;
    }

    // RECOVER the committed query state from the durable IDisk image (a restart).
    // Reopens the engine over the SAME disk and replays the durable WAL prefix into
    // a fresh memtable (WalEngine::recover — the verified crash-recovery path), then
    // rebuilds the live tip from the recovered engine. After this, a live-tip query
    // returns every committed value WITHOUT replaying the consensus log.
    //
    // `durable_len` is the durable WAL byte length (a SimDisk reports it via
    // durable_len(); a ProdDisk would use the on-disk file size). PREFIX BOUNDARIES
    // are not separately persisted, so after recovery snap_ models a single prefix
    // step [empty, recovered-tip]: live-tip reads (the recovery guarantee) are exact;
    // an older-prefix Snapshot read post-recovery clamps to the tip (re-priming via
    // apply_committed restores full per-prefix fidelity for a live session).
    void recover(std::size_t durable_len) {
        engine_ = std::make_unique<storage::WalEngine>(*sched_, *disk_);
        sched_->spawn(recover_task(durable_len));
        sched_->run();
        const storage::Seq last = engine_->last_seq();
        snap_.assign(1, storage::kNoSeq);  // prefix 0 (empty)
        if (last != storage::kNoSeq) {
            snap_.push_back(last);  // a single recovered prefix at the live tip
        }
    }

private:
    static sim::DiskFaultConfig fault_free_cfg() {
        sim::DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 0;
        return dc;
    }

    // SSTable disk factory for the DEFAULT in-memory backing's LSM: mints one
    // fault-free SimDisk per sstable_id (and the vlog ids), all on the store's owned
    // Scheduler/clock/rng so the whole engine stays single-threaded + deterministic.
    // Disks are kept alive for the run (a manifest-referenced SSTable's backing must
    // survive). Only the default backing uses this; the injected backing is
    // WAL+memtable-only (no factory).
    class StoreDiskFactory final : public storage::IDiskFactory {
    public:
        StoreDiskFactory(core::Scheduler& sched, core::SimClock& clock,
                         sim::SeededRandom& rng) noexcept
            : sched_(&sched), clock_(&clock), rng_(&rng) {}
        storage::IDisk& disk_for(std::uint64_t id) override {
            std::unique_ptr<sim::SimDisk>& slot = disks_[id];
            if (!slot) {
                slot = std::make_unique<sim::SimDisk>(*sched_, *clock_, *rng_,
                                                      fault_free_cfg());
            }
            return *slot;
        }

    private:
        core::Scheduler* sched_;
        core::SimClock* clock_;
        sim::SeededRandom* rng_;
        std::map<std::uint64_t, std::unique_ptr<sim::SimDisk>> disks_;
    };

    // Default-backing LSM tuning. flush_threshold bounds the FLUSH-ELIGIBLE (row-mode
    // / index) memtable; columnar namespaces stay resident (see kColumnarResident).
    static constexpr std::size_t kFlushThreshold = 50'000;
    static constexpr std::size_t kCompactionTrigger = 4;
    // Leading bytes of the SQL columnar block/overlay/delta namespaces (Catalog.hpp:
    // 'B' blocks, 'M' overlay manifest, 'R' overlay runs, 'T' overlay tombstones,
    // 'Z' zone map, 'd' row delta, 'c' reserved). The columnar engine bulk-manages
    // these; LSM-flushing them only churns, so they are kept resident.
    static constexpr std::string_view kColumnarResident = "BMRTZdc";

    core::Task apply_task(const txn::WriteSet& ws) {
        for (const auto& [k, v] : ws) {
            if (v == delete_sentinel()) {
                (void)co_await engine_->del(k);  // real storage tombstone (scan-dropped + GC'd)
            } else {
                (void)co_await engine_->put(k, v);
            }
        }
        // Durability barrier: every key-write of this committed write-set is durable
        // on the injected IDisk before the prefix boundary is recorded (so recovery
        // sees a clean committed prefix, never a torn half-applied write-set).
        (void)co_await engine_->sync();
        const storage::Snapshot tip = co_await engine_->snapshot();
        snap_.push_back(tip.at);
        co_return;
    }

    // As apply_task but WITHOUT the fsync — the WAL bytes are appended (buffered) and the prefix
    // boundary advances, but durability waits for a later sync_task(). The buffered writes are not
    // acked until then, so a crash before sync loses only un-acked writes (acked==durable holds).
    core::Task apply_task_nosync(const txn::WriteSet& ws) {
        for (const auto& [k, v] : ws) {
            if (v == delete_sentinel()) {
                (void)co_await engine_->del(k);
            } else {
                (void)co_await engine_->put(k, v);
            }
        }
        const storage::Snapshot tip = co_await engine_->snapshot();
        snap_.push_back(tip.at);
        co_return;
    }

    // The deferred durability barrier: fsync the WAL once for all nosync-applied writes.
    core::Task sync_task() {
        (void)co_await engine_->sync();
        co_return;
    }

    core::Task recover_task(std::size_t durable_len) {
        (void)co_await engine_->recover(durable_len);
        co_return;
    }

    // A fixed seed for the standalone storage sim. The OBSERVABLE result must NOT
    // depend on it (commit prefixes map onto engine snapshot Seqs). Declared first
    // so the default ctor's member inits can read it.
    static constexpr std::uint64_t kStorageSeed = 0x5713'5713'5713'5713ULL;

    // Owned ONLY for the default in-memory backing (null when an IDisk is injected,
    // in which case the caller's Scheduler/disk are borrowed via the pointers).
    std::unique_ptr<core::Scheduler> owned_sched_;
    core::Scheduler* sched_;
    std::unique_ptr<core::SimClock> owned_clock_;
    std::unique_ptr<sim::SeededRandom> owned_rng_;
    std::unique_ptr<sim::SimDisk> owned_disk_;
    core::IDisk* disk_;
    // Default-backing LSM only (null for the injected backing). Declared BEFORE
    // engine_ so member-init order constructs the manifest disk + SSTable factory
    // before the engine that references them.
    std::unique_ptr<sim::SimDisk> owned_manifest_;
    std::unique_ptr<StoreDiskFactory> owned_factory_;
    std::unique_ptr<storage::WalEngine> engine_;

    // prefix p -> engine Snapshot Seq after applying the first p write-sets.
    // snap_[0] = kNoSeq (empty prefix); snap_.size()-1 == live tip.
    std::vector<storage::Seq> snap_{storage::kNoSeq};
};

// ----------------------------------------------------------------------------
// THE DATABASE / CLIENT. The single developer-facing object. It owns the txn
// Executor (the LANDED deterministic_factory by default) and a storage::Engine
// for the standalone read/query path. ONE-SHOT only: submit(batch) executes a
// whole batch of TxnFns; run(Query) executes a typed read at its D5 level.
// ----------------------------------------------------------------------------
class Database {
public:
    // Default: the real verified deterministic executor (deterministic_factory) and
    // a default in-memory persistent store (an internally-owned fault-free SimDisk).
    // Existing callers that inject no disk get this — behaviour is byte-identical to
    // the old per-query-rebuild path, but the committed state now lives in ONE
    // engine maintained incrementally instead of being rebuilt per query.
    Database()
        : exec_factory_(txn::deterministic_factory()),
          store_(std::make_unique<PersistentStore>()) {}
    explicit Database(txn::ExecutorFactory factory)
        : exec_factory_(std::move(factory)),
          store_(std::make_unique<PersistentStore>()) {}

    // INJECTION SEAM (Phase 7 S5a closure): back the committed query state with a
    // durable WalEngine over the caller's IDisk (ProdDisk for real recovery; SimDisk
    // for a deterministic crash test), driven on the caller's Scheduler (the one the
    // disk runs on — engine + disk MUST share a scheduler). The committed state
    // survives + recovers on a restart over the SAME disk. The caller owns the
    // scheduler + disk; both must outlive this DB.
    Database(core::Scheduler& sched, core::IDisk& disk)
        : exec_factory_(txn::deterministic_factory()),
          store_(std::make_unique<PersistentStore>(sched, disk)) {}
    Database(txn::ExecutorFactory factory, core::Scheduler& sched, core::IDisk& disk)
        : exec_factory_(std::move(factory)),
          store_(std::make_unique<PersistentStore>(sched, disk)) {}

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
        // Read the LIVE persistent engine directly (no per-query rebuild). The store
        // already holds the committed MVCC history (applied incrementally + WAL'd),
        // and snap_for(prefix) gives the engine Snapshot Seq for any committed prefix
        // — so a D5 read AS-OF an older prefix is exact (V-D5-SAFE preserved).
        store_->scheduler().spawn(
            run_query(store_->engine(), q, replica_lag, session_last_write, out));
        store_->scheduler().run();
        return out;
    }

    // INCREMENTAL durable apply (the new write path): apply ONE committed write-set
    // to the persistent engine (WAL'd + synced) ONCE and advance the live tip. The
    // caller (wire::Server) dedups, so each committed write-set lands exactly once.
    // Returns the new live tip. This REPLACES the "re-run the whole batch + re-prime
    // the full history per submit" model with an incrementally-maintained durable
    // store — query results are identical (the conformance gate proves it).
    Seq apply_committed(const txn::WriteSet& ws) {
        store_->apply_committed(ws);
        return store_->tip();
    }

    // GROUP-COMMIT: apply a committed write-set but DEFER the fsync (see PersistentStore). The
    // caller must sync() before acking. Returns the new (durably-pending) tip.
    Seq apply_committed_nosync(const txn::WriteSet& ws) {
        store_->apply_committed_nosync(ws);
        return store_->tip();
    }

    // Flush the deferred fsync once — after this, all nosync-applied writes are durable.
    void sync() { store_->sync_durable(); }

    // Prime the standalone read-path with a committed write-set HISTORY IN ORDER:
    // `history[p-1]` is the write-set committed by the p-th commit. Rebuilds the
    // persistent store from scratch (a fresh engine over the SAME backing) and
    // applies the history incrementally — so the prefix->Seq mapping the D5 read
    // path needs is exact. A pure function of `history`. Returns the tip.
    //
    // Kept for the standalone Database surface + the round-trip oracle. The wire
    // Server now drives the incremental apply_committed() path instead (one apply
    // per new commit) rather than re-priming the whole history each submit.
    [[nodiscard]] Seq prime(const std::vector<txn::WriteSet>& history) {
        if (store_->owns_default_backing()) {
            // Default in-memory backing: rebuild from scratch over a fresh disk and
            // apply the whole history (the standalone surface's "history is exactly
            // this" semantics). Pure function of `history`.
            store_ = std::make_unique<PersistentStore>();
            for (const txn::WriteSet& ws : history) {
                store_->apply_committed(ws);
            }
        } else {
            // Injected DURABLE backing: an append-structured WAL cannot be rewound,
            // so prime() applies only the TAIL beyond the current tip (history is a
            // monotonic superset of what is already durably applied — the wire
            // Server appends one commit at a time). This keeps the durable WAL a
            // single append-only committed prefix.
            const std::size_t have = static_cast<std::size_t>(store_->tip());
            for (std::size_t p = have; p < history.size(); ++p) {
                store_->apply_committed(history[p]);
            }
        }
        return store_->tip();
    }

    [[nodiscard]] Seq tip() const noexcept { return store_->tip(); }

    // Recover the committed query state from the durable IDisk after a restart.
    void recover(std::size_t durable_len) { store_->recover(durable_len); }

private:
    // A fixed seed for the standalone storage sim. The OBSERVABLE result must NOT
    // depend on it (we map commit prefixes onto engine snapshot Seqs), so a
    // constant keeps the read path a pure function of (history, query).
    static constexpr std::uint64_t kStorageSeed = 0x5713'5713'5713'5713ULL;

    txn::ExecutorFactory exec_factory_;

    // The DURABLE committed query state: ONE persistent WalEngine over the injected
    // (or default in-memory) IDisk, maintained incrementally + read live.
    std::unique_ptr<PersistentStore> store_;

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

    // Read the LIVE persistent engine at the planned committed prefix and record
    // results + served diagnostics. The engine already holds the committed MVCC
    // history (applied incrementally + WAL'd); snap_for(prefix) maps a committed
    // prefix to its engine Snapshot Seq, so an as-of-older-prefix read is exact.
    template <typename L>
    core::Task run_query(storage::Engine& engine, const Query<L>& q, Seq replica_lag,
                         Seq session_last_write, QueryResult& out) {
        const Seq tip_prefix = store_->tip();

        // Resolve the level to ONE committed prefix (the planner's job).
        const Plan plan = plan_query(q, tip_prefix, replica_lag, session_last_write);
        out.level = plan.level;
        out.served_version = plan.read_prefix;

        const storage::Seq snap_seq = store_->snap_for(plan.read_prefix);

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
                std::vector<storage::KeyValue> rows =
                    co_await engine.scan(r, storage::Snapshot{snap_seq});
                RangeResult rr;
                rr.lo = st.key;
                rr.hi = st.hi;
                rr.hi_unbounded = st.hi_unbounded;
                rr.rows = std::move(rows);  // move the whole vector, never copy it
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
