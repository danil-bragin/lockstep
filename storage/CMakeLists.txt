# storage — MVCC key-value engine (WAL, memtable, SSTable, compaction).
#
# Phase 3 §5 step 1 (verification-machinery-first): the Engine INTERFACE seam
# (Engine.hpp), the trivial reference ORACLE (Oracle.hpp), and the DIFFERENTIAL
# HARNESS (DiffHarness.hpp) — header-only, so this is an INTERFACE lib (same
# wiring shape as harness/ + providers/sim). Exposes storage/include so callers
# get <lockstep/storage/...>. Real engine bodies (WAL/memtable/SSTable/...) land
# in §5 steps 2+ and append sources here.
#
# NOTE: storage/ is NOT lint-exempt (only providers/ is). These headers are pure
# deterministic C++ — all time is core::Tick (virtual), no wall-clock, no threads,
# no ambient randomness; the only randomness is the injected sim SeededRandom.
add_library(lockstep_storage INTERFACE)
target_include_directories(lockstep_storage INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(lockstep_storage INTERFACE cxx_std_23)

# core/ provides Future/Scheduler/Error (the async seam); the deterministic
# runtime + sim PRNG back the oracle's scheduler-bound Futures and the harness's
# seeded op stream. Link them when present (concurrent-agent-tolerant guards,
# mirroring the root CMakeLists pattern so a half-landed tree still configures).
if(TARGET lockstep_core)
    target_link_libraries(lockstep_storage INTERFACE lockstep_core)
endif()
if(TARGET lockstep_runtime)
    target_link_libraries(lockstep_storage INTERFACE lockstep_runtime)
endif()
if(TARGET lockstep_providers_sim)
    target_link_libraries(lockstep_storage INTERFACE lockstep_providers_sim)
endif()
