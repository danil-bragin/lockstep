# harness — sim driver, fault injectors, checkers, seed infrastructure.
#
# Phase 2 batch 2 (stage A): the history recorder + checker framework FOUNDATION
# (specs/checker-framework.md §2/§3; briefs/phase2-batch2.md LOCKED API
# CONTRACT). Header-only, so this is an INTERFACE lib — same wiring shape as
# providers/sim. Exposes harness/include so callers get <lockstep/harness/...>.
#
# NOTE: harness/ is NOT lint-exempt (only providers/ is). These headers are pure
# deterministic C++ — all time is core::Tick (virtual), no wall-clock, no
# threads, no ambient randomness. Later stages (toy KV system, checker set,
# teeth fixture) code against the headers here.
add_library(lockstep_harness INTERFACE)
target_include_directories(lockstep_harness INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# core/ provides core::Tick (IClock.hpp). Link it when present (concurrent-agent
# tolerant, mirroring the root CMakeLists guard pattern).
if(TARGET lockstep_core)
    target_link_libraries(lockstep_harness INTERFACE lockstep_core)
endif()
target_compile_features(lockstep_harness INTERFACE cxx_std_23)

# Stage B (toy replicated KV system + reusable run harness) lives in
# include/lockstep/harness/kv/. It builds on the sim providers (SimNetwork +
# SimDisk) and the deterministic runtime, so the harness lib links them when
# present (concurrent-agent-tolerant guard, mirroring the root pattern). Headers
# stay determinism-clean and lint-scanned (harness/ is NOT lint-exempt). The
# checker set + teeth fixture (stage C) reuse run_kv_sim() from kv/KvSim.hpp.
if(TARGET lockstep_runtime)
    target_link_libraries(lockstep_harness INTERFACE lockstep_runtime)
endif()
if(TARGET lockstep_providers_sim)
    target_link_libraries(lockstep_harness INTERFACE lockstep_providers_sim)
endif()
