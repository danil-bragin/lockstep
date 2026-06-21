# core/ — abstraction-boundary interface headers (header-only).
#
# `lockstep_core` is an INTERFACE (header-only) library: it has no compiled
# sources, only the pure-virtual boundary headers under include/. Everything
# above core/ codes against these interfaces; providers (sim/prod) implement
# them. core/ depends on nothing above it (spec C0.4).

add_library(lockstep_core INTERFACE)

target_include_directories(lockstep_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# These headers are C++23 and use std::span / nested namespaces.
target_compile_features(lockstep_core INTERFACE cxx_std_23)

# --- lockstep_runtime (Phase 1, C1.1–C1.4) ----------------------------------
# The deterministic runtime substrate: Future/Promise, Task, Scheduler, SimClock,
# and the event-trace recorder. Header-only (coroutine machinery is templated and
# inline), so it is an INTERFACE library layered on the boundary headers. It
# implements IScheduler/IClock; the seeded IRandom lives in providers/sim. Kept
# separate from lockstep_core so the frozen boundary interface stays minimal.
add_library(lockstep_runtime INTERFACE)
target_include_directories(lockstep_runtime INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(lockstep_runtime INTERFACE lockstep_core)
target_compile_features(lockstep_runtime INTERFACE cxx_std_23)
