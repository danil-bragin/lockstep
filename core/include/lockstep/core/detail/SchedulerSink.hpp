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

namespace lockstep::core::detail {

// The minimal scheduler capabilities used by the async primitives. The concrete
// Scheduler implements this; primitives hold a non-owning pointer to it.
class SchedulerSink {
public:
    virtual ~SchedulerSink() = default;

    // Schedule a coroutine onto the ready queue (FIFO). This is the ONLY way a
    // waiter is ever resumed after suspension — promise fulfillment and task
    // completion both route through here, never an inline `.resume()` (L1).
    // `why` is the trace action recorded for the enqueue (Schedule or TimerFire).
    virtual void schedule(std::coroutine_handle<> h, TraceAction why, std::string payload) = 0;

    // Append a raw event to the run trace (for primitive-level events such as a
    // promise being set). Returns the assigned sequence number.
    virtual std::uint64_t trace(TraceAction action, std::string payload) = 0;

    // Current virtual time, for trace stamping at the primitive layer.
    [[nodiscard]] virtual std::int64_t vtime() const noexcept = 0;
};

} // namespace lockstep::core::detail
