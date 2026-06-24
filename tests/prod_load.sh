#!/usr/bin/env bash
# prod_load.sh — Phase 8 S8.1 / S8.3. THE CONCURRENT-LOAD harness. The S7 baseline
# (prod_perf.sh) was CLOSED-LOOP (concurrency 1 -> throughput == 1/latency by
# construction); it measured latency, NOT the real throughput ceiling. THIS harness
# drives the `lockstep_admin pbench` verb, which keeps K requests IN FLIGHT on one
# persistent connection (a sliding window) AND/OR drives C concurrent connections.
#
# S8.3 HONEST COMMIT THROUGHPUT (the HEADLINE): accept_tput is the leader's LOCAL-APPEND
# rate (submit returns at append, BEFORE commit) — with S8.2b batching it inflates far
# past the real commit ceiling. So pbench now drives the load, then POLLS a CHEAP O(1)
# commit-index query until the cluster commit_index COVERS all accepted submits, and
# reports commit_tput = accepted / wall-until-committed as the HEADLINE. accept_tput is
# kept as a clearly-labeled secondary (leader-append, NOT commit). The cheap poll is O(1)
# (no log serialized) so the hot path no longer degrades as the durable log grows. It RAMPS
# the in-flight depth (1,4,16,64) on a 1-node cluster (isolates reactor CPU + the serve
# loop; accept does NOT block on fsync) AND a 3-node cluster (adds real TCP replication),
# so the SATURATION KNEE (where QPS stops rising with more in-flight) is visible and the
# BOTTLENECK is attributable.
#
# THIS IS MEASUREMENT, NOT an optimization benchmark, and NOT a production benchmark. It
# runs inside a CPU/mem-limited Docker container on a developer laptop; absolute numbers
# are only meaningful relative to a re-run on the SAME setup. Real wall-clock -> numbers
# vary run-to-run; we report the MEDIAN of a few repeats per depth (we do NOT cherry-pick).
#
# PROCESS-CLEANUP GUARANTEE (NON-NEGOTIABLE — a host freeze happened): every lockstepd is
# launched in THIS script's group + tracked by PID; a trap on EXIT/INT/TERM SIGKILLs every
# tracked child on EVERY exit path. lockstepd ALSO self-deadlines (--run-seconds). Every
# wait is bounded by an ABSOLUTE deadline; the load is FINITE (fixed --count + window +
# the pbench wall guard). Run UNDER the fork-parent wall-guard (`to N bash tests/prod_load.sh`).
# Verify pgrep -x lockstepd EMPTY after.

set -u  # NOT -e: run cleanup + print results even on a soft failure.

# ---- tunables (env-overridable; finite + bounded) ---------------------------
LOAD_COUNT="${LOAD_COUNT:-4000}"             # submits per measured run (FINITE)
LOAD_VALUE_BYTES="${LOAD_VALUE_BYTES:-16}"
LOAD_DEPTHS="${LOAD_DEPTHS:-1 4 16 64}"      # in-flight window depths to ramp (the curve)
LOAD_CONNS="${LOAD_CONNS:-1}"                # concurrent connections C (default 1: pipeline)
LOAD_REPEATS="${LOAD_REPEATS:-3}"            # repeats per depth (report the MEDIAN)
RUN_SECONDS="${RUN_SECONDS:-90}"             # each daemon self-deadline (> whole budget)
SEED="${SEED:-12345}"

# ---- locate the binaries (env override, else common build dirs) -------------
LOCKSTEPD="${LOCKSTEPD:-}"
ADMIN="${LOCKSTEP_ADMIN:-}"
find_bin() {
  local name="$1"
  for d in build/lrel build/ldev build cmake-build-debug build/Debug .; do
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
echo "config: count=$LOAD_COUNT value_bytes=$LOAD_VALUE_BYTES depths='$LOAD_DEPTHS' conns=$LOAD_CONNS repeats=$LOAD_REPEATS"

# ---- machine descriptor (recorded with the results) -------------------------
NPROC="$(nproc 2>/dev/null || echo '?')"
MEMKB="$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo '?')"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB (container limits via --cpus/--memory)"

# ---- shared cleanup state ---------------------------------------------------
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_load_XXXXXX")"
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

# Kill every tracked PID (fresh daemons next pass).
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

# median of a whitespace-separated list of numbers (floats ok).
median() {
  printf '%s\n' "$@" | LC_ALL=C sort -g | awk '
    { a[NR]=$1 }
    END {
      if (NR==0) { print "0"; exit }
      if (NR%2==1) print a[(NR+1)/2];
      else printf "%.1f\n", (a[NR/2]+a[NR/2+1])/2.0
    }'
}
minof() { printf '%s\n' "$@" | LC_ALL=C sort -g | head -1; }
maxof() { printf '%s\n' "$@" | LC_ALL=C sort -g | tail -1; }

declare -a R_CP R_AP R_ID
RESULT_SUMMARY=""

# Launch N FRESH daemons; wait (bounded) for a leader. Fills PIDS. Returns 0 on leader-up.
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

# Run one config (label,N): for EACH depth, LOAD_REPEATS passes on FRESH daemons; record
# the MEDIAN accept_tput. Prints per-pass PBENCH lines + a per-depth median.
run_config() {
  local label="$1"; local n="$2"
  echo
  echo "############################################################"
  echo "### CONFIG: $label  (N=$n)"
  echo "############################################################"

  local ALL_APORTS="" k
  for ((k=0;k<n;k++)); do ALL_APORTS="$ALL_APORTS --host ${R_AP[$k]}"; done

  local depth
  local prev_med=""
  local knee=""
  for depth in $LOAD_DEPTHS; do
    local -a c_tput=()
    local -a a_tput=()
    local pass
    for ((pass=1; pass<=LOAD_REPEATS; pass++)); do
      if ! launch_cluster "$label" "$n" "d${depth}p${pass}"; then
        echo "  depth=$depth pass=$pass: no leader within deadline (skipping)"; kill_all; continue
      fi
      local out=""
      out="$("$ADMIN" pbench --count "$LOAD_COUNT" --inflight "$depth" --conns "$LOAD_CONNS" \
                --value-bytes "$LOAD_VALUE_BYTES" $ALL_APORTS 2>/dev/null | grep '^PBENCH' | head -1)"
      kill_all
      if [ -z "$out" ]; then
        echo "  depth=$depth pass=$pass: no PBENCH output (skipping)"; continue
      fi
      echo "  depth=$depth pass=$pass: $out"
      local uf; uf="$(field "$out" unfinished)"
      [ "$uf" = "1" ] && echo "  WARN: unfinished=1 (a stall) depth=$depth pass=$pass"
      local cc; cc="$(field "$out" commit_covered)"
      [ "$cc" = "0" ] && echo "  WARN: commit_covered=0 (commit did not reach target) depth=$depth pass=$pass"
      # HEADLINE = commit_tput (HONEST commit throughput). accept_tput kept as secondary.
      c_tput+=("$(field "$out" commit_tput)")
      a_tput+=("$(field "$out" accept_tput)")
    done
    if [ "${#c_tput[@]}" = "0" ]; then
      echo "  depth=$depth: FAIL no successful pass"; continue
    fi
    local cm clo chi am
    cm="$(median "${c_tput[@]}")"; clo="$(minof "${c_tput[@]}")"; chi="$(maxof "${c_tput[@]}")"
    am="$(median "${a_tput[@]}")"
    echo "  --- depth=$depth MEDIAN commit_tput = $cm ops/s (min=$clo max=$chi)  [accept_tput median=$am, NOT commit] ---"
    RESULT_SUMMARY="$RESULT_SUMMARY
$label|$n|$depth|$cm|$clo|$chi|$am"

    # KNEE detection: the first depth where commit_tput stops rising meaningfully (<10%
    # over the previous depth) — the saturation point. Reported, not authoritative.
    if [ -n "$prev_med" ] && [ -z "$knee" ]; then
      local rise
      rise="$(awk -v a="$cm" -v b="$prev_med" 'BEGIN{ if(b>0) printf "%.3f", (a-b)/b; else print "1" }')"
      local below
      below="$(awk -v r="$rise" 'BEGIN{ print (r<0.10)?1:0 }')"
      [ "$below" = "1" ] && knee="$prev_depth"
    fi
    prev_med="$cm"; prev_depth="$depth"
  done
  [ -n "$knee" ] && echo "  >>> $label SATURATION KNEE ~ depth $knee (tput stops rising >10%)" \
                 || echo "  >>> $label: no clear knee within tested depths (still rising or flat throughout)"
  kill_all
}

# ============================================================================
# CONFIG 1: single node (quorum=1, NO replication) — isolates reactor CPU + serve loop.
# ============================================================================
R_ID=(1); R_CP=(19311); R_AP=(19411)
run_config "1-node" 1

# ============================================================================
# CONFIG 2: three nodes (quorum=2, REAL TCP replication).
# ============================================================================
R_ID=(1 2 3); R_CP=(19321 19322 19323); R_AP=(19421 19422 19423)
run_config "3-node" 3

# ============================================================================
# FINAL QPS-vs-DEPTH TABLE
# ============================================================================
echo
echo "============================================================"
echo "S8.3 HONEST COMMIT-THROUGHPUT SUMMARY  (median of $LOAD_REPEATS passes; count=$LOAD_COUNT value_bytes=$LOAD_VALUE_BYTES conns=$LOAD_CONNS)"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB"
echo "commit_tput = accepted ops / wall until commit_index COVERS them (HONEST, the HEADLINE)"
echo "accept_tput = leader LOCAL-APPEND rate (submit returns at append, BEFORE commit; NOT commit)"
echo "------------------------------------------------------------"
printf '%-8s %4s %8s %16s %12s %12s %16s\n' "config" "N" "depth" "commit_tput" "min" "max" "accept_tput"
echo "$RESULT_SUMMARY" | while IFS='|' read -r lbl n depth ctput clo chi atput; do
  [ -z "$lbl" ] && continue
  printf '%-8s %4s %8s %16s %12s %12s %16s\n' "$lbl" "$n" "$depth" "$ctput" "$clo" "$chi" "$atput"
done
echo "============================================================"
echo "[prod_load] done"
exit 0
