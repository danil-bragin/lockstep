cmake_minimum_required(VERSION 3.24)

# Pin the C++ standard BEFORE project() so it applies to compiler detection too.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Toolchain)

project(lockstep
  VERSION 0.0.0
  DESCRIPTION "Lockstep — a C++23 deterministic distributed database"
  LANGUAGES CXX)

# Only the top-level build needs CTest + the shared flag modules.
if(PROJECT_IS_TOP_LEVEL)
  include(CTest)            # defines BUILD_TESTING (ON by default) and enable_testing()
endif()

include(Warnings)          # INTERFACE target: lockstep_warnings
include(Sanitizers)        # INTERFACE target: lockstep_sanitizers (driven by LOCKSTEP_SANITIZER)

# --- Components (disjoint ownership per the Phase 0 integration contract) -----
# core/ owns nothing above it. core/CMakeLists.txt is authored by agent A2 and
# exposes the header-only INTERFACE target `lockstep_core`. We reference it but
# tolerate its absence (concurrent agent) so A1's portion still configures.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
  add_subdirectory(core)
else()
  message(WARNING
    "core/CMakeLists.txt not present yet (owned by agent A2). Building without "
    "lockstep_core; the smoke test will link it once A2 lands.")
endif()

add_subdirectory(providers/sim)
add_subdirectory(providers/prod)
add_subdirectory(storage)
add_subdirectory(log)
add_subdirectory(txn)
add_subdirectory(query)
add_subdirectory(harness)
add_subdirectory(consensus)
add_subdirectory(bench)
add_subdirectory(cli)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
