# Snapshot.tla — TLC model-check record (Phase 4, C4.3 log snapshotting / compaction)

Model-checked source of truth for log compaction + InstallSnapshot safety on top of the Raft
replicated log (Consensus.tla). The implementation must conform; behavior changes require editing
this spec and re-running TLC first.

## Exact TLC command (bounded wrapper only)

```
cd /Users/npden4ik/Projects/lockstep
scripts/tlc.sh -config specs/Snapshot.cfg specs/Snapshot.tla
```

The wrapper (`scripts/tlc.sh`) caps heap, puts the metadir/scratch in `/tmp` (auto-removed), and
bounds workers — a bare `java tlc2.TLC` once dumped ~15 GB into `states/` and froze the host. Do
NOT run bare java.

## Instance (specs/Snapshot.cfg)

| Constant   | Value                |
|------------|----------------------|
| Server     | {s1, s2}             |
| Key        | {k1, k2}             |
| Value      | {v1, v2}             |
| Nil        | model value `Nil`    |
| MaxLogLen  | 3                    |

- **CONSTRAINT** `StateConstraint`: `Len(committed) <= MaxLogLen` (caps the canonical committed
  log; everything else is bounded by it via `TypeOK`'s structural relations).
- **SYMMETRY** `Symmetry == Permutations(Server) \cup Permutations(Key) \cup Permutations(Value)`
  — all three are interchangeable model-value sets.
- **SPECIFICATION** `Spec` (`Init /\ [][Next]_vars`).
- **INVARIANT** `TypeOK`, `CompactionPreservesState`, `SnapshotPrefixConsistent`,
  `RecoveredEqualsFull`, `AppliedStateSafety`.
- **CHECK_DEADLOCK FALSE** — "committed log full, all servers caught up + compacted" is a
  legitimate terminal state (the oracle stopped growing at the cap), not a liveness hole.

## Result (reproduced twice, identical)

```
Model checking completed. No error has been found.
12025 states generated, 3591 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 17.
```

- All five invariants hold: **TypeOK, CompactionPreservesState, SnapshotPrefixConsistent,
  RecoveredEqualsFull, AppliedStateSafety.**
- `0 states left on queue` = the bounded model was **exhaustively explored**.
- No `states/` directory leaked (wrapper writes only to `/tmp`); `ls states` -> none.

## State

Per the focused model (we do NOT re-derive Raft election/replication — Consensus.tla already
model-checks StateMachineSafety; we take the committed log as a given oracle):

- `committed` — Seq(entry); the **canonical committed log** (consensus oracle). Grows only, never
  discarded, so it is the ground truth the invariants fold against. `entry == [key, value]` (a
  single-key write).
- `log[s]` — the (possibly compacted) **suffix** server s physically retains.
- `logBase[s]` — absolute index just before `log[s][1]` (kept `= snapshot[s].lastIncludedIndex`).
- `commitIndex[s]` — how far s has learned the committed prefix (`= logBase + Len(log)`).
- `appliedIndex[s]` — abs index of the last entry folded into `applied[s]`.
- `applied[s]` — the live state-machine map `[Key -> Value \cup {Nil}]`.
- `snapshot[s]` — `[ lastIncludedIndex, state ]`; `state` is the full map after applying
  `[1..lastIncludedIndex]` (the discarded prefix folded in).

## Actions

- **CommitEntry(k,v)** — oracle growth; stands in for "Raft committed an entry on a quorum".
- **AppendCommitted(s)** — normal AppendEntries-style: s learns the next committed entry (abs
  index `commitIndex+1`) and appends it to its retained suffix. Only ever appends contiguously
  after the retained tail, copying the canonical entry verbatim (prefix-consistent by
  construction).
- **Apply(s)** — fold the next committed-and-retained entry into `applied`; advances
  `appliedIndex`. Requires the entry to still be in the retained log (above the snapshot point).
- **TakeSnapshot(s)** — capture applied state through `i = appliedIndex[s]`, set the snapshot, and
  **DISCARD the log prefix `<= i`**. The fold: `snapshot.state = ReconstructUpTo(s, i)` =
  `snapshot.state(old) folded over retained[logBase+1..i]` = the full state after `[1..i]`. So the
  discarded prefix's writes are **folded into snapshot.state** before they're dropped. Guarded by
  `i <= appliedIndex` (never snapshot an un-applied entry).
- **InstallSnapshot(s, d)** — a lagging follower `d` whose next needed index is at/below leader
  `s`'s snapshot point (`commitIndex[d] < snapshot[s].lastIncludedIndex` — AppendEntries can't
  catch it up because those entries were discarded by s) **adopts s's snapshot wholesale**:
  `applied[d] := snapshot[s].state`, `appliedIndex/logBase[d] := lastIncludedIndex`, and the
  retained `log[d]` := s's committed suffix (verbatim from the canonical log, bounded by what s
  holds).

## How compaction folds the prefix into snapshot.state

`applied[s]` is maintained as `fold(committed[1..appliedIndex])` (CompactionPreservesState).
`TakeSnapshot` sets `snapshot.state := ReconstructUpTo(s, appliedIndex)`, which is precisely
`fold(committed[1..appliedIndex])` (SnapshotPrefixConsistent confirms
`snapshot.state = FoldCommittedPrefix(lastIncludedIndex)`). The prefix `[1..lastIncludedIndex]` is
then physically dropped from `log`, but every one of its writes survives inside `snapshot.state` —
so `ReconstructUpTo(s, n)` (snapshot.state then the retained suffix) equals the full-log fold
`FoldCommittedPrefix(n)` for any `n >= lastIncludedIndex`. That equality is exactly what
RecoveredEqualsFull and CompactionPreservesState assert.

## Safety invariants (all hold)

1. **CompactionPreservesState** — `applied[s] = FoldCommittedPrefix(appliedIndex[s])` AND
   `ReconstructUpTo(s, appliedIndex) = FoldCommittedPrefix(appliedIndex)`. Compaction never
   changed the state-machine value: snapshot+suffix reconstructs the same state as folding the
   whole committed log. **HOLDS.**
2. **SnapshotPrefixConsistent** — `snapshot.lastIncludedIndex <= Len(committed)`,
   `snapshot.state = FoldCommittedPrefix(lastIncludedIndex)` (no committed entry below the
   snapshot lost — each folded in), and the retained log is verbatim-equal to the canonical
   committed entries at the same indices. **HOLDS.**
3. **RecoveredEqualsFull** — for every server (esp. a follower just InstallSnapshot'd),
   `ReconstructUpTo(s, commitIndex) = FoldCommittedPrefix(commitIndex)`: snapshot + suffix yields
   the same state as the full committed log up to commitIndex. The recovery guarantee. **HOLDS.**
4. **AppliedStateSafety** — both servers' applied maps equal the canonical fold at their own
   applied indices, so at any commonly-applied index they agree (no divergence). **HOLDS.**
5. **TypeOK** — structural: `logBase = snapshot.lastIncludedIndex`, retained log is the contiguous
   suffix above the snapshot point, `commitIndex = logBase + Len(log)`,
   `logBase <= appliedIndex <= commitIndex`. Catches any action that breaks the representation.
   **HOLDS.**

## Violations caught during development (mutation sanity — invariants are non-vacuous)

Run as deliberate mutants on the verified spec to confirm the invariants bite and that the
snapshot/install actions are genuinely reachable:

1. **Wrong snapshot fold** — `TakeSnapshot` sets `snapshot.state := ReconstructUpTo(s, i-1)`
   (folds one entry short, i.e. the action would discard an entry without folding it into the
   snapshot). TLC: `Invariant CompactionPreservesState is violated.` This is the exact danger the
   spec rules out — discarding an un-folded entry corrupts recovered state. The FIX is in the
   ACTION (fold through the full applied index `i`, never `i-1`).
2. **Wrong InstallSnapshot state** — follower adopts `EmptyState` instead of the leader's
   `snapshot.state`. TLC: `Invariant CompactionPreservesState is violated.` Confirms
   InstallSnapshot is actually exercised (a lagging follower is caught up via the leader's
   snapshot) and that adopting the wrong state is caught. FIX in the ACTION (adopt
   `snapshot[s].state` verbatim).

Both mutants point at ACTION bugs, exactly as the discipline requires; the verified spec's
actions are correct and all five invariants hold.

## Modeling decisions

- **Consensus output as an oracle.** We do not re-run Raft here. `committed` is a single monotone
  sequence standing in for the agreed committed log (Consensus.tla proves committed entries never
  diverge/reorder). This keeps the C4.3 model centered on compaction safety — the thing this spec
  adds — rather than re-checking Phase-4 replication.
- **`committed` is never discarded** so the invariants can always compare a server's compacted
  `(snapshot.state + retained log)` against folding the WHOLE committed log. The per-server `log`
  is the physically-retained suffix that compaction actually shrinks.
- **Single-key-write entries + map state machine.** A richer value model is unnecessary; key
  overwrites are enough to make "fold of full log == snapshot + suffix" a real, non-trivial check
  (later writes shadow earlier ones, so a lost prefix entry is observable iff it was the last
  writer of its key — and the fold catches exactly that).
- **Snapshot taken at `appliedIndex`** (the freshest fully-applied point). Snapshotting only
  through an applied index is the core safety guard: you may never discard a log entry you have
  not yet folded into the snapshot's state.
- **Small instance (2 servers, 2 keys, 2 values, MaxLogLen 3).** Exhaustive in <1s, depth 17,
  3591 distinct states. Large enough to exercise multi-entry replication, Apply, TakeSnapshot
  (real prefix discard), and a lagging follower caught up by InstallSnapshot; symmetry over all
  three model-value sets keeps it tight.
