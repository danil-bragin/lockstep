# Lockstep — Phase 5 Brief Batch (Distributed Txn + D5 Consistency Contract)

> Source of truth: specs/CommitOrdering.tla (MODEL-CHECKED CLEAN + HUMAN-APPROVED, commit 3d2283f),
> CommitOrdering.check.md, lockstep-phase-specs-all.md Phase 5 (C5.1-C5.6), master-plan D1/D2/D5.
> core-no-freelance. Universal merge gate + spec-conformance + per-D5-level checkers + linearizability + differential-vs-oracle.

## Scope (the verified spec covers)
- C5.1 one-shot txn submission (params in / result out; NO interactive sessions — D1).
- C5.2 deterministic ordering: txns sequenced into a single global order (the Phase-4 consensus log = the seqLog);
  every node executes that order deterministically. NO 2PC, NO distributed deadlocks.
- C5.3 OLLP reconnaissance: predict read/write footprint from a snapshot, sequence, then at execution DETECT if the
  actual footprint differs -> RE-SEQUENCE with fresh recon (bounded MaxRetry; exhausted -> terminal abort; starvation-avoid).
- C5.4 D5 selector (call-site-visible / type-encoded): StrictSerializable (default), Snapshot, BoundedStaleness(log-lag),
  ReadYourWrites(session). The level is in the API/type so it can't be silently misused.
- C5.5 read path: snapshot reads at a version (Phase-3 MVCC); bounded-staleness routed to a local replica w/ log-lag check;
  session tracking for RYW. C5.6 deterministic abort/retry.

## Staged build (verification-machinery-first)
- **Stage M (DISPATCH NOW):** the txn SEAM (one-shot Transaction + the D5 selector types) + a trivial strict-serializable
  ORACLE (apply txns in seqLog order, sequentially — ground truth) + a DIFFERENTIAL harness + per-D5-level CONSISTENCY
  CHECKERS (each asserts EXACTLY its contract) + a linearizability (Elle-style) check for the default path, with a TEETH
  test (a deliberately-wrong txn executor — skips OLLP recon / applies out of order / serves stale for strict level — MUST be flagged).
- **Stage I (after M):** the real deterministic-txn layer over Phase-4 consensus (seqLog) + Phase-3 storage (MVCC), C5.1-C5.6.
- **Stage V (after I):** gate — strict-serializable verified by the linearizability checker under fault storms; EACH D5
  level verified to honor exactly its contract (not stronger/weaker); OLLP retry terminates w/o starvation; differential
  vs oracle; mutation ≥ threshold.

## Conformance mapping (CommitOrdering.tla invariants -> running system)
- SerializedBySeqLog: there is one global serialization order; observable results match SOME serial execution in that order.
- ReadsMatchSerialPrefix / StoreReflectsHistory: a txn's reads see the serial prefix; final store = applied history.
- OLLPSound: a committed txn's recon was valid at ITS OWN serialization point (footprint match), stable under later writes.
- ExactlyOnce: a committed txn's effects apply exactly once (no lost/duplicated effects under faults).
- D5: StrictSerializable=linearizable; Snapshot=consistent-as-of a version, no real-time guarantee; BoundedStaleness=
  within the stated log-lag; ReadYourWrites=a session observes its own prior writes.

## Determinism / faults / reuse
All over Phase-4 consensus (SimNetwork+SimDisk) + Phase-3 storage + scheduler. Pure fn of seed. Reuse ALL prior lessons:
per-record CRC + recover-to-prefix; no pointer across co_await (V-RKV1); CTest TIMEOUT; bounded scheduler run_until for
non-quiescing parts; seed-burn + shrink; the V-XCHECK cross-impl predicate idea where relevant.

## RESOURCE DISCIPLINE (a freeze happened): heavy/gate-running agents run ONE AT A TIME. ulimit -c 0; ulimit -s 16384.
Build -j6. In-gate sweeps <= 64 seeds (env override). No unbounded loops. Run the gate ONCE per agent. Stray *.mutbak in
source = interrupted mutation -> restore from it before trusting. Use scripts/tlc.sh for any TLC.
