#!/usr/bin/env bash
# prod_perf.sh — Phase 7 S7. THE PROD PERFORMANCE BASELINE harness. Bring up the REAL
# lockstepd cluster (reusing S5b-2's launch + the NON-NEGOTIABLE process cleanup), drive
# a finite CLOSED-LOOP write load through the `lockstep_admin bench` verb (ONE persistent
# client connection — measures the SERVER submit path, NOT process-spawn overhead), and
# record throughput + submit->commit p50/p99 latency for BOTH a 1-node (quorum=1, no
# replication) and a 3-node (quorum=2, real TCP replication) cluster, so the replication
# cost is visible. A READ CHECK (the bench verb's committed-log verify) confirms
# correctness under load.
#
# THIS IS A BASELINE TO REGRESS AGAINST — NOT an optimization benchmark, and NOT a
# production benchmark. It runs inside a CPU/mem-limited Docker container on a developer
# laptop; absolute numbers are only meaningful relative to a future re-run on the SAME
# setup. We report a MEDIAN of a few repeats + the spread (we do NOT cherry-pick the best
# run). Real wall-clock, so numbers vary run-to-run; that is expected for a baseline.
#
# WHAT THE NUMBERS REFLECT (be honest):
#   * submit_tput / submit_p50,p99 = the SUBMIT->ACCEPT path: client send -> leader
#     appends to its log -> reply. Closed loop, concurrency 1 (one request in flight),
#     over ONE reused reactor + connection -> NO per-call process/reactor construction.
#     This is the server's accept latency, not a saturation throughput (single client).
#   * commit_p50,p99 = a SAMPLE of submit->COMMIT latency (poll STATUS until commit_index
#     covers the value's index). Poll-bounded -> OVER-counts by up to one poll turn.
#     Approximate, stated as such. For 3-node this includes real replication RTT.
#
# PROCESS-CLEANUP GUARANTEE (NON-NEGOTIABLE — a host freeze happened): every lockstepd is
# launched in THIS script's group + tracked by PID; a trap on EXIT/INT/TERM SIGKILLs
# every tracked child on EVERY exit path. lockstepd ALSO self-deadlines (--run-seconds)
# so a daemon dies even if this script is killed first. Every wait is bounded by an
# ABSOLUTE deadline; the load is FINITE (fixed --count + per-run deadline). Run UNDER the
# brief's fork-parent wall-guard (`to N bash tests/prod_perf.sh`). Verify pgrep -x
# lockstepd EMPTY after.

set -u  # NOT -e: run cleanup + print the baseline even on a soft failure.

# ---- tunables (env-overridable; finite + bounded) ---------------------------
PERF_COUNT="${PERF_COUNT:-2000}"          # submits per measured run (FINITE)
PERF_VALUE_BYTES="${PERF_VALUE_BYTES:-16}"
PERF_COMMIT_SAMPLES="${PERF_COMMIT_SAMPLES:-128}"
PERF_REPEATS="${PERF_REPEATS:-3}"         # repeats per config (report the MEDIAN)
RUN_SECONDS="${RUN_SECONDS:-40}"          # each daemon self-deadline (> whole budget)
SEED="${SEED:-12345}"

# ---- locate the binaries (env override, else common build dirs) -------------
LOCKSTEPD="${LOCKSTEPD:-}"
ADMIN="${LOCKSTEP_ADMIN:-}"
find_bin() {
  local name="$1"
  for d in build/ldev build cmake-build-debug build/Debug .; do
    if [ -x "$d/cli/$name" ]; then echo "$d/cli/$name"; return 0; fi
    if [ -x "$d/$name" ]; then echo "$d/$name"; return 0; fi
  done
  command -v "$name" 2>/dev/null && return 0
  return 1
}
[ -z "$LOCKSTEPD" ] && LOCKSTEPD="$(find_bin lockstepd)"
[ -z "$ADMIN" ] && ADMIN="$(find_bin lockstep_admin)"
if [ -z "$LOCKSTEPD" ] || [ ! -x "$LOCKSTEPD" ]; then
  echo "FATAL: lockstepd not found (set LOCKSTEPD=/path)"; exit 2
fi
if [ -z "$ADMIN" ] || [ ! -x "$ADMIN" ]; then
  echo "FATAL: lockstep_admin not found (set LOCKSTEP_ADMIN=/path)"; exit 2
fi
echo "lockstepd      = $LOCKSTEPD"
echo "lockstep_admin = $ADMIN"
echo "config: count=$PERF_COUNT value_bytes=$PERF_VALUE_BYTES commit_samples=$PERF_COMMIT_SAMPLES repeats=$PERF_REPEATS"

# ---- machine descriptor (recorded in the baseline) --------------------------
NPROC="$(nproc 2>/dev/null || echo '?')"
MEMKB="$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo '?')"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB (container limits via --cpus/--memory)"

# ---- shared cleanup state ---------------------------------------------------
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_perf_XXXXXX")"
PIDS=()

cleanup() {
  trap - EXIT INT TERM
  # SIGKILL every TRACKED child PID (the only lockstepd we spawned). We do NOT pkill by
  # pattern (the harness parent argv contains the lockstepd path -> would kill ourselves).
  for pid in "${PIDS[@]:-}"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null
    fi
  done
  wait 2>/dev/null
  rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

now_ms() { date +%s%3N; }
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }

# Kill every tracked PID between configs (so each config starts on fresh daemons).
kill_all() {
  for idx in "${!PIDS[@]}"; do
    local pid="${PIDS[$idx]}"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null
    fi
  done
  wait 2>/dev/null
  PIDS=()
}

# median of a whitespace-separated list of numbers (floats ok). Echoes the median.
median() {
  printf '%s\n' "$@" | LC_ALL=C sort -g | awk '
    { a[NR]=$1 }
    END {
      if (NR==0) { print "0"; exit }
      if (NR%2==1) print a[(NR+1)/2];
      else printf "%.2f\n", (a[NR/2]+a[NR/2+1])/2.0
    }'
}
# min/max for spread reporting.
minof() { printf '%s\n' "$@" | LC_ALL=C sort -g | head -1; }
maxof() { printf '%s\n' "$@" | LC_ALL=C sort -g | tail -1; }

# ============================================================================
# Run ONE config: launch N daemons, wait for a leader, run PERF_REPEATS bench passes,
# print the per-pass BENCH lines + a MEDIAN summary. Args: <label> <N> <cport0..> // <aport0..>
# We pass topology via globals set by the caller to keep it simple.
# ============================================================================
declare -a R_CP R_AP R_ID
RESULT_SUMMARY=""

# Launch N FRESH daemons for one pass; wait (bounded) for a leader. Returns 0 on a
# leader-up cluster. Fills the global PIDS + ALL_APORTS (set by caller via R_*). A FRESH
# data dir per pass keeps the 3 repeats COMPARABLE — they are independent samples, not a
# growing-log sequence (the log grows within a pass, never across passes).
launch_cluster() {
  local label="$1"; local n="$2"; local tag="$3"
  local PEERS="" k
  for ((k=0;k<n;k++)); do PEERS="$PEERS --peer ${R_ID[$k]}:${R_CP[$k]}"; done
  PIDS=()
  for ((k=0;k<n;k++)); do
    local id="${R_ID[$k]}"; local dd="$WORKDIR/${label}_${tag}_node$id"
    mkdir -p "$dd"
    "$LOCKSTEPD" --node-id "$id" --listen-port "${R_CP[$k]}" \
      --admin-port "${R_AP[$k]}" $PEERS --data-dir "$dd" --seed "$SEED" \
      --run-seconds "$RUN_SECONDS" >"$WORKDIR/${label}_${tag}_node$id.log" 2>&1 &
    PIDS[$k]=$!
  done
  local DEADLINE=$(( $(now_ms) + 20000 ))
  local leader=0
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    leader=0
    for ((k=0;k<n;k++)); do
      local line; line="$("$ADMIN" status --host "${R_AP[$k]}" 2>/dev/null | head -1)"
      [ "$(field "$line" ok)" = "1" ] || continue
      [ "$(field "$line" role)" = "2" ] && leader=1
    done
    [ "$leader" = "1" ] && break
    sleep 0.3
  done
  [ "$leader" = "1" ] && return 0 || return 1
}

run_config() {
  local label="$1"; local n="$2"
  echo
  echo "############################################################"
  echo "### CONFIG: $label  (N=$n)"
  echo "############################################################"

  local ALL_APORTS="" k
  for ((k=0;k<n;k++)); do ALL_APORTS="$ALL_APORTS --host ${R_AP[$k]}"; done

  # PERF_REPEATS measured passes, EACH on a FRESH cluster (comparable samples).
  local -a a_tput a_sp50 a_sp99 a_cp50 a_cp99
  local pass
  for ((pass=1; pass<=PERF_REPEATS; pass++)); do
    if ! launch_cluster "$label" "$n" "p$pass"; then
      echo "  pass $pass: no leader within deadline (skipping)"; kill_all; continue
    fi
    local out=""
    # bounded: the bench run is finite; the wall-guard outlives us regardless.
    out="$("$ADMIN" bench --count "$PERF_COUNT" --value-bytes "$PERF_VALUE_BYTES" \
              --commit-samples "$PERF_COMMIT_SAMPLES" $ALL_APORTS 2>/dev/null | grep '^BENCH' | head -1)"
    kill_all  # fresh daemons next pass
    if [ -z "$out" ]; then
      echo "  pass $pass: no BENCH output (skipping)"; continue
    fi
    echo "  pass $pass: $out"
    local rc; rc="$(field "$out" read_check)"
    if [ "$rc" != "ok" ]; then echo "  WARN: read_check=$rc on pass $pass"; fi
    a_tput+=("$(field "$out" submit_tput)")
    a_sp50+=("$(field "$out" submit_p50_us)")
    a_sp99+=("$(field "$out" submit_p99_us)")
    a_cp50+=("$(field "$out" commit_p50_us)")
    a_cp99+=("$(field "$out" commit_p99_us)")
  done

  if [ "${#a_tput[@]}" = "0" ]; then
    echo "  FAIL: no successful bench pass for $label"; kill_all; return 1
  fi

  local m_tput m_sp50 m_sp99 m_cp50 m_cp99 lo_tput hi_tput
  m_tput="$(median "${a_tput[@]}")"; lo_tput="$(minof "${a_tput[@]}")"; hi_tput="$(maxof "${a_tput[@]}")"
  m_sp50="$(median "${a_sp50[@]}")"; m_sp99="$(median "${a_sp99[@]}")"
  m_cp50="$(median "${a_cp50[@]}")"; m_cp99="$(median "${a_cp99[@]}")"

  echo "  --- MEDIAN ($PERF_REPEATS passes) ---"
  echo "    submit_tput   = $m_tput ops/s   (min=$lo_tput max=$hi_tput)"
  echo "    submit p50/p99 = $m_sp50 / $m_sp99 us"
  echo "    commit p50/p99 = $m_cp50 / $m_cp99 us"
  RESULT_SUMMARY="$RESULT_SUMMARY
$label|$n|$m_tput|$lo_tput|$hi_tput|$m_sp50|$m_sp99|$m_cp50|$m_cp99"

  kill_all
  return 0
}

# ============================================================================
# CONFIG 1: single node (quorum=1, NO replication)
# ============================================================================
R_ID=(1); R_CP=(19111); R_AP=(19211)
run_config "1-node" 1

# ============================================================================
# CONFIG 2: three nodes (quorum=2, REAL TCP replication)
# ============================================================================
R_ID=(1 2 3); R_CP=(19121 19122 19123); R_AP=(19221 19222 19223)
run_config "3-node" 3

# ============================================================================
# FINAL BASELINE TABLE (machine-parseable + human summary)
# ============================================================================
echo
echo "============================================================"
echo "PERF BASELINE SUMMARY  (median of $PERF_REPEATS passes; count=$PERF_COUNT value_bytes=$PERF_VALUE_BYTES)"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB"
echo "------------------------------------------------------------"
printf '%-8s %4s %12s %10s %10s %10s %10s\n' \
  "config" "N" "tput(ops/s)" "sP50(us)" "sP99(us)" "cP50(us)" "cP99(us)"
echo "$RESULT_SUMMARY" | while IFS='|' read -r lbl n tput lo hi sp50 sp99 cp50 cp99; do
  [ -z "$lbl" ] && continue
  printf '%-8s %4s %12s %10s %10s %10s %10s\n' "$lbl" "$n" "$tput" "$sp50" "$sp99" "$cp50" "$cp99"
done
echo "============================================================"
echo "[prod_perf] done"
exit 0
