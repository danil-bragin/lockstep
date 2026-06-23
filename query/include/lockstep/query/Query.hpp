#pragma once

// Query.hpp — Phase 6 Stage F. THE TYPED, COMPOSABLE, NON-SQL READ SURFACE +
// the CALL-SITE-VISIBLE D5 consistency level (V-D5-SAFE) + a simple PLANNER.
//
// Source of truth: lockstep-phase-specs-all.md Phase 6 C6.2 (read/query language);
// master-plan D3 (a NEW non-SQL data model) / D5 (the four read-consistency
// levels, call-site-visible). It is the developer-facing read surface the wire
// protocol / drivers / CLI (Stage B) build on. It WRAPS the LANDED txn D5 Level
// types (txn/Transaction.hpp) — it does NOT reinvent them.
//
// ============================================================================
// THE MODEL (D3, NON-SQL): the user composes typed READ requests over an opaque
// byte key space — a POINT get(key) or a half-open RANGE scan([lo, hi)) — never a
// SQL string. A query is a small value (a vector of read steps); a PLANNER maps it
// to versioned storage::Engine MVCC reads at ONE chosen snapshot. There is no
// query parser, no expression engine: the composition IS the plan.
//
// ============================================================================
// THE D5 LEVEL — CALL-SITE-VISIBLE / TYPE-ENCODED (C6.2 / D5 / V-D5-SAFE).
// The consistency level is a TEMPLATE PARAMETER on the query builder, so it is
// part of the TYPE and is impossible to omit or silently misuse: a reader writes
//   Query<Strict>()        — default: linearizable, the committed prefix
//   Query<Snapshot>(ver)   — consistent as-of ONE committed version (no torn read)
//   Query<Bounded>(maxlag) — a local-replica read within maxlag of the tip
//   Query<RYW>(session)    — observe my session's own prior committed writes
// The bare `Query()` is Strict by construction (the strong default), so a
// forgotten level is the STRONGEST contract, never an accidental stale read. The
// level rides on every read step (reusing txn::Read), so the SAME per-D5-level
// checkers (txn/Checkers.hpp) judge a query read exactly as they judge a txn read.
//
// ============================================================================
// FORBIDDEN (query/ is NOT lint-exempt): wall-clock, threads, std::*_distribution,
// unordered iteration affecting output, any nondeterminism. The surface exposes
// only deterministic, value-shaped read requests; the planner is a pure function
// of (query, snapshot). No pointer into a growable container across a co_await
// (V-RKV1): the engine reads are consumed into values step by step.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::query {

// Reuse the LANDED opaque byte types + the D5 Level + the call-site read shape.
// We do NOT redefine them — a query read IS a txn::Read, so the txn D5 checkers
// judge it identically.
using Key = txn::Key;
using Value = txn::Value;
using Seq = txn::Seq;
using SessionId = txn::SessionId;
using Level = txn::Level;
inline constexpr Seq kNoSeq = txn::kNoSeq;

// ----------------------------------------------------------------------------
// THE LEVEL TAG TYPES — the call-site-visible D5 selector, encoded as TYPES so a
// query's consistency level is part of its C++ type. Each tag carries the level's
// parameters by value; the static `level` pins which txn::Level it maps to.
// ----------------------------------------------------------------------------

// Strict (default): linearizable — every read sees the committed prefix strictly
// before this query's serialization point. Carries no parameters.
struct Strict {
    static constexpr Level level = Level::StrictSerializable;
};

// Snapshot: consistent as-of ONE committed version (no torn read). `version ==
// kNoSeq` means "this query's own serialization snapshot" (the strict default
// snapshot). NO real-time guarantee.
struct Snapshot {
    static constexpr Level level = Level::Snapshot;
    Seq version = kNoSeq;
};

// BoundedStaleness: a local-replica read at most `max_lag` committed entries
// behind the live tip.
struct Bounded {
    static constexpr Level level = Level::BoundedStaleness;
    Seq max_lag = 0;
};

// ReadYourWrites: within session `session`, observe that session's own prior
// committed writes.
struct RYW {
    static constexpr Level level = Level::ReadYourWrites;
    SessionId session = 0;
};

// ----------------------------------------------------------------------------
// A query is composed of typed READ STEPS: a POINT get or a half-open RANGE scan.
// Each step records what it asks for; the level is carried on the owning Query.
// ----------------------------------------------------------------------------
enum class StepKind : std::uint8_t { Point = 0, Range = 1 };

struct Step {
    StepKind kind = StepKind::Point;
    Key key;       // Point: the key. Range: the inclusive lower bound `lo`.
    Key hi;        // Range: the exclusive upper bound (ignored if hi_unbounded).
    bool hi_unbounded = false;  // Range: when true, scan to the end.
};

// ----------------------------------------------------------------------------
// THE TYPED, COMPOSABLE QUERY BUILDER. The level tag L is part of the type, so
// the consistency contract is CALL-SITE-VISIBLE and cannot be silently omitted.
// `.get(k)` and `.scan(lo, hi)` compose more read steps; the builder is a small
// value (V-RKV1: no parked pointer, just an owned step vector).
// ----------------------------------------------------------------------------
template <typename L = Strict>
class Query {
public:
    Query() = default;
    explicit Query(L tag) : tag_(tag) {}

    // Compose a POINT read of `key`.
    Query& get(Key key) {
        steps_.push_back(Step{StepKind::Point, std::move(key), Key{}, false});
        return *this;
    }

    // Compose a half-open RANGE scan [lo, hi). Keys k with lo <= k < hi.
    Query& scan(Key lo, Key hi) {
        steps_.push_back(Step{StepKind::Range, std::move(lo), std::move(hi), false});
        return *this;
    }

    // Compose a RANGE scan [lo, +inf): every key with lo <= k.
    Query& scan_from(Key lo) {
        steps_.push_back(Step{StepKind::Range, std::move(lo), Key{}, true});
        return *this;
    }

    [[nodiscard]] const std::vector<Step>& steps() const noexcept { return steps_; }
    [[nodiscard]] const L& tag() const noexcept { return tag_; }
    [[nodiscard]] static constexpr Level level() noexcept { return L::level; }

private:
    L tag_{};
    std::vector<Step> steps_;
};

// Convenience constructors so a call site names the level with its parameters and
// the TYPE follows. These are the call-site-visible entry points the spec wants.
[[nodiscard]] inline Query<Strict> strict_query() { return Query<Strict>{}; }
[[nodiscard]] inline Query<Snapshot> snapshot_query(Seq version = kNoSeq) {
    return Query<Snapshot>{Snapshot{version}};
}
[[nodiscard]] inline Query<Bounded> bounded_query(Seq max_lag) {
    return Query<Bounded>{Bounded{max_lag}};
}
[[nodiscard]] inline Query<RYW> ryw_query(SessionId session) {
    return Query<RYW>{RYW{session}};
}

// ----------------------------------------------------------------------------
// A QUERY RESULT — the value(s) each step observed, plus per-read level
// diagnostics (the served prefix) so the SAME txn D5 checkers can judge a query
// read against its contract.
// ----------------------------------------------------------------------------
using ReadResult = txn::ReadResult;        // std::optional<Value>
using KeyValue = std::pair<Key, Value>;    // one scan row, key-ascending

struct PointResult {
    Key key;
    ReadResult value;
};

struct RangeResult {
    Key lo;
    Key hi;
    bool hi_unbounded = false;
    std::vector<KeyValue> rows;  // live values in [lo, hi), key-ascending
};

struct QueryResult {
    Level level = Level::StrictSerializable;
    Seq served_version = kNoSeq;  // the committed prefix the query read as-of
    std::vector<PointResult> points;
    std::vector<RangeResult> ranges;

    // The per-read level diagnostics for every POINT read in the query, in the
    // SAME shape as txn::CommitInfo::ServedRead, so txn/Checkers.hpp judges them
    // unchanged (V-CONFORM: relaxed levels checked by their own D5 checker).
    std::vector<txn::CommitInfo::ServedRead> served_reads;
};

// ----------------------------------------------------------------------------
// THE PLANNER. Maps a typed Query<L> to ONE versioned storage snapshot to read
// as-of (the D5 contract resolved to a concrete committed prefix) + the per-step
// engine reads. It is a PURE function of (query, the resolved level parameters,
// the live tip). The execution against storage::Engine lives in Database.hpp; the
// planner here decides WHICH committed prefix each level reads from — the single
// place the D5 semantics are realized for the read path, mirroring the executor's
// D5 read path exactly so a planned read and a txn read agree.
// ----------------------------------------------------------------------------

// The resolved plan: the level + the single committed prefix to read as-of + the
// ordered read steps. `serial_prefix` is the query's own serialization point (the
// live committed tip at issue time); `read_prefix` is what the level resolves to.
struct Plan {
    Level level = Level::StrictSerializable;
    Seq serial_prefix = kNoSeq;  // committed tip at issue time (own point)
    Seq read_prefix = kNoSeq;    // the committed prefix the level reads as-of
    std::vector<Step> steps;
};

// Resolve a level tag + the live tip to the committed prefix the read serves from.
// This is the SAME resolution the DeterministicExecutor uses on its read path:
//   Strict   -> the own serialization prefix (the committed tip).
//   Snapshot -> an explicit version (clamped to the tip) or the own prefix.
//   Bounded  -> max(tip - replica_lag, 0), honoring max_lag when replica_lag<=lag.
//   RYW      -> at least the session's last own write (>= the own prefix here,
//               since a query is read-only and the strongest prefix trivially
//               includes the session's own committed writes).
[[nodiscard]] inline Seq resolve_prefix(const Strict&, Seq tip, Seq /*replica_lag*/,
                                        Seq /*session_last_write*/) {
    return tip;
}
[[nodiscard]] inline Seq resolve_prefix(const Snapshot& s, Seq tip, Seq /*replica_lag*/,
                                        Seq /*session_last_write*/) {
    if (s.version != kNoSeq && s.version <= tip) {
        return s.version;
    }
    return tip;
}
[[nodiscard]] inline Seq resolve_prefix(const Bounded& b, Seq tip, Seq replica_lag,
                                        Seq /*session_last_write*/) {
    // The local replica lags the tip by `replica_lag` entries; clamp the lag the
    // replica actually exhibits to the contract's max_lag (the planner never
    // serves staler than the contract permits).
    const Seq lag = (replica_lag <= b.max_lag) ? replica_lag : b.max_lag;
    return (tip > lag) ? (tip - lag) : 0;
}
[[nodiscard]] inline Seq resolve_prefix(const RYW& /*r*/, Seq tip, Seq /*replica_lag*/,
                                        Seq session_last_write) {
    return (tip > session_last_write) ? tip : session_last_write;
}

// Build the plan for a typed query at the live tip. `replica_lag` is the
// BoundedStaleness replica lag for this read; `session_last_write` is the highest
// committed prefix the RYW session has written (0 if none / not RYW).
template <typename L>
[[nodiscard]] Plan plan_query(const Query<L>& q, Seq tip, Seq replica_lag = 0,
                              Seq session_last_write = 0) {
    Plan p;
    p.level = L::level;
    p.serial_prefix = tip;
    p.read_prefix = resolve_prefix(q.tag(), tip, replica_lag, session_last_write);
    p.steps = q.steps();
    return p;
}

}  // namespace lockstep::query
