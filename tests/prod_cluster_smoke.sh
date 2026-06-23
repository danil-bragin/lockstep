#!/usr/bin/env bash
# prod_cluster_smoke.sh — Phase 7 S5b-2. THE DISTRIBUTED MILESTONE: orchestrate a REAL
# multi-PROCESS Raft cluster — 3 lockstepd processes over real TCP — and assert leader
# uniqueness, replicated-commit AGREEMENT, and kill+restart CATCH-UP from ProdDisk.
#
# WHAT IT ASSERTS (each bounded by an ABSOLUTE deadline; a missed deadline FAILS):
#   (1) ELECTION: launch 3 daemons; poll STATUS until exactly ONE node is LEADER (role
#       2) and the other two are FOLLOWERS (role 0) — one leader at a time.
#   (2) REPLICATED COMMIT AGREEMENT: SUBMIT several distinct values via the leader-find
#       client (lockstep_admin submit retries the next host on NotLeader). Poll until
#       ALL 3 nodes' commit_index covers them AND their committed-log digests AGREE
#       (same values, same order) — real replication + commit over TCP.
#   (3) KILL + RESTART CATCH-UP: kill ONE follower (SIGKILL), restart it pointing at the
#       SAME data dir; poll until it rejoins and its committed log CATCHES UP to the
#       others (recovery from ProdDisk + replication).
#
# PROCESS-CLEANUP GUARANTEE (NON-NEGOTIABLE — a host freeze happened from runaway
# procs): every lockstepd is launched in THIS script's process group; a `trap` on
# EXIT/INT/TERM SIGKILLs every tracked child PID on EVERY exit path (pass, fail,
# error, the outer wall-guard tripping). lockstepd ALSO self-deadlines (--run-seconds)
# so a daemon dies even if this script is killed before cleanup runs. Ports are a fixed
# loopback range; a taken port makes lockstepd fail fast (no spin).
#
# Run UNDER the fork-parent wall-guard (see the brief's `to()`), e.g. `to 60 bash
# tests/prod_cluster_smoke.sh`, so even a buggy cleanup path here is outlived by the
# guard. This script is NOT lint-exempt scaffolding in the syscall sense (it is shell),
# but it embodies the same bounded + cleanup discipline.

set -u  # NOT -e: we want to run cleanup + print a verdict even on a failed assertion.

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

# ---- cluster topology (fixed loopback ports) --------------------------------
# 3 nodes; each has a consensus listen port + an admin port. The peer map (id:port)
# is identical for every node (the full cluster); each dials the other two.
IDS=(1 2 3)
CPORTS=(19101 19102 19103)   # consensus ports
APORTS=(19201 19202 19203)   # admin ports
PEERS="--peer 1:${CPORTS[0]} --peer 2:${CPORTS[1]} --peer 3:${CPORTS[2]}"
RUN_SECONDS=45               # each daemon self-deadline (> the whole test budget)
SEED=12345

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_cluster_XXXXXX")"
PIDS=()                      # tracked child PIDs (filled as we launch)

# ---- CLEANUP GUARANTEE: SIGKILL every tracked child on EVERY exit path -------
cleanup() {
  trap - EXIT INT TERM
  # SIGKILL every TRACKED child PID (the only lockstepd processes we spawned). We do
  # NOT pkill by command-line pattern: under ctest the harness parent's argv contains
  # the lockstepd PATH, so a `pkill -f $LOCKSTEPD` would SIGKILL the test harness
  # itself. Tracked PIDs are precise + complete (every launch records its PID).
  for pid in "${PIDS[@]:-}"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null
    fi
  done
  wait 2>/dev/null
  rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---- launch one daemon (records its PID for cleanup) ------------------------
launch() {
  local i="$1"
  local id="${IDS[$i]}"
  local dd="$WORKDIR/node$id"
  mkdir -p "$dd"
  "$LOCKSTEPD" --node-id "$id" --listen-port "${CPORTS[$i]}" \
    --admin-port "${APORTS[$i]}" $PEERS --data-dir "$dd" --seed "$SEED" \
    --run-seconds "$RUN_SECONDS" >"$WORKDIR/node$id.log" 2>&1 &
  local pid=$!
  PIDS[$i]=$pid
  echo "launched node $id pid=$pid consensus=${CPORTS[$i]} admin=${APORTS[$i]} data=$dd"
}

now_ms() { date +%s%3N; }

# admin STATUS for one port -> echoes the raw "STATUS ..." line (ok=0 if unreachable).
status_of() { "$ADMIN" status --host "$1" 2>/dev/null | head -1; }

# Extract a field (role=/term=/commit=/ok=) from a STATUS line.
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }
logfield() { echo "$1" | sed -n "s/.*log=\([^ ]*\).*/\1/p"; }

# ============================================================================
PASS=1
fail() { echo "FAIL: $*"; PASS=0; }

echo
echo "=== launch 3 lockstepd processes ==="
for i in 0 1 2; do launch "$i"; done

# ---- (1) ELECTION: bounded wait for exactly one LEADER + two FOLLOWERS -------
echo
echo "=== (1) ELECTION — wait for exactly one LEADER, two FOLLOWERS ==="
DEADLINE=$(( $(now_ms) + 20000 ))   # 20 s absolute deadline
LEADER_PORT=""
LEADER_ID=""
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  leaders=0; followers=0; LEADER_PORT=""; LEADER_ID=""
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"
    [ -z "$line" ] && continue
    [ "$(field "$line" ok)" = "1" ] || continue
    role="$(field "$line" role)"
    if [ "$role" = "2" ]; then leaders=$((leaders+1)); LEADER_PORT="${APORTS[$i]}"; LEADER_ID="${IDS[$i]}"; fi
    if [ "$role" = "0" ]; then followers=$((followers+1)); fi
  done
  if [ "$leaders" = "1" ] && [ "$followers" = "2" ]; then break; fi
  sleep 0.3
done
if [ "$leaders" = "1" ] && [ "$followers" = "2" ]; then
  echo "PASS election: leader=node$LEADER_ID (admin $LEADER_PORT), 2 followers"
else
  fail "no unique leader within deadline (leaders=$leaders followers=$followers)"
fi

# ---- (2) REPLICATED COMMIT AGREEMENT ----------------------------------------
echo
echo "=== (2) REPLICATED COMMIT — submit values, wait for all 3 to agree ==="
VALUES=(val-alpha val-bravo val-charlie val-delta)
ALL_APORTS="--host ${APORTS[0]} --host ${APORTS[1]} --host ${APORTS[2]}"
for v in "${VALUES[@]}"; do
  # leader-find submit (retries the next host on NotLeader). Bounded retry across a
  # short deadline in case of an election in flight.
  sdl=$(( $(now_ms) + 8000 ))
  ok=0
  while [ "$(now_ms)" -lt "$sdl" ]; do
    out="$("$ADMIN" submit "$v" $ALL_APORTS 2>/dev/null | head -1)"
    if [ "$(field "$out" accepted)" = "1" ]; then
      echo "  submitted $v -> $(echo "$out" | sed 's/^SUBMIT //')"
      ok=1; break
    fi
    sleep 0.3
  done
  [ "$ok" = "1" ] || fail "submit $v not accepted within deadline"
done

NVALS=${#VALUES[@]}
EXPECTED="$(IFS=,; echo "${VALUES[*]}")"
echo "  expecting commit_index>=$NVALS and committed-log digest containing: $EXPECTED"
DEADLINE=$(( $(now_ms) + 20000 ))
agreed=0
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  ok_all=1; digest=""; mismatch=0
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"
    if [ "$(field "$line" ok)" != "1" ]; then ok_all=0; break; fi
    commit="$(field "$line" commit)"
    lg="$(logfield "$line")"
    # require commit_index covers all submitted values
    if [ -z "$commit" ] || [ "$commit" -lt "$NVALS" ]; then ok_all=0; break; fi
    # committed prefix = first NVALS entries of the log digest
    prefix="$(echo "$lg" | cut -d, -f1-"$NVALS")"
    if [ -z "$digest" ]; then digest="$prefix"; elif [ "$digest" != "$prefix" ]; then mismatch=1; fi
  done
  if [ "$ok_all" = "1" ] && [ "$mismatch" = "0" ] && [ "$digest" = "$EXPECTED" ]; then agreed=1; break; fi
  sleep 0.3
done
if [ "$agreed" = "1" ]; then
  echo "PASS replication: all 3 nodes commit_index>=$NVALS and committed logs AGREE ($EXPECTED)"
else
  echo "  last seen:"; for i in 0 1 2; do echo "    node${IDS[$i]}: $(status_of "${APORTS[$i]}")"; done
  fail "nodes did not agree on committed log within deadline"
fi

# ---- (3) KILL + RESTART CATCH-UP --------------------------------------------
echo
echo "=== (3) KILL a follower, RESTART it on the SAME data dir, wait for CATCH-UP ==="
# pick a follower (a non-leader node) to kill.
VICTIM=-1
for i in 0 1 2; do
  if [ "${IDS[$i]}" != "$LEADER_ID" ]; then VICTIM=$i; break; fi
done
if [ "$VICTIM" -lt 0 ]; then
  fail "could not pick a follower to kill"
else
  vid="${IDS[$VICTIM]}"; vpid="${PIDS[$VICTIM]}"
  echo "  killing follower node$vid pid=$vpid (SIGKILL)"
  kill -KILL "$vpid" 2>/dev/null
  wait "$vpid" 2>/dev/null
  PIDS[$VICTIM]=""
  # submit one MORE value while the victim is down (it must catch up to this too).
  EXTRA=val-echo
  sdl=$(( $(now_ms) + 8000 )); ok=0
  while [ "$(now_ms)" -lt "$sdl" ]; do
    out="$("$ADMIN" submit "$EXTRA" $ALL_APORTS 2>/dev/null | head -1)"
    [ "$(field "$out" accepted)" = "1" ] && { echo "  submitted $EXTRA while node$vid down"; ok=1; break; }
    sleep 0.3
  done
  [ "$ok" = "1" ] || fail "submit $EXTRA (victim down) not accepted"
  VALUES+=("$EXTRA"); NVALS=${#VALUES[@]}; EXPECTED="$(IFS=,; echo "${VALUES[*]}")"

  echo "  restarting node$vid on the SAME data dir -> must recover from ProdDisk + catch up"
  launch "$VICTIM"

  DEADLINE=$(( $(now_ms) + 25000 ))
  caught=0
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    line="$(status_of "${APORTS[$VICTIM]}")"
    if [ "$(field "$line" ok)" = "1" ]; then
      commit="$(field "$line" commit)"
      prefix="$(logfield "$line" | cut -d, -f1-"$NVALS")"
      if [ -n "$commit" ] && [ "$commit" -ge "$NVALS" ] && [ "$prefix" = "$EXPECTED" ]; then caught=1; break; fi
    fi
    sleep 0.3
  done
  if [ "$caught" = "1" ]; then
    echo "PASS catch-up: restarted node$vid commit_index>=$NVALS and committed log == $EXPECTED"
  else
    echo "    node$vid: $(status_of "${APORTS[$VICTIM]}")"
    fail "restarted node$vid did not catch up within deadline"
  fi

  # final agreement check across all 3 (including the recovered node).
  ok_all=1; digest=""
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"
    [ "$(field "$line" ok)" = "1" ] || { ok_all=0; continue; }
    prefix="$(logfield "$line" | cut -d, -f1-"$NVALS")"
    [ -z "$digest" ] && digest="$prefix"
    [ "$digest" = "$prefix" ] || ok_all=0
  done
  if [ "$ok_all" = "1" ] && [ "$digest" = "$EXPECTED" ]; then
    echo "PASS final agreement: all 3 nodes (incl recovered) committed == $EXPECTED"
  else
    fail "final 3-node agreement check failed (digest=$digest expected=$EXPECTED)"
  fi
fi

echo
if [ "$PASS" = "1" ]; then
  echo "[prod_cluster_smoke] ALL PASS"
  exit 0
else
  echo "[prod_cluster_smoke] FAILED"
  exit 1
fi
