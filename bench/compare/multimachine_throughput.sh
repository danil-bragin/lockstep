#!/usr/bin/env bash
# Horizontal THROUGHPUT validation: N containers, each an independent Lockstep node on a
# distinct IP, EACH driven by its OWN local lockstep_admin client (so the single-client cap
# that bounds the on-box bench does NOT bound the aggregate). Sum the per-container committed
# throughput -> the horizontal aggregate. "Add a container, add throughput."
#
# Each container is pinned to CPUS cores. The aggregate is compared to a single Postgres for
# scale context (Postgres-HA would still write through ONE primary).
#
# Usage: bash multimachine_throughput.sh "<N-list>"   e.g. "1 2 3 5"
set -u
NLIST="${1:-1 2 3}"
CPUS="${CPUS:-2}"
OPCOUNT="${OPCOUNT:-60000}"
CONC="${CONC:-64}"
NET=lstput
IPBASE=172.31.0
IMG=lockstep-dev:latest
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LSD=/work/build/lrel/cli/lockstepd
ADM=/work/build/lrel/cli/lockstep_admin

cleanup(){ docker ps -aq --filter "name=lstp" | xargs -r docker rm -f >/dev/null 2>&1; docker network rm "$NET" >/dev/null 2>&1; }
trap cleanup EXIT
cleanup

echo "### Horizontal throughput: per-container local client, summed (cpus/container=$CPUS) ###"
docker network create --subnet "${IPBASE}.0/16" "$NET" >/dev/null

run_n(){  # $1 = N containers
  local N=$1 i
  docker ps -aq --filter "name=lstp" | xargs -r docker rm -f >/dev/null 2>&1
  # Launch N independent single-node daemons, one per container, distinct IP, pinned to CPUS.
  for i in $(seq 1 "$N"); do
    docker run -d --name "lstp$i" --network "$NET" --ip "${IPBASE}.$((10+i))" \
      --cpus "$CPUS" -v "$ROOT:/work" -w /work -e LOCKSTEP_BIND_ADDR=0.0.0.0 --memory=2g "$IMG" \
      bash -lc "ulimit -c 0; mkdir -p /tmp/n && exec $LSD --node-id 1 --listen-port 7001 \
        --admin-port 7101 --peer 1:7001 --data-dir /tmp/n --seed 12345 --run-seconds 120" >/dev/null
  done
  # Wait for every container's local leader.
  for i in $(seq 1 "$N"); do
    for _ in $(seq 1 40); do
      docker exec "lstp$i" "$ADM" status --host 7101 2>/dev/null | grep -q "role=2" && break; sleep 0.3
    done
  done
  # Fire each container's LOCAL pbench concurrently; collect commit_tput from each.
  local pids="" tmp; tmp=$(mktemp -d)
  for i in $(seq 1 "$N"); do
    ( docker exec "lstp$i" "$ADM" pbench --count "$OPCOUNT" --inflight "$CONC" --conns 1 \
        --value-bytes 16 --host 7101 2>/dev/null \
        | sed -n 's/.*commit_tput=\([0-9.]*\).*/\1/p' > "$tmp/$i" ) &
    pids="$pids $!"
  done
  for p in $pids; do wait "$p"; done
  local sum=0 line
  for i in $(seq 1 "$N"); do
    line=$(cat "$tmp/$i" 2>/dev/null); line=${line:-0}
    sum=$(python3 -c "print($sum + $line)")
  done
  rm -rf "$tmp"
  printf "N=%-2d containers  aggregate_commit_tput=%.0f ops/s  (per-container ~%.0f)\n" \
    "$N" "$sum" "$(python3 -c "print($sum/$N)")"
}

for n in $NLIST; do run_n "$n"; done
echo "### done — aggregate scales with containers (each has its own client; no shared-client cap) ###"
