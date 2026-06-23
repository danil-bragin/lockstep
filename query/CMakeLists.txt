# query/ — Phase 6 Stage F. The DEVELOPER-FACING client + query surface.
#
# Source of truth: lockstep-phase-specs-all.md Phase 6 C6.1 (transaction-function
# model) + C6.2 (read/query language); master-plan D1/D3/D5; briefs/phase6.md
# (V-DET-USER / V-D5-SAFE / V-CONFORM). It WRAPS the landed seams — txn::Executor
# (deterministic_factory) + storage::Engine MVCC — and is what the wire protocol /
# drivers / CLI (Stage B) build on.
#
# Header-only, so this is an INTERFACE lib (same wiring shape as txn/ + harness/).
# Exposes query/include so callers get <lockstep/query/...>.
#
# NOTE: query/ is NOT lint-exempt (only providers/ is). These headers are pure
# deterministic C++ — no wall-clock, no threads, no std::*_distribution, no
# ambient randomness, ordered maps only. The standalone read path drives the
# storage engine on a fixed-seed scheduler so a query is a pure fn of (history,
# query); the txn path delegates to the verified deterministic executor.
add_library(lockstep_query INTERFACE)
target_include_directories(lockstep_query INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(lockstep_query INTERFACE cxx_std_23)

# The surface builds OVER txn/ (the executor + D5 selector + checkers) and
# storage/ (MVCC reads), which in turn pull core/ + the sim providers (the
# standalone read path drives a WalEngine on the deterministic scheduler). Link
# them when present (concurrent-agent tolerant, mirroring the root guard).
if(TARGET lockstep_txn)
    target_link_libraries(lockstep_query INTERFACE lockstep_txn)
endif()
if(TARGET lockstep_storage)
    target_link_libraries(lockstep_query INTERFACE lockstep_storage)
endif()
if(TARGET lockstep_core)
    target_link_libraries(lockstep_query INTERFACE lockstep_core)
endif()
if(TARGET lockstep_runtime)
    target_link_libraries(lockstep_query INTERFACE lockstep_runtime)
endif()
if(TARGET lockstep_providers_sim)
    target_link_libraries(lockstep_query INTERFACE lockstep_providers_sim)
endif()
