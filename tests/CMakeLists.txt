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

  # --- Pins the PORTABLE mul128 path in SeededRandom (#else schoolbook branch, dead
  #     on __int128 hosts) so the mutation gate can kill its arithmetic mutants. ---
  add_executable(lockstep_seeded_random_portable_test seeded_random_portable_test.cpp)
  target_link_libraries(lockstep_seeded_random_portable_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_providers_sim)
  add_test(NAME seeded_random_portable COMMAND lockstep_seeded_random_portable_test)

  # --- Phase 1 determinism: SAME-TICK timer firing order (mutation backprop) ---
  # Kills the surviving mutant at Scheduler.hpp:238 (the timer insertion-sort
  # `b - 1` -> `b - 0`). Arms MULTIPLE timers DUE AT THE SAME virtual tick with
  # the pending-timer vector holding them at SCRAMBLED indices (arm_seq order !=
  # vector order), and asserts they FIRE in the documented (due, arm_seq) order.
  # A `b - 0` no-op sort fires them in vector order instead -> this test FAILS,
  # killing that mutant. Links the header-only runtime only (no PRNG needed).
  add_executable(lockstep_scheduler_timer_order_test scheduler_timer_order_test.cpp)
  target_link_libraries(lockstep_scheduler_timer_order_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime)
  add_test(NAME scheduler_timer_order COMMAND lockstep_scheduler_timer_order_test)

  # --- S8.7 SharedState pool: recycle-on-last-drop, no early recycle ---
  # Locks the per-sink SharedState pool's load-bearing safety: a freed slot is
  # RECYCLED by the next mint, a still-held slot is NEVER aliased, and a
  # set-but-not-consumed state survives a dropped Promise (the no-UAF argument made
  # executable). Header-only runtime; ASan/UBSan watch the churn loop for any UAF.
  add_executable(lockstep_shared_state_pool_test shared_state_pool_test.cpp)
  target_link_libraries(lockstep_shared_state_pool_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime)
  add_test(NAME shared_state_pool COMMAND lockstep_shared_state_pool_test)

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

# --- Phase 7 S1: reusable boundary-provider CONTRACT CONFORMANCE suite --------
# P7-S1 gate (briefs/phase7.md S1; V-PROD-CONTRACT). The REUSABLE conformance
# harness any IClock/IRandom/IDisk/INetwork impl MUST pass: the UNIVERSAL contract
# tier (conformance::universal::*) parameterized over provider FACTORIES, run here
# against the SIM providers (validates the harness — sim already conforms) and
# the foundation every PROD provider in S2–S4 plugs into by swapping the factory.
# Cleanly separates (A) universal contract from (B) sim-only fault behaviours
# (torn/lying/drop/partition stay tier B, NOT in the universal suite). Asserts the
# universal suite PASSES vs sim + is a pure fn of seed (run twice, byte-identical
# Report). tests/ is on the include path so "provider_conformance/..." resolves.
# Links the header-only runtime + sim providers. Non-provider TU → forbidden-lint
# scans it; all time virtual, all randomness SeededRandom. Inherits TIMEOUT.
if(TARGET lockstep_runtime AND TARGET lockstep_providers_sim)
  add_executable(lockstep_provider_conformance_test provider_conformance_test.cpp)
  target_include_directories(lockstep_provider_conformance_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_provider_conformance_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME provider_conformance_test COMMAND lockstep_provider_conformance_test)
else()
  message(WARNING
    "lockstep_runtime / providers_sim not found; provider_conformance_test not "
    "built (Phase 7 S1 pending).")
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

# --- Phase 2 batch 2 (C2.7 + C2.8): seed-burn farm + shrinking gate -----------
# B2D self-test (specs/checker-framework.md §6.1, §7: V-SEED1/V-SEED2/V-SHRINK1).
# Asserts (A) the seed-burn farm runs N seeds with the MUST-HOLD invariants
# holding on the HONEST system (C-DUR=0, no INT-1/DUR-2 fabrication), tracking
# the expected failover anomalies; (B) a KNOWN buggy-variant failure auto-shrinks
# (delta-debugging) to a STRICTLY-SMALLER config that still triggers the SAME
# violation class (violation PRESERVED), replaying byte-identically; (C) a seed
# replays byte-identically (V-SEED1). Builds on the pure SeedBurn library
# (harness/include/lockstep/harness/SeedBurn.hpp) + BuggyKvSystems. Needs harness
# + sim providers + runtime. Builds only when all are present.
if(TARGET lockstep_harness AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_kv_seedburn_test kv_seedburn_test.cpp)
  target_include_directories(lockstep_kv_seedburn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_kv_seedburn_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_harness
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME kv_seedburn_test COMMAND lockstep_kv_seedburn_test)
else()
  message(WARNING
    "lockstep_harness / runtime / providers_sim not all found; "
    "kv_seedburn_test not built (Phase 2 batch 2 C2.7/C2.8 pending).")
endif()

# --- Phase 3 step 1: storage differential harness self-test ------------------
# P3-MACHINERY self-test (specs/storage-engine.md §4/§5 step 1). Proves the
# storage VERIFICATION MACHINERY before any real engine: the Engine interface
# seam + trivial Oracle + differential harness. Asserts (a) Oracle-vs-Oracle is an
# always-match baseline across a seed sweep (the comparator compares), (b) TEETH —
# deliberately-WRONG engines (off-by-one snapshot bound; a delete that doesn't
# delete) are FLAGGED with a replayable witness (a harness that passes a known-
# wrong engine is itself the bug), and (c) the harness is a pure fn of seed
# (same seed ⇒ byte-identical witness). Links the header-only storage lib + the
# runtime + sim PRNG (the harness drives the seeded op stream on the scheduler).
# Non-provider code → forbidden-lint scans it. Inherits the dir-wide TIMEOUT 90.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_storage_diff_test storage_diff_test.cpp)
  target_include_directories(lockstep_storage_diff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_storage_diff_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME storage_diff_test COMMAND lockstep_storage_diff_test)
else()
  message(WARNING
    "lockstep_storage / runtime / providers_sim not all found; "
    "storage_diff_test not built (Phase 3 step 1 pending).")
endif()

# --- Phase 3 step 2: real storage engine (WAL + memtable + MVCC + recovery) ---
# P3-ENGINE-CORE gate (specs/storage-engine.md §2/§4; §5 step 2). The FIRST real
# Lockstep engine (storage/WalEngine.hpp). Asserts (A) DIFFERENTIAL no-faults —
# WalEngine vs Oracle over a seed sweep, 0 mismatch (the core gate); (B) CRASH-
# CONSISTENCY under the full fault envelope — crash() at seeded points, recover()
# to a valid oracle PREFIX, 0 fabrication (V-PREFIX/V-NOTORN); (B') durable-
# survives on an honest disk (V-DUR); (C) MVCC under concurrent writers — a
# snapshot stays stable (V-SNAP); (D) determinism — same seed ⇒ byte-identical
# recovered reads + on-disk durable image fingerprint (V-DET). Links the header-
# only storage lib + runtime + sim PRNG + SimDisk. Non-provider TU → forbidden-
# lint scans it. Inherits the dir-wide TIMEOUT 90.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_storage_engine_test storage_engine_test.cpp)
  target_include_directories(lockstep_storage_engine_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_storage_engine_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME storage_engine_test COMMAND lockstep_storage_engine_test)
else()
  message(WARNING
    "lockstep_storage / runtime / providers_sim not all found; "
    "storage_engine_test not built (Phase 3 step 2 pending).")
endif()

# --- Phase 3 step 4: SSTable layer (flush + manifest + bloom + scan) ----------
# P3-SSTABLE gate (specs/storage-engine.md §2/§3/§6; §5 step 4). The memtable
# flushes to immutable on-disk SSTables (per-block CRC + sparse index + bloom +
# CRC'd footer), atomically installed via a CRC'd Seq-contiguous MANIFEST; reads
# span memtable + SSTables (newest→oldest); scan() merges them at a snapshot.
# Asserts (A) DIFFERENTIAL WITH FLUSHES — WalEngine(LSM) vs Oracle over a seed
# sweep, get+scan, 0 mismatch (the core gate, with many forced flushes); (B)
# METAMORPHIC — a forced flush changes no visible get/scan value (flush ⊥ change
# reads); (C) CRASH-DURING-FLUSH — crash incl. mid-flush under the full fault
# envelope, recover from the manifest's committed SSTable set + WAL prefix to a
# valid oracle PREFIX, 0 fabrication, no half-SSTable surviving (V-PREFIX/
# V-NOTORN, atomic install); (D) BLOOM no false negative (a present key is never
# skipped); (E) determinism — same seed ⇒ byte-identical recovered reads + WAL +
# SSTable on-disk fingerprint (V-DET). Links the header-only storage lib +
# runtime + sim PRNG + SimDisk. Non-provider TU → forbidden-lint scans it.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_storage_sstable_test storage_sstable_test.cpp)
  target_include_directories(lockstep_storage_sstable_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_storage_sstable_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME storage_sstable_test COMMAND lockstep_storage_sstable_test)
else()
  message(WARNING
    "lockstep_storage / runtime / providers_sim not all found; "
    "storage_sstable_test not built (Phase 3 step 4 pending).")
endif()

# --- Phase 3 step 5: compaction + version GC + WAL truncation -----------------
# P3-COMPACTION gate (specs/storage-engine.md §2 V-GC/V-PREFIX/V-NOTORN; §3/§6;
# §5 step 5). Size-tiered compaction k-way-merges the live SSTable set into ONE,
# dropping versions no live snapshot can see (GC under a watermark), then
# truncates the WAL prefix the merged set durably covers — all atomic via the
# append-only, CRC'd, entry_no-contiguous MANIFEST (INSTALL/OBSOLETE/WAL-TRUNCATE
# records). Asserts (A) DIFFERENTIAL WITH COMPACTION — WalEngine(LSM+compaction)
# vs Oracle over a seed sweep, get+scan, 0 mismatch (many flushes+compactions);
# (B) METAMORPHIC — a forced compaction changes no visible get/scan value
# (compaction ⊥ change reads); (C) GC-SAFETY (V-GC) — a held live snapshot still
# sees its versions after a watermark-bounded compaction AND dead versions are
# reclaimed (space drops); (D) CRASH-DURING-COMPACTION — crash incl. mid-
# compaction under the full fault envelope, recover from the folded manifest live
# set + WAL prefix to a valid oracle PREFIX, 0 fabrication, no half-/mixed-SSTable
# loss (V-PREFIX/V-NOTORN, atomic install+obsolete); (E) determinism — same seed
# ⇒ byte-identical recovered reads + WAL + SSTable on-disk fingerprint (V-DET).
# Links the header-only storage lib + runtime + sim PRNG + SimDisk. Non-provider
# TU → forbidden-lint scans it. Inherits the dir-wide TIMEOUT 90.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_storage_compaction_test storage_compaction_test.cpp)
  target_include_directories(lockstep_storage_compaction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_storage_compaction_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME storage_compaction_test COMMAND lockstep_storage_compaction_test)
else()
  message(WARNING
    "lockstep_storage / runtime / providers_sim not all found; "
    "storage_compaction_test not built (Phase 3 step 5 pending).")
endif()

# --- Phase 3 step 6: WiscKey large-value separation ---------------------------
# P3-WISCKEY gate (specs/storage-engine.md §1/§2/§3/§6 V-NOTORN/V-PREFIX/V-GC;
# master-plan D4). A value over a size threshold is written ONCE to an append-only
# VALUE LOG (CRC'd records); the LSM stores only a small {gen,offset,len,crc}
# pointer. Small values stay inline. Asserts (A) DIFFERENTIAL WITH LARGE VALUES —
# WalEngine(LSM+WiscKey) vs an INLINE Oracle over a seed sweep on a mixed small/
# large workload, get+scan, 0 mismatch (the value is identical inline or
# separated); (B) WRITE-AMP — a compaction of large values writes FAR fewer
# SSTable bytes than rewriting them inline would (the WiscKey point); (C) CRASH-
# AROUND-VLOG — crash incl. around the vlog append / between vlog and WAL under the
# full fault envelope (torn + lying-fsync on WAL + vlog), recover to a valid oracle
# PREFIX, 0 fabrication, no pointer resolving to a torn/fabricated value
# (V-NOTORN); (D) determinism — same seed ⇒ byte-identical recovered reads + WAL +
# SSTable + VLOG on-disk fingerprint (V-DET). Links the header-only storage lib +
# runtime + sim PRNG + SimDisk. Non-provider TU → forbidden-lint scans it.
if(TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_storage_wisckey_test storage_wisckey_test.cpp)
  target_include_directories(lockstep_storage_wisckey_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_storage_wisckey_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME storage_wisckey_test COMMAND lockstep_storage_wisckey_test)
else()
  message(WARNING
    "lockstep_storage / runtime / providers_sim not all found; "
    "storage_wisckey_test not built (Phase 3 step 6 pending).")
endif()

# --- Phase 4 Stage M: consensus verification machinery teeth gate ------------
# P4-MACHINERY gate (specs/Consensus.tla; briefs/phase4.md Stage M). Proves the
# consensus SEAM (consensus/ConsensusNode.hpp) + the conformance / linearizability
# / cross-check HARNESS have TEETH — BEFORE either real Raft impl exists. Asserts
# (A) each deliberately-WRONG ConsensusNode stub is FLAGGED by the conformance
# checker mapping to the Consensus.tla invariant it breaks (ElectionSafety /
# LogMatching / StateMachineSafety / LeaderAppendOnly), with witness + replayable
# seed; (B) the no-op "always-follower" baseline does NOT false-positive the
# safety checkers yet makes NO progress (vacuous safety ≠ correctness); (C) the
# dual-impl cross-check agrees impl-vs-itself and catches a divergent pair; (D)
# determinism — same seed ⇒ byte-identical ObservedRun (in-test + external diff).
# Links the header-only consensus lib + runtime + sim PRNG/disk/network + harness
# (the driver wires SimNetwork + SimDisk on the scheduler; reuses the harness
# Verdict shape). Non-provider TU → forbidden-lint scans it. Inherits TIMEOUT 90.
if(TARGET lockstep_consensus AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim AND TARGET lockstep_harness)
  add_executable(lockstep_consensus_harness_test consensus_harness_test.cpp)
  target_include_directories(lockstep_consensus_harness_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_consensus_harness_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_consensus
      lockstep_harness
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME consensus_harness_test COMMAND lockstep_consensus_harness_test)

  # --- Phase 4 Stage I: DUAL independent Raft implementations (pre-wired) ------
  # impl A and impl B are authored by TWO different agents against the same seam
  # + verified spec; targets pre-registered here so neither agent edits this file.
  # Each builds only if its source exists (half-landed batch still configures).
  foreach(impl_test consensus_impl_a_test consensus_impl_b_test)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${impl_test}.cpp")
      add_executable(lockstep_${impl_test} ${impl_test}.cpp)
      target_include_directories(lockstep_${impl_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(lockstep_${impl_test}
        PRIVATE
          lockstep_warnings
          lockstep_sanitizers
          lockstep_consensus
          lockstep_harness
          lockstep_runtime
          lockstep_providers_sim)
      add_test(NAME ${impl_test} COMMAND lockstep_${impl_test})
    endif()
  endforeach()

  # --- Phase 4 Stage V: DUAL-IMPL CROSS-CHECK GATE (the Phase-4 gate) ----------
  # P4-XCHECK gate (master-plan §6.5; briefs/phase4.md Stage V). Runs BOTH
  # independently-built Raft impls (raft_a::RaftNodeA + raft_b::RaftNodeB, each
  # already conformant) on the SAME seed + workload + fault schedule over a seed
  # sweep under the full fault envelope and asserts: (1) AGREE — their committed
  # logs never conflict at a shared index (one is a prefix of the other), via
  # cross_check; (2) CONFORM — both impls individually pass all five conformance
  # checkers (belt and suspenders); (3) PROGRESS — commits accumulate (agreement
  # non-vacuous); (4) DETERMINISM — same seed ⇒ byte-identical output (in-test +
  # external double-run diff under a stable marker). Builds only if its source
  # exists. Non-provider TU → forbidden-lint scans it. Inherits TIMEOUT 90.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/consensus_crosscheck_test.cpp")
    add_executable(lockstep_consensus_crosscheck_test consensus_crosscheck_test.cpp)
    target_include_directories(lockstep_consensus_crosscheck_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_consensus_crosscheck_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_consensus
        lockstep_harness
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME consensus_crosscheck_test
             COMMAND lockstep_consensus_crosscheck_test)
  endif()

  # --- Phase 4 C4.3: LOG SNAPSHOTTING / COMPACTION gate (the snapshot gate) ----
  # P4-SNAPSHOT gate (specs/Snapshot.tla, model-checked clean + committed). Drives
  # BOTH independently-built Raft impls (raft_a + raft_b) under a snapshot-heavy
  # fault storm (≫threshold submits + crash/restart so a node must be caught up by
  # InstallSnapshot) and asserts conformance to Snapshot.tla WITHOUT regressing
  # Consensus.tla: (a) StateMachineSafety (no committed entry lost/diverged after
  # compaction); (b) RecoveredEqualsFull (a node caught up via InstallSnapshot has
  # the same committed state); (c) compaction actually fires + bounds the in-memory
  # log (measured); (d) determinism byte-identical; (e) the five checkers still 0
  # violations on both impls; (f) the A-vs-B cross-check still agrees. TEETH: a
  # deliberately-wrong compaction (empty/dropped snapshot state) is CAUGHT. Builds
  # only if its source exists. Non-provider TU → forbidden-lint scans it. Inherits
  # TIMEOUT 90.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/consensus_snapshot_test.cpp")
    add_executable(lockstep_consensus_snapshot_test consensus_snapshot_test.cpp)
    target_include_directories(lockstep_consensus_snapshot_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_consensus_snapshot_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_consensus
        lockstep_harness
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME consensus_snapshot_test
             COMMAND lockstep_consensus_snapshot_test)
  endif()

  # --- Phase 4 C4.2: single-server MEMBERSHIP CHANGE conformance gate --------
  # Proves both impls implement add/remove ONE server at a time conformant to
  # specs/Membership.tla (ElectionSafety/ConfigChangeSafety across the change,
  # QuorumOverlap, single-server chain, config-log up-to-date voting, commit-
  # before-next) WITHOUT regressing base Consensus/Snapshot conformance. TEETH: a
  # 2-server jump breaks overlap (mirrors specs/Membership2.cfg). Non-provider TU
  # -> forbidden-lint scans it. Inherits TIMEOUT 90.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/consensus_membership_test.cpp")
    add_executable(lockstep_consensus_membership_test consensus_membership_test.cpp)
    target_include_directories(lockstep_consensus_membership_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_consensus_membership_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_consensus
        lockstep_harness
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME consensus_membership_test
             COMMAND lockstep_consensus_membership_test)
  endif()
else()
  message(WARNING
    "lockstep_consensus / runtime / providers_sim / harness not all found; "
    "consensus_harness_test not built (Phase 4 Stage M pending).")
endif()

# --- Phase 4 C4.4: the multi-shard SEQUENCER conformance gate ----------------
# C4.4 gate (specs/Sequencer.tla, model-checked clean + committed). Proves the
# real multi-shard sequencer (consensus/sequencer/Sequencer.hpp) — the
# deterministic CROSS-shard merge that combines N per-shard committed consensus
# logs into ONE global total order (Calvin epoch batching) — conforms to the
# spec's four safety invariants on a seed sweep (<=64 in-gate): (a)
# GlobalOrderDeterministic (same shard logs => byte-identical global order, run
# twice); (b) PerShardOrderPreserved; (c) ExactlyOnceGlobal (each sealed input
# entry once, none fabricated); (d) EpochMonotone. Plus TEETH — a deliberately-
# wrong (reversed-idx / by-arrival) merge is CAUGHT by the determinism /
# per-shard-order assertion. Links ONLY the header-only consensus lib (the
# sequencer is a pure value function; no scheduler/sim needed). Non-provider TU
# -> forbidden-lint scans it. Inherits the dir-wide TIMEOUT.
if(TARGET lockstep_consensus)
  add_executable(lockstep_sequencer_test sequencer_test.cpp)
  target_include_directories(lockstep_sequencer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sequencer_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_consensus)
  add_test(NAME sequencer_test COMMAND lockstep_sequencer_test)
else()
  message(WARNING
    "lockstep_consensus target not found; sequencer_test not built "
    "(Phase 4 C4.4 pending).")
endif()

# --- Phase 5 Stage M: distributed-txn verification machinery teeth gate -------
# P5-MACHINERY gate (specs/CommitOrdering.tla; briefs/phase5.md Stage M). Proves
# the txn SEAM (txn/Transaction.hpp: the one-shot Transaction + the call-site-
# visible D5 selector) + the strict-serializable ORACLE + the differential /
# linearizability HARNESS + the per-D5-level CONSISTENCY CHECKERS have TEETH —
# BEFORE the real distributed-txn impl exists. Asserts (A) each deliberately-
# WRONG txn executor is FLAGGED by the checker mapping to the CommitOrdering.tla
# invariant / D5 rule it breaks (OLLPSound / SerializedBySeqLog / ReadsMatch
# SerialPrefix / D5ReadYourWrites), with witness + replayable seed; (B) the
# HONEST executor passes all checkers + makes progress (no false positive) and
# the no-progress baseline is vacuously safe but caught as making NO progress
# (vacuous != correct); (C) determinism — same seed => byte-identical rendered
# outcome (in-test + the external double-run diff under a stable marker). Links
# ONLY the header-only txn lib (self-contained value-shaped seam + checkers; no
# scheduler/sim needed at Stage M). Non-provider TU -> forbidden-lint scans it.
# Inherits the dir-wide TIMEOUT 90.
if(TARGET lockstep_txn)
  add_executable(lockstep_txn_harness_test txn_harness_test.cpp)
  target_include_directories(lockstep_txn_harness_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_txn_harness_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_txn)
  add_test(NAME txn_harness_test COMMAND lockstep_txn_harness_test)
else()
  message(WARNING
    "lockstep_txn target not found; txn_harness_test not built "
    "(Phase 5 Stage M pending).")
endif()

# --- Phase 5 Stage I: the REAL deterministic distributed-txn executor ---------
# P5-IMPL gate (specs/CommitOrdering.tla; briefs/phase5.md C5.1-C5.6). The FIRST
# real Lockstep transaction executor (txn/DeterministicExecutor.hpp): one-shot
# submission (C5.1), Calvin-style deterministic SEQUENTIAL commit ordering over
# the seqLog (C5.2, no 2PC), OLLP recon + bounded re-sequence at the txn's
# serialization-point snapshot (C5.3), the four D5 read levels each served to
# EXACTLY its contract (C5.4/C5.5), deterministic abort/retry + ExactlyOnce apply
# (C5.6) — backed by the REAL Phase-3 MVCC storage::WalEngine driven on the
# deterministic core::Scheduler. Asserts, over a seed sweep (<=64 in-gate, env
# override): the Stage-M DiffHarness + ALL 8 checkers pass with 0 violations
# (differential-vs-oracle match, strict-serializable/linearizable, each D5 level
# EXACTLY its contract, OLLP sound + retry terminates, ExactlyOnce) + PROGRESS
# (txns commit, not vacuous) + DETERMINISM (same seed => byte-identical rendered
# outcome). Links the txn lib + the real storage engine + runtime + sim PRNG/
# disk (the executor drives the engine on a scheduler internally). Non-provider
# TU -> forbidden-lint scans it. Inherits the dir-wide TIMEOUT 90.
if(TARGET lockstep_txn AND TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim)
  add_executable(lockstep_txn_impl_test txn_impl_test.cpp)
  target_include_directories(lockstep_txn_impl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_txn_impl_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME txn_impl_test COMMAND lockstep_txn_impl_test)

  # --- Phase 5 Stage I: the SECOND, INDEPENDENT txn executor (impl B) ----------
  # P5-IMPL-B gate (specs/CommitOrdering.tla; briefs/phase5.md; master-plan §6.5).
  # The DUAL independent transaction executor (txn/IndependentExecutor.hpp): built
  # FROM THE SPEC, NOT from impl A — a pure value model (history-scan
  # ValueAfterPrefix; NO MVCC engine, NO scheduler, NO coroutines) so the two impls
  # do not share a blind spot. Asserts the SAME deliverable as impl A's gate over a
  # seed sweep (<=64 in-gate, LOCKSTEP_TXN_SEEDS override): the Stage-M DiffHarness
  # + ALL 8 checkers, 0 violations (differential-vs-oracle, strict-serializable,
  # each D5 level EXACTLY its contract, OLLP sound + bounded retry, ExactlyOnce) +
  # PROGRESS + DETERMINISM. Links the txn lib only (the value model needs no engine).
  # Non-provider TU -> forbidden-lint scans it. Inherits the dir-wide TIMEOUT.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/txn_impl_b_test.cpp")
    add_executable(lockstep_txn_impl_b_test txn_impl_b_test.cpp)
    target_include_directories(lockstep_txn_impl_b_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_txn_impl_b_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_txn
        lockstep_storage
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME txn_impl_b_test COMMAND lockstep_txn_impl_b_test)
  endif()

  # --- Phase 5 Stage V: DUAL-IMPL CROSS-CHECK GATE (the §6.5 gate) -------------
  # P5-XCHECK gate (master-plan §6.5; briefs/phase5.md). Runs BOTH independently-
  # built txn executors (DeterministicExecutor impl A + IndependentExecutor impl B)
  # on the SAME seed + workload + ExecConfig over a seed sweep and asserts: (1)
  # CROSS-CHECK — A and B produce the SAME committed effects (per txn: status,
  # writes, result, seq/version, strict-read values); a divergence at a committed
  # effect = at least one is wrong (reported with seed + witnessing txn); (2)
  # CONFORM — both impls individually match the strict-serializable oracle + pass
  # the 8-checker battery; (3) PROGRESS — commits accumulate (agreement non-
  # vacuous); (4) DETERMINISM — same seed => byte-identical rendered outcome for
  # each impl. Links the txn lib + the real storage engine (impl A drives it) +
  # runtime + sim. Non-provider TU -> forbidden-lint scans it. Inherits TIMEOUT.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/txn_crosscheck_test.cpp")
    add_executable(lockstep_txn_crosscheck_test txn_crosscheck_test.cpp)
    target_include_directories(lockstep_txn_crosscheck_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_txn_crosscheck_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_txn
        lockstep_storage
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME txn_crosscheck_test COMMAND lockstep_txn_crosscheck_test)
  endif()

  # --- Phase 5/6: END-TO-END distributed-txn FAULT-STORM gate ------------------
  # P5-E2E gate (specs/CommitOrdering.tla + Consensus.tla + Sequencer.tla;
  # master-plan D1/D2/D5/§6.5). Closes the last verification debt: the Phase-5 txn
  # layer was verified over the seqLog ABSTRACTION + a real MVCC store, but NOT
  # end-to-end through the REAL Phase-4 consensus + Phase-3 storage under a
  # crash/partition fault storm. This wires the WHOLE stack: a REAL cluster of
  # consensus nodes (raft_a) ORDERS a seeded stream of one-shot txn submissions
  # UNDER the full fault envelope (partition/heal keeping quorum, crash/restart, net
  # reorder/drop/dup, SimDisk latency) on the deterministic scheduler; the COMMITTED
  # CONSENSUS LOG (the agreed total order, surviving crash/restart) feeds the
  # Phase-5 DeterministicExecutor over the REAL WalEngine MVCC store. Asserts, over
  # a seed sweep (<=32 in-gate, LOCKSTEP_E2E_SEEDS override; every run TERMINATES via
  # the run_until step-backstop -> progress_stalled==0): the committed history is
  # STRICT-SERIALIZABLE == the txn Oracle on the consensus order, EXACTLY-ONCE,
  # DURABLE across crash (cold recovery from the committed order reproduces it),
  # PROGRESS (non-vacuous), DETERMINISM (byte-identical), and TEETH (executing in
  # arrival order instead of the consensus order is caught). Links the txn lib + the
  # real consensus lib + storage + runtime + sim. Non-provider TU -> forbidden-lint
  # scans it. Inherits the dir-wide TIMEOUT 90.
  if(TARGET lockstep_consensus AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/txn_end_to_end_test.cpp")
    add_executable(lockstep_txn_end_to_end_test txn_end_to_end_test.cpp)
    target_include_directories(lockstep_txn_end_to_end_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_txn_end_to_end_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_txn
        lockstep_consensus
        lockstep_storage
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME txn_end_to_end_test COMMAND lockstep_txn_end_to_end_test)
  endif()

  # --- Phase 9 S9.3: CROSS-SHARD ATOMIC COMMIT gate (the hardest piece) --------
  # P9-XSHARD gate (specs/XShardCommit.tla, TLC-verified clean + teeth; +
  # Sequencer.tla C4.4 + CommitOrdering.tla Phase 5). Wires a transaction touching
  # keys on MULTIPLE shards through the ALREADY-VERIFIED building blocks: M
  # independent single-shard Raft clusters (S9.1 model) ORDER, under the full fault
  # storm, the txns that touch them (a cross-shard txn lands in EACH involved
  # shard's committed log); the per-shard committed logs feed the C4.4
  # Sequencer::merge -> ONE global order; the S9.3 dedup (txn/CrossShard.hpp)
  # collapses a cross-shard txn's per-shard appearances to ONE global position GATED
  # on it being committed on ALL its shards (all-or-nothing); the Phase-5
  # DeterministicExecutor applies each txn's whole write set ATOMICALLY at that one
  # position (Calvin-style, NO 2PC). Asserts over a seed sweep (<=16 in-gate x
  # cluster matrix, LOCKSTEP_XSHARD_SEEDS override): (a) ATOMICITY (XShardCommit.tla
  # AtomicAllOrNone, re-derived independently per shard — all-or-nothing, no partial
  # commit); (b) STRICT-SERIALIZABLE == the txn Oracle on the sequencer order +
  # EXACTLY-ONCE + the 8-checker battery, 0 violations; (c) DETERMINISM (same seed
  # => byte-identical global order + history); TEETH — a partial cross-shard commit
  # AND an out-of-sequencer-order apply are CAUGHT. Reuses the verified Sequencer +
  # DeterministicExecutor + Oracle + Checkers UNCHANGED. Links the txn lib + the
  # real consensus lib + storage + runtime + sim. Non-provider TU -> forbidden-lint
  # scans it. Inherits the dir-wide TIMEOUT.
  if(TARGET lockstep_consensus AND
     EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/txn_cross_shard_test.cpp")
    add_executable(lockstep_txn_cross_shard_test txn_cross_shard_test.cpp)
    target_include_directories(lockstep_txn_cross_shard_test
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lockstep_txn_cross_shard_test
      PRIVATE
        lockstep_warnings
        lockstep_sanitizers
        lockstep_txn
        lockstep_consensus
        lockstep_storage
        lockstep_runtime
        lockstep_providers_sim)
    add_test(NAME txn_cross_shard_test COMMAND lockstep_txn_cross_shard_test)
  endif()
else()
  message(WARNING
    "lockstep_txn / storage / runtime / providers_sim not all found; "
    "txn_impl_test not built (Phase 5 Stage I pending).")
endif()

# --- Phase 6 Stage F: developer-facing client + query surface CONFORMANCE -----
# P6-SURFACE gate (lockstep-phase-specs-all.md Phase 6 C6.1/C6.2; master-plan
# D1/D3/D5; briefs/phase6.md V-DET-USER / V-D5-SAFE / V-CONFORM). The CLIENT
# SURFACE (query/Database.hpp + query/Query.hpp): a typed Database that lets a
# user AUTHOR + submit ONE-SHOT txn functions over the verified txn::Executor
# seam (deterministic_factory) with a PURE deterministic handle (TxnContext: no
# clock/rng/IO — V-DET-USER, compile-enforced), plus a typed, composable, NON-SQL
# read surface (get/scan/range) over storage::Engine MVCC at a CALL-SITE-VISIBLE
# D5 level (Query<L> — V-D5-SAFE). Authors example one-shot txns (a bank-transfer
# workload) + reads at each D5 level THROUGH THE SURFACE over a seed sweep (<=64
# in-gate, LOCKSTEP_QUERY_SEEDS override) and asserts (A) results == the strict-
# serializable oracle + all 8 checkers, 0 violations on the default path; (B) each
# D5 level honors EXACTLY its contract (the matching txn D5 checker); (C) user txn
# bodies are pure (compile-enforced); (D) determinism (same seed => byte-
# identical); (E) TEETH — a Strict read served STALE is caught. Links the header-
# only query lib (which pulls txn + storage + runtime + sim). Non-provider TU ->
# forbidden-lint scans it. Inherits the dir-wide TIMEOUT.
if(TARGET lockstep_query AND TARGET lockstep_txn AND TARGET lockstep_storage
   AND TARGET lockstep_runtime AND TARGET lockstep_providers_sim)
  add_executable(lockstep_query_surface_test query_surface_test.cpp)
  target_include_directories(lockstep_query_surface_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_query_surface_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME query_surface_test COMMAND lockstep_query_surface_test)

  # --- Phase 6 Stage B (C6.3): the CLIENT<->SERVER WIRE PROTOCOL gate ---------
  # Drives a ClientStub <-> Server exchange over SimNetwork across a seed sweep
  # (LOCKSTEP_PROTOCOL_SEEDS override, <=64 in-gate) and asserts: (A) round-trip
  # correctness — a Submit/Query through the wire equals the same op done DIRECTLY
  # on the Server's Database surface (and thus the oracle); (B) EXACTLY-ONCE under
  # net faults (dup/reorder/drop) — a re-delivered Submit applies no duplicate txn
  # effect; (C) a TORN/corrupt frame is rejected at decode, never mis-decoded;
  # (D) determinism — same seed => byte-identical. Links the header-only query lib
  # (which pulls txn + storage + runtime + sim). Non-provider TU -> forbidden-lint
  # scans it. Inherits the dir-wide TIMEOUT.
  add_executable(lockstep_query_protocol_test query_protocol_test.cpp)
  target_include_directories(lockstep_query_protocol_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_query_protocol_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME query_protocol_test COMMAND lockstep_query_protocol_test)

  # --- Phase 6 Stage B (C6.6): the END-TO-END CONFORMANCE GATE ----------------
  # Exercises the FULL developer surface stack — the C6.4 reference Driver
  # (Connection) -> wire ClientStub -> SimNetwork -> wire Server -> Database ->
  # txn deterministic executor -> WalEngine MVCC — over a seed sweep under net
  # faults, and asserts: (A) end-to-end balances == the strict-serializable oracle
  # on the default path (clean AND under dup/reorder/drop); (B) EXACTLY-ONCE end
  # to end (a re-delivered Submit applies no duplicate effect); (C) each D5 level
  # driven through the driver honors EXACTLY its contract (the matching txn D5
  # checker, reusing txn/Oracle.hpp + txn/Checkers.hpp unchanged); (D) determinism
  # (same seed => byte-identical trace + balances); (E) TEETH — a stale Strict read
  # is caught. LOCKSTEP_CONFORMANCE_SEEDS override (<=64 in-gate). Non-provider TU
  # -> forbidden-lint scans it. Inherits the dir-wide TIMEOUT.
  add_executable(lockstep_query_conformance_test query_conformance_test.cpp)
  target_include_directories(lockstep_query_conformance_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_query_conformance_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME query_conformance_test COMMAND lockstep_query_conformance_test)

  # --- Phase 7 S5a closure: QUERY-PATH PERSISTENCE + CRASH RECOVERY -----------
  # Proves the committed query state is DURABLE on an injected core::IDisk and
  # RECOVERS on a restart (write via the durable query path -> "crash" (drop the
  # in-memory Database/Server, keep the SimDisk image) -> reopen over the SAME
  # IDisk -> recover() -> every committed value is queryable, recovered from the
  # query-side disk, NOT by replaying the consensus log). Reuses the verified
  # storage::WalEngine recovery. Deterministic (fixed SimDisk seed). Non-provider
  # TU -> forbidden-lint scans it. Inherits the dir-wide TIMEOUT.
  add_executable(lockstep_query_persist_test query_persist_test.cpp)
  target_include_directories(lockstep_query_persist_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_query_persist_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME query_persist_test COMMAND lockstep_query_persist_test)

  # --- SQL SURFACE: parser unit tests + SQL-vs-typed-query conformance ---------
  # A NEW layer (query/sql/*) that maps a bounded SQL subset onto the EXISTING
  # verified surface: SELECT -> Database::run(Query<L>) get/scan/range; INSERT/
  # UPDATE/DELETE -> a txn submit. core/sim/consensus/txn/storage UNCHANGED.
  #
  # sql_parser_test: the hand-written recursive-descent parser produces the
  # expected typed AST for valid input + a clean positioned error for invalid input
  # (deterministic; no exceptions/UB). sql_conformance_test: a seeded SQL workload
  # produces results IDENTICAL to an independent reference model (SELECT rows +
  # PK-ascending order + INSERT/UPDATE/DELETE effects + error reporting), a SELECT
  # AT SNAPSHOT honors the call-site-visible D5 level, determinism is byte-identical,
  # and the TEETH catch a wrong lowering. Links the header-only query lib (which
  # pulls txn + storage + runtime + sim). Non-provider TU -> forbidden-lint scans it.
  add_executable(lockstep_sql_parser_test sql_parser_test.cpp)
  target_include_directories(lockstep_sql_parser_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_parser_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_parser_test COMMAND lockstep_sql_parser_test)

  add_executable(lockstep_sql_conformance_test sql_conformance_test.cpp)
  target_include_directories(lockstep_sql_conformance_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_conformance_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_conformance_test COMMAND lockstep_sql_conformance_test)

  # sql_columnar_codec_test: the COLUMNAR key-schema codec gate (pure functions, no
  # engine/storage). Proves col_key grouping/order + per-column value round-trip +
  # PK recovery + namespace disjointness BEFORE the columnar write/read path is wired,
  # so a layout bug can't reach the durable core. Step 1 of the columnar rollout.
  add_executable(lockstep_sql_columnar_codec_test sql_columnar_codec_test.cpp)
  target_include_directories(lockstep_sql_columnar_codec_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_columnar_codec_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_columnar_codec_test COMMAND lockstep_sql_columnar_codec_test)

  # sql_column_block_test: the DURABLE columnar SoA-block codec gate (pure functions).
  # Proves the byte-exact block serialisation (round-trip INT/TEXT/NULL + determinism +
  # empty/all-null) BEFORE the columnar storage engine's write/read/flush path is wired,
  # so a format bug can't reach the durable core. Foundation of columnar option A.
  add_executable(lockstep_sql_column_block_test sql_column_block_test.cpp)
  target_include_directories(lockstep_sql_column_block_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_column_block_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_column_block_test COMMAND lockstep_sql_column_block_test)

  # sql_columnar_test: the COLUMNAR layout end-to-end conformance gate. Runs an IDENTICAL
  # seeded workload (INSERT/NULL/UPDATE/DELETE/projection/PK-fast/filter/aggregate/index)
  # on a columnar engine and a row-mode engine; asserts byte-identical output for every
  # statement. Row-mode is already proven == the reference model, so columnar == reference.
  add_executable(lockstep_sql_columnar_test sql_columnar_test.cpp)
  target_include_directories(lockstep_sql_columnar_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_columnar_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_columnar_test COMMAND lockstep_sql_columnar_test)

  # sql_wire_test: SQL-OVER-THE-WIRE end-to-end gate. Drives a CREATE/INSERT/UPDATE/DELETE/
  # SELECT session through the real ClientStub<->Server wire exchange over SimNetwork; asserts
  # the per-statement result (ok/error/affected/row-count) is byte-identical to a twin in-process
  # SqlEngine oracle, and EXACTLY-ONCE under dup/drop/reorder faults (a retried SqlExec must not
  # re-apply). Proves SQL runs through the wire, not just in-process.
  add_executable(lockstep_sql_wire_test sql_wire_test.cpp)
  target_include_directories(lockstep_sql_wire_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_wire_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_wire_test COMMAND lockstep_sql_wire_test)

  # sql_distributed_test: the DISTRIBUTED-SQL (scatter-gather) gate. A table sharded across M
  # SqlEngines via DistributedSql (CREATE broadcast, INSERT routed by PK hash, aggregate SELECT
  # scattered + merged by kind) must answer BYTE-IDENTICALLY to one SqlEngine holding all rows —
  # scalar + GROUP BY aggregates, filtered, point read; rows proven spread across shards.
  add_executable(lockstep_sql_distributed_test sql_distributed_test.cpp)
  target_include_directories(lockstep_sql_distributed_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_distributed_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_distributed_test COMMAND lockstep_sql_distributed_test)

  # sql_distributed_wire_test: DISTRIBUTED SQL OVER THE WIRE (B5). The same DistributedSql coordinator
  # but each shard is a REMOTE SqlEngine reached over wire::Server <-> ClientStub on SimNetwork, with
  # SELECT rows shipped back in the SqlResult blob and merged. Asserts byte-identical to one in-process
  # engine — SQL is no longer in-process-only; the coordinator is transport-agnostic.
  add_executable(lockstep_sql_distributed_wire_test sql_distributed_wire_test.cpp)
  target_include_directories(lockstep_sql_distributed_wire_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_distributed_wire_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_distributed_wire_test COMMAND lockstep_sql_distributed_wire_test)

  # sql_catalog_recover_test: the DURABLE SQL CATALOG + recover gate (C7). Build a table (+ index +
  # rows) through a durable SqlEngine, drop it (a crash) keeping the disk, reopen + recover() — the
  # schema AND data must be back (queries that would error "unknown table" now equal the no-crash
  # oracle byte-for-byte). Proves the SQL catalog survives a restart, not just the row/block data.
  add_executable(lockstep_sql_catalog_recover_test sql_catalog_recover_test.cpp)
  target_include_directories(lockstep_sql_catalog_recover_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_catalog_recover_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_catalog_recover_test COMMAND lockstep_sql_catalog_recover_test)

  # sql_wire_catalog_recover_test: the wire/prod analogue — a wire::Server with SEPARATE durable
  # disks for keyed/SQL-data/SQL-catalog runs DDL+INSERT via dispatch(), is dropped (a crash), then
  # reconstructed over the same disks + recover_sql(data_len, catalog_len). The table + rows must be
  # back. Validates the durable-catalog wiring (own WAL/Seq line) that ProdServerNode/lockstepd use.
  add_executable(lockstep_sql_wire_catalog_recover_test sql_wire_catalog_recover_test.cpp)
  target_include_directories(lockstep_sql_wire_catalog_recover_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_wire_catalog_recover_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_wire_catalog_recover_test COMMAND lockstep_sql_wire_catalog_recover_test)

  # sql_columnar_durability_test: the columnar crash/recovery gate (A5). Proves the
  # committed columnar state (blocks + delta) survives a crash + recovery byte-for-byte
  # over a deterministic SimDisk — projection/aggregate/GROUP BY identical post-recovery,
  # flush atomicity (one commit batch), determinism. Columnar is a new storage path;
  # durability is sacrosanct.
  add_executable(lockstep_sql_columnar_durability_test sql_columnar_durability_test.cpp)
  target_include_directories(lockstep_sql_columnar_durability_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_columnar_durability_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_columnar_durability_test COMMAND lockstep_sql_columnar_durability_test)

  # sql_aggregates_test (SQL v2): the EXTENDED SELECT surface — WHERE on ANY column
  # (a general boolean predicate tree), aggregates (COUNT(*)/COUNT(col)/SUM/MIN/MAX/
  # AVG) + GROUP BY + HAVING, ORDER BY [ASC|DESC] + LIMIT/OFFSET, DISTINCT. Each is
  # cross-checked == an INDEPENDENT reference model (hand-written relational ops over
  # a typed-row vector, NO KV/Database/txn path): SQL rows/order/group/aggregate
  # values/errors == the model exactly. AVG over INT truncates toward zero (pinned).
  # The TEETH catch a wrong aggregate (SUM-as-COUNT) + a dropped ORDER BY. Still pure
  # sugar over the verified scan/get/range surface; deterministic (seeded SplitMix).
  add_executable(lockstep_sql_aggregates_test sql_aggregates_test.cpp)
  target_include_directories(lockstep_sql_aggregates_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_aggregates_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_aggregates_test COMMAND lockstep_sql_aggregates_test)

  # sql_multirow_insert_test (D6): INSERT ... VALUES (..),(..) == N single-row inserts, atomic.
  add_executable(lockstep_sql_multirow_insert_test sql_multirow_insert_test.cpp)
  target_include_directories(lockstep_sql_multirow_insert_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_multirow_insert_test
    PRIVATE lockstep_warnings lockstep_sanitizers lockstep_query lockstep_txn
            lockstep_storage lockstep_runtime lockstep_providers_sim)
  add_test(NAME sql_multirow_insert_test COMMAND lockstep_sql_multirow_insert_test)

  # sql_join_test (SQL v3): the EXTENDED FROM surface — INNER / LEFT [OUTER] / comma-
  # cross joins with ON predicates, qualified columns (table.col + AS alias), multi-
  # join (left-deep), self-join, 3-table join, and NULL-introducing LEFT joins. Each
  # is cross-checked == an INDEPENDENT relational reference model (hand-written
  # join/filter/group/order ops over typed-row maps, NO KV/Database/txn path): joined
  # rows / default order / NULL semantics (COUNT(col) skips NULL, comparison-with-NULL
  # is false) / aggregates == the model exactly. The hash-join and nested-loop paths
  # agree. The TEETH catch a wrong join (LEFT's NULL rows returned for an INNER, or a
  # dropped ON => cartesian). Still pure sugar over the verified scan surface.
  add_executable(lockstep_sql_join_test sql_join_test.cpp)
  target_include_directories(lockstep_sql_join_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_join_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_join_test COMMAND lockstep_sql_join_test)

  # sql_index_test (SQL secondary indexes): CREATE INDEX + INSERT/UPDATE/DELETE with
  # atomic index maintenance + the planner index access path (indexed eq / BETWEEN ->
  # index range scan -> point-get -> residual predicate). Each indexed query is cross-
  # checked == an INDEPENDENT reference model (no indexes) over a seeded workload:
  # the indexed result == the full-scan/reference result (an index is an access path,
  # not a semantic change). The TEETH catch a deliberately STALE index (skipped UPDATE
  # maintenance) + a dropped residual predicate. Determinism byte-identical. Still pure
  # sugar (KV pairs + planner) over the verified scan/get/range surface.
  add_executable(lockstep_sql_index_test sql_index_test.cpp)
  target_include_directories(lockstep_sql_index_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_index_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_index_test COMMAND lockstep_sql_index_test)

  # sql_subquery_test (SQL v4): explicit NULL (nullable columns + IS [NOT] NULL +
  # three-valued logic in WHERE/aggregates/GROUP BY) + subqueries (scalar (SELECT agg),
  # [NOT] IN (SELECT col), [NOT] EXISTS (...)). Each is cross-checked == an INDEPENDENT
  # reference model (ordered maps of typed cells with explicit NULLs, NO KV/Database/txn
  # path): IS NULL / comparison-with-NULL-dropped / aggregates-skip-NULL / the NULL-in-
  # NOT-IN UNKNOWN rule / scalar-subquery cardinality error == the model over a LARGE
  # seeded sweep. The TEETH catch a wrong NULL rule (NULL=NULL true / NOT IN ignoring a
  # subquery NULL) + a missed scalar-subquery cardinality error. Determinism byte-
  # identical. Still pure sugar over the verified scan/get/range surface (subqueries
  # reuse the SELECT pipeline; NULL is row/predicate-level).
  add_executable(lockstep_sql_subquery_test sql_subquery_test.cpp)
  target_include_directories(lockstep_sql_subquery_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_subquery_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim)
  add_test(NAME sql_subquery_test COMMAND lockstep_sql_subquery_test)
else()
  message(WARNING
    "lockstep_query / txn / storage / runtime / providers_sim not all found; "
    "query_surface_test not built (Phase 6 Stage F pending).")
endif()

# --- Phase 7 S2: PROD provider conformance + record-replay --------------------
# P7-S2 gate (briefs/phase7.md S2; V-PROD-CONTRACT + V-PROD-REPLAY). Runs the
# REUSABLE S1 universal suite (conformance::universal::*) VERBATIM against the
# PROD providers (prod::ProdRandom — full tier-A; prod::ProdClock — now()-monotonic
# only, delay/timers deferred to S4), proves sim==prod PRNG byte-identity for
# several seeds, and proves the record-replay TRACE round-trips byte-identical.
# tests/ is on the include path so "provider_conformance/..." resolves. Links the
# header-only runtime + sim + prod providers. Non-provider TU -> forbidden-lint
# scans it (no <chrono>/<random>/<thread> here; real time/entropy stays in
# providers/prod/). Inherits the dir-wide TIMEOUT.
if(TARGET lockstep_runtime AND TARGET lockstep_providers_sim
   AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_provider_test prod_provider_test.cpp)
  target_include_directories(lockstep_prod_provider_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_provider_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_sim
      lockstep_providers_prod)
  add_test(NAME prod_provider_test COMMAND lockstep_prod_provider_test)
else()
  message(WARNING
    "lockstep_runtime / providers_sim / providers_prod not all found; "
    "prod_provider_test not built (Phase 7 S2 pending).")
endif()

# --- Phase 7 S3: PROD Disk conformance + crash-injection + disk record-replay -
# P7-S3 gate (briefs/phase7.md S3; V-PROD-CONTRACT + V-PROD-REPLAY). Runs the
# REUSABLE S1 universal IDisk suite (conformance::universal::check_disk_contract)
# VERBATIM against the PROD file-backed disk (prod::ProdDisk) via a ProdDiskFactory
# with the SAME member shape as the S1 SimDiskFactory; a CRASH-INJECTION durability
# test over a REAL temp file (synced survives close+reopen byte-identical; nothing
# synced lost — no lying fsync; un-synced tail may be absent); and a disk
# RECORD->REPLAY byte-identity proof. tests/ is on the include path so
# "provider_conformance/..." resolves. Links the header-only runtime + sim + prod
# providers. Non-provider TU -> forbidden-lint scans it (NO <chrono>/<random>/
# <thread>, NO raw file IO here; all real disk IO stays in providers/prod/). The
# test owns a UNIQUE mkdtemp scratch dir and cleans up after itself. Inherits the
# dir-wide TIMEOUT.
if(TARGET lockstep_runtime AND TARGET lockstep_providers_sim
   AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_disk_test prod_disk_test.cpp)
  target_include_directories(lockstep_prod_disk_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_disk_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_sim
      lockstep_providers_prod)
  add_test(NAME prod_disk_test COMMAND lockstep_prod_disk_test)
else()
  message(WARNING
    "lockstep_runtime / providers_sim / providers_prod not all found; "
    "prod_disk_test not built (Phase 7 S3 pending).")
endif()

# --- Phase 7 S4a: PROD REACTOR (epoll event loop + real timers) [LINUX ONLY] --
# P7-S4a gate (briefs/phase7.md S4; the loop + real timers, NO sockets yet). The
# prod REACTOR (providers/prod/ProdReactor.hpp) is a real epoll-driven event loop
# implementing the SAME two core surfaces the sim Scheduler does (core::IScheduler
# + core::detail::SchedulerSink), so the verified async core (Future/Promise/Task)
# runs on it UNCHANGED. delay() is wired to it via prod::ITimerRegistrar, resolving
# the S2 deferral. Asserts the deferred S1 clock checks now pass on prod, ADAPTED
# from sim's exact-virtual-d to a real-elapsed tolerance: (a) timers-fire-in-time-
# order (universal — the nearer deadline fires first); (b) delay completes after
# >= d REAL elapsed (the prod analogue of sim's exact-d advance; the exact-d
# assertion is sim-specific virtual time, NOT universal); (c) delay(<=0) completes
# promptly without hanging; (d) the core actually runs on the reactor — a coroutine
# co_awaits ProdClock::delay() a couple times + a Promise resolved by another
# scheduled task, driven by ProdReactor::run() to quiescence, completes in real-time
# order. epoll is LINUX-ONLY: this target is added ONLY on Linux (if(UNIX AND NOT
# APPLE)) so the macOS host build stays green (the reactor target simply absent).
# Bounded: real timers are a few ms; run() drives to quiescence. Non-provider TU ->
# forbidden-lint scans it (NO <chrono>/<thread>/<sys/epoll.h> here; all real time +
# epoll stay inside providers/prod/ProdReactor.hpp). Inherits the dir-wide TIMEOUT.
if(UNIX AND NOT APPLE AND TARGET lockstep_runtime AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_reactor_test prod_reactor_test.cpp)
  target_include_directories(lockstep_prod_reactor_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_reactor_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME prod_reactor_test COMMAND lockstep_prod_reactor_test)
endif()

# sql_parallel_agg_test: the MORSEL-PARALLELISM determinism gate. The columnar scalar-aggregate
# fast path folds its chunks across ProdParallelExecutor worker threads; this asserts the parallel
# result is BYTE-IDENTICAL to the serial fold for every worker count / type / size (the fixed-order
# merge makes parallelism a pure wall-clock optimization). Links providers_prod (the threads live
# there); TSan in the full gate proves the pool dispatch/merge race-free. Linux-only (prod-threaded).
if(UNIX AND NOT APPLE AND TARGET lockstep_query AND TARGET lockstep_providers_prod)
  add_executable(lockstep_sql_parallel_agg_test sql_parallel_agg_test.cpp)
  target_include_directories(lockstep_sql_parallel_agg_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_sql_parallel_agg_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME sql_parallel_agg_test COMMAND lockstep_sql_parallel_agg_test)
endif()

# --- Phase 9 S9.2: PROD io_uring async-fdatasync path [LINUX ONLY] ------------
# P9-S9.2 gate. The prod io_uring ring (providers/prod/ProdUring.hpp) + the async
# fdatasync path on ProdDisk (the reactor-bound ctor), integrated with the reactor's
# epoll loop (the ring fd is registered; CQEs are reaped + resolve the parked promise).
# Asserts: (A) ring setup / availability (cleanly !valid() + SYNC fallback when seccomp
# blocks io_uring_setup — correctness never depends on the ring); (B) an async fsync
# completion fires on its CQE (res>=0 == durable); (C) the DURABILITY BARRIER — append
# then co_await the ASYNC sync(), and the bytes SURVIVE a close+reopen byte-identical
# (a synced prefix is never lost; nothing is acked before the fsync CQE). io_uring is
# LINUX-ONLY: added ONLY on Linux so the macOS host build stays green. Bounded (tiny
# payloads + an absolute reactor-deadline wall guard so a missing CQE can NEVER hang).
# NOTE: the async assertions engage only with --security-opt seccomp=unconfined (default
# Docker seccomp BLOCKS io_uring_setup → the test takes the SYNC-fallback path + passes).
if(UNIX AND NOT APPLE AND TARGET lockstep_runtime AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_uring_test prod_uring_test.cpp)
  target_include_directories(lockstep_prod_uring_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_uring_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME prod_uring_test COMMAND lockstep_prod_uring_test)
endif()

# --- Phase 7 S4b: PROD NETWORK (real TCP on the epoll reactor) [LINUX ONLY] ---
# P7-S4b gate (briefs/phase7.md S4; real sockets on the S4a reactor). The prod
# INetwork provider (providers/prod/ProdNetwork.hpp) moves bytes over real TCP on
# loopback, registered on the S4a ProdReactor's epoll loop (add_fd/remove_fd), so
# the verified async core runs UNCHANGED. Asserts: (1) the tier-A INetwork universal
# checks pass on prod with TWO nodes on loopback (local stable; send accepted; recv
# from+payload correct; per-link delivery in SEND ORDER — a TCP guarantee resolving
# the S1 per-link-order flag); (2) an integration test exchanges messages, DROPS a
# connection, RECONNECTS, and messages flow again with no corruption; (3) a network
# RECORD -> REPLAY round-trip reproduces identical recv observations. epoll +
# sockets are LINUX-ONLY: this target is added ONLY on Linux (if(UNIX AND NOT
# APPLE)) so the macOS host build stays green (the target simply absent). Bounded:
# small message counts + an absolute reactor deadline guard so a lost frame / half-
# open socket can NEVER hang the loop. Non-provider TU -> forbidden-lint scans it
# (NO socket/epoll syscall here; all real network plumbing stays inside
# providers/prod/). Inherits the dir-wide TIMEOUT.
if(UNIX AND NOT APPLE AND TARGET lockstep_runtime AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_network_test prod_network_test.cpp)
  target_include_directories(lockstep_prod_network_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_network_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME prod_network_test COMMAND lockstep_prod_network_test)
endif()

# --- Phase 7 S5a: SINGLE-NODE PROD SERVER (real TCP round-trip) [LINUX ONLY] --
# P7-S5a gate (briefs/phase7.md S5; the precursor to lockstepd, multi-node Raft is
# S5b). Stands up a prod::ProdServerNode (ProdNetwork real TCP -> wire::Server ->
# Database -> txn deterministic executor -> MVCC store) on the PROD providers driven
# by the S4a ProdReactor, and a REAL wire client (wire::ClientStub / query::Connection
# over a ProdNetwork client endpoint) connecting over LOOPBACK: it submits a txn,
# reads the value back, and asserts it MATCHES (and a second DISTINCT submit applies
# EXACTLY once — applied_submits() advances by one; reads do not apply). Proves the
# sim-proven client-facing stack runs on real sockets with ZERO change to core/sim/
# query/txn/storage. Everything driven by ONE in-process reactor; BOUNDED with an
# ABSOLUTE reactor deadline so a lost connection can NEVER hang. epoll/sockets are
# LINUX-ONLY -> target added ONLY on Linux (if(UNIX AND NOT APPLE)) so the macOS host
# stays green (the target simply absent). Non-provider TU -> forbidden-lint scans it
# (NO socket/epoll/clock syscall here; all real plumbing stays inside providers/prod/;
# the client surface is the UNCHANGED wire::ClientStub / Connection). Links the query
# lib (wire::Server/ClientStub + Connection) + the prod providers (reactor/network/
# disk/clock/random). Inherits the dir-wide TIMEOUT.
if(UNIX AND NOT APPLE AND TARGET lockstep_query AND TARGET lockstep_txn
   AND TARGET lockstep_storage AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_server_test prod_server_test.cpp)
  target_include_directories(lockstep_prod_server_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_server_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_sim
      lockstep_providers_prod)
  add_test(NAME prod_server_test COMMAND lockstep_prod_server_test)

  # --- Phase 7 S5a closure: QUERY STATE on a REAL ProdDisk file [LINUX ONLY] ---
  # Networking-free prod half of the persistence gate: drives query::Database over
  # an injected prod::ProdDisk (real file) — apply_committed (WAL'd + fdatasync'd) ->
  # "crash" (drop the DB + close the fd, file survives) -> reopen ProdDisk + Database
  # over the SAME file -> recover() -> every committed value queryable from the real
  # file (NOT the consensus log). Proves the seam recovers on a REAL device, not just
  # SimDisk. No reactor/TCP (independent of the prod-server round-trip path). Inherits
  # the dir-wide TIMEOUT.
  add_executable(lockstep_query_persist_prod_test query_persist_prod_test.cpp)
  target_include_directories(lockstep_query_persist_prod_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_query_persist_prod_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_query
      lockstep_txn
      lockstep_storage
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME query_persist_prod_test COMMAND lockstep_query_persist_prod_test)
endif()

# --- Phase 7 S5b-1: REAL Raft CONSENSUS node on PROD providers [LINUX ONLY] ---
# P7-S5b-1 gate (briefs/phase7.md S5; the 1-node consensus precursor to multi-PROCESS
# Raft, which is S5b-2). Assembles a REAL Raft ConsensusNode (impl A via
# make_raft_a_factory) on the PROD providers (ProdReactor epoll loop + the reactor's
# ONE shared ProdClock + ProdRandom(seed) + ProdNetwork real TCP + ProdDisk over the
# data dir) as a 1-node cluster, drives client SUBMITs over a minimal admin protocol
# on a real socket, observes COMMIT (1-node leader, quorum=1), then PROVES durable
# crash/restart recovery: a FRESH reactor/bus/node over the SAME data_dir rebuilds the
# committed log FROM the real ProdDisk file (consensus.wal) — the durability proof
# S5a flagged (its query store was in-memory; here the consensus LOG is on ProdDisk).
# Links the consensus lib (RaftNodeA + ConsensusNode) + the prod providers (reactor/
# network/disk/clock/random). epoll/sockets LINUX-ONLY -> built ONLY on Linux
# (if(UNIX AND NOT APPLE)) so the macOS host stays green (target absent). NOT in the
# providers/ lint-exempt zone: forbidden-lint scans it (NO raw syscall here — only
# the provider + consensus surfaces). BOUNDED: an absolute reactor deadline ABOVE the
# 1-node election window so the node reaches Leader yet can NEVER hang.
if(UNIX AND NOT APPLE AND TARGET lockstep_consensus AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_sim AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_consensus_test prod_consensus_test.cpp)
  target_include_directories(lockstep_prod_consensus_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_consensus_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_consensus
      lockstep_runtime
      lockstep_providers_sim
      lockstep_providers_prod)
  add_test(NAME prod_consensus_test COMMAND lockstep_prod_consensus_test)
endif()

# --- Phase 9 S9.2: N=1 commit-FOLLOWS-fsync DURABILITY TEETH [LINUX ONLY] -----
# The teeth for the S9.2 durability fix: a 1-node Raft self-commit must advance
# commit_index only AFTER the entry's fdatasync COMPLETES, not at persist-enqueue.
# Builds a REAL 1-node RaftNodeA (impl A) on the prod stack (ProdReactor + ProdDisk)
# over a StallSyncDisk that holds sync() completion so the enqueue->fsync window is
# deterministic, then asserts: appended-but-un-fsynced ⇒ commit_index == 0 (pre-fix
# this was 1 — a committed-yet-un-durable entry an abrupt crash would lose); after
# fsync ⇒ commit_index == 1 (commit follows durability). Runs under BOTH the async
# io_uring fsync (--security-opt seccomp=unconfined) and the sync-fdatasync fallback.
# LINUX-ONLY (ProdReactor/ProdDisk are __linux__); the macOS host stays green.
if(UNIX AND NOT APPLE AND TARGET lockstep_consensus AND TARGET lockstep_runtime
   AND TARGET lockstep_providers_prod)
  add_executable(lockstep_prod_consensus_durability_teeth_test
                 prod_consensus_durability_teeth_test.cpp)
  target_include_directories(lockstep_prod_consensus_durability_teeth_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lockstep_prod_consensus_durability_teeth_test
    PRIVATE
      lockstep_warnings
      lockstep_sanitizers
      lockstep_consensus
      lockstep_runtime
      lockstep_providers_prod)
  add_test(NAME prod_consensus_durability_teeth_test
           COMMAND lockstep_prod_consensus_durability_teeth_test)
  set_tests_properties(prod_consensus_durability_teeth_test PROPERTIES TIMEOUT 60)
endif()

# --- Phase 7 S5b-2: REAL MULTI-PROCESS Raft CLUSTER smoke [LINUX ONLY] --------
# P7-S5b-2 gate (briefs/phase7.md S5; THE distributed milestone). Orchestrates a REAL
# 3-process lockstepd cluster over loopback TCP via tests/prod_cluster_smoke.sh and
# asserts: (1) exactly ONE leader emerges (others followers); (2) client SUBMITs (via
# the lockstep_admin leader-find client) REPLICATE + COMMIT on ALL 3 nodes with their
# committed logs AGREEING (same values, same order); (3) a killed follower RESTARTED on
# the SAME data dir REJOINS and its committed log CATCHES UP (recovery from ProdDisk +
# replication). PROCESS-CLEANUP GUARANTEE: the script traps EXIT/INT/TERM to SIGKILL
# every child on every path, and each lockstepd self-deadlines (--run-seconds), so no
# orphan can outlive the test. BOUNDED: every wait has an absolute deadline; the ctest
# has its own TIMEOUT (a hard wall guard) below. Built ONLY on Linux (needs the
# lockstepd + lockstep_admin Linux targets); the macOS host stays green (test absent).
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin)
  add_test(
    NAME prod_cluster_smoke
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_cluster_smoke.sh)
  # A generous ceiling that still kills any true hang fast (the script's own internal
  # deadlines are well under this; sanitizer/CI jitter is absorbed).
  set_tests_properties(prod_cluster_smoke PROPERTIES TIMEOUT 120)
endif()

# --- TLS TRANSPORT ENCRYPTION: mTLS cluster + auth teeth [LINUX ONLY] ----------
# Orchestrates a REAL 3-process lockstepd cluster whose ENTIRE transport (consensus peer
# replication + admin/client) is wrapped in mTLS (providers/prod/ProdTls.hpp over OpenSSL),
# and asserts: (1) the encrypted cluster WORKS — one leader, replicated commit over the TLS
# wire, a client TLS round-trip; (2) TEETH — a PLAINTEXT client and a WRONG-CA client are
# REFUSED (the encryption + mutual auth actually gate, not cosmetic), with a correct-cert
# control proving the refusals are the auth gate not a dead node. Generates ephemeral dev
# certs (a self-signed CA + node/client certs + a rogue CA) with the `openssl` CLI in a
# scratch dir. PROCESS-CLEANUP: traps EXIT/INT/TERM to SIGKILL every tracked child; each
# lockstepd self-deadlines (--run-seconds). Built ONLY on Linux + only when TLS is enabled
# (the lockstepd/lockstep_admin Linux targets carry LOCKSTEP_TLS via OpenSSL); the macOS
# host stays green (test absent). NOTE: needs openssl on PATH (the dev image's libssl-dev).
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin AND OpenSSL_FOUND)
  add_test(
    NAME prod_tls_test
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_tls_test.sh)
  set_tests_properties(prod_tls_test PROPERTIES TIMEOUT 150)
endif()

# --- AUTH + RBAC over mTLS (Phase: auth-rbac) [LINUX] ------------------------
# Application-level identity (mTLS cert CN -> principal) + per-operation RBAC. Brings up a
# single-node mTLS cluster with an --auth-policy and asserts the allow/deny MATRIX + the
# TEETH (a reader's REFUSED write did NOT commit; an unknown principal is default-denied;
# an authorized write still commits). Built ONLY on Linux + only when TLS is enabled (the
# cert-CN extraction is OpenSSL-gated); the macOS host stays green (test absent). Needs
# openssl on PATH (the dev image's libssl-dev). PROCESS-CLEANUP: traps EXIT/INT/TERM to
# SIGKILL every tracked child; the daemon self-deadlines (--run-seconds).
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin AND OpenSSL_FOUND)
  add_test(
    NAME prod_auth_test
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_auth_test.sh)
  set_tests_properties(prod_auth_test PROPERTIES TIMEOUT 150)
endif()

# --- Phase 7 S6: JEPSEN-STYLE PROD-CLUSTER CAPSTONE (V-PROD-CLUSTER) [LINUX] ---
# P7-S6 gate (briefs/phase7.md S6 — THE capstone). Drives the REAL multi-process
# lockstepd cluster (5 nodes) under a FAULT STORM — process CRASH/RESTART (SIGKILL +
# restart on the same data dir, recover from ProdDisk) plus a NETWORK PARTITION
# (NET_ADMIN-FREE: SIGSTOP freezes a node so peers see it unresponsive == partitioned;
# SIGCONT heals) — across several SEEDED scenarios, while a concurrent client workload
# submits a stream of DISTINCT values. Records the client-observed history (each
# submit's accept/term/index + periodic per-node STATUS snapshots) and CHECKS the
# SAFETY properties the sim fault-storm proved, judged over the REAL committed Raft
# log: (A) NO SPLIT-BRAIN — committed logs are prefix-compatible at every snapshot,
# converge after heal; (B) ACK DURABILITY / EXACTLY-ONCE — every ACKed submit appears
# exactly once in the final committed log; (C) LEADER UNIQUENESS — per-node term
# monotonicity + no divergent same-index commits; (D) PROGRESS — non-trivial commits
# under faults; (E) V-XCHECK — per-client program-order preserved on the final log.
# These ARE the linearizability/strict-serializability properties for a Raft total-
# order store (the commit index is the linearization point). TEETH: the checker is run
# on a FABRICATED bad history and must FAIL. PROCESS-CLEANUP: the script traps
# EXIT/INT/TERM to SIGCONT-then-SIGKILL every tracked child (a SIGSTOPed node is
# CONTinued before kill so none is left stopped/zombie); each lockstepd self-deadlines
# (--run-seconds). BOUNDED: scenarios/rounds/submits/waits all capped. Built ONLY on
# Linux (needs lockstepd + lockstep_admin); the macOS host stays green (test absent).
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin)
  add_test(
    NAME prod_jepsen
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_jepsen.sh)
  # A generous ceiling that still kills any true hang fast. The script's own internal
  # absolute deadlines + each daemon's --run-seconds are well under this; the outer
  # fork-parent wall-guard (brief's `to()`) is an additional backstop in-gate.
  set_tests_properties(prod_jepsen PROPERTIES TIMEOUT 300)
endif()

# --- Phase 9 S9.4: REPLICATED SHARDS (3-node Raft group per shard, HA) [LINUX] -
# P9-S9.4 gate. Each shard is an N-node Raft group spread across N PROCESSES (combines
# the S9.1 thread-per-shard daemon with the S5b-2 multi-process Raft cluster — verified
# consensus core UNCHANGED). prod_replicated_shards.sh brings up N=3 procs x M shard-
# replicas, asserts: (1) every shard's group elects a leader; (2) key-routed rsubmit
# commits durably on a quorum; (3) SIGKILL one process (one replica of EVERY shard) ->
# every shard stays available + keeps committing on the surviving quorum (HA); (4)
# restart that process on its SAME data dir -> its replicas rejoin + catch up from
# ProdDisk with NO committed-data loss. PROCESS-CLEANUP: trap SIGKILLs every tracked
# child on every path; each lockstepd self-deadlines (--run-seconds). BOUNDED throughout.
# Built ONLY on Linux (needs lockstepd + lockstep_admin); macOS host stays green.
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin)
  add_test(
    NAME prod_replicated_shards
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_replicated_shards.sh)
  # A generous ceiling that still kills any true hang fast; the script's internal
  # absolute deadlines + each daemon's --run-seconds are well under this.
  set_tests_properties(prod_replicated_shards PROPERTIES TIMEOUT 240)
endif()

# --- Phase 10 OBSERVABILITY: metrics reality-check (V-METRICS-REAL) [LINUX] ---
# P10 gate. Brings up a REAL 3-process lockstepd cluster, drives a known workload of N
# durable submits, scrapes the new METRICS verb in Prometheus exposition format, and
# asserts the metrics REFLECT REALITY (the teeth — a metric that doesn't track the real
# event is a bug): (1) METRICS returns valid Prometheus text per node; (2) role/term/
# commit_index gauges == STATUS; (3) submits_committed counter tracks the values actually
# committed; (4) leader_changes bumps when the leader is SIGKILLed and a new one is
# elected. PROCESS-CLEANUP: trap SIGKILLs every tracked child on every path; each
# lockstepd self-deadlines (--run-seconds). BOUNDED throughout. Built ONLY on Linux (needs
# lockstepd + lockstep_admin); the macOS host stays green (test absent).
if(UNIX AND NOT APPLE AND TARGET lockstepd AND TARGET lockstep_admin)
  add_test(
    NAME prod_metrics_test
    COMMAND ${CMAKE_COMMAND} -E env
      LOCKSTEPD=$<TARGET_FILE:lockstepd>
      LOCKSTEP_ADMIN=$<TARGET_FILE:lockstep_admin>
      bash ${CMAKE_CURRENT_SOURCE_DIR}/prod_metrics_test.sh)
  set_tests_properties(prod_metrics_test PROPERTIES TIMEOUT 180)
endif()

# --- Harness liveness invariant (orchestrator) -------------------------------
# Every test is WALL-CLOCK BOUNDED. A non-terminating run (infinite loop /
# livelock — e.g. a broken binary search) must surface as a CTest FAILURE, never
# an unbounded spin that saturates the host. Backprop from the SimBus::in_side_a
# off-by-one infinite loop (mid+0 vs mid+1) that hung standalone yet slipped a
# timeout-less ctest. Deterministic sims finish in well under a second; 90s is a
# generous ceiling that still kills any true hang fast.
#
# SANITIZER SCALING: instrumented builds run several times slower (MSan with
# origin-tracking ~3-5x, ASan/TSan/UBSan ~2-3x). The heavy consensus seed-sweep
# tests legitimately spill past 90s under MSan (observed: consensus_impl_a /
# consensus_crosscheck timed out at 90s while finishing well under a second in a
# plain build). Scale the ceiling by the sanitizer so the timeout still catches
# a TRUE hang fast without false-failing slow-but-terminating instrumented runs.
if(LOCKSTEP_SANITIZER STREQUAL "msan")
  set(_lockstep_test_timeout 600)   # MSan+origins: the slowest instrumentation
elseif(NOT LOCKSTEP_SANITIZER STREQUAL "")
  set(_lockstep_test_timeout 300)   # ASan / TSan / UBSan
else()
  set(_lockstep_test_timeout 90)    # plain debug/release
endif()
get_property(_lockstep_all_tests DIRECTORY PROPERTY TESTS)
if(_lockstep_all_tests)
  set_tests_properties(${_lockstep_all_tests}
    PROPERTIES TIMEOUT ${_lockstep_test_timeout})
endif()
