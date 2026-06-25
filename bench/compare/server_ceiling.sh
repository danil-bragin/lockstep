#!/usr/bin/env bash
# Measure the TRUE single-node server commit-throughput ceiling by removing the single-client
# bottleneck: run M CONCURRENT lockstep_admin pbench PROCESSES (each its own reactor +
# connection) against ONE daemon and SUM their commit_tput. Sweep M; the peak aggregate is
# the server ceiling (a single client tops out far below it). Runs inside lockstep-dev.
#
# Pin: server on cores [0,SCORES); clients on the rest. Use as the BEFORE/AFTER instrument
# for per-op (direction-2) optimizations — a single client can't see server-side gains.
#
# Usage (inside container, repo at /work):
#   SCORES=6 CCORES=6-11 MLIST="1 2 4 6" COUNT=40000 bash server_ceiling.sh
set -u
B=/work/build/lrel/cli
SCORES="${SCORES:-6}"            # server cores [0,SCORES)
CCORES="${CCORES:-6-11}"         # client taskset range
MLIST="${MLIST:-1 2 4 6}"        # concurrent-client counts to sweep
COUNT="${COUNT:-40000}"          # ops per client process
CONC="${CONC:-64}"               # pbench inflight per client
RUN_SECONDS="${RUN_SECONDS:-160}"
DD=$(mktemp -d); mkdir -p "$DD/n1"
trap 'pkill -9 lockstepd 2>/dev/null; rm -rf "$DD"' EXIT
ulimit -c 0; ulimit -s 16384 2>/dev/null || true

taskset -c "0-$((SCORES-1))" "$B/lockstepd" --node-id 1 --listen-port 7001 --admin-port 7101 \
  --peer 1:7001 --data-dir "$DD/n1" --seed 1 --run-seconds "$RUN_SECONDS" >/dev/null 2>&1 &
for _ in $(seq 1 40); do "$B/lockstep_admin" status --host 7101 2>/dev/null | grep -q role=2 && break; sleep 0.3; done

best=0
for M in $MLIST; do
  tmp=$(mktemp -d); pids=""
  for i in $(seq 1 "$M"); do
    ( taskset -c "$CCORES" "$B/lockstep_admin" pbench --count "$COUNT" --inflight "$CONC" \
        --conns 1 --value-bytes 16 --host 7101 2>/dev/null \
        | sed -n "s/.*commit_tput=\([0-9.]*\).*/\1/p" > "$tmp/$i" ) & pids="$pids $!"
  done
  for p in $pids; do wait "$p"; done
  s=0; for i in $(seq 1 "$M"); do v=$(cat "$tmp/$i" 2>/dev/null); s=$(awk "BEGIN{print $s+${v:-0}}"); done
  echo "M=$M clients  aggregate_commit_tput=$s"
  awk "BEGIN{exit !($s>$best)}" && best=$s
  rm -rf "$tmp"
done
echo "SERVER_CEILING=$best ops/s (server cores=$SCORES)"
