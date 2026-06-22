# Lockstep — Storage Engine (Phase 3 spec)

> Source of truth: lockstep-phase-specs-all.md Phase 3 (C3.1–C3.8), master-plan D4 (LSM+MVCC+WiscKey) + D2 (pure log-order).
> Single-node, crash-consistent, MVCC KV engine running ENTIRELY inside the sim. Verification-machinery-first:
> the differential oracle + harness come BEFORE the engine. Caveman-encoded. `!`=must `⊥`=never `∀`=∀ `∃`=∃ `→`=leads-to `≤`=at-most.

## §0 Scope
C3.1 WAL · C3.2 memtable · C3.3 SSTable · C3.4 compaction · C3.5 MVCC · C3.6 WiscKey · C3.7 bench · C3.8 crash recovery.
All IO through sim IDisk (torn/lying-fsync/crash modeled). All async on the deterministic scheduler. Pure fn of (seed, ops).

## §1 Interface (the engine API agents code against)
```cpp
namespace lockstep::storage {
using Key = std::string;     // opaque byte string (D4)
using Value = std::string;   // opaque; large values → WiscKey value-log (C3.6)
using Seq = std::uint64_t;   // monotonic commit sequence = MVCC version (D2 log-order). 0 = ∅/before-first.

struct Snapshot { Seq at; };                 // read-as-of a version
class Engine {                               // single node; all methods async (Future), on the scheduler via IDisk
  Future<Seq>            put(Key, Value);    // returns the commit seq assigned; durable only after sync()
  Future<Seq>            del(Key);           // tombstone; returns commit seq
  Future<std::optional<Value>> get(Key, Snapshot); // MVCC read as-of snap.at
  Future<Snapshot>       snapshot();         // current committed version
  Future<Error>          sync();             // durability barrier (group commit); after this, committed data survives crash
  // scan(range, Snapshot) → ordered [(Key,Value)]   (added when SSTable lands)
};
}
```
- Version model (DERIVED from D2+D4, not a new decision): each committed mutation gets a monotonic `Seq`. A `get(k, {at})` returns the value of the newest version of k with `seq ≤ at` (∅ if none / tombstone). Snapshot read = consistent as-of that Seq.

## §2 Contracts / invariants (the gate asserts these)
- V-DUR: after `sync()` returns ok → those mutations survive any subsequent crash. Before sync, a crash MAY lose them.
- V-PREFIX: crash at ANY instruction → recovery yields a consistent PREFIX of the committed history (some suffix may be lost, never a gap, never a fabricated value). (Same contract the batch-2 WAL CRC bug violated — reuse that lesson: per-record integrity; stop replay at first corrupt record.)
- V-SNAP: a `get(k, snap)` is a pure function of (k, snap.at) — never observes a partial/in-flight mutation, never a version > snap.at, never a torn value. Snapshot reads are stable under concurrent writers.
- V-MONO: assigned Seq strictly increases per commit; ⊥ reused, ⊥ gap that hides a lost commit.
- V-NOTORN: ⊥ torn-write corruption survives recovery (a torn SSTable block / WAL record is detected + discarded, recover to prefix).
- V-GC: old-version GC under a watermark ⊥ removes a version still visible to a live snapshot.
- V-DET: whole engine = pure fn of (seed, op stream). Same seed → byte-identical observable history + on-disk image fingerprint.

## §3 Architecture (D4)
write: put/del → WAL append (IDisk) + memtable insert (MVCC version) → on sync, group-commit fsync → memtable flush to SSTable when full → background compaction (leveled|tiered, EMPIRICAL via bench, not guessed).
read: get(k,snap) → memtable (newest ≤ snap) → SSTables newest→oldest (bloom filter skip, sparse index seek) → WiscKey value-log deref for large values.
recover: replay WAL (integrity-checked, stop at first corrupt) over the last durable SSTable set → consistent prefix.

## §4 Verification (built FIRST — machinery before engine)
- ORACLE: a trivial, obviously-correct in-memory map-of-(key→sorted versions). Same op stream, same Seq assignment, same snapshot semantics. NO disk, NO LSM — just correctness ground truth.
- DIFFERENTIAL HARNESS (THE gate, master-plan §6.4): drive an identical seeded op stream (put/del/get/snapshot/sync + crash/recover) against BOTH engine & oracle; assert every get/scan result MATCHES the oracle under the version mapping. Runs under the sim with the full disk-fault envelope.
- CRASH-CONSISTENCY: crash at arbitrary points (sim IDisk) → recovered engine state = a valid prefix the oracle agrees with.
- MVCC under concurrent writers: interleaved writer coroutines; snapshot reads stay consistent (V-SNAP).
- Property/metamorphic: e.g. get(k, snapshot-after-put(k,v)) == v; compaction ⊥ change any visible value; flush ⊥ change reads.
- Mutation score ≥ threshold (the now-real engine). Differential + crash + MVCC are the teeth.

## §5 Build order (verification-machinery-first)
1. Engine interface header + the ORACLE + the DIFFERENTIAL HARNESS (no real engine yet — harness drives oracle-vs-oracle to prove itself, plus a teeth-test: a deliberately-wrong toy engine MUST be flagged).
2. WAL (C3.1) + crash recovery (C3.8) → first real engine passes differential under crash faults (memtable-only, no SSTable yet).
3. Memtable (C3.2) + MVCC (C3.5).
4. SSTable (C3.3) + flush + scan.
5. Compaction (C3.4).
6. WiscKey large-value separation (C3.6).
7. Bench harness (C3.7) → settles D4 tuning empirically.
Each step: differential + crash + MVCC + mutation, through the batch-2 merge gate.

## §6 Notes
- Reuse batch-2 lessons: per-record/per-block integrity (CRC) + stop-at-first-corrupt (V-PREFIX/V-NOTORN); ctest TIMEOUT (liveness); no pointer into a growable container across co_await (V-RKV1).
- Go WIDE (plan): the merge gate is real now → fan agents across §5 steps once the interface+oracle+harness (step 1) land. Keep WAL/recovery/MVCC spec-anchored; let SSTable/bloom/bench run broader.
- D4 tuning (compaction strategy, level sizing, bloom bits, block size) = EMPIRICAL via §C3.7 bench, ⊥ guessed now.
