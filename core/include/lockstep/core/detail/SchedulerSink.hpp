#pragma once

// SchedulerSink.hpp — the narrow surface that Future/Promise/Task need from the
// Scheduler, factored into an abstract sink to break the include cycle (the
// Scheduler includes Future/Task; Future/Task only need to *schedule* work and
// *record* trace, not the whole Scheduler). The concrete Scheduler implements
// this. Keeping it abstract also makes the "fulfilling a Promise SCHEDULES its
// waiter" rule (L1) a single, testable call site.

#include <coroutine>
#include <cstdint>
#include <string>

#include <lockstep/core/Trace.hpp>
#include <lockstep/core/detail/SharedStatePool.hpp>

namespace lockstep::core::detail {

// The minimal scheduler capabilities used by the async primitives. The concrete
// Scheduler implements this; primitives hold a non-owning pointer to it.
class SchedulerSink {
public:
    virtual ~SchedulerSink() = default;

    // S8.7 — the per-sink SharedState pool. make_promise<T>() mints the Future/Promise
    // shared state through this pool's allocator so a completed+consumed SharedState's
    // storage is RECYCLED (free-listed) instead of heap-freed, eliminating the dominant
    // per-op make_shared allocation. The pool is a CONCRETE member of this abstract base
    // so BOTH the sim Scheduler and the prod ProdReactor inherit it; it lives and dies
    // with the sink (freed in ~SharedStatePool). It is pure memory reuse — INVISIBLE to
    // output, so the sim stays byte-identical. Single-threaded with the sink (L6).
    [[nodiscard]] SharedStatePool& shared_state_pool() noexcept { return ss_pool_; }

    // Schedule a coroutine onto the ready queue (FIFO). This is the ONLY way a
    // waiter is ever resumed after suspension — promise fulfillment and task
    // completion both route through here, never an inline `.resume()` (L1).
    // `why` is the trace action recorded for the enqueue (Schedule or TimerFire).
    virtual void schedule(std::coroutine_handle<> h, TraceAction why, std::string payload) = 0;

    // Append a raw event to the run trace (for primitive-level events such as a
    // promise being set). Returns the assigned sequence number.
    virtual std::uint64_t trace(TraceAction action, std::string payload) = 0;

    // PERF HINT: whether trace() will actually STORE the event. Callers that must
    // BUILD a payload string (an allocation) consult this first and skip the build
    // when storage is off — the whole emit then costs one virtual call instead of a
    // malloc + format + discard. Default true (store), so a sink that never disables
    // tracing behaves exactly as before; when tracing IS enabled the guarded calls
    // all still happen, keeping the trace byte-identical.
    [[nodiscard]] virtual bool trace_wanted() const noexcept { return true; }

    // Current virtual time, for trace stamping at the primitive layer.
    [[nodiscard]] virtual std::int64_t vtime() const noexcept = 0;

private:
    // S8.7 — recycled storage for Future/Promise SharedState. One per sink; never shared
    // across sinks (sim and prod each own theirs). Freed with the sink.
    SharedStatePool ss_pool_{};
};

} // namespace lockstep::core::detail
