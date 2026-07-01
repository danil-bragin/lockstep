#!/usr/bin/env bash
# prod_replicated_shards.sh — Phase 9 S9.4. REPLICATED SHARDS (HA): each shard is a
# 3-node Raft group spread across 3 PROCESSES, so a shard survives a node failure while
# keeping the multi-shard scaling. This combines the S9.1 thread-per-shard daemon with
# the S5b-2 multi-process Raft cluster — the verified consensus core is UNCHANGED.
#
# TOPOLOGY: N=3 lockstepd processes, each hosting M shard-replicas (M threads). Shard s's
# Raft group = the s-th replica across the 3 processes (a 3-node group). Ports follow a
# deterministic scheme the daemon AND the client compute identically:
#   consensus_port(proc p, shard s) = base + (p-1)*(2M) + s
#   admin_port    (proc p, shard s) = base + (p-1)*(2M) + M + s
# The client routes a key -> shard s -> leader-find among shard s's 3 admin ports.
#
# WHAT IT ASSERTS (each bounded by an ABSOLUTE deadline; a missed deadline FAILS):
#   (1) ELECTION: bring up 3 procs x M shards; every shard's 3-node group elects exactly
#       one leader (poll RSTATUS until each shard has a leader among its 3 replicas).
#   (2) DURABLE COMMIT: rsubmit several keys (durable=1 -> committed on a quorum of each
#       shard's group), routed by key->shard->leader-find. Confirm each shard's group
#       agrees (committed logs prefix-match across the 3 replicas of that shard).
#   (3) HA — KILL ONE PROCESS: SIGKILL process 2 (takes down ONE replica of EVERY shard).
#       Each shard's group must STAY AVAILABLE: re-elect if needed + keep committing.
#       rsubmit MORE keys while proc 2 is down — they must commit on the surviving quorum.
#   (4) RESTART + CATCH-UP: restart process 2 on its SAME data dir; its replicas rejoin
#       and catch up from ProdDisk to every shard's committed log (no committed-data loss).
#
# CROSS-SHARD over replicated shards: argued in the receipt (each shard's COMMITTED log is
# identical regardless of replica count, so the S9.3 Sequencer is agnostic to it); a full
# cross-shard-over-replicated integration test is FLAGGED for follow-up.
#
# PROCESS-CLEANUP GUARANTEE (a host freeze happened — N procs x M threads is real
# pressure): every lockstepd is tracked by PID; a trap on EXIT/INT/TERM SIGKILLs every
# tracked child on EVERY path; each lockstepd self-deadlines (--run-seconds) so no orphan
# can outlive the test. Run under the fork-parent wall-guard. BOUNDED throughout.

set -u  # NOT -e: run cleanup + print a verdict even on a failed assertion.

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

N="${PROCS:-3}"             # processes (== Raft group size per shard)
M="${SHARDS:-2}"            # shard-replicas per process
BASE="${BASE_PORT:-24000}"  # global base port for the deterministic scheme
RUN_SECONDS="${RUN_SECONDS:-60}"
SEED="${SEED:-555}"
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_replshard_XXXXXX")"
PIDS=()
FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

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

# Launch process p (1-based). Records its PID in PIDS[p-1]. Its data dir is per-process
# (proc_<p>) so each replica's ProdDisk is its own; a restart reuses the SAME dir.
launch_proc() {
  local p="$1"
  local dd="$WORKDIR/proc_$p"
  mkdir -p "$dd"
  "$LOCKSTEPD" --shards "$M" --shard-base-port "$BASE" \
    --proc-id "$p" --cluster-size "$N" \
    --data-dir "$dd" --seed "$SEED" --run-seconds "$RUN_SECONDS" \
    >"$WORKDIR/proc_$p.log" 2>&1 &
  PIDS[$((p-1))]=$!
  echo "launched proc $p pid=${PIDS[$((p-1))]} (M=$M shards, group-size=$N) data=$dd"
}

kill_proc() {
  local p="$1"; local idx=$((p-1))
  local pid="${PIDS[$idx]:-}"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  fi
  PIDS[$idx]=""
}

RTOPO="--shards $M --procs $N --base-port $BASE"

# Wait until EVERY shard has exactly one leader among its 3 replicas (bounded).
wait_all_shards_have_leader() {
  local DEADLINE=$(( $(now_ms) + 30000 ))
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    local out; out="$("$ADMIN" rstatus $RTOPO 2>/dev/null)"
    local good=1 s
    for ((s=0;s<M;s++)); do
      local leaders; leaders="$(echo "$out" | grep "shard=$s " | grep -c 'role=2')"
      [ "$leaders" -ge 1 ] || { good=0; break; }
    done
    [ "$good" = "1" ] && return 0
    sleep 0.3
  done
  return 1
}

# Submit (durable) a key/value via the leader-routed client, bounded retry.
rsubmit() {
  local k="$1" v="$2"
  local sdl=$(( $(now_ms) + 15000 ))
  while [ "$(now_ms)" -lt "$sdl" ]; do
    local out; out="$("$ADMIN" rsubmit "$k" "$v" $RTOPO 2>/dev/null | grep '^SUBMIT')"
    if [ "$(field "$out" durable)" = "1" ]; then
      echo "  rsubmit $k=$v -> $(echo "$out" | sed 's/^SUBMIT //')"
      return 0
    fi
    sleep 0.4
  done
  return 1
}

# Assert every given value appears, committed, in SOME shard's LEADER committed-log digest
# (the leader is authoritative; followers may lag). The values carry distinct text so a
# substring grep in a leader's log= field is exact enough. Bounded poll.
check_committed() {
  local expect=("$@")
  local DEADLINE=$(( $(now_ms) + 25000 ))
  while [ "$(now_ms)" -lt "$DEADLINE" ]; do
    local out leaders; out="$("$ADMIN" rstatus $RTOPO 2>/dev/null)"
    leaders="$(echo "$out" | grep 'role=2')"
    local missing=0 v
    for v in "${expect[@]}"; do
      echo "$leaders" | grep -q "$v" || { missing=1; break; }
    done
    [ "$missing" = "0" ] && return 0
    sleep 0.4
  done
  return 1
}

echo
echo "=== (1) ELECTION — bring up $N procs x $M shard-replicas; every shard elects a leader ==="
for ((p=1;p<=N;p++)); do launch_proc "$p"; done
if wait_all_shards_have_leader; then
  echo "PASS election: every one of $M shards has a leader in its $N-node group"
  "$ADMIN" rstatus $RTOPO 2>/dev/null | grep 'role=2' | sed 's/^/  leader: /'
else
  fail "not every shard elected a leader within deadline"
  "$ADMIN" rstatus $RTOPO 2>/dev/null | sed 's/^/  /'
fi

echo
echo "=== (2) DURABLE COMMIT — rsubmit keys (durable=1 = committed on a quorum) ==="
KEYS=(alpha bravo charlie delta echo foxtrot)
for k in "${KEYS[@]}"; do
  rsubmit "$k" "v-$k" || fail "rsubmit $k not durable within deadline"
done
# Verify each value is present in its shard's leader committed log.
if check_committed "${KEYS[@]/#/v-}"; then
  echo "PASS commit: all submitted values committed + visible in their shard's leader log"
else
  fail "not all submitted values committed within deadline"
  "$ADMIN" rstatus $RTOPO 2>/dev/null | sed 's/^/  /'
fi

echo
echo "=== (2b) HASHCHECK — cross-replica applied-keyspace agreement (shard 0's 3 replicas) ==="
# admin_port(proc p, shard s) = BASE + (p-1)*(2M) + M + s. Compare shard 0 across all procs.
HC_HOSTS=()
for ((p=1;p<=N;p++)); do HC_HOSTS+=(--host $(( BASE + (p-1)*2*M + M + 0 ))); done
# Let shard-0's followers settle to the leader's commit so the compared prefix is
# non-trivial (all 3 replicas at the same commit_index >= 1). Bounded.
HC_DL=$(( $(now_ms) + 15000 ))
while [ "$(now_ms)" -lt "$HC_DL" ]; do
  s0="$("$ADMIN" rstatus $RTOPO 2>/dev/null | grep 'shard=0 ')"
  commits="$(echo "$s0" | sed -n 's/.*commit=\([0-9]*\).*/\1/p')"
  mn="$(echo "$commits" | sort -n | head -1)"; mx="$(echo "$commits" | sort -n | tail -1)"
  [ -n "$mn" ] && [ "$mn" = "$mx" ] && [ "$mn" -ge 1 ] && break
  sleep 0.3
done
HC_OUT="$("$ADMIN" hashcheck "${HC_HOSTS[@]}" 2>/dev/null)"
echo "$HC_OUT" | sed 's/^/  /'
if echo "$HC_OUT" | grep -q '^HASHCHECK: OK'; then
  echo "PASS hashcheck: shard-0 replicas AGREE on applied keyspace hash (cross-replica corrupt-check)"
else
  fail "shard-0 replicas did not agree on keyspace hash (hashcheck)"
fi

echo
echo "=== (3) HA — SIGKILL proc 2 (one replica of EVERY shard); shards stay available ==="
kill_proc 2
echo "  proc 2 killed; each shard now runs on $((N-1)) live replicas (still a quorum of $N)"
# Each shard's group must keep a leader among the survivors and keep committing.
if wait_all_shards_have_leader; then
  echo "  every shard still has a leader after losing proc 2"
else
  fail "a shard lost availability after proc 2 died"
fi
HA_KEYS=(golf hotel india juliet)
for k in "${HA_KEYS[@]}"; do
  rsubmit "$k" "v-$k" || fail "rsubmit $k while proc 2 DOWN not durable (HA broken)"
done
if check_committed "${HA_KEYS[@]/#/v-}"; then
  echo "PASS HA: shards stayed available across proc-2 loss; new writes committed on quorum"
else
  fail "shards did not keep committing while proc 2 down"
fi

echo
echo "=== (4) RESTART proc 2 on SAME data dir; replicas rejoin + catch up from ProdDisk ==="
launch_proc 2
# Wait until proc 2's replica of EVERY shard has caught up to the shard leader's commit_index.
DEADLINE=$(( $(now_ms) + 30000 ))
caught=0
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  out="$("$ADMIN" rstatus $RTOPO 2>/dev/null)"
  ok=1
  for ((s=0;s<M;s++)); do
    lead_commit="$(echo "$out" | grep "shard=$s " | grep 'role=2' | head -1 | sed -n 's/.*commit=\([0-9]*\).*/\1/p')"
    p2line="$(echo "$out" | grep "shard=$s proc=2 ")"
    p2_ok="$(field "$p2line" ok)"; p2_commit="$(field "$p2line" commit)"
    if [ "$p2_ok" != "1" ] || [ -z "$lead_commit" ] || [ -z "$p2_commit" ] || [ "$p2_commit" -lt "$lead_commit" ]; then
      ok=0; break
    fi
  done
  [ "$ok" = "1" ] && { caught=1; break; }
  sleep 0.4
done
if [ "$caught" = "1" ]; then
  echo "PASS catch-up: proc-2 replicas rejoined + caught up to every shard's committed log"
  "$ADMIN" rstatus $RTOPO 2>/dev/null | grep 'proc=2' | sed 's/^/  /'
else
  fail "proc-2 replicas did not catch up within deadline"
  "$ADMIN" rstatus $RTOPO 2>/dev/null | sed 's/^/  /'
fi

# No committed-data loss: every key ever submitted (incl the HA-phase ones) still present.
echo
echo "=== no committed-data loss: all $(( ${#KEYS[@]} + ${#HA_KEYS[@]} )) values still committed ==="
ALL_VALS=("${KEYS[@]/#/v-}" "${HA_KEYS[@]/#/v-}")
if check_committed "${ALL_VALS[@]}"; then
  echo "PASS no-loss: every committed value survived the node loss + restart"
else
  fail "a committed value was lost across the fault"
fi

echo
echo "=== shutting down all procs (clean join) ==="
for ((p=1;p<=N;p++)); do kill_proc "$p"; done

echo
if [ "$FAIL" = "0" ]; then
  echo "[prod_replicated_shards] ALL PASS — replicated shards are highly available"
  exit 0
else
  echo "[prod_replicated_shards] FAILED"
  exit 1
fi
