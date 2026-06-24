# XShardCommit.tla — TLC model-check record (Phase 9 S9.3)

Source of truth for Lockstep CROSS-SHARD ATOMIC COMMIT: how a transaction that
touches keys on MULTIPLE shards commits ATOMICALLY (all-or-nothing) and is placed
at ONE global position by the deterministic Sequencer merge — Calvin-style, NO 2PC.
Behavior changes require editing this spec and re-running TLC first.

## Why a NEW spec (the gap the two existing specs do NOT close)

S9.3's brief asks: confirm whether `Sequencer.tla` + `CommitOrdering.tla` already
cover cross-shard atomic commit; if a property is missing, ADD it to a spec +
TLC-check it BEFORE coding. They do NOT cover it:

- **`Sequencer.tla`** models `ShardOf : [Txn -> Shard]` — each txn is owned by
  EXACTLY ONE shard, lands in exactly one shard's input log, and is emitted ONCE.
  A cross-shard txn touches `>= 2` shards, so it is committed into `>= 2` shard
  Raft logs, and a naive merge would emit it MULTIPLE times at MULTIPLE global
  positions — applying its writes more than once and risking a PARTIAL commit
  (some shards, not others). `Sequencer.tla`'s `ExactlyOnceGlobal` is keyed on
  `(shard, idx)`, so it does NOT forbid the SAME txn appearing once per shard.
- **`CommitOrdering.tla`** executes a single global `seqLog` deterministically but
  ASSUMES each txn already sits at exactly one position; it says nothing about how
  a txn replicated across shards collapses to one position, nor about cross-shard
  atomicity (writes on ALL its shards or NONE).

So cross-shard ATOMICITY is a genuinely new property. **Decision: add a new spec
`XShardCommit.tla`, TLC-check it clean BEFORE coding** (spec-before-code, the
cardinal rule). The C++ wiring (S9.3) then conforms to it on the real
Sequencer + DeterministicExecutor.

## Exact TLC command (BOUNDED wrapper only)

```
cd /Users/npden4ik/Projects/lockstep
scripts/tlc.sh -config specs/XShardCommit.cfg specs/XShardCommit.tla
```

`scripts/tlc.sh` caps heap, bounds workers, and puts the metadir in /tmp so TLC
never writes `states/` into the repo. Do NOT run bare `java ... tlc2.TLC`.

## Model (Calvin-style cross-shard, NO 2PC)

**State.**
- `inputLog : [Shard -> Seq([txn, epoch])]` — each shard's committed Raft log.
  A cross-shard txn appears in EVERY shard it touches (replicated into each).
- `epoch` / `sealed` — the open epoch / the highest sealed epoch (as in Sequencer).
- `globalLog : Seq([txn, epoch, gshard])` — the produced global order: each txn
  ONCE, with its global epoch + its ordering (lowest-rank) shard.
- `appliedOn : [Txn -> SUBSET Shard]` — the shards on which the executor has
  applied the txn's writes. The atomicity ground truth.

**The dedup + seal-gate rule (what makes it atomic + exactly-once).**
- A txn is GLOBALLY SEALABLE only once it is committed on ALL its shards into a
  sealed epoch. Its GLOBAL epoch = the MAX epoch any of its shards committed it at
  (a laggard shard pins the whole txn's epoch). It is ordered within that epoch's
  batch by the rank of its LOWEST-ranked involved shard, tie-broken by a fixed
  injective txn-id rank (`TxnKey`) — a total order ⇒ a UNIQUE sorted batch.
- It is emitted EXACTLY ONCE (the per-shard appearances collapse to one position).
  All its writes — across every shard — are applied together (`Apply` sets
  `appliedOn[t] := ShardsOf[t]` in one atomic step). A txn not yet sealed
  everywhere is absent from the global log: NONE of its writes are visible. So:
  all-or-nothing, with no 2PC and no distributed lock — atomicity is a consequence
  of the deterministic global order.

**Actions.** `Commit(s,t)` (shard `s` it touches commits `t` — fires once per
shard for a cross-shard txn), `AdvanceEpoch`, `SealEpoch` (only a CLOSED epoch,
guard `sealed+1 < epoch`, recomputes `globalLog = GlobalLogUpto(sealed+1)` from
scratch), `Apply(t)` (atomic all-shards apply of a globally-placed txn).

## Instance (specs/XShardCommit.cfg)

| Constant   | Value                                                      |
|------------|------------------------------------------------------------|
| Shard      | {sh1, sh2}                                                  |
| Txn        | {a, b, c}                                                   |
| ShardsOf   | a->{sh1,sh2}, b->{sh1,sh2}, c->{sh2} (`ShardsOfDef`)        |
| ShardRank  | sh1->0, sh2->1 (`ShardRankDef`, injective)                 |
| MaxEpoch   | 2                                                          |

`a`,`b` are CROSS-SHARD (touch both shards); `c` is single-shard (sh2 only). This
exercises: a cross-shard txn replicated into BOTH shard logs and sealed only when
present on both (the all-or-nothing seal gate + laggard-epoch rule); the dedup to
ONE global position (a,b each appear once despite two log entries); a single-shard
txn alongside cross-shard ones in one batch; the atomic apply.

- **CONSTRAINT** `StateConstraint`: caps `epoch`, `sealed`, and per-shard log
  length — bounds the search to a finite, exhaustively-checkable space.
- **CHECK_DEADLOCK FALSE**: all txns committed everywhere + all closed epochs
  sealed + all sealed txns applied is the legitimate terminal state of a drained
  batch, not a stall. `WF_vars(Next)` keeps the spec progress-fair.
- **SYMMETRY**: none. `ShardsOf` binds specific txns to specific shard sets and
  `ShardRank` is an asymmetric total order, so permuting Shard/Txn is not a model
  symmetry. Instance kept tiny instead.

## Result (clean — all 7 invariants hold)

```
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  val = 1.2E-13
3014 states generated, 1631 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 11.
Finished in 00s.
```

- All seven invariants hold. `0 states left on queue` = the bounded model was
  **exhaustively explored**. No `states/` directory in the repo (scratch in /tmp).

## Safety invariants (the deliverable) — all hold

1. **AtomicAllOrNone** — THE cross-shard atomicity property. Every txn's applied
   set is EITHER `{}` (committed on none of its shards yet) OR exactly `ShardsOf[t]`
   (committed on ALL). NEVER a strict non-empty subset — a PARTIAL cross-shard
   commit is forbidden. **Holds.**
2. **OneGlobalPosition** — each txn appears AT MOST ONCE in the global log (the
   per-shard appearances of a cross-shard txn collapse to one position). **Holds.**
3. **ExactlyOnceGlobal** — a txn is in the global log IFF it is sealable (fully
   committed on all its shards into a sealed epoch): no sealable txn lost, no
   not-yet-fully-committed txn fabricated. **Holds.**
4. **AppliedImpliesGlobal** — the executor applies only txns the sequencer placed
   in the global log. With (1): applied ⇒ in global log ⇒ sealable everywhere ⇒
   atomic. **Holds.**
5. **GlobalOrderDeterministic** — `globalLog = GlobalLogUpto(sealed)`: the produced
   order equals the pure from-scratch recomputation (a function of `inputLog` +
   `sealed` alone). Two replicas ⇒ identical single positions ⇒ identical atomic
   apply. **Holds.**
6. **PerShardSubmissionPreserved** — within a global epoch + ordering shard, the
   global order follows the fixed `TxnKey` order (no arbitrary within-batch
   choice). **Holds.**
7. **EpochMonotone** — `sealed <= epoch`, and global entries appear in
   non-decreasing global-epoch order. **Holds.**

## Teeth (the spec CATCHES a partial cross-shard commit)

To prove `AtomicAllOrNone` has bite, a teeth variant (`XShardCommitTeeth`, scratch
— not committed) added a `BuggyApplyPartial(t)` action that applies a txn's writes
to a STRICT, NON-EMPTY SUBSET of its shards (e.g. wiring that applies the txn on
only the shard that ordered it, forgetting the others — a non-2PC partial commit).
TLC immediately found the violation:

```
Error: Invariant AtomicAllOrNone is violated.
State 6: <BuggyApplyPartial ...>
  appliedOn = (a :> {sh1} @@ b :> {} @@ c :> {})
```

Cross-shard txn `a` (touches {sh1, sh2}) was applied on only {sh1} — a partial
cross-shard commit — and `AtomicAllOrNone` flagged it. The atomicity invariant is
non-vacuous: it is exactly what forbids a half-applied cross-shard txn. (The teeth
.tla/.cfg are removed after demonstrating the violation; this record preserves the
trace.)

## Modeling decisions

- **Per-shard consensus is a given (Phase 4).** Each `inputLog[s]` is append-only;
  we do not re-derive consensus. A cross-shard txn lands in each touched shard's
  log via that shard's own Raft (`Commit(s,t)` once per shard).
- **Dedup key = lowest-rank involved shard + fixed txn rank.** The global position
  of a cross-shard txn is a pure structural function of the shard logs (no clock,
  no nondeterminism), so every replica computes the same single position — the
  multi-shard generalization of Sequencer.tla's `(ShardRank, idx)` merge key.
- **Seal gate = fully-committed-everywhere.** A cross-shard txn is sealable only
  when present in EVERY touched shard's log within a sealed epoch (laggard epoch
  pins it). This is what makes the atomic apply all-or-nothing: a txn missing on
  any shard is simply absent from the global order, so none of its writes show.
- **Pure-function oracle (`GlobalLogUpto`) vs live `globalLog`.** As in
  Sequencer.tla, the action recomputes the whole global log from scratch and the
  invariant pins it equal — the strongest statement of determinism.
