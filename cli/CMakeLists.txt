# cli — Phase 6 Stage B, C6.5 + C6.6. The LOCKSTEP CLI / ADMIN TOOL + the worked
# DRIVER EXAMPLE.
#
# Source of truth: briefs/phase6.md C6.5 (a small command-driven admin/client tool
# driving the C6.4 reference driver against an in-process sim server) + C6.6 (a
# worked example app using the driver). Both are standalone executables built over
# the header-only query lib (Driver.hpp / LocalCluster.hpp), which pulls txn +
# storage + runtime + sim. They are deterministic (sim transport, seeded) — NO
# wall-clock. This TU is NOT in the providers/ lint-exempt zone: forbidden-lint
# scans it.
#
# The CLI exe is BUILT by the gate (proving the driver surface is usable + compiles
# clean) and also drives a cheap deterministic ctest (cli_determinism) that runs a
# scripted command list TWICE and asserts byte-identical output. The example exe is
# built by the gate; it runs trivially fast.

if(TARGET lockstep_query AND TARGET lockstep_txn AND TARGET lockstep_storage
   AND TARGET lockstep_runtime AND TARGET lockstep_providers_sim)

  add_executable(lockstep_cli lockstep_cli.cpp)
  target_link_libraries(lockstep_cli
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)

  add_executable(lockstep_bank_example bank_example.cpp)
  target_link_libraries(lockstep_bank_example
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)

  # A cheap deterministic ctest: run the SAME scripted command list twice and
  # assert the CLI output is byte-identical (determinism witness). Wall-clock
  # bounded like every Lockstep test (a true hang dies fast).
  if(BUILD_TESTING)
    add_test(
      NAME cli_determinism
      COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:lockstep_cli>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_determinism.cmake)
    set_tests_properties(cli_determinism PROPERTIES TIMEOUT 90)
  endif()

  # --- Phase 7 S5a: lockstepd — the single-node PROD server daemon [LINUX ONLY] -
  # The precursor to the full lockstepd (multi-node Raft is S5b). A thin main()
  # that parses a tiny config (--node-id / --seed / --data-dir / --run-seconds),
  # assembles a prod::ProdServerNode on the PROD providers (ProdReactor epoll loop +
  # the reactor's ONE shared ProdClock + ProdRandom + ProdDisk + ProdNetwork), spawns
  # the UNCHANGED wire::Server's serve coroutine, and runs the reactor's BOUNDED loop
  # (absolute now()-deadline wall guard). epoll/sockets are LINUX-ONLY -> built ONLY
  # on Linux (if(UNIX AND NOT APPLE)) so the macOS host stays green (target absent).
  # NOT in the providers/ lint-exempt zone: forbidden-lint scans it (no raw syscall
  # here — only the provider surfaces). Links the query lib (wire::Server) + the prod
  # providers (which pull the reactor/network/disk/clock/random).
  if(UNIX AND NOT APPLE AND TARGET lockstep_providers_prod)
    add_executable(lockstepd lockstepd.cpp)
    target_link_libraries(lockstepd
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_consensus
        lockstep_query
        lockstep_txn
        lockstep_storage
        lockstep_runtime
        lockstep_providers_sim
        lockstep_providers_prod)
  endif()

else()
  message(WARNING
    "lockstep_query / txn / storage / runtime / providers_sim not all found; "
    "the CLI + bank example are not built (Phase 6 Stage B pending).")
endif()
