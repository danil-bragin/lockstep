# txn/ — Phase 5 Stage M. The distributed-transaction SEAM + the D5 consistency
# selector + the strict-serializable ORACLE + the differential / linearizability
# HARNESS + the per-D5-level CONSISTENCY CHECKERS + the TEETH stubs, authored
# VERIFICATION-FIRST (before the real distributed-txn implementation). Source of
# truth: specs/CommitOrdering.tla (model-checked clean + human-approved);
# briefs/phase5.md; master-plan D1/D2/D5.
#
# Header-only, so this is an INTERFACE lib (same wiring shape as harness/ and
# consensus/). Exposes txn/include so callers get <lockstep/txn/...>.
#
# NOTE: txn/ is NOT lint-exempt (only providers/ is). These headers are pure
# deterministic C++ — the only entropy is a seed consumed by an inlined
# SplitMix; no wall-clock, no threads, no std::*_distribution, no ambient
# randomness, ordered maps only. The Stage-I real impl codes against
# Transaction.hpp (the binding seam).
add_library(lockstep_txn INTERFACE)
target_include_directories(lockstep_txn INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(lockstep_txn INTERFACE cxx_std_23)

# The Stage-M machinery is self-contained (pure value-shaped seam + checkers), so
# it needs no other component to build. The Stage-I real impl will additionally
# link consensus/ (the seqLog source) + storage/ (MVCC reads); link them when
# present (concurrent-agent tolerant, mirroring the root CMakeLists guard).
if(TARGET lockstep_core)
    target_link_libraries(lockstep_txn INTERFACE lockstep_core)
endif()
if(TARGET lockstep_consensus)
    target_link_libraries(lockstep_txn INTERFACE lockstep_consensus)
endif()
if(TARGET lockstep_storage)
    target_link_libraries(lockstep_txn INTERFACE lockstep_storage)
endif()
