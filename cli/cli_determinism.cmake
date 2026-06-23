# cli_determinism.cmake — Phase 6 C6.5 determinism ctest driver.
#
# Runs the lockstep_cli (path in -DCLI) on a FIXED scripted command list TWICE and
# asserts the two outputs are BYTE-IDENTICAL. This is the CLI determinism witness:
# same seed + same script => identical bytes (no wall-clock, no ambient entropy).
# Invoked via `cmake -P` from the cli_determinism ctest.

if(NOT DEFINED CLI)
  message(FATAL_ERROR "cli_determinism.cmake: -DCLI=<path> required")
endif()

# A representative scripted command list exercising writes, reads at each D5 level,
# a scan, and a ping — all in one deterministic run.
set(SCRIPT
  --seed 7 --faults --
  put acct:a 100
  put acct:b 100
  transfer acct:a acct:b 30
  get acct:a
  get acct:b --level strict
  get acct:a --level snapshot --arg 1
  get acct:a --level bounded --arg 5
  get acct:a --level ryw --arg 9
  scan acct:a acct:z
  ping)

execute_process(
  COMMAND ${CLI} ${SCRIPT}
  OUTPUT_VARIABLE OUT1
  RESULT_VARIABLE RC1)
execute_process(
  COMMAND ${CLI} ${SCRIPT}
  OUTPUT_VARIABLE OUT2
  RESULT_VARIABLE RC2)

if(NOT RC1 EQUAL 0)
  message(FATAL_ERROR "cli run 1 exited non-zero: ${RC1}\n${OUT1}")
endif()
if(NOT RC2 EQUAL 0)
  message(FATAL_ERROR "cli run 2 exited non-zero: ${RC2}\n${OUT2}")
endif()
if(NOT OUT1 STREQUAL OUT2)
  message(FATAL_ERROR
    "cli output NOT byte-identical across two runs (determinism violated)\n"
    "--- run 1 ---\n${OUT1}\n--- run 2 ---\n${OUT2}")
endif()

message(STATUS "cli_determinism: byte-identical across two runs OK")
message(STATUS "${OUT1}")
