#pragma once

// Engine.hpp — Phase 3 §1. The storage-engine INTERFACE every real engine
// implements (the seam) and the differential harness + oracle code against.
//
// This is THE frozen boundary for Phase 3 (storage-engine.md §1): a single-node,
// crash-consistent, MVCC key-value engine running ENTIRELY inside the sim. All
// methods are ASYNC (return a core::Future) and run on the deterministic
// Scheduler; a real engine drives its IO through the sim IDisk. The oracle (a
// trivial in-memory reference) and the differential harness both speak this
// interface so a system-under-test and the oracle are swappable behind the SAME
// driver — that pluggability is the whole point of verification-machinery-first.
//
// VERSION MODEL (DERIVED from master-plan D2 log-order + D4, not a new decision):
//   * Key / Value are OPAQUE byte strings (std::string is the byte container).
//   * Seq is a monotonic uint64 commit sequence == the MVCC version. Seq 0 is
//     the reserved "∅ / before-first-commit" sentinel: no real commit gets 0.
//   * Each committed mutation (put OR del) is assigned the NEXT strictly-greater
//     Seq (V-MONO: ⊥ reuse, ⊥ gap that hides a lost commit).
//   * A del writes a TOMBSTONE at its commit Seq (a versioned "absent" marker),
//     NOT a physical erase — older versions remain visible to older snapshots.
//   * get(k, Snapshot{at}) returns the value of the NEWEST version of k whose
//     seq <= at; ∅ (std::nullopt) if there is no such version OR that newest
//     version ≤ at is a tombstone. This is a PURE function of (k, at) (V-SNAP):
//     never observes a partial/in-flight mutation, never a version > at.
//   * snapshot() returns the current committed version (the latest assigned Seq,
//     or 0 if nothing committed). Reading as-of that Seq is consistent.
//   * sync() is the durability barrier (V-DUR): after it returns ok, the
//     committed mutations survive any subsequent crash; before sync, a crash MAY
//     lose a suffix (V-PREFIX) — never a gap, never a fabricated value.
//
// FORBIDDEN (storage/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output, any nondeterminism.
// All time is core::Tick (virtual); all randomness is the injected core::IRandom.
//
// PHASE-2 LESSON CARRIED FORWARD (V-RKV1): a real engine must NOT hold a pointer
// or reference into a growable container (vector/map node) across a co_await —
// the await can resume after the container grew/reallocated. Re-fetch versions
// AFTER any await. The interface here is await-bearing precisely so implementers
// keep that in mind.

#include <cstdint>
#include <functional>  // scan_visit's per-entry callback
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>

namespace lockstep::storage {

using core::Error;
using core::Future;

// Opaque byte strings (D4): a Key/Value may hold arbitrary bytes including '\0'.
// std::string is just the byte container; we never assume printable/text.
using Key = std::string;
using Value = std::string;

// Monotonic commit sequence == MVCC version (D2 log-order). 0 == ∅ / before the
// first commit; a real commit always gets a strictly-greater, never-reused Seq.
using Seq = std::uint64_t;

// The reserved sentinel: "no version / before first commit". A get against a
// snapshot at kNoSeq always returns ∅.
inline constexpr Seq kNoSeq = 0;

// A read-as-of-version handle. `at` is the MVCC version the read observes: a
// get(k, snap) sees the newest version of k with seq <= snap.at. Obtained from
// Engine::snapshot() (or constructed directly for a point-in-time read).
struct Snapshot {
    Seq at = kNoSeq;
};

// A half-open key range [lo, hi) for scan(): keys k with lo <= k < hi. An empty
// `hi` (the default) means "unbounded above" — every key with lo <= k.
struct Range {
    Key lo;       // inclusive lower bound (empty == from the smallest key)
    Key hi;       // exclusive upper bound (empty == unbounded above)
    bool hi_unbounded = true;  // when true, `hi` is ignored (scan to the end)
};

// One (Key,Value) pair returned by scan(), in key-ascending order. Tombstoned /
// not-visible keys are omitted (a scan returns only LIVE values at the snapshot).
using KeyValue = std::pair<Key, Value>;

// Engine — the abstract single-node MVCC KV store. All methods async on the
// scheduler. A real engine (WAL+memtable+SSTable+compaction+WiscKey) and the
// trivial Oracle both implement this; the differential harness drives both.
class Engine {
public:
    Engine() = default;
    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Commit a key→value mutation. Returns the commit Seq assigned (strictly
    // greater than every previously-assigned Seq; V-MONO). Durable only after a
    // subsequent successful sync() (V-DUR); before that, a crash MAY lose it.
    [[nodiscard]] virtual Future<Seq> put(Key key, Value value) = 0;

    // Commit a tombstone for key (a versioned delete). Returns the commit Seq.
    // After this, a get at any snapshot whose newest version of key is this
    // tombstone returns ∅; older snapshots still see the pre-delete value.
    [[nodiscard]] virtual Future<Seq> del(Key key) = 0;

    // MVCC read as-of snap.at: the value of the newest version of key with
    // seq <= snap.at, or ∅ if none exists / that newest version is a tombstone.
    // Pure function of (key, snap.at) (V-SNAP): never a version > snap.at, never
    // a partial/torn value.
    [[nodiscard]] virtual Future<std::optional<Value>> get(Key key, Snapshot snap) = 0;

    // The current committed version: a Snapshot{at = latest assigned Seq} (0 if
    // nothing committed). Reading as-of it is a consistent snapshot read.
    [[nodiscard]] virtual Future<Snapshot> snapshot() = 0;

    // Durability barrier / group commit. After the returned Error is ok, every
    // mutation committed before this call survives a subsequent crash (V-DUR).
    [[nodiscard]] virtual Future<Error> sync() = 0;

    // scan(range, snap) → the LIVE (Key,Value) pairs whose key is in [range.lo,
    // range.hi) at snapshot snap.at, KEY-ASCENDING. For each key in range, the
    // newest version with seq <= snap.at is taken; a key whose newest visible
    // version is a tombstone (or has no version ≤ snap.at) is OMITTED. A pure
    // function of (range, snap.at) under the same MVCC rule as get (V-SNAP).
    // Spans the memtable + every durable SSTable, merged newest-version-per-key
    // (storage-engine.md §1/§5 step 4).
    [[nodiscard]] virtual Future<std::vector<KeyValue>> scan(Range range, Snapshot snap) = 0;

    // K1 perf seam: synchronously VISIT the live entries of `range` at snapshot `at` —
    // the SAME key-ascending, newest-wins, tombstone-filtered sequence scan() returns,
    // WITHOUT materialising an output vector or crossing a Task/Promise boundary. An
    // engine that cannot serve the range synchronously returns false (the default; a
    // WalEngine with the value log active must await derefs) and the caller falls back
    // to scan(). The callback's references are valid ONLY for the duration of the call.
    [[nodiscard]] virtual bool scan_visit(
        const Range& range, Seq at, const std::function<void(const Key&, const Value&)>& fn) {
        (void)range;
        (void)at;
        (void)fn;
        return false;
    }
};

}  // namespace lockstep::storage
