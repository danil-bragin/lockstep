#pragma once

// Transaction.hpp — Phase 5 Stage M. THE TXN SEAM.
//
// Source of truth: specs/CommitOrdering.tla (model-checked clean + human-
// approved) + briefs/phase5.md + lockstep-phase-specs-all.md C5.1-C5.6 +
// master-plan D1 (one-shot, Calvin-style, NO 2PC) / D2 (pure log-order) / D5
// (the four read-consistency levels + the "call-site-visible" contract).
//
// Authored VERIFICATION-FIRST: this seam exists BEFORE the real distributed-txn
// implementation (Stage I), so the strict-serializable ORACLE, the differential
// + linearizability HARNESS, and the per-D5-level CHECKERS can judge BOTH the
// real impl AND the deliberately-wrong teeth stubs through ONE identical surface.
// A real impl and a teeth stub plug in identically (a factory) — exactly as the
// Phase-4 ConsensusNode seam did.
//
// ============================================================================
// THE MODEL — mapped DIRECTLY onto CommitOrdering.tla.
// ============================================================================
// A transaction is ONE-SHOT (D1 / C5.1): parameters in, a DETERMINISTIC body
// over the reads, a result + writes out. It is NOT an interactive session; there
// is no client-held cursor, no read-then-think-then-write round trip. The whole
// txn is submitted, sequenced into a single global total order (the Phase-4
// consensus log == the spec's `seqLog`), then executed DETERMINISTICALLY and
// SEQUENTIALLY in that order (no concurrency at apply, no 2PC). The sequencer
// order IS the serialization order — that is what makes the default path
// strict-serializable (CommitOrdering.tla SerializedBySeqLog /
// ReadsMatchSerialPrefix).
//
//   TLA concept                         →  seam type
//   ---------------------------------     --------------------------------------
//   Txn id                              →  Txn::id (a stable client-chosen tag)
//   RSet[t] (predicted read set)        →  Txn::declared_reads  (OLLP recon)
//   WSet[t] (write set)                 →  Txn::writes          (key→value to set)
//   the deterministic command body      →  Txn::body (a pure fn of the reads)
//   history[i].reads (serial snapshot)  →  CommitInfo::reads_observed
//   seqLog position                     →  CommitInfo::seq_index (1-based)
//   status ∈ {committed,aborted,...}    →  CommitInfo::status
//   OLLP recon valid at serial point    →  CommitInfo::footprint_valid
//   retries[t] (re-sequence count)      →  CommitInfo::retries
//
// ============================================================================
// THE D5 SELECTOR — call-site-visible / type-encoded (C5.4).
// ============================================================================
// Each READ inside a txn carries its consistency level AS A VALUE on the read
// request, so a level can never be silently misused: the call site that issues a
// read names exactly which contract it wants. The default is StrictSerializable.
//   StrictSerializable  — linearizable: the read sees the committed prefix
//                         strictly before this txn's serialization point. (Real
//                         time honored; CommitOrdering.tla Reads/StoreReflects.)
//   Snapshot(version)   — consistent as-of ONE committed version: every key in
//                         the txn reads at the SAME prefix (no torn read). NO
//                         real-time guarantee. (CommitOrdering.tla D5Snapshot.)
//   BoundedStaleness(K) — a local-replica read at most K committed entries behind
//                         the live tip. (CommitOrdering.tla D5BoundedStale.)
//   ReadYourWrites(sid) — within session `sid`, a read observes that session's
//                         own prior committed writes. (CommitOrdering.tla
//                         D5ReadYourWrites.)
//
// ============================================================================
// FORBIDDEN (txn/ is NOT lint-exempt): wall-clock (std::chrono), std::thread/
// atomics, std::*_distribution, unordered iteration affecting output, any
// nondeterminism. Everything here is a PURE deterministic function of its inputs
// (ultimately of the seed). No pointer into a growable container across a
// co_await (V-RKV1) — this seam is intentionally synchronous-value-shaped so a
// real impl awaits storage/consensus internally and returns plain value results.
// ============================================================================

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lockstep::txn {

// Opaque byte strings, exactly as storage::Key/Value (D4). std::string is the
// byte container; never assumed printable.
using Key = std::string;
using Value = std::string;

// A monotonic commit sequence == the seqLog position == MVCC version (D2 log-
// order, mirrors storage::Seq). 0 == "∅ / before the first commit": no real
// commit gets 0. A read-as-of version is this number.
using Seq = std::uint64_t;
inline constexpr Seq kNoSeq = 0;

// A session id for ReadYourWrites. 0 == "no session" (a read outside any RYW
// session). Sessions are client-chosen and stable for the client's life.
using SessionId = std::uint64_t;

// ----------------------------------------------------------------------------
// D5 LEVEL — the call-site-visible consistency selector (C5.4). Type-encoded so
// the level cannot be silently misused: every read names its contract.
// ----------------------------------------------------------------------------
enum class Level : std::uint8_t {
    StrictSerializable = 0,  // default: linearizable
    Snapshot = 1,            // consistent as-of one version, no real-time
    BoundedStaleness = 2,    // within K committed entries of the tip
    ReadYourWrites = 3,      // observe own session's prior writes
};

[[nodiscard]] inline const char* level_name(Level l) noexcept {
    switch (l) {
        case Level::StrictSerializable:
            return "StrictSerializable";
        case Level::Snapshot:
            return "Snapshot";
        case Level::BoundedStaleness:
            return "BoundedStaleness";
        case Level::ReadYourWrites:
            return "ReadYourWrites";
    }
    return "?";
}

// A single READ request inside a txn, carrying its D5 contract on it. The factory
// helpers below are the call-site-visible constructors: a reader writes
//   read_strict(k)               — default, linearizable
//   read_snapshot(k, version)    — as-of a specific committed version
//   read_bounded(k, max_lag)     — within max_lag of the tip
//   read_ryw(k, session)         — observe my session's own writes
// so the level is impossible to omit by accident.
struct Read {
    Key key;
    Level level = Level::StrictSerializable;

    // Snapshot: the committed version to read as-of. kNoSeq ⇒ "this txn's own
    // serialization snapshot" (the strict default snapshot).
    Seq snapshot_version = kNoSeq;

    // BoundedStaleness: max permitted committed-entry lag behind the live tip.
    Seq max_lag = 0;

    // ReadYourWrites: the session whose own writes must be visible.
    SessionId session = 0;
};

[[nodiscard]] inline Read read_strict(Key key) {
    return Read{std::move(key), Level::StrictSerializable, kNoSeq, 0, 0};
}
[[nodiscard]] inline Read read_snapshot(Key key, Seq version) {
    return Read{std::move(key), Level::Snapshot, version, 0, 0};
}
[[nodiscard]] inline Read read_bounded(Key key, Seq max_lag) {
    return Read{std::move(key), Level::BoundedStaleness, kNoSeq, max_lag, 0};
}
[[nodiscard]] inline Read read_ryw(Key key, SessionId session) {
    return Read{std::move(key), Level::ReadYourWrites, kNoSeq, 0, session};
}

// The value a read observed: the latest version <= its snapshot, or ∅ (absent /
// tombstone). Mirrors storage get(k, snap) -> optional<Value>.
using ReadResult = std::optional<Value>;

// A read map: key -> observed value at this txn's serialization snapshot. Ordered
// (std::map) so iteration is deterministic (NO unordered_map / hash iteration).
using ReadView = std::map<Key, ReadResult>;

// A write set: key -> value to commit (a del is represented by a tombstone value;
// at this abstraction we model writes as key->value puts, matching WSet[t]).
using WriteSet = std::map<Key, Value>;

// ----------------------------------------------------------------------------
// THE ONE-SHOT TRANSACTION (D1 / C5.1). Parameters in, deterministic body over
// the reads, result + writes out. NOT an interactive session.
// ----------------------------------------------------------------------------
//
// `declared_reads` is the OLLP-predicted read footprint (RSet[t]): the keys the
// txn *predicts* it will read, computed from a cheap recon snapshot. At execution
// the actual footprint may be larger (a value-dependent access) — the executor
// detects the mismatch and RE-SEQUENCES (C5.3). `body` is the deterministic
// command: given the values it actually read, it returns its result + the writes
// to commit. It is a PURE function of its ReadView — same reads ⇒ same writes ⇒
// determinism (V-DET). The body may also surface an "actual footprint" (the keys
// it really touched) so the OLLP recon check has ground truth.
struct Txn {
    std::uint64_t id = 0;  // stable client tag; identifies the txn in history

    // OLLP-predicted read footprint (RSet[t]). The executor presents exactly
    // these keys' values to the body unless recon re-expands the footprint.
    std::vector<Read> declared_reads;

    // The deterministic command body. Input: the values read for declared_reads
    // (plus any re-sequenced extra reads). Output: the writes to commit + an
    // opaque result token (e.g. a return value the client observes). MUST be a
    // pure function of `reads` (no clock, no randomness, no hidden state).
    //
    // value-dependent footprint (the OLLP trigger): if reading some key returns a
    // non-∅ "trigger" value, the body's `extra_reads` names the keys it then ALSO
    // needs — modeling Trigger[t]/Extra[t] in CommitOrdering.tla. An honest
    // executor re-sequences when extra_reads is non-empty and the txn had not
    // already been given those keys (recon mismatch).
    struct Outcome {
        WriteSet writes;
        std::string result;            // observable result token
        std::vector<Key> extra_reads;  // value-dependent footprint expansion
    };
    std::function<Outcome(const ReadView& reads)> body;
};

// ----------------------------------------------------------------------------
// OBSERVABLE COMMIT INFO — what the executor reports per submitted txn. Maps onto
// CommitOrdering.tla's history record + status + retries so the checkers can
// assert the spec invariants on a running executor.
// ----------------------------------------------------------------------------
enum class Status : std::uint8_t {
    Committed = 0,  // applied exactly once, in seqLog order
    Aborted = 1,    // recon mismatch persisted past MaxRetry (C5.6 terminal)
    Pending = 2,    // submitted, not yet decided (transient; never a final result)
};

[[nodiscard]] inline const char* status_name(Status s) noexcept {
    switch (s) {
        case Status::Committed:
            return "Committed";
        case Status::Aborted:
            return "Aborted";
        case Status::Pending:
            return "Pending";
    }
    return "?";
}

struct CommitInfo {
    std::uint64_t txn_id = 0;
    Status status = Status::Pending;

    // 1-based position in the global serialization order (seqLog). 0 for a txn
    // that never committed. CommitOrdering.tla SerializedBySeqLog pins that
    // committed txns appear in exactly this order.
    Seq seq_index = 0;

    // The commit version assigned (the MVCC Seq of this txn's writes). 0 if not
    // committed. Strictly increases with seq_index for committed txns.
    Seq commit_version = kNoSeq;

    // The values this txn actually OBSERVED at its serialization point, per read.
    // This is history[i].reads in the spec — recorded so the read-consistency
    // checkers can compare against the serial prefix, snapshot-stable under later
    // writes (OLLPSound routes through this, not the live store).
    ReadView reads_observed;

    // The writes this txn committed (its effects). Empty for an aborted txn.
    WriteSet writes_committed;

    // The observable result token the body produced.
    std::string result;

    // OLLP: was the recon footprint valid AT THIS TXN'S serialization point?
    // (FootprintValid in the spec.) True for an honest commit; a checker flags a
    // committed txn with footprint_valid=false (committed on a stale footprint).
    bool footprint_valid = true;

    // OLLP re-sequence count (retries[t]). Must stay <= max_retry (C5.6 bound).
    Seq retries = 0;

    // Per-read level diagnostics: the level each read was served at + the prefix
    // (committed version) it was actually served from. The D5 checkers compare
    // these served prefixes against the contract each level promises.
    struct ServedRead {
        Key key;
        Level level = Level::StrictSerializable;
        Seq served_version = kNoSeq;  // the prefix actually read (served prefix p)
        SessionId session = 0;
        Seq max_lag = 0;
        ReadResult value;
    };
    std::vector<ServedRead> served_reads;
};

// The full result of running a submitted batch: the per-txn commit info IN
// SEQLOG ORDER (the committed txns in serialization order, with aborts inter
// leaved at their re-sequence-exhaustion point), plus the live tip version.
struct RunResult {
    std::vector<CommitInfo> commits;  // in seqLog (serialization) order
    Seq tip_version = kNoSeq;         // the live committed tip after the batch
};

// ----------------------------------------------------------------------------
// THE EXECUTOR INTERFACE — submits a batch of one-shot txns and returns their
// results + observable commit info. The strict-serializable ORACLE, the real
// Stage-I impl, AND every teeth stub implement THIS. Swapping the factory swaps
// the whole executor behind the identical harness + checkers.
// ----------------------------------------------------------------------------
//
// `submit_batch` takes the txns ALREADY in their agreed global order (the seqLog
// — produced by the Phase-4 consensus layer; here the harness supplies a seeded
// order to stand in for it) and a config (MaxRetry, the BoundedStaleness lag a
// local replica exhibits, etc.). It executes them deterministically and returns
// the run result. It is intentionally synchronous-value-shaped: a real impl
// awaits consensus + storage internally; the seam exposes only plain values so a
// checker is a pure function of the result and there is no span/pointer parked
// across an await (V-RKV1).
struct ExecConfig {
    Seq max_retry = 2;  // OLLP re-sequence bound (CommitOrdering.tla MaxRetry).

    // The committed-entry lag a BoundedStaleness local-replica read exhibits in
    // this run (the replica is this many entries behind the tip). The honest
    // executor must keep served_version >= tip - replica_lag for such reads, and
    // a checker asserts the lag never exceeds the read's stated max_lag.
    Seq replica_lag = 0;
};

class Executor {
public:
    virtual ~Executor() = default;

    // Execute `ordered_txns` (already in seqLog order) deterministically and
    // return the per-txn observable results. Pure function of (ordered_txns, cfg).
    [[nodiscard]] virtual RunResult submit_batch(const std::vector<Txn>& ordered_txns,
                                                 const ExecConfig& cfg) = 0;

    // Short identifier for dashboards / witnesses.
    [[nodiscard]] virtual std::string name() const = 0;
};

// THE FACTORY. Returns ONE executor. The harness swaps impl ↔ oracle ↔ teeth-stub
// by swapping the factory — the workload, the seqLog order, and the checkers are
// untouched. THIS SHAPE IS BINDING: the Stage-I impl is constructed through it.
using ExecutorFactory = std::function<std::unique_ptr<Executor>()>;

}  // namespace lockstep::txn
