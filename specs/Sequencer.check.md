# Sequencer.tla — TLC model-check record (Phase 4, C4.4)

Source of truth for the Lockstep SEQUENCER: how the per-shard committed input logs (Phase 4,
one consensus order PER shard) combine into ONE global deterministic total order (Calvin-style
epoch batching, D2 pure log-order). This global order is the linearization point the
distributed-txn layer (Phase 5) consumes — `CommitOrdering.tla` ASSUMES a single global `seqLog`;
the sequencer is what PRODUCES that `seqLog` from N shards. Behavior changes require editing this
spec and re-running TLC first.

## Exact TLC command (BOUNDED wrapper only)

```
cd /Users/npden4ik/Projects/lockstep
scripts/tlc.sh -config specs/Sequencer.cfg specs/Sequencer.tla
```

`scripts/tlc.sh` caps heap (TLC_XMX=4g), bounds workers (TLC_WORKERS=2), and puts the metadir in
/tmp so TLC never writes `states/` into the repo. Do NOT run bare `java ... tlc2.TLC` (a bare run
once dumped ~15 GB and froze the host).

## State + actions + the deterministic merge rule

**State.**
- `inputLog` : `[Shard -> Seq([txn, epoch])]` — each shard's committed input log, txn ids in that
  shard's fixed consensus order, each entry epoch-tagged. Append-only (per-shard order is fixed by
  Phase-4 consensus).
- `epoch` : `Nat` — the current OPEN epoch shards commit into ("now").
- `sealed` : `Nat` — highest epoch the sequencer has sealed into the global log (epochs `1..sealed`
  done; `sealed < epoch` means batches still pending).
- `globalLog` : `Seq([txn, shard, idx, epoch])` — the produced global total order; `idx` is the
  entry's per-shard 1-based index.

**Actions.**
- `Commit(s, t)` — shard `s` (the owner, `ShardOf[t]=s`) commits txn `t` into its input log, tagged
  with the open `epoch`. Guarded so a txn is committed at most once overall. Append-only.
- `AdvanceEpoch` — opens the next epoch (`epoch+1`), bounded by `MaxEpoch`. Monotone.
- `SealEpoch` — seals the next epoch (`sealed -> sealed+1`) into the global log, appending that
  epoch's deterministically-merged batch. Guard `sealed+1 < epoch`: only a CLOSED epoch (the open
  epoch has moved past it, so no future commit can land in it) may be sealed, and epochs are sealed
  strictly IN ORDER.

**The deterministic merge rule.** For each sealed epoch `e`, the batch = every shard's epoch-`e`
entries, sorted by the FIXED total order `LessEntry`: `(ShardRank[shard], idx)`. `ShardRank` is an
injective rank over shards, so cross-shard ties never occur; within a shard `idx` (strictly
increasing along the log) breaks them. This is a TOTAL order on `(shard, idx)`, so `SortEntries`
produces exactly ONE sequence — the merge is a PURE FUNCTION with no nondeterministic choice.
`GlobalLogUpto(sealed)` recomputes the whole global log from `inputLog + sealed` alone; the live
`globalLog` is pinned equal to it (invariant 1).

## Instance (specs/Sequencer.cfg)

| Constant   | Value                                              |
|------------|----------------------------------------------------|
| Shard      | {sh1, sh2} (model values)                          |
| Txn        | {a, b, c, d} (model values)                        |
| ShardOf    | a->sh1, b->sh1, c->sh2, d->sh2 (`ShardOfDef`)      |
| ShardRank  | sh1->0, sh2->1 (`ShardRankDef`, injective)         |
| MaxEpoch   | 2                                                  |

`ShardOf` and `ShardRank` are `<-`-mapped to operators (`ShardOfDef`, `ShardRankDef`) in the .tla,
because function literals like `(a :> sh1)` are not legal in a .cfg. 2 txns per shard => a per-shard
index order to preserve; 2 epochs => both same-epoch multi-shard merges and cross-epoch ordering are
exercised.

- **CONSTRAINT** `StateConstraint`: `epoch <= MaxEpoch`, `sealed <= MaxEpoch`,
  `Len(inputLog[s]) <= Cardinality(Txn)`. Bounds the search to a finite, exhaustively-checkable space.
- **CHECK_DEADLOCK FALSE**: all txns committed + all closed epochs sealed (and the open epoch at
  `MaxEpoch`) is the legitimate terminal state of a drained batch, not a stall. `WF_vars(Next)` keeps
  the spec progress-fair so the global log always drains.
- **SYMMETRY**: none. `ShardOf` binds specific txns to specific shards and `ShardRank` is an
  asymmetric total order, so permuting `Shard` or `Txn` is NOT a model symmetry. Instance kept tiny
  instead (same call as CommitOrdering.tla, whose footprint functions are likewise asymmetric).
- **SPECIFICATION** `Spec` (`Init /\ [][Next]_vars /\ WF_vars(Next)`).
- **INVARIANTS**: `GlobalOrderDeterministic`, `PerShardOrderPreserved`, `EpochMonotone`,
  `ExactlyOnceGlobal`, `NoLossSealed`.

## Result (reproduced twice, identical)

```
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  val = 2.7E-15
451 states generated, 267 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 7.
Finished in 00s.
```

- All five invariants hold.
- `0 states left on queue` = the bounded model was **exhaustively explored**.
- No `states/` directory is created in the repo (`ls states` -> none; scratch lives in /tmp).
- Re-run is byte-identical (451 generated / 267 distinct / 0 left on queue, depth 7).

## Safety invariants (the deliverable) — all hold

1. **GlobalOrderDeterministic** — `globalLog = GlobalLogUpto(sealed)`. The produced global log is a
   DETERMINISTIC function of the per-shard logs + the sealed boundary; `GlobalLogUpto` references only
   `inputLog + sealed`, never any nondeterministic choice. Two runs with the same shard logs and
   sealed boundary produce the IDENTICAL global order. **Holds.**
2. **PerShardOrderPreserved** — for any two global entries from the SAME shard, `p < q =>
   idx[p] < idx[q]`: their relative order in the global log equals their per-shard consensus
   (index) order. The merge never reorders within a shard. **Holds.**
3. **EpochMonotone** — `sealed <= epoch`, and global entries appear in non-decreasing epoch order
   (epoch `e`'s whole batch precedes `e+1`'s); epochs are sealed in order. **Holds.**
4. **ExactlyOnceGlobal** — no two global positions share the same `(shard, idx)` (no duplication),
   and every committed input entry whose epoch is `<= sealed` is present (no loss): each sealed input
   txn appears EXACTLY ONCE. **Holds.**
5. **NoLossSealed** — the converse: every global entry corresponds to a real committed input entry
   (`inputLog[shard][idx]` matches its txn + epoch) with `epoch <= sealed` (nothing fabricated).
   Together with (4): the sealed prefix of every shard's input log appears in the global log exactly
   once. **Holds.**

## Invariant violation caught during development (the spec caught a design bug)

**`GlobalOrderDeterministic` violated** by a wrong SEAL GUARD. The correct guard is
`sealed + 1 < epoch` — only a CLOSED epoch (one the open epoch has already moved past) may be sealed.
A tempting weaker guard `sealed + 1 <= epoch` lets the sequencer seal the STILL-OPEN epoch. TLC then
found this trace: shard `sh1` commits `a` into epoch 1; the sequencer seals epoch 1 (still open) into
`globalLog`; then shard `sh2` commits `c` into epoch 1 as well — but epoch 1 is already sealed, so
`c` lands in an already-published batch. The live `globalLog` (which got `a` but not `c`) no longer
equals `GlobalLogUpto(sealed)` (which now includes `c`): a late commit changed an already-sealed
epoch's batch, breaking determinism. FIX (to the ACTION, NOT the invariant): tighten `SealEpoch`'s
guard to `sealed + 1 < epoch`, so an epoch is sealed only after the open epoch has advanced past it
and no further commit can land in it. After the fix the run is clean. This is the sequencer analogue
of "you cannot publish a Calvin epoch's order until the epoch boundary has closed."

(During bring-up there were also two pure TLA+ shape errors — fixed before any state was explored,
not design bugs: a two-variable set comprehension whose second bound depended on the first, rewritten
as a `UNION`; and the `globalLog` entry record missing its `epoch` field that the epoch invariants
read. Neither is a protocol issue.)

## Modeling decisions

- **Per-shard consensus is a given (Phase 4).** Each `inputLog[s]` is append-only and its order is
  whatever order `Commit(s,_)` fires — we do NOT re-derive consensus here; Consensus.tla owns that.
  The sequencer's only job is the deterministic CROSS-shard merge, so the model abstracts a shard's
  log to "a fixed epoch-tagged sequence of txn ids."
- **Epoch batching = the determinism boundary.** A batch can be sealed only once its epoch is closed
  (`sealed+1 < epoch`). This is exactly what makes `EpochEntries(sealed+1)` FINAL at seal time and
  the merge a pure function of the (now-frozen) shard prefixes — the heart of GlobalOrderDeterministic.
- **Merge key `(ShardRank, idx)`, not txn id or commit time.** D2 is pure log-order: no clocks. The
  total order is structural (shard rank + per-shard index), so it is reproducible on any replica from
  the shard logs alone. `ShardRank` injective => total order => `SortEntries` yields a unique sequence.
- **Entry identity `(shard, idx)`.** Each global entry carries its source `(shard, idx, txn, epoch)`,
  so ExactlyOnceGlobal / NoLossSealed can pin global entries one-to-one against the input logs (no
  loss, no duplication, no fabrication) — the multi-shard analogue of CommitOrdering's `ExactlyOnce`.
- **Pure-function oracle (`GlobalLogUpto`) vs live state (`globalLog`).** The spec maintains both: the
  action incrementally appends sealed batches to `globalLog`; the invariant recomputes the whole thing
  from scratch and asserts equality. This is the strongest possible statement of determinism — the
  incremental production can never diverge from the from-scratch pure merge.
