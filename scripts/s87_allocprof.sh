#!/usr/bin/env bash
# S8.7 alloc/op profiler run (container). Builds the alloc-profiling lockstepd
# (-DLOCKSTEP_PROFILE_ALLOC), drives a 1-node pbench load, prints the ALLOCSTATS line
# (heap allocs per committed op). Bounded; SIGKILLs every daemon on every exit path.
set -u
ulimit -c 0; ulimit -s 16384

BUILD="${BUILD:-build/lprof}"
COUNT="${COUNT:-4000}"
DEPTH="${DEPTH:-16}"
RUN_SECONDS="${RUN_SECONDS:-6}"
PORT=19551
ADMIN=19552
SEED=12345
DATADIR="$(mktemp -d)"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; pkill -9 -x lockstepd 2>/dev/null; rm -rf "$DATADIR"; }
trap cleanup EXIT INT TERM

cmake -S . -B "$BUILD" -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-DLOCKSTEP_PROFILE_ALLOC" >/dev/null 2>&1
cmake --build "$BUILD" -j6 --target lockstepd lockstep_admin >/dev/null 2>&1 || { echo "BUILD FAIL"; exit 1; }

DAEMON="$BUILD/cli/lockstepd"
ADMINCLI="$BUILD/cli/lockstep_admin"
[ -x "$DAEMON" ] || DAEMON="$(find "$BUILD" -name lockstepd -type f | head -1)"
[ -x "$ADMINCLI" ] || ADMINCLI="$(find "$BUILD" -name lockstep_admin -type f | head -1)"

# 1-node cluster (self-commits at append): arm the alloc profiler at run time.
LOCKSTEP_ALLOC_PROFILE=1 "$DAEMON" --node-id 1 --listen-port $PORT --admin-port $ADMIN \
   --peer 1:$PORT --data-dir "$DATADIR" --seed $SEED --run-seconds $RUN_SECONDS \
   > "$DATADIR/d.log" 2>&1 &
PIDS+=($!)
sleep 1.5
"$ADMINCLI" pbench --count "$COUNT" --inflight "$DEPTH" --conns 1 --value-bytes 16 \
   --host $ADMIN > "$DATADIR/pbench.log" 2>&1
# Let the daemon self-deadline (--run-seconds) and print DISKSTATS + ALLOCSTATS on its
# CLEAN shutdown path (the alloc counter is reported only on the deadline exit, NOT on a
# signal). Bounded wait for that line, then reap.
for i in $(seq 1 $((RUN_SECONDS*4 + 8))); do grep -q "ALLOCSTATS" "$DATADIR/d.log" && break; sleep 0.25; done
wait "${PIDS[0]}" 2>/dev/null

echo "===== pbench ====="; grep -E "commit_tput|accept_tput|PBENCH" "$DATADIR/pbench.log" | head -2
echo "===== alloc ====="; grep -E "ALLOCSTATS|ALLOCHIST" "$DATADIR/d.log"
