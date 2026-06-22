# Lockstep — Phase 4 Brief Batch (Replicated Log / Consensus)

> Source of truth: specs/Consensus.tla (MODEL-CHECKED CLEAN + HUMAN-APPROVED 2026-06-22, commit 8282b4d),
> Consensus.check.md, lockstep-phase-specs-all.md Phase 4 (C4.1-C4.6). MAX SCRUTINY. core-no-freelance.
> Universal merge gate + spec-conformance + linearizability + DUAL implementation cross-check.

## Scope (what the approved spec covers)
- IN the verified spec → IMPLEMENT NOW: C4.1 leader election + log replication + commit index; persist term/vote/log
  before responding (spec's persist-before-reply); the 4 safety invariants (ElectionSafety, LogMatching,
  StateMachineSafety, LeaderAppendOnly).
- NOT in the verified spec → DEFER (each needs its own spec + TLC round before code): C4.2 membership change,
  C4.3 snapshot/log-compaction. C4.4 sequencer (deterministic global total order — separate, deterministic merge)
  and C4.5 recovery/failover come AFTER the core consensus passes.

## Staged build (verification-machinery-first)
- **Stage M (DISPATCH NOW):** consensus SEAM (interface both impls implement) + the cross-check / spec-conformance /
  linearizability HARNESS over SimNetwork+SimDisk+scheduler, with a TEETH test (a deliberately-wrong node — e.g.
  allows two leaders in a term, or loses a committed entry — MUST be flagged).
- **Stage I (after M):** DUAL independent implementation — impl A + impl B by TWO DIFFERENT agents from the same
  verified spec (master-plan §6.5 blind-spot coverage). Each conforms to Consensus.tla.
- **Stage V (after I):** run the harness over BOTH impls under partition+crash+reorder storms: (1) each conforms to
  the spec safety invariants; (2) the two impls AGREE on every generated history; (3) linearizability checker passes;
  (4) mutation ≥ threshold. THIS is the Phase-4 gate.

## Conformance mapping (the harness asserts the spec's invariants on the real impl)
- ElectionSafety: never two leaders in the same term (observe role+term across nodes every step).
- LogMatching: same (index,term) across nodes ⇒ identical prefix.
- StateMachineSafety: a committed index never holds different entries across nodes; a committed entry is never lost
  or reordered across leader failover.
- LeaderAppendOnly: a leader never overwrites/deletes its own log.
- Linearizability: the committed log is a single total order; client submit→commit history is linearizable.
- Persist-before-reply: a node's vote/term/log is durable (IDisk) before it acts on it (crash mid-step ⇒ recovery
  respects the durable decision; never two leaders after a crash).

## Determinism / faults
All over SimNetwork (delay/reorder/dup/drop/partition+heal) + SimDisk (crash/torn/lying-fsync) + scheduler. Pure fn
of seed. Reuse all Phase-2/3 lessons (per-record CRC + recover-to-prefix for persisted state; no pointer across
co_await; CTest TIMEOUT; seed-burn + shrink). The harness logs every seed; failures replay byte-identically.
