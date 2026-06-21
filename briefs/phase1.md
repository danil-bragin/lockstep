# Lockstep — Phase 1 Brief Batch (Deterministic Runtime Core)

> Source of truth: `lockstep-phase-specs-all.md` Phase 1 (C1.1–C1.5 + Determinism requirements + Gate),
> `lockstep-master-plan.md` §4 cardinal rules, `lockstep-agent-briefs.md` Example B.
> Owner mode: **core-no-freelance**. Smallest spec-anchored changes. NARROW pool (plan: do not widen here).
> Universal merge gate applies. Phase 1 has NO TLA+ spec (TLA is Phase 4/5); the phase ticket IS the spec.

## LOCKED SEMANTICS (no-freelance — agent may NOT change these without a spec change)
- L1: fulfilling a Promise **schedules** its waiter onto the ready queue — NEVER inline-resumes. (C1.1)
- L2: `co_await` on a not-ready Future **suspends** the coroutine and yields to the scheduler. (C1.2)
- L3: ready-queue dequeue order is **deterministic + documented** — FIFO, or a deterministic monotonic-key
      priority. NEVER pointer address, NEVER unordered_map iteration order. (C1.3)
- L4: virtual time advances **only** when the ready queue is empty and timers pend → jump to earliest timer. (C1.3/C1.4)
- L5: the whole run is a **pure function of `(seed, initial tasks)`**. Byte-identical trace for identical input. (Determinism req)
- L6: all randomness flows through one seeded `IRandom`; no wall-clock anywhere; no real threads/locks/coordination-atomics. (§4)

## LOCKED API CONTRACT (both Phase 1 agents code against these exact shapes; freeze before churn)
```cpp
namespace lockstep::core {
template <class T> class Promise;   // write end: set_value(T) / set_error(Error); move-only
template <class T> class Future;    // awaitable (co_await); supports Future<void> + error completion
class Task;                          // coroutine return type; completion may fulfill a Future (tasks compose)

class Scheduler {                    // implements IScheduler; the deterministic engine
  void spawn(Task);                  // enqueue a coroutine onto the ready queue
  void run();                        // resume ready continuations; when none ready + timers pend, advance vtime to earliest, fire
  // IClock surface bound to this scheduler's virtual time (impl may expose via a SimClock adapter):
  Tick now() const;                  // virtual ticks (int64). NO wall-clock.
  Future<void> delay(Duration d);    // completes when virtual time has advanced by d
};
}
// providers/sim/ (lint-exempt zone):
class SeededRandom;                  // implements IRandom; single seeded PRNG
```
- `Tick`/`Duration` = `int64_t` (already in IClock.hpp from Phase 0). Reuse the Phase 0 boundary headers; the concrete
  Scheduler/SimClock/SeededRandom implement IScheduler/IClock/IRandom.
- LANDED-API NOTE (P1-CORE delivered): Promise minted via `make_promise<T>(sink)` / `Promise<T>(shared_state)` (scheduler-bound),
  NOT bare default-ctor. Public surface unchanged: `Promise<T>::set_value/set_error` (move-only), `Future<T>` co_await-able,
  `Scheduler::spawn/run/now/delay`, `SeededRandom : IRandom`. Runtime is header-only INTERFACE lib `lockstep_runtime`.
  Error completion via `Future::has_error()/error()`. Verifier: code against the ACTUAL landed headers (read them) — they are truth.
- **Determinism trap (HARD):** `std::uniform_int_distribution` & friends are implementation-defined → NON-portable →
  FORBIDDEN even inside providers/. SeededRandom must compute ranges itself (e.g. splitmix64/xoshiro engine +
  Lemire/modulo range), so the trace is byte-identical across stdlib implementations.

---

## BRIEF P1-CORE — deterministic runtime substrate  (DISPATCH NOW)
Phase: 1 · Owner mode: core-no-freelance · Pool: 1 agent.
Inputs: Phase 0 boundary headers (`core/include/lockstep/core/I*.hpp`, Error.hpp, `lockstep_core`); Phase 1 spec C1.1–C1.5;
        Example B; the LOCKED SEMANTICS + API CONTRACT above.
Build (cohesive — one coherent substrate, NOT split):
  - C1.1 Future<T>/Promise<T> (+ Future<void>, error completion) on C++20 coroutines.
  - C1.2 Task coroutine return type (suspend on not-ready co_await; completion can fulfill a Future).
  - C1.3 Scheduler: single-thread cooperative, documented FIFO ready queue, spawn()/run(), implements IScheduler.
  - C1.4 SimClock: virtual now()/delay() bound to the scheduler; vtime advances per L4.
  - C1.5 SeededRandom (providers/sim/): single seeded PRNG implementing IRandom; portable ranges (no std::*_distribution).
  - Event-trace recorder: scheduler records (seq, action, vtime, ...) to a replayable trace buffer; documented format.
  - A minimal in-repo determinism self-test proving same-seed ⇒ identical trace (lightweight; the FULL adversarial gate
    is P1-VERIFY's job — do not gold-plate tests here, but DO prove the substrate runs deterministically).
Invariants: L1–L6 hold. run() is a pure function of (seed, initial tasks). Dequeue order never depends on pointer/
        unordered_map iteration. No determinism leaks.
Forbidden: std::chrono, std::thread, std::rand, std::random_device, coordination atomics (memory_order/fences),
        std::*_distribution, inline resume on promise fulfilment (L1), wall-clock, real threads/locks.
Gate (this agent, minimal): tree builds + ctest green under presets debug/asan/tsan/ubsan; `bash scripts/gate.sh`
        stays GREEN (forbidden-lint included — your randomness lives in providers/ so it's exempt, but core/ must be clean);
        a ping-pong demo runs and prints a stable trace; same-seed run twice ⇒ byte-identical trace.
Deliverables: runtime headers/impl under core/ + providers/sim/Random; event-trace recorder; the minimal self-test;
        a `docs/` note documenting the dequeue order + clock-advance rule (the no-freelance semantics).
Done when: builds+ctest green across debug/asan/tsan/ubsan; gate.sh green; same-seed trace byte-identical (self-test asserts it).
NOTE TO AGENT: if implementing coroutines forces an API tweak vs the contract above, STOP and report the exact needed
        change in your receipt rather than silently diverging — the verifier codes against this contract.

## BRIEF P1-VERIFY — independent determinism gate  (DISPATCH AFTER P1-CORE LANDS, against real API)
Phase: 1 · Owner mode: spec-anchored · Pool: 1 agent (independent author → tests have teeth).
Inputs: the LANDED P1-CORE headers/API; Phase 1 Gate ("deterministic ping-pong"); §6.2/§6.8.
Build: the full ping-pong gate test (two coroutines exchange a token N times via futures with clock.delay between);
       assertions — (1) same seed ⇒ byte-identical trace; (2) different seed ⇒ valid-but-different trace where randomness
       applies; (3) final virtual time + token count exactly as predicted. Plus: a multi-seed sweep runner (large sweep in
       CI, scaled locally) that logs every seed; a forced-reordering-bug check proving a planted bug reproduces byte-for-byte
       from its logged seed; a property/fuzz test over scheduler interleavings. All under ASan/TSan/UBSan.
Invariants: the checker asserts EXACTLY the determinism contract — no stronger/weaker. Every run logs its seed; failures replay.
Gate: ping-pong green across the seed sweep under sanitizers; a forced reordering bug is reproducible from its logged seed;
       mutation score on the runtime ≥ threshold (this is where the substrate's tests get their teeth proven).
Deliverables: gate test + seed-sweep runner + forced-bug repro test + property/fuzz test + CI wiring of the sweep.
Done when: Phase 1 spec Done-condition met — gate green across many seeds in CI under sanitizers; forced bug byte-reproducible.
