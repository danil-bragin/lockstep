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

# --- Phase 2 batch 2 stage A: checker framework + history recorder self-test --
# B2A FOUNDATION self-test (specs/checker-framework.md §2/§3; briefs/
# phase2-batch2.md BRIEF B2A). Builds a synthetic history via HistoryRecorder,
# runs CheckerRunner with the example checkers, asserts verdicts AND determinism
# (same seed/inputs ⇒ byte-identical history render + verdicts). Links the
# header-only harness (core::Tick) — no system-under-test (decoupled). Inherits
# the directory-wide TIMEOUT 90 set below.
if(TARGET lockstep_harness)
  add_executable(lockstep_checker_framework_test checker_framework_test.cpp)
  target_link_libraries(lockstep_checker_framework_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_harness)
  add_test(NAME checker_framework_test COMMAND lockstep_checker_framework_test)
else()
  message(WARNING
    "lockstep_harness target not found; checker_framework_test not built "
    "(Phase 2 batch 2 stage A pending).")
endif()

# --- Phase 2 batch 2 stage B: toy KV system + run harness self-test ----------
# B2B self-test (specs/checker-framework.md §5; briefs/phase2-batch2.md stage B).
# Drives the toy replicated KV register system under the FULL fault envelope via
# run_kv_sim(seed,cfg) and asserts the HARNESS RUNS DETERMINISTICALLY + the
# system is ALIVE (NOT deep correctness — that is stage C's checker set):
#   (a) progress: some client ops ack under faults,
#   (b) same-seed run twice => byte-identical History (in-test render diff),
#   (c) the run terminates (inherits the directory-wide TIMEOUT 90 ceiling),
#   (d) crash+restart is exercised (node lifecycle C2.3).
# Needs harness + sim providers + runtime (run_kv_sim wires SimNetwork + SimDisk
# on the scheduler). Builds only when all are present.
if(TARGET lockstep_harness AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_kv_sim_test kv_sim_test.cpp)
  target_include_directories(lockstep_kv_sim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_kv_sim_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_harness
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME kv_sim_test COMMAND lockstep_kv_sim_test)
else()
  message(WARNING
    "lockstep_harness / runtime / providers_sim not all found; kv_sim_test not "
    "built (Phase 2 batch 2 stage B pending).")
endif()

# --- Phase 2 batch 2 stage C: §4 checker set teeth + honest-system run --------
# B2C-checkers self-test (specs/checker-framework.md §4; briefs/phase2-batch2.md
# stage C). Author ≠ toy-system author (DECISION-D). Proves the §4 checker set
# (C-INT / C-MONO / C-LIN / C-DUR) has TEETH: each checker FLAGS its canonical
# hand-crafted bad History with a witness (V-CHK5 non-vacuous), and the honest
# system PASSES all four across a seed sweep under the full fault envelope
# (V-TOY1) — or a real bug is surfaced reproducibly (seed + witness). Needs
# harness + sim providers + runtime (run_kv_sim_checked wires SimNetwork +
# SimDisk on the scheduler). Builds only when all are present.
if(TARGET lockstep_harness AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_kv_checkers_test kv_checkers_test.cpp)
  target_include_directories(lockstep_kv_checkers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_kv_checkers_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_harness
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME kv_checkers_test COMMAND lockstep_kv_checkers_test)
else()
  message(WARNING
    "lockstep_harness / runtime / providers_sim not all found; "
    "kv_checkers_test not built (Phase 2 batch 2 stage C pending).")
endif()

# --- Phase 2 batch 2 §6: harness-has-teeth GATE (V-TEETH1) -------------------
# B2C-TEETH self-test (specs/checker-framework.md §6 + §6.1). Author = a THIRD
# independent agent (≠ system author, ≠ checker author — §9 DECISION-D). Feeds
# the §4 checker set DELIBERATELY-BROKEN KV systems (BuggyKvSystems.hpp) through
# the SAME run_kv_sim_with driver and asserts the checkers FLAG each (witness +
# replayable seed). Also asserts the honest system has NO durability/fabrication
# FALSE POSITIVES (C-DUR=0, no INT-1). Needs harness + sim providers + runtime
# (run_kv_sim_with wires SimNetwork + SimDisk on the scheduler). Builds only when
# all are present.
if(TARGET lockstep_harness AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_kv_teeth_test kv_teeth_test.cpp)
  target_include_directories(lockstep_kv_teeth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_kv_teeth_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_harness
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME kv_teeth_test COMMAND lockstep_kv_teeth_test)
else()
  message(WARNING
    "lockstep_harness / runtime / providers_sim not all found; "
    "kv_teeth_test not built (Phase 2 batch 2 §6 teeth gate pending).")
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
