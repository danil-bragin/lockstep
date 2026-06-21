#pragma once

// IClock.hpp — virtual-time clock interface. The ONLY source of "the current
// time" in the system. There is intentionally NO wall-clock: ordering and
// strict serializability come from log order (master-plan D2), and time here is
// purely the simulation's virtual clock. Real wall-clock is advisory and lives
// behind a separate prod-only surface, never here.
//
// Bodies arrive in Phase 1 (virtual clock) and Phase 7 (prod). This header is
// the frozen contract; keep it minimal to avoid ABI churn.

#include <cstdint>

namespace lockstep::core {

// A virtual instant, measured in opaque monotonic ticks since an unspecified
// origin. Not a wall-clock time. Comparable/orderable; the unit is defined by
// the provider (the sim defines 1 tick == 1 virtual time unit). Signed so that
// (later - earlier) differences are representable without wraparound surprises.
using Tick = std::int64_t;

// A span of virtual time, in the same tick unit as Tick. A delay of `Duration`
// ticks advances virtual time by exactly that many ticks once no work is ready.
using Duration = std::int64_t;

// Forward declaration only. The concrete awaitable is defined in Phase 1
// (`Future<void>` in <lockstep/core/Future.hpp>). The boundary references it by
// name so this header stays free of coroutine machinery and compiles standalone;
// callers in Phase 1+ include the real definition. Declaring a method that
// returns this type is legal here because no body is emitted.
template <class T> class Future;

// Pure-virtual virtual-time clock. No state, no ownership.
class IClock {
public:
    virtual ~IClock() = default;

    // Returns the current virtual time as a monotonic tick count. Never
    // decreases across calls on the same clock. Has NO relation to wall-clock.
    [[nodiscard]] virtual Tick now() const noexcept = 0;

    // Schedules a timer `d` ticks in the future and returns an awaitable that
    // completes once virtual time has advanced by `d`. Per the Phase 1 rule,
    // virtual time only advances when no continuation is ready, so the future
    // completes deterministically. `d <= 0` is permitted and completes at the
    // current virtual time (i.e. yields without advancing the clock).
    [[nodiscard]] virtual Future<void> delay(Duration d) = 0;
};

} // namespace lockstep::core
