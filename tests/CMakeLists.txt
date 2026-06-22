# tests — Phase 0 smoke test + CTest registration.
#
# Frozen names (integration contract): target `lockstep_smoke_test`, registered
# with CTest as `add_test(NAME smoke ...)`. The test links the header-only
# INTERFACE lib `lockstep_core` (authored by agent A2) so integration across the
# boundary is exercised.

add_executable(lockstep_smoke_test smoke_test.cpp)

target_link_libraries(lockstep_smoke_test
  PRIVATE
    lockstep_warnings
    lockstep_sanitizers)

# Link lockstep_core when A2's target exists. Guarded so A1's tree still builds
# green before A2 lands; the smoke_test.cpp side also #if-guards the include.
# Once core/CMakeLists.txt is present, this is a hard link and the include path
# carries <lockstep/core/...> into the translation unit.
if(TARGET lockstep_core)
  target_link_libraries(lockstep_smoke_test PRIVATE lockstep_core)
else()
  message(WARNING
    "lockstep_core target not found (owned by agent A2). lockstep_smoke_test "
    "will build standalone for now; it links lockstep_core once A2 lands.")
endif()

add_test(NAME smoke COMMAND lockstep_smoke_test)

# --- Phase 1 runtime determinism self-test ----------------------------------
# Minimal proof that the deterministic substrate (Future/Task/Scheduler/SimClock
# + SeededRandom) runs a ping-pong byte-identically for the same (seed, tasks)
# (L5). The FULL adversarial gate (seed sweep, forced-bug repro, fuzz, mutation)
# is P1-VERIFY's separate target. Links the header-only runtime + sim PRNG.
if(TARGET lockstep_runtime AND TARGET lockstep_providers_sim)
  add_executable(lockstep_runtime_determinism_test runtime_determinism_test.cpp)
  target_link_libraries(lockstep_runtime_determinism_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME runtime_determinism COMMAND lockstep_runtime_determinism_test)

  # --- Phase 1 ADVERSARIAL determinism gate (P1-VERIFY, independent author) ---
  # The FULL gate the brief P1-VERIFY calls for: the ping-pong gate (3 spec
  # assertions), a multi-seed sweep, a forced-bug byte-reproducibility check, and
  # a property/fuzz over scheduler interleavings. All link the header-only runtime
  # + sim PRNG and #include the shared harness under tests/harness/. The tests/
  # dir is on the include path so "harness/..." resolves. These are non-provider
  # files: the forbidden-call lint scans them; all randomness is SeededRandom.
  set(LOCKSTEP_VERIFY_TESTS
    ping_pong_gate_test
    seed_sweep_test
    forced_bug_repro_test
    scheduler_fuzz_test)
  foreach(verify_test IN LISTS LOCKSTEP_VERIFY_TESTS)
    add_executable(lockstep_${verify_test} ${verify_test}.cpp)
    target_include_directories(lockstep_${verify_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_${verify_test}
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME ${verify_test} COMMAND lockstep_${verify_test})
  endforeach()
else()
  message(WARNING
    "lockstep_runtime / lockstep_providers_sim not found; runtime determinism "
    "self-test not built (Phase 1 substrate pending).")
endif()

# --- Phase 2 sim-provider tests (pre-wired by orchestrator; disjoint ownership) -
# S1 owns sim_network_test.cpp, S2 owns sim_disk_test.cpp. Targets pre-registered
# here so the provider agents never edit this shared file. Each builds only if its
# source exists (so a half-landed batch still configures green) and links the
# header-only runtime + sim providers. tests/ is on the include path for shared
# harness headers. These are non-provider TUs → forbidden-lint scans them.
if(TARGET lockstep_runtime AND TARGET lockstep_providers_sim)
  foreach(sim_test sim_network_test sim_disk_test)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sim_test}.cpp")
      add_executable(lockstep_${sim_test} ${sim_test}.cpp)
      target_include_directories(lockstep_${sim_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(lockstep_${sim_test}
        PRIVATE
          lockstep_warnings
          lockstep_sanitizers
          lockstep_runtime
          lockstep_providers_sim)
      add_test(NAME ${sim_test} COMMAND lockstep_${sim_test})
    endif()
  endforeach()
endif()

# --- Harness liveness invariant (orchestrator) -------------------------------
# Every test is WALL-CLOCK BOUNDED. A non-terminating run (infinite loop /
# livelock — e.g. a broken binary search) must surface as a CTest FAILURE, never
# an unbounded spin that saturates the host. Backprop from the SimBus::in_side_a
# off-by-one infinite loop (mid+0 vs mid+1) that hung standalone yet slipped a
# timeout-less ctest. Deterministic sims finish in well under a second; 90s is a
# generous ceiling that still kills any true hang fast.
get_property(_lockstep_all_tests DIRECTORY PROPERTY TESTS)
if(_lockstep_all_tests)
  set_tests_properties(${_lockstep_all_tests} PROPERTIES TIMEOUT 90)
endif()
