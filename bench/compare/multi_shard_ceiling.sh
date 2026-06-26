#!/usr/bin/env bash
# Multi-shard x multi-client single-node throughput ceiling. The single-shard server_ceiling.sh
# tops out at ~206k commits/s because ONE shard's single reactor saturates; this drives M shards
# (lockstepd --shards M, thread-per-shard) with K concurrent pbench client PROCESSES PER SHARD
# and sums every client's durable commit_tput. Shows the node-level horizontal ceiling a single
# client/shard hides. Runs inside lockstep-dev (repo at /work).
#
# Usage (inside container):
#   SHARDS="1 2 4 8" KPER=2 SCORES=8 CCORES=8-13 COUNT=40000 bash multi_shard_ceiling.sh
set -u
B=/work/build/lrel/cli
SHARDS="${SHARDS:-1 2 4 8}"       # shard counts to sweep
KPER="${KPER:-2}"                 # concurrent pbench clients PER shard
SCORES="${SCORES:-8}"             # server cores [0,SCORES) for the shard threads
CCORES="${CCORES:-8-13}"          # client taskset range (disjoint from server)
COUNT="${COUNT:-40000}"           # ops per client process
CONC="${CONC:-64}"                # pbench inflight per client
RUN_SECONDS="${RUN_SECONDS:-300}" # daemon lifetime (must outlast the whole sweep)
BASE="${BASE:-30000}"
ulimit -c 0; ulimit -s 16384 2>/dev/null || true

best=0; best_m=0
for M in $SHARDS; do
    DD=$(mktemp -d)
    # Launch an M-shard daemon (each shard = its own thread + reactor + disk + admin port BASE+s).
    taskset -c "0-$((SCORES-1))" "$B/lockstepd" --shards "$M" --shard-base-port "$BASE" \
        --data-dir "$DD" --seed 1 --run-seconds "$RUN_SECONDS" >/dev/null 2>&1 &
    DPID=$!
    # Wait for every shard to elect (its admin port reports leader role=2).
    ok=1
    for s in $(seq 0 $((M-1))); do
        up=0
        for _ in $(seq 1 60); do
            "$B/lockstep_admin" status --host $((BASE+s)) 2>/dev/null | grep -q role=2 && { up=1; break; }
            sleep 0.3
        done
        [ "$up" = 1 ] || ok=0
    done
    if [ "$ok" != 1 ]; then echo "M=$M shards  FAILED to elect"; kill -9 "$DPID" 2>/dev/null; rm -rf "$DD"; continue; fi

    # Spawn K pbench clients PER shard (K*M total), each its own reactor + connection.
    tmp=$(mktemp -d); pids=""
    for s in $(seq 0 $((M-1))); do
        for c in $(seq 1 "$KPER"); do
            ( taskset -c "$CCORES" "$B/lockstep_admin" pbench --count "$COUNT" --inflight "$CONC" \
                --conns 1 --value-bytes 16 --host $((BASE+s)) 2>/dev/null \
                | sed -n "s/.*commit_tput=\([0-9.]*\).*/\1/p" > "$tmp/${s}_${c}" ) & pids="$pids $!"
        done
    done
    for p in $pids; do wait "$p"; done

    s_sum=0; n=0
    for f in "$tmp"/*; do v=$(cat "$f" 2>/dev/null); s_sum=$(awk "BEGIN{print $s_sum+${v:-0}}"); n=$((n+1)); done
    echo "M=$M shards x K=$KPER clients ($n procs)  aggregate_commit_tput=$s_sum"
    awk "BEGIN{exit !($s_sum>$best)}" && { best=$s_sum; best_m=$M; }
    kill -9 "$DPID" 2>/dev/null; pkill -9 -x lockstepd 2>/dev/null; rm -rf "$tmp" "$DD"
    sleep 1
done
echo "MULTI_SHARD_CEILING=$best ops/s (at M=$best_m shards, K=$KPER clients/shard, server cores=$SCORES)"
