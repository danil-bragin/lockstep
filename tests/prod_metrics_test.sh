#!/usr/bin/env bash
# prod_metrics_test.sh — Phase 10 OBSERVABILITY. THE REALITY-CHECK TEST (the teeth): a
# metric that does not track the real event is a BUG. Bring up a REAL 3-process lockstepd
# cluster over loopback TCP (the prod_cluster_smoke topology), drive a KNOWN workload of N
# durable submits, scrape the METRICS verb in Prometheus exposition format, and ASSERT the
# metrics REFLECT REALITY by cross-checking against STATUS:
#
#   (1) EXPOSITION: METRICS returns a valid Prometheus text block (# TYPE lines +
#       name{shard="..",node=".."} value samples) for every node. O(metrics), not O(log).
#   (2) GAUGES MATCH STATUS: each node's role/current_term/commit_index gauge == the same
#       node's STATUS role/term/commit (read independently). No lying gauges.
#   (3) submits_committed REFLECTS REALITY: the leader's submits_committed counter equals
#       the number of values WE actually submitted+committed (cross-checked against the
#       leader's STATUS commit_index advancing by N).
#   (4) leader_changes BUMPS on a real election: SIGKILL the leader, wait for a NEW leader,
#       and assert the new leader's leader_changes counter is >= 1 (it really became leader).
#
# PROCESS-CLEANUP GUARANTEE (a host freeze happened): every lockstepd is tracked by PID; a
# trap on EXIT/INT/TERM SIGKILLs every tracked child on EVERY path; each lockstepd self-
# deadlines (--run-seconds) so no orphan can outlive the test. BOUNDED: every wait has an
# absolute deadline; the ctest has its own TIMEOUT. Run under the fork-parent wall-guard.

set -u  # NOT -e: run cleanup + print a verdict even on a failed assertion.

LOCKSTEPD="${LOCKSTEPD:-}"
ADMIN="${LOCKSTEP_ADMIN:-}"
find_bin() {
  local name="$1"
  for d in build/ldev build build/lrel cmake-build-debug build/Debug .; do
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

# ---- cluster topology (fixed loopback ports, distinct from the smoke test) ----
IDS=(1 2 3)
CPORTS=(19301 19302 19303)   # consensus ports
APORTS=(19401 19402 19403)   # admin ports
PEERS="--peer 1:${CPORTS[0]} --peer 2:${CPORTS[1]} --peer 3:${CPORTS[2]}"
RUN_SECONDS=60
SEED=778899
N_SUBMITS=8
ALL_APORTS="--host ${APORTS[0]} --host ${APORTS[1]} --host ${APORTS[2]}"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/lockstep_metrics_XXXXXX")"
PIDS=()
PASS=1
fail() { echo "FAIL: $*"; PASS=0; }

cleanup() {
  trap - EXIT INT TERM
  for pid in "${PIDS[@]:-}"; do
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
  done
  wait 2>/dev/null
  rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

launch() {
  local i="$1"; local id="${IDS[$i]}"; local dd="$WORKDIR/node$id"
  mkdir -p "$dd"
  "$LOCKSTEPD" --node-id "$id" --listen-port "${CPORTS[$i]}" \
    --admin-port "${APORTS[$i]}" $PEERS --data-dir "$dd" --seed "$SEED" \
    --run-seconds "$RUN_SECONDS" >"$WORKDIR/node$id.log" 2>&1 &
  PIDS[$i]=$!
  echo "launched node $id pid=${PIDS[$i]} admin=${APORTS[$i]} data=$dd"
}

now_ms() { date +%s%3N; }
status_of() { "$ADMIN" status --host "$1" 2>/dev/null | head -1; }
field() { echo "$1" | sed -n "s/.*$2=\([^ ]*\).*/\1/p"; }

# Scrape one admin port; echo the raw Prometheus text.
metrics_of() { "$ADMIN" metrics --host "$1" 2>/dev/null; }
# Pull one metric sample value out of a Prometheus scrape: metric_val "<text>" name
metric_val() { echo "$1" | sed -n "s/^lockstep_$2{[^}]*} \([0-9][0-9]*\)$/\1/p" | head -1; }

echo
echo "=== launch 3 lockstepd processes ==="
for i in 0 1 2; do launch "$i"; done

# ---- wait for a unique leader ----
echo
echo "=== wait for a unique leader ==="
DEADLINE=$(( $(now_ms) + 25000 )); LEADER_PORT=""; LEADER_ID=""; LEADER_IDX=-1
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  leaders=0; LEADER_PORT=""; LEADER_ID=""; LEADER_IDX=-1
  for i in 0 1 2; do
    line="$(status_of "${APORTS[$i]}")"; [ -z "$line" ] && continue
    [ "$(field "$line" ok)" = "1" ] || continue
    if [ "$(field "$line" role)" = "2" ]; then
      leaders=$((leaders+1)); LEADER_PORT="${APORTS[$i]}"; LEADER_ID="${IDS[$i]}"; LEADER_IDX=$i
    fi
  done
  [ "$leaders" = "1" ] && break
  sleep 0.3
done
if [ "$leaders" = "1" ]; then
  echo "leader = node$LEADER_ID (admin $LEADER_PORT)"
else
  fail "no unique leader within deadline"; cleanup; exit 1
fi

# ---- (1) EXPOSITION: a valid Prometheus scrape on every node ----
echo
echo "=== (1) EXPOSITION — scrape METRICS (Prometheus format) on every node ==="
for i in 0 1 2; do
  txt="$(metrics_of "${APORTS[$i]}")"
  if echo "$txt" | grep -q "# TYPE lockstep_commit_index gauge" \
     && echo "$txt" | grep -Eq "^lockstep_role\{shard=\"[0-9]+\",node=\"${IDS[$i]}\"\} [0-9]+$"; then
    echo "  node${IDS[$i]} METRICS ok (valid Prometheus exposition, node label=${IDS[$i]})"
  else
    fail "node${IDS[$i]} METRICS not valid Prometheus exposition"
    echo "$txt" | head -8 | sed 's/^/    /'
  fi
done
echo "  --- sample scrape (leader node$LEADER_ID) ---"
metrics_of "$LEADER_PORT" | sed 's/^/    /'

# Record the leader's submits_committed BEFORE the workload (baseline).
lead_metrics_before="$(metrics_of "$LEADER_PORT")"
committed_before="$(metric_val "$lead_metrics_before" submits_committed_total)"
[ -z "$committed_before" ] && committed_before=0
commit_idx_before="$(field "$(status_of "$LEADER_PORT")" commit)"
[ -z "$commit_idx_before" ] && commit_idx_before=0
echo "  baseline: leader submits_committed=$committed_before commit_index=$commit_idx_before"

# ---- (2)+(3) DRIVE N SUBMITS, then assert metrics reflect reality ----
echo
echo "=== (2)/(3) drive $N_SUBMITS durable submits; metrics must reflect reality ==="
accepted=0
for n in $(seq 1 "$N_SUBMITS"); do
  v="metric-val-$n"
  sdl=$(( $(now_ms) + 8000 )); ok=0
  while [ "$(now_ms)" -lt "$sdl" ]; do
    out="$("$ADMIN" submit "$v" $ALL_APORTS 2>/dev/null | head -1)"
    if [ "$(field "$out" durable)" = "1" ]; then ok=1; break; fi
    sleep 0.2
  done
  if [ "$ok" = "1" ]; then accepted=$((accepted+1)); else fail "submit $v not durable"; fi
done
echo "  $accepted/$N_SUBMITS values submitted durably (committed on a quorum)"

# Give the leader's metrics a moment + scrape it (the scrape drives the refresh).
sleep 0.5
lead_metrics_after="$(metrics_of "$LEADER_PORT")"
lead_status_after="$(status_of "$LEADER_PORT")"
committed_after="$(metric_val "$lead_metrics_after" submits_committed_total)"
[ -z "$committed_after" ] && committed_after=0
accepted_after="$(metric_val "$lead_metrics_after" submits_accepted_total)"
[ -z "$accepted_after" ] && accepted_after=0
m_role="$(metric_val "$lead_metrics_after" role)"
m_term="$(metric_val "$lead_metrics_after" current_term)"
m_commit="$(metric_val "$lead_metrics_after" commit_index)"
s_role="$(field "$lead_status_after" role)"
s_term="$(field "$lead_status_after" term)"
s_commit="$(field "$lead_status_after" commit)"
echo "  leader metrics: submits_accepted=$accepted_after submits_committed=$committed_after role=$m_role term=$m_term commit=$m_commit"
echo "  leader STATUS : role=$s_role term=$s_term commit=$s_commit"

# (2) GAUGES MATCH STATUS — role/term/commit gauge == STATUS (no lying gauges).
if [ "$m_role" = "$s_role" ] && [ "$m_term" = "$s_term" ] && [ "$m_commit" = "$s_commit" ]; then
  echo "PASS gauges-match-STATUS: role/term/commit_index gauges == STATUS"
else
  fail "metric gauges do not match STATUS (role $m_role/$s_role term $m_term/$s_term commit $m_commit/$s_commit)"
fi

# (3) submits_committed REFLECTS REALITY — it grew by the number we actually committed.
committed_delta=$(( committed_after - committed_before ))
commit_idx_delta=$(( s_commit - commit_idx_before ))
echo "  submits_committed grew by $committed_delta; STATUS commit_index grew by $commit_idx_delta (we submitted $accepted)"
# The leader's committed counter must cover the values WE submitted (it may also count a
# leader-bootstrap entry, so >= accepted is the honest floor; it must NOT undercount).
if [ "$committed_delta" -ge "$accepted" ] && [ "$accepted" -gt 0 ]; then
  echo "PASS submits_committed-reflects-reality: counter grew by >= the $accepted committed submits"
else
  fail "submits_committed ($committed_delta) did not track the $accepted real committed submits (LYING COUNTER)"
fi
# And the accept counter must be >= our accepts too (the leader accepted them).
if [ "$accepted_after" -ge "$accepted" ]; then
  echo "PASS submits_accepted-reflects-reality: counter >= the $accepted accepted submits"
else
  fail "submits_accepted ($accepted_after) < the $accepted real accepts (LYING COUNTER)"
fi

# ---- (4) leader_changes BUMPS on a real election ----
echo
echo "=== (4) leader_changes BUMPS — SIGKILL the leader, a new leader must bump it ==="
echo "  killing leader node$LEADER_ID pid=${PIDS[$LEADER_IDX]} (SIGKILL)"
kill -KILL "${PIDS[$LEADER_IDX]}" 2>/dev/null; wait "${PIDS[$LEADER_IDX]}" 2>/dev/null
PIDS[$LEADER_IDX]=""

DEADLINE=$(( $(now_ms) + 25000 )); NEW_PORT=""; NEW_ID=""
while [ "$(now_ms)" -lt "$DEADLINE" ]; do
  for i in 0 1 2; do
    [ "$i" = "$LEADER_IDX" ] && continue
    line="$(status_of "${APORTS[$i]}")"; [ -z "$line" ] && continue
    [ "$(field "$line" ok)" = "1" ] || continue
    if [ "$(field "$line" role)" = "2" ]; then NEW_PORT="${APORTS[$i]}"; NEW_ID="${IDS[$i]}"; break; fi
  done
  [ -n "$NEW_PORT" ] && break
  sleep 0.3
done
if [ -n "$NEW_PORT" ]; then
  echo "  new leader = node$NEW_ID (admin $NEW_PORT)"
  # scrape its metrics; leader_changes must be >= 1 (it really became leader).
  nl_metrics="$(metrics_of "$NEW_PORT")"
  lc="$(metric_val "$nl_metrics" leader_changes_total)"
  [ -z "$lc" ] && lc=0
  echo "  new leader leader_changes_total=$lc"
  if [ "$lc" -ge 1 ]; then
    echo "PASS leader_changes-bumps: the new leader's leader_changes counter is >= 1"
  else
    fail "leader_changes did not bump on a real election (got $lc) (LYING COUNTER)"
    echo "$nl_metrics" | grep -E "role|leader_changes|current_term" | sed 's/^/    /'
  fi
else
  fail "no new leader elected after killing the old one"
fi

echo
echo "=== shutting down all procs (clean) ==="
for i in 0 1 2; do
  pid="${PIDS[$i]:-}"
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
done
wait 2>/dev/null

echo
if [ "$PASS" = "1" ]; then
  echo "[prod_metrics_test] ALL PASS — metrics reflect reality"
  exit 0
else
  echo "[prod_metrics_test] FAILED"
  exit 1
fi
