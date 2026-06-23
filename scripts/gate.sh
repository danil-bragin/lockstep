#!/usr/bin/env bash
# =============================================================================
# Lockstep -- gate.sh : LOCAL gate-as-code runner.
#
# Reproduces the SAME logical merge-gate battery as .github/workflows/ci.yml
# (master-plan §8), in the same ordered stages, but runnable on THIS dev host
# (macOS + Apple clang). It is the local mirror of the CI required-checks.
#
#   compile (debug + release)
#     -> forbidden-call lint + clang-tidy + clang static analyzer (scan-build)
#       -> sanitizers ASan / TSan / UBSan / MSan (build + ctest)
#         -> deterministic simulation battery        [no-op hook, Phase >= 2]
#           -> conforms-to-formal-spec                [no-op hook, Phase >= 2]
#             -> applicable linearizability checks    [no-op hook, Phase >= 2]
#               -> mutation score not regressed
#
# HONEST HOST LIMITS (cardinal: skips are LOUD, never silent, never pass-by-
# omission). Apple's toolchain on this macOS host has NO clang-tidy, NO
# scan-build, and NO MSan (MemorySanitizer is unsupported on macOS / Apple
# libc++). Those three stages print a [SKIP host-limited] banner here and run
# FOR REAL in CI (ubuntu + llvm). Everything else runs for real locally.
#
# Exit code: non-zero if ANY *real* stage FAILED. host-limited SKIPs and
# Phase->=2 NOOP hooks never, by themselves, make the gate pass or fail.
#
# Tolerant by design: this runs while sibling agents (A1 build, A2 headers,
# A3 lint) may not have fully landed. A missing preset / target / lint script
# is surfaced as a real FAIL with its real error -- gate.sh itself stays
# correct even when run against a half-built tree.
# =============================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ---- host detection ---------------------------------------------------------
UNAME="$(uname)"
IS_DARWIN=0
[ "$UNAME" = "Darwin" ] && IS_DARWIN=1

have() { command -v "$1" >/dev/null 2>&1; }

# ---- RESOURCE GUARDRAILS (backprop: a parallel-gate + 15GB-TLC-scratch +
# runaway-test event froze the host). Keep one runaway from taking the machine
# down. macOS has no cgroups and `ulimit -v` is a no-op there, so we use the
# levers that DO work: no core dumps (a SIGSEGV core of a big proc can be GBs and
# fill the disk), a bounded stack (deep recursion SIGSEGVs fast instead of
# thrashing), and HALF-core build parallelism so even two concurrent gates don't
# catastrophically oversubscribe. Override JOBS via env if you really mean it.
ulimit -c 0 2>/dev/null || true          # no core dumps
ulimit -s 16384 2>/dev/null || true      # 16 MB stack ceiling (recursion guard)
if have nproc; then _NCPU="$(nproc)"; elif have sysctl; then _NCPU="$(sysctl -n hw.ncpu)"; else _NCPU=2; fi
# half the cores, floor 2 — leaves headroom for a second concurrent build/agent.
JOBS="${JOBS:-$(( _NCPU/2 > 2 ? _NCPU/2 : 2 ))}"

# ---- dashboard accounting ---------------------------------------------------
# Parallel arrays: STAGE_NAMES[i] -> STAGE_RESULTS[i] in {PASS,FAIL,SKIP,NOOP}.
STAGE_NAMES=()
STAGE_RESULTS=()
ANY_REAL_FAIL=0

record() {  # record <stage-name> <PASS|FAIL|SKIP|NOOP>
  STAGE_NAMES+=("$1")
  STAGE_RESULTS+=("$2")
  [ "$2" = "FAIL" ] && ANY_REAL_FAIL=1
  return 0
}

header() {
  echo ""
  echo "------------------------------------------------------------------------"
  echo ">>> $1"
  echo "------------------------------------------------------------------------"
}

skip_host() {  # skip_host <stage-name> <reason>
  echo "[SKIP host-limited] $1: $2 (runs in CI)"
  record "$1" SKIP
}

noop_hook() {  # noop_hook <stage-name>
  echo "[no-op hook] $1 (Phase >= 2)"
  record "$1" NOOP
}

# Run a command, recording PASS/FAIL for a stage. Never aborts the script
# (set -e is suspended around the call) so the full dashboard always prints.
run_stage() {  # run_stage <stage-name> <cmd...>
  local name="$1"; shift
  echo "+ $*"
  set +e
  "$@"
  local rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    echo "[PASS] $name"
    record "$name" PASS
  else
    echo "[FAIL] $name (exit $rc)"
    record "$name" FAIL
  fi
  return 0
}

# Configure + build + ctest a single preset as one logical stage.
gate_preset() {  # gate_preset <stage-name> <preset>
  local name="$1" preset="$2"
  echo "+ cmake --preset $preset && cmake --build --preset $preset -j $JOBS && ctest --preset $preset"
  set +e
  cmake --preset "$preset" \
    && cmake --build --preset "$preset" -j "$JOBS" \
    && ctest --preset "$preset"
  local rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    echo "[PASS] $name"
    record "$name" PASS
  else
    echo "[FAIL] $name (exit $rc) -- preset/target may not have landed yet; real error above"
    record "$name" FAIL
  fi
  return 0
}

echo "========================================================================"
echo " Lockstep LOCAL gate  (host: $UNAME, jobs: $JOBS)"
echo " Mirrors .github/workflows/ci.yml -- the universal merge gate (§8)."
echo "========================================================================"

# =============================================================================
# STAGE 1 -- COMPILE (debug + release)
# =============================================================================
header "STAGE 1/8  compile: debug"
gate_preset "compile+test (debug)" debug

header "STAGE 1/8  compile: release"
gate_preset "compile+test (release)" release

# =============================================================================
# STAGE 2 -- LINT + STATIC ANALYSIS
#   forbidden-call lint (real) + clang-tidy (host-skip) + scan-build (host-skip)
# =============================================================================
header "STAGE 2/8  forbidden-call lint"
if [ -f tools/lint/forbidden_calls.py ]; then
  run_stage "forbidden-call lint" python3 tools/lint/forbidden_calls.py .
else
  echo "[FAIL] forbidden-call lint: tools/lint/forbidden_calls.py not present yet (A3)"
  record "forbidden-call lint" FAIL
fi

header "STAGE 2/8  clang-tidy"
if have clang-tidy; then
  # Runs against the debug compile_commands.json if present.
  if [ -f build/debug/compile_commands.json ]; then
    # Lint the .cpp translation units only — they ARE in compile_commands.json,
    # so includes resolve. Headers are checked transitively via the .clang-tidy
    # HeaderFilterRegex (passing a .hpp directly has no compile command -> bogus
    # "file not found"). providers/ is exempt (the nondeterminism quarantine);
    # tools/ is exempt too (tools/lint/fixtures/dirty/*.cpp are DELIBERATELY-bad
    # code — rand/sockets/syscalls — that exercise the forbidden-lint, not real
    # code, so they must not be clang-tidy'd).
    run_stage "clang-tidy" bash -c \
      'git ls-files "*.cpp" | grep -v "^providers/" | grep -v "^tools/" | xargs -r clang-tidy -p build/debug'
  else
    echo "[FAIL] clang-tidy: build/debug/compile_commands.json missing (configure debug first)"
    record "clang-tidy" FAIL
  fi
else
  skip_host "clang-tidy" "Apple toolchain ships no clang-tidy"
fi

header "STAGE 2/8  clang static analyzer (scan-build)"
if have scan-build; then
  run_stage "scan-build" bash -c \
    'scan-build --status-bugs cmake --build --preset debug -j '"$JOBS"
else
  skip_host "scan-build" "Apple toolchain ships no scan-build"
fi

# =============================================================================
# STAGE 3 -- SANITIZERS (build + ctest): ASan, TSan, UBSan, MSan
# =============================================================================
header "STAGE 3/8  sanitizer: ASan"
gate_preset "sanitizer ASan" asan

header "STAGE 3/8  sanitizer: TSan"
gate_preset "sanitizer TSan" tsan

header "STAGE 3/8  sanitizer: UBSan"
gate_preset "sanitizer UBSan" ubsan

header "STAGE 3/8  sanitizer: MSan"
if [ "$IS_DARWIN" -eq 1 ]; then
  skip_host "sanitizer MSan" "MemorySanitizer unsupported on macOS / Apple libc++"
else
  gate_preset "sanitizer MSan" msan
fi

# =============================================================================
# STAGE 4 -- DETERMINISTIC SIMULATION BATTERY  (Phase >= 2)
# =============================================================================
header "STAGE 4/8  deterministic simulation battery"
noop_hook "deterministic simulation battery"

# =============================================================================
# STAGE 5 -- CONFORMS-TO-FORMAL-SPEC  (Phase >= 2; TLA+/TLC)
# =============================================================================
header "STAGE 5/8  conforms-to-formal-spec"
noop_hook "conforms-to-formal-spec"

# =============================================================================
# STAGE 6 -- APPLICABLE LINEARIZABILITY CHECKS  (Phase >= 2)
# =============================================================================
header "STAGE 6/8  linearizability checks"
noop_hook "linearizability checks"

# =============================================================================
# STAGE 7 -- MUTATION SCORE NOT REGRESSED  (adequacy meta-gate, §6.7)
# =============================================================================
header "STAGE 7/8  mutation score not regressed"
run_stage "mutation score" python3 tools/mutation/run_mutation.py

# =============================================================================
# STAGE 8 -- DASHBOARD
# =============================================================================
header "STAGE 8/8  dashboard"
echo ""
printf "  %-40s | %s\n" "STAGE" "RESULT"
printf "  %-40s-+-%s\n" "----------------------------------------" "-----------"
n=${#STAGE_NAMES[@]}
n_pass=0; n_fail=0; n_skip=0; n_noop=0
for ((i=0; i<n; i++)); do
  name="${STAGE_NAMES[$i]}"
  res="${STAGE_RESULTS[$i]}"
  case "$res" in
    PASS) tag="PASS";          n_pass=$((n_pass+1)) ;;
    FAIL) tag="FAIL";          n_fail=$((n_fail+1)) ;;
    SKIP) tag="SKIP(host)";    n_skip=$((n_skip+1)) ;;
    NOOP) tag="NOOP(Phase>=2)";n_noop=$((n_noop+1)) ;;
    *)    tag="$res" ;;
  esac
  printf "  %-40s | %s\n" "$name" "$tag"
done
echo ""
echo "  totals: PASS=$n_pass  FAIL=$n_fail  SKIP(host)=$n_skip  NOOP=$n_noop"
echo ""

if [ "$ANY_REAL_FAIL" -ne 0 ]; then
  echo "GATE: FAIL -- at least one real stage failed (see [FAIL] above)."
  echo "      (host-limited SKIPs and Phase>=2 NOOPs do not affect this verdict)"
  exit 1
fi

echo "GATE: PASS -- all real, host-runnable stages green."
echo "      host-limited stages (clang-tidy / scan-build / MSan) run in CI."
exit 0
