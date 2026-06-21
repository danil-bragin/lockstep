# Lockstep — Detailed Phase Specs (Phases 0–7)

> Companion to `lockstep-master-plan.md`. Each phase is a ticket at the depth agents execute against.
> Operating model: **agents write all code; trust comes from mechanical verification, not human review.**
> Universal merge gate (every PR, every phase): compiles → lint + static analysis + ASan/TSan/UBSan/MSan →
> deterministic simulation battery → conforms to formal spec where one exists → applicable linearizability checks →
> mutation score not regressed. Core-protocol behavior changes require the TLA+ spec updated + re-model-checked **first**.
> Briefing format for any agent task: (a) interface header / formal spec, (b) invariants to preserve, (c) named gate scenarios to pass.

---

## Phase 0 — Scaffolding

**Goal.** Stand up the repository, build, CI, the verification skeleton, and the abstraction-boundary interface headers.
Nothing functional yet — but the gates that govern everything else exist and pass.

**Owner / agents.** Fully agent-executable.

**Components.**
- **C0.1 Build.** CMake, C++23 toolchain pinned. Dependency policy: minimal, vendored where reasonable; every external
  dependency justified in writing and scanned. No dependency may call the clock/network/disk on behalf of core code.
- **C0.2 CI pipeline.** Jobs: debug+release build matrix; ASan, TSan, UBSan, MSan; clang-tidy; clang static analyzer;
  coverage; mutation-testing runner (skeleton); **forbidden-call lint** that greps the whole tree (outside `providers/`)
  for `std::chrono`, `std::rand`, `std::random_device`, `std::thread`, raw socket/file syscalls, coordination atomics.
- **C0.3 Abstraction-boundary headers (interfaces only).** `IClock`, `INetwork`, `IDisk`, `IRandom`, `IScheduler`.
  These are the contract every later layer codes against; bodies arrive in Phases 1–2 and 7.
- **C0.4 Repo layout.**
  `core/` (runtime; depends on nothing above it) · `providers/{sim,prod}/` · `storage/` · `log/` · `txn/` · `query/` ·
  `harness/` (sim driver, fault injectors, checkers) · `specs/` (TLA+) · `tests/` · `bench/`.
- **C0.5 Standards + gate-as-code.** Coding standards doc; the merge gate encoded directly in CI so it cannot be bypassed.

**Gate.** CI green on a trivial test; every sanitizer job runs; forbidden-call lint active and passing; the
mutation-testing runner executes end-to-end (even if it has almost nothing to mutate yet).

**Done.** A fresh clone builds, all CI jobs run and pass, and a deliberately planted forbidden call (e.g. a stray
`std::chrono::system_clock::now()`) is rejected by the lint.

---

## Phase 1 — Deterministic Runtime Core

**Goal.** The minimal deterministic execution substrate: coroutine async model, single-thread deterministic scheduler,
virtual clock. At phase end the system runs cooperative coroutines exchanging values through futures with
**byte-identical, reproducible behavior from a seed**. No storage, network, or DB logic yet.

**Owner / agents.** Core semantics are spec-anchored and dual-reviewed; agents implement against tight specs and may
own the event-trace recorder, property tests, fuzzers, and the forbidden-call lint. Agents may **not** alter scheduler
dequeue semantics, coroutine suspend/resume, or the clock-advance rule without a spec change.

**Components.**
- **C1.1 `Future<T>` / `Promise<T>`.** `Promise` is the write end; `Future` is awaitable (`co_await`). Fulfilling a
  promise **schedules** the waiter onto the ready queue (never resumes inline). Support `Future<void>` and error completion.
  ```cpp
  template <class T> class Future;   // awaitable
  template <class T> class Promise;  // set_value(T) / set_error(Error)
  class Task;                        // coroutine return type
  ```
- **C1.2 `Task`.** Return type of every coroutine. On `co_await` of a not-ready future it suspends and yields to the
  scheduler. Task completion may fulfill a future (tasks compose).
- **C1.3 Deterministic scheduler.** Single OS thread, cooperative, no preemption. Owns a ready queue with a
  **deterministic, documented dequeue order** (FIFO or a deterministic monotonic-key priority — never pointer order).
  `run()`: while work exists, resume the next continuation; when the ready queue is empty but timers pend, advance
  virtual time to the earliest timer and fire it. Exposes `spawn(Task)`.
- **C1.4 Virtual clock (`IClock`).** `now()` returns virtual ticks (not wall-clock). `delay(d)` returns `Future<void>`
  completing when virtual time advances by `d`. Virtual time advances **only** when no continuation is ready.
- **C1.5 `IRandom`.** A single seeded PRNG — the only randomness source in the entire system.

**Determinism requirements.** No wall-clock anywhere. No real threads/locks/coordination-atomics. Ready-queue order
deterministic and documented. The whole run is a pure function of `(seed, initial tasks)`. All randomness via `IRandom`.

**Gate — deterministic ping-pong.** Two coroutines exchange a token N times through futures, with `clock.delay()` between
exchanges. The scheduler records an event trace. Assertions: (1) same seed ⇒ byte-identical trace; (2) different seed ⇒
valid but different trace where randomness applies; (3) final virtual time and token count exactly as predicted. Runs in
CI under all sanitizers; every run logs its seed; a failing seed replays identically.

**Done.** C1.1–C1.5 implemented; gate green across many seeds in CI under sanitizers; a forced bug is reproducible
byte-for-byte from its seed; no wall-clock / real thread / raw randomness anywhere (lint-enforced).

---

## Phase 2 — Simulation Harness + Fault Injection

**Goal.** Build the deterministic world: simulated providers, fault injection, checkers, and seed infrastructure.
**This phase produces the merge gate for everything after it** — it is treated as the product.

**Owner / agents.** Human-led on the core sim and checker semantics (they define what "correct" means); agents own
additional fault injectors, workload generators, shrinking, and dashboards.

**Components.**
- **C2.1 Sim `INetwork`.** In-memory message bus. Per-link controllable latency, reordering, duplication, drop, and
  partition (with heal/unheal), all derived from `IRandom` + virtual clock.
- **C2.2 Sim `IDisk`.** In-memory storage model with injected latency, IO faults, **torn writes** (partial page),
  **lying fsync** (ack-before-durable, then lose on crash), optional bit-rot. Crash drops un-fsynced data per real semantics.
- **C2.3 Node lifecycle.** Spawn / crash / restart / kill logical "machines" (each a set of actors). Crash discards
  in-flight non-durable state exactly as a real crash would.
- **C2.4 `buggify`.** Injection points compiled into real code, **active only in simulation**, seed-driven, that force
  rare branches (slow paths, allocation failure, extra retries, delayed delivery).
- **C2.5 Workload generator.** Drives operations against the system under test; fully deterministic and seeded.
- **C2.6 Checker framework.** Pluggable invariant/property checkers run during and after a run; a history recorder
  captures the observable history for offline checking. Each checker has a written spec of the exact invariant it asserts.
- **C2.7 Seed infrastructure.** Seed logging, one-command replay, and a **seed-burn farm** runner (parallel seeds,
  continuous burning) that surfaces and stores any failing seed.
- **C2.8 Shrinking.** On failure, automatically reduce the fault schedule to a minimal reproducing case.

**Determinism requirements.** The whole simulation is a pure function of its seed; the entire fault schedule derives from
`IRandom`; partition/heal/crash events are scheduled on virtual time. No wall-clock.

**Gate.** A multi-node toy actor (e.g. a tiny replicated counter) survives randomized fault storms; every induced failure
is reproducible byte-identically and auto-shrinks to a minimal case. **Harness-has-teeth test:** feed the checkers a
deliberately broken toy implementation and confirm they flag the violation — a harness that passes a known-buggy system
is itself the bug. Mutation testing of the harness/checkers passes.

**Done.** Sim providers + fault injection + checkers + seed farm operational; teeth-test passes; shrinking works;
dashboards show seed-burn results. From here, the universal merge gate is real.

---

## Phase 3 — Storage Engine (single node)

**Goal.** A persistent, crash-consistent, MVCC key-value engine on one node, running entirely inside the sim.

**Owner / agents.** Agents implement against the storage spec + invariants; benchmark-driven tuning (empirical D4) is agent work.

**Components.**
- **C3.1 WAL.** Append-only through `IDisk`; group commit; recovery replay.
- **C3.2 Memtable.** In-memory sorted structure (skiplist) holding MVCC versions.
- **C3.3 SSTable.** Immutable sorted runs; block format; per-table bloom filters; sparse index.
- **C3.4 Compaction.** Leveled or tiered (decided **empirically** via the benchmark harness, not now); background, but
  in sim it runs on the deterministic scheduler.
- **C3.5 MVCC.** Versioned keys; snapshot reads at a version; old-version GC under a watermark.
- **C3.6 WiscKey separation.** Large values to a separate value log above a size threshold, to cut write amplification.
- **C3.7 Benchmark harness.** Measures write/read throughput, write amplification, space amplification across configs —
  this is how D4 tuning is settled.
- **C3.8 Crash recovery.** Rebuild a consistent state from WAL + SSTables after a simulated crash at **any** point.

**Contracts.** Durable after fsync; a crash at any instruction recovers to a consistent **prefix**; snapshot reads never
observe partial transactions; no torn-write corruption survives recovery.

**Gate.** Under sim disk faults (torn writes, lying fsync, crashes at arbitrary points): crash-consistency holds and the
recovered state is always a valid prefix; MVCC snapshot correctness under concurrent writers; **differential test** vs a
trivial in-memory map oracle; mutation score ≥ threshold.

**Done.** Engine survives the disk-fault storm; recovery always lands on a valid prefix; benchmark harness produces the
data that fixes D4 tuning; all checks green.

---

## Phase 4 — Replicated Log / Consensus

**Goal.** A fault-tolerant, **linearizable** replicated log per shard, plus the sequencer that establishes the global
total order. This is the heart of the system and gets the highest scrutiny.

**Owner / agents.** No freelancing zone. **TLA+ spec model-checked first.** Agents implement strictly against the
verified spec; a **dual independent implementation** is maintained and cross-checked.

**Components.**
- **C4.1 Consensus per replica-group** (Raft or chosen variant): leader election, log replication, commit index;
  term/vote/log persisted through `IDisk`.
- **C4.2 Membership change** (joint consensus or single-server change) — spec'd and checked.
- **C4.3 Snapshotting / log compaction** for the replicated log.
- **C4.4 Sequencer.** How per-shard logs combine into a global deterministic total order (Calvin-style epoch batching:
  a deterministic merge of partitioned input logs by epoch). This total order is the linearization point used downstream.
- **C4.5 Recovery / failover.** A rejoining replica replays snapshot + log; leader failover never loses a committed entry.
- **C4.6 Dual implementation.** Two agent-built implementations from the same spec, cross-checked against each other and the oracle.

**Formal spec (TLA+, model-checked in TLC).** Leader uniqueness per term; log matching; state-machine safety (no committed
entry lost or reordered); linearizability of the log.

**Gate.** Spec model-checked with all safety invariants holding. Under partition + crash + message-reorder storms in sim:
never two leaders in a term; no committed write lost or reordered; linearizability checker passes; both implementations
agree on every history; mutation score ≥ threshold.

**Done.** Model-check clean; fault-storm survival with linearizability intact; dual implementations agree; the global
total order is available to Phase 5.

---

## Phase 5 — Distributed Transactions + Consistency Contract

**Goal.** Cross-shard **one-shot** transactions with deterministic commit, plus the D5 per-operation guarantee selector.

**Owner / agents.** **TLA+ spec of the deterministic commit ordering, model-checked first.** Agents implement against it;
the per-level consistency checkers are first-class deliverables.

**Components.**
- **C5.1 Transaction submission.** One-shot function form (parameters in / result out). No interactive sessions (D1).
- **C5.2 Deterministic ordering.** Transactions are sequenced into the global order (Phase 4); every node executes the
  agreed order deterministically. No 2PC, no distributed deadlocks.
- **C5.3 OLLP reconnaissance reads.** Predict the read/write set with a cheap read pass; submit as a unit; detect set
  change at execution and retry. Retry semantics spec'd: bounded retries, starvation avoidance.
- **C5.4 D5 selector.** `strict-serializable` (default), `snapshot`, `bounded-staleness` (log-lag), `read-your-writes`.
  The chosen level is encoded in the API/type **at the call site** so it cannot be silently misused.
- **C5.5 Read path.** Snapshot reads at a version; bounded-staleness reads routed to local replicas with a log-lag check;
  session tracking for read-your-writes.
- **C5.6 Abort/retry semantics.** Deterministic; clearly defined what triggers a retry vs a surfaced error.

**Formal spec.** Strict serializability of the default path (sequencer order is the linearization point); the exact
guarantee of each relaxed level; no lost or duplicated transaction effects under faults.

**Gate.** Spec model-checked. Strict-serializable verified by an Elle/linearizability checker under fault storms.
**Each D5 level** verified by its own checker to honor exactly its contract — not stronger, not weaker. OLLP retry
terminates with no starvation under contention. Differential vs oracle. Mutation score ≥ threshold.

**Done.** Commit spec model-checked; default path proven strict-serializable under faults; every relaxed level proven to
match its stated contract; the controllable-guarantee killer feature is real and verified.

---

## Phase 6 — Query / Protocol Layer + Breadth

**Goal.** The developer-facing surface: the new transaction/query model (D3), wire protocol, drivers, tooling, docs.
**Agents go wide here** — this is the breadth zone where large agent fan-out pays off.

**Owner / agents.** Wide agent autonomy under the gate.

**Components.**
- **C6.1 Transaction-function model.** How users author one-shot transaction functions (a defined embedding/language).
  User transaction code must itself be deterministic — the runtime sandboxes or rejects nondeterministic constructs.
- **C6.2 Read/query language.** Typed, composable, non-SQL surface; a planner mapping queries to versioned storage reads;
  the D5 consistency selector exposed safely in the language/API.
- **C6.3 Wire protocol.** The client network protocol (new; Postgres-wire shim explicitly deferred to a later adoption track).
- **C6.4 Drivers / SDKs.** One reference-language driver first, then fan out across languages (agents).
- **C6.5 CLI + admin tooling.**
- **C6.6 Docs, examples, conformance suite** for the transaction/query semantics.

**Requirements.** User transaction functions remain deterministic (enforced by the runtime). The query surface exposes
D5 levels only through the safe, call-site-visible mechanism. Everything still runs under the sim.

**Gate.** Conformance suite passes; transaction/query semantics differential-tested vs oracle; full system still green
under sim + sanitizers; mutation score held; fuzzing on the wire protocol and the query parser.

**Done.** A developer can author and run one-shot transactions and queries against a node; drivers + CLI + docs exist;
the whole stack still survives the fault-storm gate.

---

## Phase 7 — Production Providers + Hardening

**Goal.** Real-world execution behind the same interfaces, plus operability and the trust-building that no amount of
agent labor can shortcut.

**Owner / agents.** Agents implement prod providers + ops tooling; the human signs off on real-cluster chaos results.

**Components.**
- **C7.1 Prod `INetwork`.** io_uring/epoll, real sockets, the real **thread-per-core, shared-nothing reactor** —
  cross-core only via message passing, no shared mutable state, no locks.
- **C7.2 Prod `IDisk`.** Async file IO on NVMe via io_uring; real fsync/durability.
- **C7.3 Prod `IClock` / `IRandom`.**
- **C7.4 Thread-per-core runtime.** Each core runs the identical actor logic from sim; only the providers and the
  executor differ. The DB logic is **the same code** that was verified in simulation.
- **C7.5 Observability.** Metrics, tracing, structured logs, dashboards.
- **C7.6 Ops.** Deployment, backup/restore, membership operations, rolling upgrade path.
- **C7.7 Real-cluster chaos + soak.** Jepsen against the live cluster; long-running soak; performance validation against targets.

**Requirements (critical).** Prod providers must satisfy the contracts the sim providers assumed. In particular, the
real disk must actually guarantee what the sim's worst case modeled (e.g. fsync must truly be durable). The sim's fault
model is the safety envelope: **prod must not exhibit faults outside the modeled set, or the model must be widened and the
affected protocols re-verified.**

**Gate.** Prod providers pass the **identical** actor/integration tests the sim providers passed; real-cluster Jepsen finds
no linearizability violations; extended soak is stable; performance targets met.

**Done.** The same verified logic runs on real hardware; live Jepsen is clean; soak is stable; the system is ready to begin
earning the one thing agents can't generate — trust, in calendar time, under real failures.

---

## Cross-phase notes

- **Order is load-bearing.** Verification machinery (Phases 0–2) precedes everything it must judge. Do not start Phase 3+
  features until the Phase 2 gate is real.
- **Specs before core code.** Phases 4 and 5 do not write implementation before their TLA+ spec is model-checked.
- **Mutation score is the adequacy metric**, not coverage — especially because the tests are agent-written.
- **The human gates by dashboard:** model-check results, seed-burn outcomes, linearizability findings, mutation scores,
  conformance and soak. Not line-by-line code.
