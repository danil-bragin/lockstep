#!/usr/bin/env bash
# prod_scale.sh — Phase 9 S9.1. THE MULTI-SHARD THROUGHPUT-SCALING harness. Phase 8
# proved a SINGLE single-node Raft reactor tops out ~17k durable commits/s, bounded by
# per-op reactor CPU. The ONLY order-of-magnitude lever is HORIZONTAL: run M independent
# single-node Raft shards across cores. This harness drives ONE lockstepd in MULTI-SHARD
# mode (--shards M: M threads, each an independent shard on its own reactor/disk/port) and
# a KEY-ROUTED load (lockstep_admin mbench: hash(key)%M -> shard port), measuring AGGREGATE
# committed throughput at M = 1,2,4,8,... The aggregate should rise ~linearly with M up to
# the core count, then flatten (cores + memory-bandwidth contention).
#
# THIS IS MEASUREMENT inside a CPU/mem-limited Docker container on a laptop; absolute
# numbers are only meaningful relative to a re-run on the SAME setup. Real wall-clock ->
# numbers vary run-to-run; we report the MEDIAN of a few repeats per M.
#
# PROCESS-CLEANUP GUARANTEE (NON-NEGOTIABLE — a host freeze happened): the single lockstepd
# is launched in THIS script's group + tracked by PID; a trap on EXIT/INT/TERM SIGKILLs it
# on EVERY exit path. lockstepd ALSO self-deadlines (--run-seconds) and its M reactor threads
# each self-deadline + are joined by lockstepd (no detached threads). Every wait is bounded
# by an ABSOLUTE deadline; the load is FINITE. Run UNDER the fork-parent wall-guard
# (`to N bash tests/prod_scale.sh`). Verify `pgrep -x lockstepd` EMPTY after.

set -u  # NOT -e: run cleanup + print results even on a soft failure.

# ---- tunables (env-overridable; finite + bounded) ---------------------------
# SCALE_PER_SHARD = submits routed to EACH shard (FIXED WORK PER SHARD — the honest
# scaling unit: aggregate work = per_shard * M, so the IDEAL aggregate-throughput curve
# is LINEAR in M up to the core count, then flattens). Total submits per run = per_shard*M.
# (If you instead want a FIXED TOTAL split across shards — which makes commit-catch-up at
# small M dominate and shows SUPER-linear early factors — set SCALE_PER_SHARD large and
# read the absolute agg_commit_tput, not the per-M factor.)
SCALE_PER_SHARD="${SCALE_PER_SHARD:-8000}"   # submits PER SHARD (FINITE)
SCALE_VALUE_BYTES="${SCALE_VALUE_BYTES:-16}"
SCALE_INFLIGHT="${SCALE_INFLIGHT:-64}"       # per-shard pipeline window depth
SCALE_SHARDS="${SCALE_SHARDS:-1 2 4 8}"      # shard counts to ramp (the scaling curve)
SCALE_REPEATS="${SCALE_REPEATS:-3}"          # repeats per M (report the MEDIAN)
RUN_SECONDS="${RUN_SECONDS:-120}"            # daemon self-deadline (> whole budget)
BASE_PORT="${BASE_PORT:-22000}"              # shard admin ports = BASE_PORT + shard_index
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
echo "config: per_shard=$SCALE_PER_SHARD (total=per_shard*M) value_bytes=$SCALE_VALUE_BYTES inflight=$SCALE_INFLIGHT shards='$SCALE_SHARDS' repeats=$SCALE_REPEATS"

NPROC="$(nproc 2>/dev/null || echo '?')"
MEMKB="$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo '?')"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB (container limits via --cpus/--memory)"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_scale_XXXXXX")"
PIDS=()

cleanup() {
  trap - EXIT INT TERM
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

# Launch ONE lockstepd in MULTI-SHARD mode with M shards. Wait (bounded) until ALL M
# shard admin ports answer with a leader (role=2) — each single-node shard self-elects.
# Fills PIDS (one entry: the daemon). Builds the --host list into HOSTS. Returns 0 on
# all-shards-up. tag distinguishes data dirs across passes (fresh disks per pass).
HOSTS=""
launch_shards() {
  local m="$1"; local tag="$2"
  local dd="$WORKDIR/shards_m${m}_${tag}"
  mkdir -p "$dd"
  PIDS=()
  "$LOCKSTEPD" --shards "$m" --shard-base-port "$BASE_PORT" \
    --data-dir "$dd" --seed "$SEED" --run-seconds "$RUN_SECONDS" \
    >"$WORKDIR/shards_m${m}_${tag}.log" 2>&1 &
  PIDS[0]=$!
  # Build the host list (admin port = BASE_PORT + shard_index).
  HOSTS=""
  local k
  for ((k=0;k<m;k++)); do HOSTS="$HOSTS --host $((BASE_PORT + k))"; done
  # Wait until every shard reports a leader.
  local DEADLINE=$(( $(now_ms) + 20000 ))
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    local all_up=1
    for ((k=0;k<m;k++)); do
      local line; line="$("$ADMIN" status --host "$((BASE_PORT + k))" 2>/dev/null | head -1)"
      [ "$(field "$line" ok)" = "1" ] && [ "$(field "$line" role)" = "2" ] || { all_up=0; break; }
    done
    [ "$all_up" = "1" ] && return 0
    sleep 0.3
  done
  return 1
}

declare -A MED_TPUT
RESULT_SUMMARY=""
SINGLE_SHARD_MED=""

run_m() {
  local m="$1"
  echo
  echo "############################################################"
  echo "### M = $m shard(s)"
  echo "############################################################"
  local -a tputs=()
  local pass
  for ((pass=1; pass<=SCALE_REPEATS; pass++)); do
    if ! launch_shards "$m" "p${pass}"; then
      echo "  M=$m pass=$pass: not all shards reached leader within deadline (skipping)"; kill_all; continue
    fi
    local out=""
    local total=$(( SCALE_PER_SHARD * m ))   # FIXED work per shard -> total scales with M
    out="$("$ADMIN" mbench --count "$total" --inflight "$SCALE_INFLIGHT" \
              --value-bytes "$SCALE_VALUE_BYTES" $HOSTS 2>/dev/null | grep '^MBENCH' | head -1)"
    kill_all
    if [ -z "$out" ]; then
      echo "  M=$m pass=$pass: no MBENCH output (skipping)"; continue
    fi
    echo "  M=$m pass=$pass: $out"
    local uf cc
    uf="$(field "$out" unfinished)"; cc="$(field "$out" all_covered)"
    [ "$uf" = "1" ] && echo "  WARN: unfinished=1 (a stall) M=$m pass=$pass"
    [ "$cc" = "0" ] && echo "  WARN: all_covered=0 (commit did not cover all shards) M=$m pass=$pass"
    tputs+=("$(field "$out" agg_commit_tput)")
  done
  if [ "${#tputs[@]}" = "0" ]; then
    echo "  M=$m: FAIL no successful pass"; return
  fi
  local md lo hi
  md="$(median "${tputs[@]}")"; lo="$(minof "${tputs[@]}")"; hi="$(maxof "${tputs[@]}")"
  echo "  --- M=$m MEDIAN agg_commit_tput = $md ops/s (min=$lo max=$hi) ---"
  MED_TPUT[$m]="$md"
  [ "$m" = "1" ] && SINGLE_SHARD_MED="$md"
  local factor="-"
  if [ -n "$SINGLE_SHARD_MED" ]; then
    factor="$(awk -v a="$md" -v b="$SINGLE_SHARD_MED" 'BEGIN{ if(b>0) printf "%.2f", a/b; else print "-" }')"
  fi
  RESULT_SUMMARY="$RESULT_SUMMARY
$m|$md|$lo|$hi|$factor"
}

for M in $SCALE_SHARDS; do
  run_m "$M"
done

echo
echo "============================================================"
echo "S9.1 MULTI-SHARD SCALING SUMMARY  (median of $SCALE_REPEATS passes; per_shard=$SCALE_PER_SHARD, total=per_shard*M, inflight=$SCALE_INFLIGHT value_bytes=$SCALE_VALUE_BYTES)"
echo "machine: nproc=$NPROC mem_total_kb=$MEMKB"
echo "agg_commit_tput = total accepted ops across all shards / wall until every shard's commit covers its load"
echo "factor = agg_commit_tput(M) / agg_commit_tput(M=1)  [ideal = M, up to the core count]"
echo "------------------------------------------------------------"
printf '%-4s %16s %12s %12s %10s\n' "M" "agg_commit_tput" "min" "max" "factor"
echo "$RESULT_SUMMARY" | while IFS='|' read -r m md lo hi factor; do
  [ -z "$m" ] && continue
  printf '%-4s %16s %12s %12s %10s\n' "$m" "$md" "$lo" "$hi" "$factor"
done
echo "============================================================"
echo "[prod_scale] done"
exit 0
