# Lockstep — Agent Briefs (template, examples, kickoff)

> Companion to `lockstep-master-plan.md` and `lockstep-phase-specs-all.md`.
> Every agent task is dispatched as a **brief**. A brief is the agent's entire contract: no brief, no work.
> The universal merge gate applies to every resulting PR (see master plan §8).

---

## The brief template

```
BRIEF: <short imperative title>
Phase:        <0–7>
Owner mode:   <agent-autonomous | spec-anchored | core-no-freelance>
Inputs:       <interface headers, formal specs, prior components this builds on>
Build:        <what to implement, concretely>
Invariants:   <the properties the work must preserve — the agent may not violate these>
Forbidden:    <cardinal-rule reminders that apply here, e.g. no wall-clock, no real threads>
Gate:         <the NAMED scenarios/checks this must pass before merge>
Deliverables: <code + tests + (if applicable) spec/checker artifacts>
Done when:    <objective, mechanically-checkable completion condition>
```

**Owner-mode meanings.**
- `agent-autonomous` — agent decides design within the gate. Periphery work.
- `spec-anchored` — agent implements against a fixed interface/invariant set; design latitude only inside it.
- `core-no-freelance` — touches runtime, log/consensus, or commit. Requires a model-checked spec; behavior changes need
  the spec updated + re-checked first; a dual implementation is maintained. Smallest possible changes.

---

## Example briefs

### Example A — Phase 0, agent-autonomous
```
BRIEF: Forbidden-call lint
Phase:        0
Owner mode:   agent-autonomous
Inputs:       repo layout (C0.4); cardinal rules §4
Build:        a CI lint that scans the whole tree EXCEPT providers/ and fails on any of:
              std::chrono, std::rand, std::random_device, std::thread, raw socket/file syscalls, coordination atomics.
Invariants:   zero false negatives on the forbidden set; provider dirs are exempt.
Forbidden:    n/a
Gate:         a fixture file containing each forbidden call is rejected; a clean tree passes; runs in CI.
Deliverables: the lint script + its own fixture tests + CI wiring.
Done when:    a planted std::chrono::now() in core/ fails CI; clean main passes.
```

### Example B — Phase 1, core-no-freelance
```
BRIEF: Deterministic scheduler
Phase:        1
Owner mode:   core-no-freelance
Inputs:       IScheduler, IClock, IRandom headers; Phase 1 spec C1.3–C1.4
Build:        single-thread cooperative scheduler with a deterministic, documented ready-queue order; spawn(Task);
              run() that resumes ready continuations and advances virtual time only when none are ready.
Invariants:   run is a pure function of (seed, initial tasks); dequeue order never depends on pointer address or
              unordered_map iteration; no wall-clock; no real threads/locks/atomics.
Forbidden:    std::chrono, std::thread, std::rand, inline resume on promise fulfilment.
Gate:         deterministic ping-pong (Phase 1 gate) byte-identical across seeds, under ASan+TSan+UBSan.
Deliverables: scheduler + event-trace recorder + property tests + the ping-pong gate test.
Done when:    gate green across a 10k-seed sweep; a forced reordering bug is reproducible from its logged seed.
```

### Example C — Phase 2, spec-anchored
```
BRIEF: Sim network — partitions
Phase:        2
Owner mode:   spec-anchored
Inputs:       INetwork header; sim message-bus (C2.1); IRandom + virtual clock
Build:        partition/heal of arbitrary server subsets, plus per-link drop/reorder/duplicate/latency, all seeded.
Invariants:   the entire fault schedule is derived from IRandom; partition/heal events scheduled on virtual time;
              no message crosses a live partition; healing restores delivery deterministically.
Forbidden:    wall-clock; nondeterministic data structures in the schedule.
Gate:         a replicated toy actor loses progress during a partition and recovers after heal — reproducibly;
              the harness-has-teeth test still flags a known-buggy actor.
Deliverables: partition injector + checker hooks + tests.
Done when:    partition scenarios reproduce byte-identically and auto-shrink to a minimal schedule.
```

### Example D — Phase 4, core-no-freelance (spec-conformant)
```
BRIEF: Leader election (impl A of dual)
Phase:        4
Owner mode:   core-no-freelance
Inputs:       specs/Consensus.tla (model-checked); IDisk for term/vote persistence; Phase 4 spec C4.1
Build:        Raft leader election: timeout→candidate, term increment, vote requests, become-leader on quorum;
              currentTerm/votedFor persisted before responding.
Invariants:   must conform to Consensus.tla — ElectionSafety (≤1 leader per term), persisted vote before reply.
Forbidden:    any behavior not permitted by the model-checked spec; changing the spec without re-running TLC.
Gate:         under partition+crash+reorder storms: never two leaders in a term; linearizability checker passes;
              cross-checks against impl B; mutation score ≥ threshold.
Deliverables: implementation A + conformance tests mapping to spec actions.
Done when:    fault-storm gate green; impl A and impl B agree on every generated history.
```

---

## Kickoff sequence (what to dispatch, in order)

Dependencies matter — verification machinery precedes what it judges.

1. **Phase 0, fully parallel.** Fan agents out: build + CI matrix, all four sanitizers, forbidden-call lint,
   abstraction-boundary headers, repo layout. Gate: trivial CI green, planted forbidden call rejected.
2. **Phase 1, small and tight.** Few agents, core-no-freelance: futures/promises, scheduler, virtual clock, IRandom.
   Gate: deterministic ping-pong. Do not widen the agent pool here.
3. **Phase 2, the real investment.** This becomes the merge gate, so build it carefully: sim providers, fault
   injectors, checker framework, seed-burn farm, shrinking — and the **harness-has-teeth test** before trusting any of it.
4. **Only now** open the breadth taps. With the Phase 2 gate real, Phases 3→7 can run with wide agent fan-out,
   each PR forced through the full battery.
5. **Before Phase 4 and Phase 5 code:** dispatch the TLA+ spec tasks first; no implementation until TLC is clean.

---

## Running the agent loop

- Dispatch by brief; collect PRs; each PR auto-runs the universal gate. Merges are gate-gated, not human-gated.
- The human reviews **dashboards**, not diffs: seed-burn outcomes, model-check results, linearizability findings,
  mutation scores, conformance, soak. Approve phase gates from those.
- Keep `core-no-freelance` changes small and dual-tracked. Let `agent-autonomous` breadth (Phase 6, drivers, docs,
  fuzzers) run wide.
- A red mutation score or an un-reproducible failure halts the line until fixed — those mean the verification itself is leaking.
