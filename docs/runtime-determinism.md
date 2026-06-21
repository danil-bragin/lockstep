# Runtime Determinism — the no-freelance semantics (Phase 1)

This note documents the two rules that make the Phase 1 runtime substrate a
**pure function of `(seed, initial tasks)`**: the **ready-queue dequeue order**
and the **virtual-clock advance rule**. These are LOCKED SEMANTICS (L1–L6 in
`briefs/phase1.md`). Future agents must **not** change them without a spec
change — they are the foundation every later phase replays against.

Components: `Future<T>`/`Promise<T>` (`core/include/lockstep/core/Future.hpp`),
`Task` (`Task.hpp`), `Scheduler` + `SimClock` (`Scheduler.hpp`), the event-trace
recorder (`Trace.hpp`), and `SeededRandom` (`providers/sim/include/lockstep/sim/
SeededRandom.hpp`).

## Ready-queue dequeue order (L3)

The scheduler owns a single **strict FIFO** ready queue
(`detail::ReadyQueue`, backed by `std::deque`):

- Work is **appended to the back** when it becomes ready (spawn, promise
  fulfillment, task completion, timer fire).
- `run()` **removes from the front** and resumes exactly that one continuation.

The order is therefore the order in which work *became ready* — a deterministic,
insertion-defined order. It is **never** a pointer address and **never**
`unordered_map`/hash iteration order. No associative or unordered container
participates in scheduling. Every enqueue is stamped with a monotonic
`enqueue_seq` that is echoed into the trace (`resume … seq=N`) so the FIFO order
is *observable and replayable*; the sequence is a label only — the deque already
imposes the order.

There is exactly **one** `handle.resume()` call site in the whole runtime: the
loop body in `Scheduler::run()`. Nothing else resumes a coroutine.

## Promise fulfillment schedules, never resumes inline (L1)

`Promise::set_value` / `set_error` (and a `Task`'s `final_suspend`, and a timer
firing) complete their shared state and then call
`SchedulerSink::schedule(handle, …)`, which **pushes the waiter onto the ready
queue**. They never call `handle.resume()`. In the trace this is always the pair
`promise_set` immediately followed by `schedule`, with the woken coroutine's
`resume` appearing **later** (after any other already-ready work) — proving the
waiter was queued, not run inline.

## `co_await` of a not-ready future suspends and yields (L2)

A `Future`'s awaiter reports `await_ready() == false` unless the result already
exists. On suspension it parks the awaiting coroutine handle in the shared state
and returns control to the scheduler loop. The parked handle is only ever
re-enqueued by L1 (fulfillment → `schedule`).

## Virtual-clock advance rule (L4)

`SimClock` exposes virtual time only; **there is no wall-clock anywhere**.
`now()` returns the scheduler's virtual time; `delay(d)` arms a timer `d` ticks
ahead and returns a `Future<void>` that completes when virtual time reaches it.

Virtual time advances under **one** condition, in **one** place
(`Scheduler::fire_earliest_timers`, called from `run()`):

> When the **ready queue is empty** and timers pend, jump virtual time to the
> **earliest** pending timer's due tick, then fire **every** timer due at that
> exact tick.

So the clock never moves while any continuation is ready — all ready work drains
first, *then* time jumps. The clock only moves **forward**. This is what makes
elapsed virtual time a deterministic function of the program, independent of host
speed.

### Timer ordering

Pending timers are ordered by the deterministic monotonic key
`(due_tick, arm_seq)`: earliest due tick first, ties broken by the order the
timers were armed (`arm_seq`, a monotonic counter). Never by pointer or hash
order. When several timers are due at the jumped-to tick, they fire in `arm_seq`
order, each fulfilling its promise (which schedules its waiter per L1).

## Event-trace format (stable; the replay contract)

`Trace::render()` emits one ASCII line per event, fields space-separated:

```
<seq> <action> vt=<vtime> [<payload>]
```

- `seq` — global monotonic event counter (decimal, 0-based).
- `action` — a fixed lowercase token (`spawn`, `resume`, `promise_set`,
  `schedule`, `timer_arm`, `timer_fire`, `clock_advance`, `task_done`,
  `run_start`, `run_end`). **Append-only**: never rename/renumber an action.
- `vt` — scheduler virtual time when the event was recorded.
- `payload` — action-specific, space-free `key=value` tokens (may be empty).

The stream is line-oriented ASCII with plain decimal numbers — no pointers, no
floats, no locale — so it is **byte-identical across stdlib implementations**.
Same `(seed, initial tasks)` ⇒ byte-identical `render()` output (L5). The
self-test `tests/runtime_determinism_test.cpp` runs a ping-pong twice and asserts
the rendered traces are equal byte-for-byte.

## Randomness (L6)

All randomness flows through one seeded `IRandom` — `SeededRandom`
(`providers/sim/`, the lint-exempt zone). It is a hand-rolled **splitmix64**
engine; integer ranges are computed with **Lemire's** bias-free method, **not**
`std::uniform_int_distribution` or any `std::*_distribution` (those are
implementation-defined and would break byte-identical replay across stdlibs).
The seed fully determines the sequence and is logged for replay.

## What is forbidden in non-provider code

`std::chrono`, `std::thread`, `std::rand`, `std::random_device`,
`std::*_distribution`, coordination atomics (`std::memory_order_*`, atomic
fences), wall-clock, real threads/locks/condition variables, and **inline
resume on promise fulfillment**. The forbidden-call lint
(`tools/lint/forbidden_calls.py`) enforces the first set across the tree except
`providers/`; L1 (no inline resume) is enforced by the single-resume-site design
above and observable in the trace.
