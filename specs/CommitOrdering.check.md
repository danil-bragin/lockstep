# CommitOrdering.tla — TLC model-check record (Phase 5)

Source of truth for Lockstep deterministic commit ordering (Calvin-style, one-shot, no 2PC).
The implementation (Phase 5) must conform to this spec; behavior changes require editing the
spec and re-running TLC first.

## Exact TLC command (BOUNDED wrapper only)

```
cd /Users/npden4ik/Projects/lockstep
scripts/tlc.sh -config specs/CommitOrdering.cfg specs/CommitOrdering.tla
```

`scripts/tlc.sh` caps heap (TLC_XMX=4g), bounds workers (TLC_WORKERS=2), and puts the metadir
in /tmp so TLC never writes `states/` into the repo. Do NOT run bare `java ... tlc2.TLC`.

## Instance (specs/CommitOrdering.cfg)

| Constant   | Value                                              |
|------------|----------------------------------------------------|
| Txn        | {t1, t2, t3} (model values)                        |
| Key        | {x, y} (model values)                              |
| Empty      | model value `Empty`                                |
| RSet       | t1->{}, t2->{x}, t3->{y}   (predicted read sets)   |
| WSet       | t1->{x}, t2->{y}, t3->{x}  (write sets)            |
| Trigger    | t1->Empty, t2->x, t3->Empty (footprint trigger key)|
| Extra      | t1->{}, t2->{y}, t3->{}    (footprint expansion)   |
| MaxRetry   | 2                                                  |

Footprint functions are `<-`-mapped to operators (`RSetDef`, `WSetDef`, `TriggerDef`,
`ExtraDef`) defined in the .tla, because function literals like `(t1 :> {x})` are not legal in a
.cfg file.

- **CONSTRAINT** `StateConstraint`: `Len(seqLog) <= Cardinality(Txn)*(MaxRetry+1)` ∧
  `retries[t] <= MaxRetry` for all t. Bounds re-sequencing so the model is finite.
- **CHECK_DEADLOCK FALSE**: all-txns-terminal (committed/aborted) is the legitimate end of a
  one-shot batch, not a liveness hole (see "deadlock" note below).
- **SYMMETRY**: none. The per-txn footprint functions (RSet/WSet/Trigger/Extra) are asymmetric,
  so permuting Txn or Key is NOT a model symmetry; the instance is kept tiny instead.
- **SPECIFICATION** `Spec` ( `Init /\ [][Next]_vars /\ WF_vars(Next)` ).
- **INVARIANTS**: `SerializedBySeqLog`, `ReadsMatchSerialPrefix`, `StoreReflectsHistory`,
  `OLLPSound`, `ExactlyOnce` (the five fixed safety invariants) + the D5 read-level contracts
  `D5Snapshot`, `D5BoundedStale`, `D5ReadYourWrites`.

## Result (reproduced twice, identical)

```
Model checking completed. No error has been found.
  Estimates of the probability that TLC did not check all reachable states
  because two distinct states had the same fingerprint:
  calculated (optimistic):  val = 1.3E-16
119 states generated, 93 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 11.
Finished in 00s.
```

- All eight invariants hold: SerializedBySeqLog, ReadsMatchSerialPrefix, StoreReflectsHistory,
  OLLPSound, ExactlyOnce, D5Snapshot, D5BoundedStale, D5ReadYourWrites.
- `0 states left on queue` = the bounded model was **exhaustively explored**.
- No `states/` directory is created in the repo (`ls states` -> none; scratch lives in /tmp).
- **"Deadlock" note.** With the default deadlock check ON, TLC flags the all-committed /
  all-aborted terminal state as a deadlock. That is the correct, intended end of a one-shot
  batch (every txn reached a deterministic terminal outcome — commit, or abort after MaxRetry
  re-sequences), not a protocol stall. Deadlock checking is therefore DISABLED via
  `CHECK_DEADLOCK FALSE`. `WF_vars(Next)` keeps the spec progress-fair so the batch always drains.

## Modeling decisions

- **Deterministic sequential apply (D1/D2).** `Sequence(t)` appends t to the single global
  `seqLog` (the Phase-4 consensus order). `Execute` applies the next sequenced entry, in order,
  one at a time — no concurrency, no 2PC. The sequencer order IS the serialization order, which
  is what `SerializedBySeqLog` / `ReadsMatchSerialPrefix` pin (strict-serializable default path).
- **Marker write model.** Each committed write stamps the written key with the txn id
  (`WriteValue(t) == [k \in WSet[t] |-> t]`). The marker uniquely identifies the writer, which
  is exactly what the serial-prefix invariants compare against; a richer value domain adds states
  without adding coverage.
- **OLLP reconnaissance + re-sequence (C5.3).** OLLP predicts a footprint (RSet/WSet) from a
  cheap recon read. At execution the ACTUAL read set is recomputed from the txn's serialization-
  point snapshot: a value-dependent access — modeled by `Trigger`/`Extra` — means the real read
  set expands (e.g. t2 also reads y once x is non-Empty: an index entry now points at a row). If
  actual ≠ predicted, the consumed seqLog slot is discarded with NO store/history effect and the
  txn is RE-SEQUENCED (`status -> "pending"`, `retries+1`) for a fresh recon — not a terminal
  abort. This is bounded by `MaxRetry`; once exhausted the txn takes a deterministic terminal
  `"aborted"` (C5.6: bounded retries, starvation avoidance, deterministic outcome).
- **Footprint validity is snapshot-stable, not live-store.** `ActualReadFrom(t, snap)` /
  `FootprintValidFrom(t, snap)` are functions of a recorded snapshot, and `Snapshot(t)` records
  the trigger key as well as RSet. `OLLPSound` (fixed invariant, calls `FootprintValid(t)`) is
  routed through the committed txn's recorded `history[i].reads`, so it asserts "every committed
  txn's recon held at its OWN serialization point" — stable even after later txns mutate the
  store. (This was the bug below.)
- **D5 read-level contracts (C5.4/C5.5).** Modeled as invariants over served read results
  `(key k, prefix p) -> ValueAfterPrefix(k, p)`: `D5Snapshot` (each key read at the SAME committed
  prefix => internally consistent, no torn read), `D5BoundedStale` (the served prefix is within a
  log-lag window of `Len(history)`), `D5ReadYourWrites` (a session is served a prefix ≥ its own
  commit index). The strict-serializable default is pinned by the four core invariants; these
  three pin the relaxed-level rules to stay inside the explored history. The Phase-5 harness
  checkers test the implementation against these same predicates over actual served pairs.

## Coverage of the OLLP paths (verified with temporary probe invariants, since removed)

- `NoRetryProbe == \A t : retries[t] = 0` was VIOLATED in the reachable space: trace shows t1
  commits (writes x=t1), then t2 executes, its snapshot sees x non-Empty -> footprint mismatch ->
  re-sequence with `retries[t2]=1`, `status[t2]="pending"`. The re-sequence path is live.
- `NoAbortProbe == \A t : status[t] /= "aborted"` was VIOLATED: a persistent mismatch past
  MaxRetry reaches `status[t2]="aborted"`. The deterministic terminal-abort path is live.

So both branches of C5.6 (bounded retry, and terminal abort after the bound) are exercised by the
exhaustive run — the recon model is not dead code.

## Invariant violation caught during development (the spec caught a design bug)

**`OLLPSound` violated.** First cut defined `FootprintValid(t)` against the LIVE `store`. t2
committed while x was Empty (recon valid at the time), then t1 committed and wrote x. In a later
state `OLLPSound` re-evaluated t2's footprint against the now-mutated live store, saw x non-Empty,
concluded t2's actual read set should have included y, and reported t2 committed with an invalid
footprint. FIX (to the ACTION/semantics, NOT the invariant): make footprint validity a function
of the txn's serialization-point SNAPSHOT, recorded in `history` (`Snapshot` now also captures the
trigger key), and route `FootprintValid(t)` for a committed txn through `history[i].reads`. OLLP
soundness is correctly a statement about each txn's recon AT ITS OWN COMMIT POINT, which is stable
under later writes. After the fix the run is clean.

Both this fix and the deadlock-check decision are to the ACTIONS / config. The five safety
invariants (`SerializedBySeqLog`, `ReadsMatchSerialPrefix`, `StoreReflectsHistory`, `OLLPSound`,
`ExactlyOnce`) and the `ValueAfterPrefix` helper are **byte-identical to the skeleton** (verified
by direct diff against `git show HEAD:specs/CommitOrdering.tla`).
