# Lockstep — Phase 2 Batch 2 Brief Batch (Checker Core + Toy System + Teeth)

> Source of truth: `specs/checker-framework.md` (HUMAN-APPROVED 2026-06-22, decisions §9 RESOLVED),
> `lockstep-phase-specs-all.md` C2.3–C2.8, master-plan §6.2/§6.3. Phase 2 = the merge gate; build carefully;
> harness-has-teeth BEFORE trusting it. Decisions: KV registers · full envelope · client-op history · separate-agent teeth.

## Staged build (dependencies real → stage, don't fan-out blindly)
- **Stage A (DISPATCH NOW):** history recorder + checker framework API. FOUNDATION — all else codes against it.
- **Stage B (after A verified):** toy replicated KV system + node lifecycle (C2.3); workload gen (C2.5) + buggify (C2.4).
- **Stage C (after B):** initial checker set (§4) [agent ≠ toy-system author]; harness-has-teeth fixture [3rd agent] (§6, DECISION-D).
- **Stage D:** seed-burn farm (C2.7) + shrinking (C2.8). Then batch-2 gate = §6 green + full-envelope seed-burn + mutation-of-harness PASS.

## LOCKED API CONTRACT (Stage A — freeze; later stages code against these exact shapes)
Place under `harness/include/lockstep/harness/` (repo layout C0.4: harness/ = sim driver + checkers). Namespace `lockstep::harness`.
```cpp
// History (spec §2): client-op-level, virtual-time stamped, byte-reproducible, PASSIVE (no execution perturbation).
enum class OpKind { Read, Write, Cas };
struct Op {
  std::uint64_t client_id, op_id;
  OpKind kind;
  std::string  key;
  std::string  value;        // Write/Cas-new (empty ⇒ ∅)
  std::string  cas_old;      // Cas only
  core::Tick   invoke_vt{};  // set at invoke
  core::Tick   return_vt{};  // set at return
  bool         ok{};         // completed vs errored
  std::string  result;       // Read result / ack token
  std::string  error;        // error detail if !ok
};
using History = std::vector<Op>;   // total order by (return_vt, seq); seq = deterministic tie-break
class HistoryRecorder {            // passive; records invoke + return; pure fn of (seed)
  std::uint64_t on_invoke(std::uint64_t client_id, OpKind, std::string key, std::string value, std::string cas_old, core::Tick now); // → op_id
  void on_return(std::uint64_t op_id, bool ok, std::string result, std::string error, core::Tick now);
  const History& history() const;
};
// Checker (spec §3): each cites a written invariant; asserts EXACTLY its level; emits replayable witness.
struct Verdict { bool ok; std::string witness; std::string explanation; std::uint64_t seed; };
class Checker {
 public:
  virtual ~Checker() = default;
  virtual void on_event(const Op& ev) {}           // optional online incremental check
  virtual Verdict final(const History&) = 0;       // required
  virtual std::string name() const = 0;
  virtual std::string spec_ref() const = 0;        // e.g. "specs/checker-framework.md §4 C-LIN"
};
class CheckerRunner {                              // runs a set during (on_event) + after (final); aggregates
  void add(std::unique_ptr<Checker>);
  void observe(const Op& ev);                      // fan to on_event
  std::vector<Verdict> finalize(const History&, std::uint64_t seed); // run all final(); collect violations
};
```
- All `core::Tick` from the runtime SimClock (virtual time). NO wall-clock.
- Recorder + runner = pure function of (seed): same seed ⇒ byte-identical history + verdicts.
- harness target: a lib `lockstep_harness` (header-only INTERFACE or compiled — agent choice) exposing harness/include.

---

## BRIEF B2A — checker framework + history recorder  (DISPATCH NOW)
Phase: 2 · Owner mode: spec-anchored · Pool: 1.
Inputs: specs/checker-framework.md §2,§3 (+§9 decisions); the LOCKED API CONTRACT above; runtime headers
        (core SimClock/Tick, Future/Task/Scheduler); harness/CMakeLists.txt (placeholder to flesh out).
Build: HistoryRecorder + Op/History + Checker interface + CheckerRunner per the contract. A trivial EXAMPLE checker
        (e.g. "every return has a prior invoke") + a self-test that records a small synthetic history, runs the runner,
        and asserts verdicts. Document each type. Keep it MINIMAL + decoupled from any specific system-under-test.
Invariants: V-HIST1 (invoke+return both recorded), V-HIST2 (pure fn of seed → byte-identical), V-HIST3 (passive,
        recording ⊥ perturb execution), V-CHK1 (checker cites spec_ref), V-CHK2 (violation → witness + seed → replayable).
Forbidden: wall-clock, std::thread/atomics, std::*_distribution, unordered iteration affecting order, any nondeterminism.
Gate: builds + ctest green under debug/asan/tsan/ubsan; `bash scripts/gate.sh` GREEN (incl forbidden-lint + mutation —
        keep runs small; mutation now timeout-bounded); self-test deterministic (same seed ⇒ byte-identical history+verdicts).
Deliverables: harness/include/lockstep/harness/{History,Checker,CheckerRunner}.hpp + harness/CMakeLists.txt (target
        lockstep_harness) + tests/checker_framework_test.cpp (register your own ctest target; you are the only stage-A agent).
Done when: framework compiles + links; self-test green under sanitizers; history+verdicts byte-identical on replay; gate.sh green.
NOTE: if the contract needs a tweak for C++ reality, STOP + report in your receipt (later stages code against this).

## Stage B/C/D briefs — sketched; dispatched after Stage A lands + is verified.
- B2B toy KV system + node lifecycle (C2.3) + workload(C2.5) + buggify(C2.4): replicated read/write/cas registers,
  simple replicator, driven deterministically; feeds the HistoryRecorder.
- B2C-checkers initial set (§4 C-INT/C-MONO/C-LIN/C-DUR) — author ≠ toy-system author (independence).
- B2C-teeth known-buggy KV fixture (drops write on partition / stale read) — 3rd agent; checkers MUST flag it (§6 V-TEETH1).
- B2D seed-burn farm (C2.7) + shrinking (C2.8).
