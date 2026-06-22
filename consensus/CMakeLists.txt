# consensus/ — Phase 4 Stage M. The consensus SEAM + verification machinery
# (the conformance / linearizability / cross-check harness + teeth), authored
# VERIFICATION-FIRST (before either real Raft implementation). Source of truth:
# specs/Consensus.tla (model-checked clean + human-approved); briefs/phase4.md.
#
# Header-only, so this is an INTERFACE lib (same wiring shape as harness/ and
# providers/sim). Exposes consensus/include so callers get
# <lockstep/consensus/...>.
#
# NOTE: consensus/ is NOT lint-exempt (only providers/ is). These headers are
# pure deterministic C++ — all time is core::Tick (virtual), all randomness is
# the injected core::IRandom, no wall-clock, no threads, no ambient randomness.
# The two Stage-I implementations (impl A + impl B) + the teeth stub all code
# against ConsensusNode.hpp here.
add_library(lockstep_consensus INTERFACE)
target_include_directories(lockstep_consensus INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(lockstep_consensus INTERFACE cxx_std_23)

# core/ provides the boundary interfaces (IClock/IDisk/INetwork/IRandom/
# IScheduler) + the runtime (Scheduler/SimClock/Future/Task). harness/ provides
# the reused Verdict/Checker verdict shape. sim providers back the cluster driver
# (SimNetworkBus + SimDisk). Link them when present (concurrent-agent tolerant,
# mirroring the root CMakeLists guard pattern), so a half-built tree still
# configures.
if(TARGET lockstep_core)
    target_link_libraries(lockstep_consensus INTERFACE lockstep_core)
endif()
if(TARGET lockstep_runtime)
    target_link_libraries(lockstep_consensus INTERFACE lockstep_runtime)
endif()
if(TARGET lockstep_harness)
    target_link_libraries(lockstep_consensus INTERFACE lockstep_harness)
endif()
if(TARGET lockstep_providers_sim)
    target_link_libraries(lockstep_consensus INTERFACE lockstep_providers_sim)
endif()
