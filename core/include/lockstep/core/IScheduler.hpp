#pragma once

// IScheduler.hpp — cooperative, single-thread deterministic scheduler surface.
// No real threads, no preemption, no coordination-atomics: this is the boundary
// that keeps the whole runtime single-threaded and reproducible (spec C1.3).
// The Phase 1 provider owns a ready queue with a DETERMINISTIC, documented
// dequeue order (FIFO or a deterministic monotonic-key priority — NEVER pointer
// order) so a run is a pure function of (seed, initial tasks).
//
// Bodies arrive in Phase 1. This header is the frozen contract; keep it minimal.

namespace lockstep::core {

// Forward declaration only. `Task` is the Phase 1 coroutine return type
// (<lockstep/core/Task.hpp>). The boundary references it by name so this header
// carries no coroutine machinery and compiles standalone (see IClock.hpp). It is
// taken by value (rvalue) because spawning transfers ownership of the coroutine
// to the scheduler; the concrete move-only type is defined in Phase 1.
class Task;

// Pure-virtual scheduler. Owns the ready queue in its implementation, but
// exposes no state and no ownership through this interface.
class IScheduler {
public:
    virtual ~IScheduler() = default;

    // Enqueues a coroutine for execution, transferring ownership to the
    // scheduler. The task becomes ready and will be resumed in the scheduler's
    // deterministic dequeue order. Does not run it inline.
    virtual void spawn(Task&& work) = 0;

    // Runs the cooperative loop: while work is ready, resume the next
    // continuation in deterministic order; when the ready queue is empty but
    // timers pend, advance virtual time to the earliest timer and fire it.
    // Returns when no work and no pending timers remain. Single-threaded; never
    // resumes a continuation inline from within another (fulfillment only
    // schedules onto the ready queue).
    virtual void run() = 0;
};

} // namespace lockstep::core
