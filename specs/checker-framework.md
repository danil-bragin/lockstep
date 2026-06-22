# Lockstep — Checker Framework + Fault Envelope (Phase 2 batch 2 spec)

> Status: DRAFT for human architect approval. This defines **what "correct" means** for every later
> phase — the load-bearing semantic core (master-plan §6.2/§6.3, phase-spec C2.6). Nothing in batch 2
> is built until this is approved. Decisions the human owns are marked **[DECISION]**.
> Caveman-encoded (spec doc). `!`=must `⊥`=never `∀`=for all `∃`=exists `→`=leads to.

---

## §0 Scope
Batch 2 deliverables: fault-model envelope · history recorder · checker framework · initial checker set ·
toy gate system (replicated counter) · harness-has-teeth test · node lifecycle (C2.3) · buggify (C2.4) ·
workload gen (C2.5) · seed-burn + one-command replay (C2.7) · shrinking (C2.8).
Providers (SimNetwork/SimDisk) + mutation engine already landed (batch 1).

## §1 Fault-model envelope — THE safety boundary
The sim may inject EXACTLY this set. Phase 7 prod ! not exhibit faults outside it, ⊥, OR envelope widened + affected protocols re-verified (master-plan §7). This list IS the verified-against-reality contract.

```
network : delay · reorder · duplicate · drop · partition(arbitrary subset)+heal
disk    : latency · io-fault · torn-write(partial page) · lying-fsync(ack≠durable) · bit-rot · crash-loses-unsynced
node    : crash · restart · kill   (crash discards in-flight non-durable state)
timing  : all fault timing ∈ virtual clock, derived from IRandom
```
- V-ENV1: ∀ injected fault → derived from `(seed)` via IRandom. ⊥ wall-clock, ⊥ nondeterministic source.
- V-ENV2: envelope is CLOSED — a fault not listed ⊥ injected. Adding one = spec change + human sign-off.
- **[DECISION-A]** batch-2 first gate (toy replicated counter): inject FULL envelope, or a named subset first then widen? Default: FULL envelope from the start (max scrutiny), partition+crash+reorder emphasized.

## §2 History recorder — what observable history is captured
Recorder ! capture a totally-ordered (by virtual time, tie-broken by a deterministic seq) log of OBSERVABLE events for offline checking (linearizability/Elle/differential reuse this in Phases 4/5).

Event shape (per client operation against system-under-test):
```
op : {client_id, op_id, kind(read|write|cas|...), args, invoke_vt, return_vt, result|error}
```
- V-HIST1: every client op records BOTH invoke_vt & return_vt (real-time order needed for linearizability later).
- V-HIST2: history = pure function of (seed). Same seed → byte-identical history.
- V-HIST3: recorder ⊥ perturb execution (observation ≠ interference); recording is passive.
- **[DECISION-B]** history granularity for batch 2: client-op-level only (invoke/return), OR also internal per-key version events (richer, enables Elle cycle-detection earlier but couples checker to storage internals)? Default: **client-op-level only** for batch 2 (keep checker decoupled; richer model deferred to Phase 4/5 where Elle lands).

## §3 Checker framework — contract
Pluggable checkers judge a run. Each checker = a written invariant + a verdict.
```
interface Checker:
  on_event(ev)         ? online incremental check during the run (optional)
  final(history) → Verdict{ok | violation{witness, explanation}}   ! required
  name, spec_ref       ! each checker cites the exact invariant it asserts (master-plan §6.3: "no stronger, no weaker")
```
- V-CHK1: ∀ checker → has a WRITTEN spec of the exact property it asserts. ⊥ vague checkers.
- V-CHK2: a violation → emits a WITNESS (the minimal observed evidence) + the seed → replayable.
- V-CHK3: checkers run during (online invariants) AND after (history checkers). Both.
- V-CHK4: checker asserts EXACTLY its level — ⊥ stronger (false alarms), ⊥ weaker (missed bugs).
- V-CHK5 (TEETH): ∀ checker → proven non-vacuous by a known-buggy fixture it MUST flag (the harness-has-teeth gate). A checker that passes a known-broken system IS the bug.

## §4 Initial checker set (batch 2 minimum — for the toy gate system)
**[DECISION-C RESOLVED → replicated KV REGISTERS]** Toy system = a replicated key→value register set
(ops `read(k)` · `write(k,v)` · `cas(k,old,new)`), N nodes, driven under the FULL fault envelope (§1).
KV-register correctness checkers (per key unless noted):
- C-INT (integrity): ∀ read → returns a value that was actually written (or the initial ∅). ⊥ fabricated/torn value. ∀ ack'd write → eventually observable; ⊥ lost ack'd write, ⊥ phantom write.
- C-MONO (read-your-writes / session monotonic): within a client session, a read ⊥ observe an EARLIER value than that session already observed/wrote for k.
- C-LIN (linearizability): ∃ a single total order of ALL ops consistent with per-op real-time (invoke_vt<return_vt across clients) + register semantics (read returns last write; cas atomic). This is the real linearizability-lite check (counter would have been weaker). Full Elle = Phase 4; batch-2 = register-specific linearization search.
- C-DUR (durability): after node crash+recover, every write ack'd-before-crash AND truly-durable → still present; lying-fsync'd → MAY be absent (envelope-permitted), but ⊥ a NON-ack'd write appear, ⊥ a committed value silently change.

## §5 Toy gate system — replicated KV register set spec
- N logical nodes (node lifecycle C2.3), each an actor set on the deterministic scheduler.
- State: a small map key→value (few keys). Ops: `write(k,v)` (exactly-once if ack'd), `read(k)`, `cas(k,old,new)` (atomic compare-and-set).
- Replication: simplest correct scheme that survives the envelope (NOT consensus yet — Phase 4). Batch-2 goal is
  to exercise the HARNESS, not build production replication. A deliberately-simple replicator (e.g. single-leader +
  crude failover, or quorum-less primary-backup) is fine; its job is to be a system the checkers + faults stress.
- V-TOY1: under FULL fault envelope + seed-burn → C-INT, C-MONO, C-LIN, C-DUR hold (or a real bug found & reported reproducibly).
- V-RKV1 (coroutine pointer-stability): ∀ server coroutine on the deterministic scheduler → ⊥ hold a pointer/reference INTO a node's `store` (or any vector that another coroutine on the same node can grow) ACROSS a `co_await`. A suspension lets a concurrent op `insert`/reallocate → dangling read. ! re-fetch the Entry AFTER any await; use it only within one synchronous span. (Backprop B-seedburn-uaf.)

## §6 Harness-has-teeth (the batch-2 GATE proper)
- V-TEETH1: feed the checker set a DELIBERATELY-BROKEN KV system (e.g. drops a write on partition, serves a stale read, skips the cas compare) via `run_kv_sim_with` → checkers MUST flag it, with witness + replayable seed. A green run on a known-buggy system halts batch 2.
- V-TEETH2: mutation testing of the harness/checkers themselves passes (the checkers' own tests have teeth).
- V-TEETH3: every induced failure auto-shrinks (C2.8) to a minimal fault schedule + replays byte-identically.

### §6.1 [DECISION-E RESOLVED — accept non-consensus limit (human gate, 2026-06-22)]
Toy KV system is non-consensus (spec §5) → inherently non-linearizable under leader failover. ∴ batch-2 honest-system gate:
- ASSERT (must hold, achievable without consensus): C-DUR durability (post WAL-CRC fix) + C-INT no-fabrication (no INT-1 fabricated value, no DUR-2). A fabrication/durability violation HALTS.
- TRACK + report reproducibly (⊥ fail the gate): expected failover anomalies — C-LIN non-linearizable, C-MONO read-your-writes, C-INT/INT-2 lost-ack. These are the documented non-consensus limit; real linearizability-under-failover is earned in Phase 4 (consensus).
- BATCH-2 GATE PROPER = §6 harness-has-teeth (V-TEETH1 buggy-system flagged) + seed reproducibility (V-SEED) + mutation-of-harness (V-TEETH2). NOT honest-system-passes-everything.
- Found-bug record: WAL torn-value fabrication FIXED (commit per-record CRC-32 + recover-to-prefix); honest sweep C-DUR 4→0, no fabrication. See [[backprop-wal-torn-fabrication]].
- Found (C2.7 seed-burn + ASan, honest sweep): USE-AFTER-REALLOC in ReplicatedKvSystem::on_client_request — an `Entry*` into `nd.store` was held across the ColdRead-buggify `co_await clock_->delay(1)`; a concurrent same-node commit `apply_record→store.insert` reallocated the vector → dangling read (ASan container-overflow). FIXED: await first, then re-fetch the Entry after any suspension (no Entry* survives a `co_await`). New invariant V-RKV1. See [[backprop-seedburn-uaf]].
- Found (C2.7 seed-burn, 3000-seed honest sweep): MUST-HOLD class is exactly INT-1 + **DUR-2** (storage-manufactured value) — both 0/3000 ✓. C-DUR/**DUR-1** ("rejected-write-surfaced") is NOT a sound honest must-hold: the toy workload tags values `c<client>_v<i>`, so one token can reach a key via BOTH a rejected cas (new-value) AND a legitimate path (lost-ack write, or a committing cas whose `cas_old` proves the register HELD it). DUR-1's provenance check sees only the rejected source → false-alarms (~1%/seed). ∴ DUR-1 is TRACKED, not asserted (seedburn::ViolationClass splits dur_fab2 vs dur_rejected). Real fix = globally-unique value tokens / version provenance, Phase 4. See [[backprop-dur1-token-reuse]].

## §7 Seed / replay / shrinking contract
- V-SEED1: every run logs its seed; one-command replay reproduces byte-identically (extends Phase 1 infra).
- V-SEED2: seed-burn farm (C2.7) runs parallel seeds continuously; any failing seed stored + surfaced.
- V-SHRINK1: on failure, fault schedule auto-reduced to a minimal reproducing case (C2.8); minimal case replays identically.

## §8 Build-order within batch 2 (proposed)
1. history recorder + checker framework API (§2,§3) — spec-anchored.
2. toy replicated counter + node lifecycle (§5, C2.3).
3. initial checker set (§4) + workload gen (C2.5) + buggify (C2.4).
4. harness-has-teeth test (§6) — BEFORE trusting the harness.
5. seed-burn farm (C2.7) + shrinking (C2.8).
Then: batch-2 gate = §6 green + full envelope seed-burn + mutation-of-harness PASS.

## §9 Decisions — RESOLVED (human gate, 2026-06-22)
- [DECISION-A] fault scope first gate: **FULL envelope from the start.**
- [DECISION-B] history granularity: **client-op-level** (invoke/return); internal version events deferred to Phase 4/5.
- [DECISION-C] toy system: **replicated KV registers** (read/write/cas) — exercises C-LIN harder than a counter.
- [DECISION-D] authorship for teeth: **separate agents** — toy-system impl, checker set, and known-buggy fixture each by a DIFFERENT agent (independence → real teeth).
