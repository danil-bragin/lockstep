# Lockstep — Master Execution Plan (v2, complete)

> Working name: **Lockstep** (rename = find-replace one token).
> This document supersedes the earlier draft. It is self-contained and built for the operating model below:
> **AI agents write all code. The human writes nothing — they architect, specify, and gate.** Read §2 and §4 before any task.

---

## 1. North Star

Lockstep is a from-scratch, distributed, transactional database in modern C++. It is **not** "Postgres but faster."
Its moat is a stack of bets incumbents cannot retrofit:

1. **Deterministic-simulation-first** — the whole cluster runs single-threaded and reproducibly, with injected faults.
2. **Thread-per-core, shared-nothing runtime**, built from scratch (no Seastar).
3. **Log-centric** — a replicated, totally-ordered log is the source of truth and the coordination point.
4. **Controllable guarantees as a clean contract** — strict-serializable by default; every operation can explicitly
   downgrade under one coherent mental model. **This is the killer feature.**

Target: general high-throughput OLTP, distributed transactions, high and tunable safety guarantees.

---

## 2. Operating Model — agents write everything

The human does not hand-write code. Therefore correctness cannot rest on human code review. It rests on
**mechanical verification**: a protocol is only trusted because a model checker proved it, and an implementation
is only trusted because it survives an overwhelming, adequacy-proven test battery.

- **Human role:** owns the formal specifications, the invariants, and the consistency contracts; reviews
  verification *results* (model-check output, sim seed-burn dashboards, linearizability findings, mutation scores);
  approves phase gates; steers agents. Reads specs and dashboards, not every line of code.
- **Agent role:** writes all implementation and all tests, against verified specs and stated invariants.
- **Trust source:** model checking + exhaustive deterministic simulation + linearizability checking +
  mutation-proven tests — never "a human looked at it."

Honest risk note: building a correct distributed transactional DB entirely via agents is unproven at this scale.
The verification battery in §6 is the *only* thing standing between this project and silent data corruption.
It is therefore built **first** and treated as the actual product during early phases. If the battery is weak, nothing else matters.

---

## 3. Locked Decisions (all five answered)

- **D1 — Commit model: deterministic execution (Calvin-style). LOCKED.**
  No 2PC, no distributed deadlocks; contention does not cause abort storms. **Transactions are one-shot units** —
  submitted as a whole function (parameters in / result out), not interactive `BEGIN…COMMIT` sessions. Dynamic
  read/write sets use reconnaissance reads (OLLP): a cheap read pass predicts the set, the transaction is submitted
  as a unit, and retries if the set changed at execution. Interactive transactions are intentionally unsupported.

- **D2 — Clock strategy: pure log-order. LOCKED.**
  Ordering and strict serializability come from the deterministic sequencer order, not clocks. Wall-clock is advisory
  only (TTLs, observability). Bounded-staleness is expressed as log-position lag. PTP/cloud time sync is a possible
  later freshness optimization, never a correctness dependency.

- **D3 — Query / transaction model: new model; transaction = whole unit. LOCKED.**
  Transactions are self-contained deterministic functions submitted to the DB (forced by D1). Read/query surface
  designed in Phase 6 (typed, composable, non-SQL). Postgres-wire compat deferred as a possible later adoption shim,
  not foundational.

- **D4 — Storage: LSM-tree + MVCC, key-value/row encoding. LOCKED (shape); tuning empirical.**
  Write path = sequential append (memtable + WAL) → SSTables → background compaction; reads = non-blocking snapshot
  at a version (MVCC). WiscKey-style key/value separation for large values to cut write amplification. Columnar
  storage is **out of scope for v1** (revisit only if an analytical path is added later). Concrete tuning
  (level sizing, compaction strategy, bloom filters, block size) is decided **empirically** via the benchmark harness, not guessed now.

- **D5 — Consistency menu. LOCKED.** Over a strict-serializable default, the per-operation menu is exactly:
  1. **Strict serializable** (default) — sequencer order is the linearization point.
  2. **Snapshot** — internally consistent read at a chosen version; no real-time guarantee.
  3. **Bounded-staleness read** — from a local replica, staleness bounded and expressed as log-position lag.
  4. **Read-your-writes (session)** — a session always observes its own prior writes.
  Contract principle: strong by default; every downgrade is an explicit per-operation opt-in with a precisely
  stated guarantee, made **visible at the call site** (encoded in the API/type so it cannot be silently misused).

---

## 4. Cardinal Rules (every agent obeys; violations fail the gate)

1. **All nondeterminism flows through the abstraction boundary** (`Clock`, `Network`, `Disk`, `Random`, `Scheduler`).
   No direct syscalls, `std::chrono`, raw sockets, raw file IO, `std::rand`, threads, or coordination-atomics outside provider impls.
2. **The verification battery is the gate.** Nothing merges unless it passes §6 in full.
3. **Seed-reproducibility is sacred.** Every run logs its seed; every failure replays byte-identically.
4. **Conform to the spec.** Where a formal spec exists (§6.1), the implementation must conform; changing core-protocol
   behavior requires updating and re-model-checking the spec *first*.
5. **Tests must have teeth.** New code ships with tests, and the mutation score must not regress below threshold.
6. **Modern C++ discipline.** C++23, RAII, no raw owning pointers, no naked `new`/`delete`, coroutines for async.
7. **No determinism leaks.** No order-dependent `unordered_map` iteration; no behavior dependent on pointer addresses;
   no background OS thread touching shared state in sim.

---

## 5. Architecture

```
        Transaction functions (one-shot)         ← submitted as units; D1/D3
                       │
        Query / consistency contract             ← per-op guarantee selector; D5
                       │
        Distributed transaction layer            ← deterministic ordering; recon (OLLP)
                       │
        Replicated log / sequencer               ← total order, source of truth; D2
                       │
        Sharded storage: LSM + MVCC              ← D4
                       │
        Deterministic actor runtime              ← Future/Promise on coroutines
                       │
        Abstraction boundary  ─┬─ Prod providers (io_uring/NVMe/sockets, reactor)
                               └─ Sim providers  (virtual time, fault injection, 1 thread, seeded)
```

---

## 6. Verification Strategy (the heart — "more guarantees, more tests")

Built first, kept load-bearing forever. Each technique catches a different bug class; together they replace human code review.

### 6.1 Formal specification + model checking
- Every protocol (consensus, deterministic commit ordering, recovery, membership change) is specified in **TLA+/PlusCal**
  and model-checked with **TLC** *before* implementation.
- Agents implement against the verified spec; the spec is the source of truth for behavior.
- **Catches:** protocol-level design bugs — the deadliest, least-testable class.

### 6.2 Deterministic simulation (primary runtime defense)
- Whole cluster runs single-threaded, seeded, with `buggify` fault injection everywhere (network: delay/reorder/drop/partition;
  disk: latency/faults/torn writes/lying fsync; nodes: crash/restart). Continuous seed-burning in CI + a dedicated seed-burn farm.
- **Catches:** implementation bugs under concurrency, partition, and crash interleavings. Every finding is reproducible from its seed.

### 6.3 Linearizability / isolation checking
- Generate concurrent transaction histories; check them against the claimed level using **Elle**-style (transactional anomaly
  detection) and **Jepsen**-style harnesses. One checker per D5 level, asserting exactly that level — no stronger, no weaker.
- **Catches:** violations of the consistency contract.

### 6.4 Differential testing against a reference oracle
- A deliberately trivial, obviously-correct single-node model is the oracle. The real system's observable behavior must
  match it under the linearizability mapping.
- **Catches:** divergence between intended and actual semantics.

### 6.5 Dual independent implementations of critical components *(enabled by cheap agent labor)*
- For the highest-risk pieces (log/consensus, commit protocol), have two agent-built implementations from the same spec,
  cross-checked against each other and the oracle.
- **Catches:** shared-blind-spot bugs a single implementation plus its own tests would both miss.

### 6.6 Property-based + metamorphic testing
- Properties and metamorphic relations over the whole API, randomized.
- **Catches:** edge cases nobody enumerated.

### 6.7 Mutation testing (meta-gate — mandatory because tests are agent-written)
- Inject deliberate bugs (mutants); the suite must kill them. Track **mutation score**; the merge gate fails on regression.
- **Catches:** weak/vacuous tests — coverage theater. This is what proves agent-written tests actually work.

### 6.8 Always-on hygiene
- Assertion-dense code (invariants asserted inline, checked in sim). Sanitizers (ASan/TSan/UBSan/MSan) on all runs.
- Fuzzing on every parser/input boundary. Coverage tracked (necessary, not sufficient; mutation score is the real adequacy metric).
- Soak + chaos on real clusters (Phase 7).

---

## 7. Phased Plan (verification-machinery-first; each gate = the §6 battery as applicable)

### Phase 0 — Scaffolding
Build (CMake, C++23), CI with all sanitizers + lint + static analysis, repo layout, abstraction-boundary interface headers.
**Gate:** CI green on an empty test; all sanitizer jobs wired; forbidden-call lint active.

### Phase 1 — Deterministic runtime core
`Future`/`Promise` on coroutines, single-thread deterministic scheduler, virtual clock, `IRandom`.
(Worked example of a per-phase ticket lives in `lockstep-phase1-spec.md` — use it as the template for expanding every later phase.)
**Gate:** deterministic ping-pong reproduces byte-identically from a seed, in CI, under sanitizers.

### Phase 2 — Simulation harness + fault injection *(this IS the merge gate for everything after)*
Sim network/disk providers, `buggify` hooks, invariant-checker scaffolding, seed logging + one-command replay, seed-burn farm.
**Gate:** a multi-node toy actor survives randomized fault storms; every failure reproducible; mutation testing of the harness itself passes.

### Phase 3 — Storage engine (single node)
LSM + MVCC + WAL + compaction; WiscKey-style large-value separation. Benchmark harness for empirical D4 tuning.
**Gate:** crash-consistency under sim disk faults; MVCC snapshot correctness under concurrent writes; differential test vs oracle; mutation score above threshold.

### Phase 4 — Replicated log / consensus
TLA+ spec model-checked first. Consensus group per shard; the sequencer / total order. Dual implementation + cross-check (6.5).
**Gate:** spec model-checked; linearizability checker passes under partition+crash storms; never two leaders; no committed write lost.

### Phase 5 — Distributed transactions + consistency contract
TLA+ spec for the deterministic commit ordering. Cross-shard one-shot transactions; OLLP recon; the D5 per-operation selector.
**Gate:** spec model-checked; strict-serializable verified by checker; each D5 relaxed level verified to honor exactly its contract.

### Phase 6 — Query / protocol layer + breadth *(agents go wide)*
The transaction-function model and read/query surface (D3), drivers, client libs, CLI, tooling, docs.
**Gate:** conformance suite; full system still green under sim + sanitizers; mutation score held.

### Phase 7 — Production providers + hardening
io_uring/NVMe/socket providers behind the same interfaces; observability; ops tooling.
**Gate:** prod providers pass the identical actor tests sim providers passed; extended soak + chaos on a real cluster.

---

## 8. Agent Orchestration Policy

- **Briefing format for any task:** (a) the relevant interface header / formal spec, (b) the invariant(s) the work must
  preserve, (c) the named §6 scenarios it must pass. Never "make it fast" — always "pass test X under the sim."
- **Merge gate (every PR, no exceptions):** compiles → lint + static analysis + sanitizers → deterministic sim battery →
  conforms to formal spec where one exists → passes applicable linearizability checks → mutation score not regressed.
- **Core protocols (log/consensus, commit):** spec change + re-model-check required *before* implementation changes; dual implementation maintained.
- **Human checkpoints:** approve each formal spec; approve each phase gate; review the seed-burn / linearizability / mutation dashboards. No line-by-line code review required — the battery is the reviewer.
- **Parallelism:** fan agents out widely on Phase 6 breadth and on test/fuzzer authoring; keep core-protocol work to small, spec-anchored, dual-tracked changes.

---

## 9. Definition of "Green"

A phase is done only when: its formal spec (if any) is model-checked; its gate passes deterministically across a large
seed sweep in CI under all sanitizers; applicable linearizability and differential checks pass; the mutation score is at
or above threshold; every failure seen is reproducible from a logged seed; and the human has approved the phase gate from the dashboards.
"It builds and the happy path works" is never done. The test battery is the product until Phase 6.
