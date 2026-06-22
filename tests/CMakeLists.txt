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
else()
  message(WARNING
    "lockstep_consensus / runtime / providers_sim / harness not all found; "
    "consensus_harness_test not built (Phase 4 Stage M pending).")
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
