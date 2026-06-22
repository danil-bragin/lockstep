# bench — benchmark harness (C3.7): write/read throughput + write/space
# amplification, swept across D4 tuning configs (flush threshold; block size +
# bloom bits-per-key reported, exposed once the engine threads a tuning struct).
#
# The harness is header-only (lockstep_bench INTERFACE), mirroring storage/ +
# harness/. It drives the real WalEngine (LSM) over SimDisk on the deterministic
# scheduler, so it links the storage engine + the runtime + the sim providers
# (SimDisk + SeededRandom). A COUNTING IDisk wrapper meters appended bytes for
# write-amp; durable_len() drives space-amp; the scheduler's VIRTUAL clock drives
# throughput. Pure fn of (seed, config) — NO wall-clock (forbidden-lint scans
# bench/: it is NOT in the providers/ lint-exempt zone).
#
# The full sweep is a standalone exe (lockstep_bench_driver) the gate BUILDS but
# does not run (it is a few seconds). A TINY smoke sweep runs as a cheap ctest
# (bench_smoke) to prove the harness runs + is deterministic inside TIMEOUT 90.

add_library(lockstep_bench INTERFACE)
target_include_directories(lockstep_bench INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lockstep_bench INTERFACE cxx_std_23)

# Link the engine + runtime + sim providers when present (concurrent-agent-
# tolerant guards, mirroring the root CMakeLists pattern so a half-landed tree
# still configures green).
if(TARGET lockstep_storage)
    target_link_libraries(lockstep_bench INTERFACE lockstep_storage)
endif()
if(TARGET lockstep_runtime)
    target_link_libraries(lockstep_bench INTERFACE lockstep_runtime)
endif()
if(TARGET lockstep_core)
    target_link_libraries(lockstep_bench INTERFACE lockstep_core)
endif()
if(TARGET lockstep_providers_sim)
    target_link_libraries(lockstep_bench INTERFACE lockstep_providers_sim)
endif()

# Build the driver exe + the smoke self-test only when the engine + runtime + sim
# providers are all present (so a half-landed tree still configures). The smoke
# self-test registers a cheap ctest; the full driver is built (gate compiles it)
# but NOT registered as a long ctest — the gate runs only the smoke sweep.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
    # --- standalone full-sweep driver (built by the gate, not run as a ctest) ---
    add_executable(lockstep_bench_driver bench_main.cpp)
    target_link_libraries(lockstep_bench_driver
        PRIVATE
            lockstep_warnings
            lockstep_sanitizers
            lockstep_bench)

    # --- cheap smoke self-test (a tiny sweep; the gate's ctest) -----------------
    add_executable(lockstep_bench_smoke_test bench_smoke_test.cpp)
    target_link_libraries(lockstep_bench_smoke_test
        PRIVATE
            lockstep_warnings
            lockstep_sanitizers
            lockstep_bench)
    if(BUILD_TESTING)
        add_test(NAME bench_smoke COMMAND lockstep_bench_smoke_test)
        # Wall-clock bounded like every Lockstep test (liveness ceiling). The smoke
        # sweep finishes in well under a second; 90s kills any true hang fast.
        set_tests_properties(bench_smoke PROPERTIES TIMEOUT 90)
    endif()
else()
    message(WARNING
        "lockstep_storage / runtime / providers_sim not all found; bench driver + "
        "smoke self-test not built (Phase 3 step 7 pending).")
endif()
