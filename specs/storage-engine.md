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
- V-GC: old-version GC under a watermark ⊥ removes a version still visible to a live snapshot. watermark = oldest LIVE snapshot Seq (single node). DROPPABLE RULE: a version vi of key k is droppable ⟺ ∃ a newer version vj (vj.seq > vi.seq) with vj.seq ≤ watermark — the newest version with seq ≤ watermark is the OLDEST any live snapshot can still read, so everything strictly older is dead. KEEP the watermark-floor survivor + every version with seq > watermark; if NO version has seq ≤ watermark, keep them ALL. A lone tombstone that is the floor survivor AND fully ≤ watermark (nothing newer) is reclaimable (key vanishes). watermark 0 ⇒ nothing droppable (every historical snapshot conservatively live). Compaction crash-discipline: merged SSTable INSTALL must be durable BEFORE the inputs' OBSOLETE records; recovery FOLDS the append-only manifest (INSTALL/OBSOLETE/WAL-TRUNCATE, entry_no-contiguous, stop-at-first-corrupt) into the live set; a crash between INSTALL and OBSOLETE loads BOTH old+merged → read merges by max-seq → identical values (no loss/fabrication). WAL-truncation watermark is clamped on recovery to the LOADED SSTable max (never skip a WAL record no surviving SSTable reproduces).
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
- BACKPROP (P3 step-2 WAL recovery): stop-at-first-CRC-corrupt is NECESSARY but NOT SUFFICIENT for V-PREFIX. A torn/io-faulted append can DROP a record from the MIDDLE of the log while LATER records (with valid CRCs) still land → a Seq GAP. Replaying past the gap resurrects a NON-prefix (a hole that hides the lost commit), surfacing a stale older version where the truth is the lost newer one. RECOVERY MUST ALSO ENFORCE SEQ CONTIGUITY: replay only while `record.seq == expected_next` (1,2,3,…); the moment a decoded record's Seq ≠ expected, STOP — the consistent prefix ends at the gap. Caught by the crash-consistency test (seed 1: k4 del @seq=5 lost, seqs 6-8 landed → gap). Carry this to SSTable/manifest recovery too: a valid checksum on a record does NOT make it part of the prefix if anything before it is missing.
- BACKPROP (P3 step-3 SSTable, two real bugs caught by crash-during-flush):
  (a) READ MUST MERGE BY MAX-SEQ across ALL sources (memtable + every SSTable), ⊥ assume memtable-is-newest. A lying-fsync-truncated WAL can leave the memtable holding an OLDER version than a durably-installed SSTable for the same key → source-priority read surfaces a stale value. `get(k,{at})` = max seq ≤ at over all sources; tombstone at the winning seq → ∅.
  (b) A manifest-referenced SSTable that fails block-CRC validation MUST STOP the install prefix (⊥ skip-and-continue) — Seq-contiguity at SSTable granularity, else a later SSTable loads past a missing earlier one → Seq gap inside the prefix. Atomic-install: an SSTable is live only once its CRC'd manifest record is durable; a crash before that → the SSTable bytes are inert, data still in WAL.
- BACKPROP (P3 step-5 compaction, latent SSTable READ bug surfaced by GC): SSTableReader::lookup(k,at) used a sparse-index seek that lands on the LAST block whose first_key ≤ k and scanned FORWARD only. A key with MANY versions SPANS several blocks (and may START mid-block, packed after a smaller key), so the forward-only scan from the last block SILENTLY DROPPED the key's EARLIER (older) versions → a read at an old snapshot saw ∅ where a live value exists. Invisible until compaction merged a key's full version chain into ONE big multi-block SSTable, then a watermark-floor read missed it. FIX (V-NOTORN/V-SNAP read-completeness): before forward-scanning, BACK UP to the run's FIRST block — over preceding blocks whose first_key == k (pure spill), then ONE more predecessor whose first_key < k (the key may begin mid-block in it). LESSON: a sparse index addresses a key's block RANGE, not a single block; lookup/scan MUST cover every block the key's versions occupy. Caught by the compaction GC-safety + metamorphic tests.
- Go WIDE (plan): the merge gate is real now → fan agents across §5 steps once the interface+oracle+harness (step 1) land. Keep WAL/recovery/MVCC spec-anchored; let SSTable/bloom/bench run broader.
- D4 tuning (compaction strategy, level sizing, bloom bits, block size) = EMPIRICAL via §C3.7 bench, ⊥ guessed now.
