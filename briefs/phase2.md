# Lockstep — Phase 2 Brief Batch (Simulation Harness + Fault Injection)

> Source of truth: `lockstep-phase-specs-all.md` Phase 2 (C2.1–C2.8 + Gate + Done), master-plan §6.2/§6.8.
> **Phase 2 IS the merge gate for everything after it — build carefully; harness-has-teeth BEFORE trusting it.**
> Plan: human-led on core SIM + CHECKER semantics (they define "correct"); agents own extra fault injectors,
> workload gen, shrinking, dashboards. Mutation engine FRONT-LOADED (closes the Phase-1 vacuous-mutation debt).

## Sequencing — two batches (order is load-bearing)
- **Batch 1 (DISPATCH NOW — foundations, well-specified, disjoint):** M1 mutation engine · S1 sim INetwork · S2 sim IDisk.
  These build on Phase 0 boundary headers + Phase 1 runtime; they are the substrate the checkers will later judge.
- **Batch 2 (AFTER batch 1 lands + HUMAN gate on checker semantics):** node lifecycle (C2.3) · buggify (C2.4) ·
  workload generator (C2.5) · **checker framework + history recorder (C2.6) ← the "what correct means" core, human-gated** ·
  seed-burn farm + one-command replay (C2.7) · shrinking (C2.8) · the toy replicated-counter gate actor ·
  the **harness-has-teeth** test (feed checkers a known-buggy impl, confirm they FLAG it).

## Integration contract (batch 1)
- Sim providers are HEADER-ONLY, added under `providers/sim/include/lockstep/sim/` — auto-exposed by the existing
  INTERFACE lib `lockstep_providers_sim` (NO CMakeLists edit needed). providers/ is the lint-exempt zone.
- Test targets `sim_network_test` / `sim_disk_test` are PRE-WIRED in tests/CMakeLists.txt (orchestrator-owned).
  S1 creates `tests/sim_network_test.cpp`; S2 creates `tests/sim_disk_test.cpp`. Agents DO NOT edit tests/CMakeLists.txt.
- Providers take a `Scheduler&` (for timers/delivery on virtual time) + `IRandom&` (fault schedule). All faults derive
  from IRandom + virtual clock. Implement the Phase-0 INetwork / IDisk interfaces.
- M1 owns `tools/mutation/**` ONLY. gate.sh already invokes `run_mutation.py` — replace its internals, keep the CLI.

---

## BRIEF M1 — real C++ mutation engine  (DISPATCH NOW; closes Phase-1 debt)
Phase: 2 · Owner mode: agent-autonomous · Pool: 1.
Inputs: tools/mutation/run_mutation.py (skeleton); master-plan §6.7 (mutation = THE adequacy meta-gate); the built ctest suite.
Build: a real source-mutation runner (python3 stdlib; may shell out to clang++/ctest). Generate mutants by applying a
       documented operator set to the C++ sources under core/ + providers/sim (e.g. relational `< ↔ <=`, `== ↔ !=`,
       boolean `&& ↔ ||`, `+ ↔ -`, off-by-one in literals, delete-statement, negate-condition, return-value tweaks).
       For each mutant: rebuild the affected target + run the ctest suite; mutant KILLED if any test fails, SURVIVED if all pass.
       Report mutation score = killed/viable; SKIP non-compiling mutants (count separately). Threshold env-overridable
       (LOCKSTEP_MUTATION_THRESHOLD); regression fails the gate. Keep run time bounded (sample/scope to changed files;
       a `--full` vs `--sample` mode; document the cap LOUDLY — no silent truncation).
Invariants: a SURVIVED mutant means a test gap — reported, not hidden. Deterministic mutant set for a given source+seed.
       Must actually KILL a planted mutant (prove non-vacuous, like the lint teeth-test).
Forbidden: faking the score; std::random for mutant selection (seed it deterministically).
Gate: run against the current tree → produces a REAL score (>0 mutants); a deliberately weak test demo shows a survived
       mutant is reported; `bash scripts/gate.sh` still green (mutation stage now meaningful, not vacuous).
Deliverables: the runner + operator modules + a self-test proving it kills a known mutant + a short docs/mutation.md.
Done when: real score on core/+providers/sim runtime; a planted surviving-mutant is detected+reported; gate.sh green.

## BRIEF S1 — sim INetwork (C2.1)
Phase: 2 · Owner mode: spec-anchored · Pool: 1.
Inputs: core INetwork.hpp; Scheduler/SimClock + IRandom (Phase 1); spec C2.1; Example C (Phase-2 partition brief) as template.
Build: in-memory message bus implementing INetwork: per-link controllable latency, reordering, duplication, drop, and
       PARTITION (arbitrary server-subset split, with heal/unheal) — ALL derived from IRandom + virtual clock; delivery
       scheduled on the scheduler's timers. Deterministic.
Invariants: entire fault schedule derived from IRandom; partition/heal events scheduled on virtual time; NO message crosses
       a live partition; heal restores delivery deterministically; whole thing a pure function of (seed, inputs); byte-identical replay.
Forbidden: wall-clock; real sockets; nondeterministic containers in the schedule (no unordered iteration); std::*_distribution.
Gate (tests/sim_network_test.cpp): a multi-node toy exchange loses messages during a partition + recovers after heal,
       REPRODUCIBLY; same-seed run twice ⇒ byte-identical event trace; drop/reorder/dup/latency each demonstrably exercised
       and deterministic; builds + passes under debug/asan/tsan/ubsan; forbidden-lint clean on the test (providers exempt).
Deliverables: SimNetwork header(s) under providers/sim/include/lockstep/sim/ + tests/sim_network_test.cpp.
Done when: partition/heal + per-link faults deterministic + byte-identical on replay; ctest green under sanitizers; gate.sh green.

## BRIEF S2 — sim IDisk (C2.2)
Phase: 2 · Owner mode: spec-anchored · Pool: 1.
Inputs: core IDisk.hpp (note its explicit sync() durability barrier); Scheduler/SimClock + IRandom; spec C2.2.
Build: in-memory storage model implementing IDisk with injected: latency, IO faults, TORN WRITES (partial page),
       LYING FSYNC (ack-before-durable, then lose-on-crash), optional bit-rot. A crash drops un-fsynced data per real
       semantics (only sync()'d data survives). All fault decisions from IRandom + virtual clock.
Invariants: only sync()-acknowledged-AND-truly-durable data survives a crash; lying-fsync data is LOST on crash exactly
       as modeled; torn writes leave a partial page; everything a pure function of (seed, inputs); byte-identical replay.
Forbidden: wall-clock; real file IO; nondeterministic schedule; std::*_distribution.
Gate (tests/sim_disk_test.cpp): write→sync→crash→recover yields a consistent PREFIX; a lying-fsync write is correctly LOST
       after crash; a torn write is observable as partial; same-seed ⇒ byte-identical; builds+passes debug/asan/tsan/ubsan;
       forbidden-lint clean.
Deliverables: SimDisk header(s) under providers/sim/include/lockstep/sim/ + tests/sim_disk_test.cpp.
Done when: crash/torn/lying-fsync faults deterministic + replayable; recovery-to-prefix demonstrated; ctest green; gate.sh green.

---

## Batch 2 (sketch — NOT dispatched yet; checker semantics need human gate)
- C2.6 Checker framework + history recorder: pluggable invariant/property checkers run during+after a run; each checker
  has a WRITTEN spec of the exact invariant. This defines "correct" for every later phase → HUMAN-GATED design.
- The toy replicated-counter actor + harness-has-teeth test (known-buggy impl MUST be flagged) — the Phase 2 gate proper.
- C2.3 node lifecycle, C2.4 buggify, C2.5 workload gen, C2.7 seed-burn farm + one-command replay, C2.8 shrinking.
- Orchestrator will draft the C2.6 checker contract + the fault-model envelope and bring to human BEFORE building batch 2.
