#!/usr/bin/env bash
# prod_shard_smoke.sh — Phase 9 S9.1. MULTI-SHARD CORRECTNESS + per-shard DURABILITY.
# Launch ONE lockstepd in MULTI-SHARD mode (--shards M: M independent single-node Raft
# shards, each on its own thread/reactor/disk/port). Assert, for EVERY shard:
#   (1) COMMIT + READ-BACK: submit a distinct value to that shard's admin port; it commits
#       (durable=1) and the committed-log digest reads it back (single-key, no cross-shard).
#   (2) PER-SHARD DURABILITY: SIGKILL the whole daemon, RESTART it on the SAME data dir,
#       and confirm every shard recovers its committed value from its ProdDisk (the smoke
#       kill+restart pattern, applied per shard).
#
# Each shard is an independent single-node group, so each is its OWN leader (no leader-find).
# Bounded by absolute deadlines; the daemon self-deadlines + joins all threads. trap SIGKILLs
# the tracked daemon on every exit path. Run under the fork-parent wall-guard.

set -u

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
if [ -z "$LOCKSTEPD" ] || [ ! -x "$LOCKSTEPD" ]; then echo "FATAL: lockstepd not found"; exit 2; fi
if [ -z "$ADMIN" ] || [ ! -x "$ADMIN" ]; then echo "FATAL: lockstep_admin not found"; exit 2; fi
echo "lockstepd      = $LOCKSTEPD"
echo "lockstep_admin = $ADMIN"

M="${SHARDS:-4}"
BASE_PORT="${BASE_PORT:-23000}"
RUN_SECONDS="${RUN_SECONDS:-30}"
SEED="${SEED:-777}"
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_shardsmoke_XXXXXX")"
DD="$WORKDIR/shards"
mkdir -p "$DD"
PIDS=()
FAIL=0

cleanup() {
  trap - EXIT INT TERM
  for pid in "${PIDS[@]:-}"; do
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM
now_ms() { date +%s%3N; }
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }

start_daemon() {
  "$LOCKSTEPD" --shards "$M" --shard-base-port "$BASE_PORT" \
    --data-dir "$DD" --seed "$SEED" --run-seconds "$RUN_SECONDS" \
    >"$WORKDIR/daemon_$1.log" 2>&1 &
  PIDS=( "$!" )
}

wait_all_leaders() {
  local DEADLINE=$(( $(now_ms) + 20000 )); local k
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    local up=1
    for ((k=0;k<M;k++)); do
      local line; line="$("$ADMIN" status --host "$((BASE_PORT + k))" 2>/dev/null | head -1)"
      [ "$(field "$line" ok)" = "1" ] && [ "$(field "$line" role)" = "2" ] || { up=0; break; }
    done
    [ "$up" = "1" ] && return 0
    sleep 0.3
  done
  return 1
}

kill_daemon() {
  for pid in "${PIDS[@]:-}"; do
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  PIDS=()
}

echo "### (1) launch M=$M shards; submit + read-back per shard"
start_daemon first
if ! wait_all_leaders; then echo "FAIL: not all shards elected"; cat "$WORKDIR"/daemon_first.log; exit 1; fi

# Submit a distinct value to each shard's port (single-node shard = its own leader).
for ((k=0;k<M;k++)); do
  VAL="shard${k}-val-deadbeef"
  OUT="$("$ADMIN" submit "$VAL" --host "$((BASE_PORT + k))" 2>/dev/null | head -1)"
  D="$(field "$OUT" durable)"
  echo "  shard $k submit: $OUT"
  if [ "$D" != "1" ]; then echo "  FAIL shard $k: not durable"; FAIL=1; fi
  # Read it back via STATUS committed-log digest.
  ST="$("$ADMIN" status --host "$((BASE_PORT + k))" 2>/dev/null | head -1)"
  LOG="$(field "$ST" log)"
  case ",$LOG," in
    *",$VAL,"*) echo "  shard $k read-back OK ($VAL present)";;
    *) echo "  FAIL shard $k: value not in committed log (log=$LOG)"; FAIL=1;;
  esac
done

echo
echo "### (2) SIGKILL daemon, RESTART on same data dir; assert each shard recovers from ProdDisk"
kill_daemon
start_daemon second
if ! wait_all_leaders; then echo "FAIL: not all shards re-elected after restart"; cat "$WORKDIR"/daemon_second.log; exit 1; fi
for ((k=0;k<M;k++)); do
  VAL="shard${k}-val-deadbeef"
  ST="$("$ADMIN" status --host "$((BASE_PORT + k))" 2>/dev/null | head -1)"
  LOG="$(field "$ST" log)"
  case ",$LOG," in
    *",$VAL,"*) echo "  shard $k RECOVERED from disk OK ($VAL present, commit=$(field "$ST" commit))";;
    *) echo "  FAIL shard $k: value NOT recovered (log=$LOG)"; FAIL=1;;
  esac
done
kill_daemon

echo
if [ "$FAIL" = "0" ]; then
  echo "PASS: all $M shards committed + read back + recovered from disk"
else
  echo "FAIL: per-shard correctness/durability assertion failed"
fi
exit "$FAIL"
