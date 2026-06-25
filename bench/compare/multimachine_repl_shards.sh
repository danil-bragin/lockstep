#!/usr/bin/env bash
# Cross-MACHINE REPLICATED SHARDS: N containers (processes), each hosting M shard-REPLICAS;
# shard s forms an N-node Raft group ACROSS the N containers (distinct IPs). Proves the
# horizontal HA topology over the loopback-lifted transport: each shard replicates across
# machines and survives losing a minority of containers.
#
# Port scheme (stride=2*M): process p (1..N), shard s (0..M-1):
#   consensus_port(p,s) = BASE + (p-1)*2M + s ; admin_port(p,s) = BASE + (p-1)*2M + M + s
#
# Usage: bash multimachine_repl_shards.sh   (N=3 procs, M=2 shards)
set -u
N=3; M=2; BASE=8000
NET=lsrepl; IPBASE=172.32.0; IMG=lockstep-dev:latest
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LSD=/work/build/lrel/cli/lockstepd; ADM=/work/build/lrel/cli/lockstep_admin
stride=$((2*M))
cport(){ echo $(( BASE + ($1-1)*stride + $2 )); }            # $1=proc(1..N) $2=shard(0..M-1)
aport(){ echo $(( BASE + ($1-1)*stride + M + $2 )); }

cleanup(){ for p in $(seq 1 $N); do docker rm -f "lsr$p" >/dev/null 2>&1; done; docker network rm "$NET" >/dev/null 2>&1; }
trap cleanup EXIT
cleanup

echo "### Cross-machine REPLICATED shards: $N containers x $M shards (each shard = $N-node group) ###"
docker network create --subnet "${IPBASE}.0/16" "$NET" >/dev/null
PH=""; for p in $(seq 1 $N); do PH="$PH --proc-host ${p}:${IPBASE}.$((10+p))"; done

for p in $(seq 1 $N); do
  docker run -d --name "lsr$p" --network "$NET" --ip "${IPBASE}.$((10+p))" \
    -v "$ROOT:/work" -w /work -e LOCKSTEP_BIND_ADDR=0.0.0.0 --memory=2g "$IMG" \
    bash -lc "ulimit -c 0; mkdir -p /tmp/r && exec $LSD --shards $M --cluster-size $N \
      --proc-id $p --shard-base-port $BASE $PH --data-dir /tmp/r --seed 7 --run-seconds 100" >/dev/null
  echo "  started lsr$p @ ${IPBASE}.$((10+p))"
done

# For a shard, find which container holds its leader (local admin port reports role=2).
shard_leader(){ # $1=shard -> echoes container name or empty
  local s=$1 p
  for p in $(seq 1 $N); do
    docker exec "lsr$p" "$ADM" status --host "$(aport $p $s)" 2>/dev/null | grep -q "role=2" && { echo "lsr$p"; return; }
  done
}

echo "--- waiting for every shard to elect a leader across containers ---"
for s in $(seq 0 $((M-1))); do
  ldr=""; for _ in $(seq 1 50); do ldr=$(shard_leader $s); [ -n "$ldr" ] && break; sleep 0.4; done
  [ -z "$ldr" ] && { echo "FAIL: shard $s elected no leader"; exit 1; }
  echo "  shard $s leader = $ldr"
done

echo "--- commit a value to EACH shard via its leader; verify all $N containers agree ---"
for s in $(seq 0 $((M-1))); do
  ldr=$(shard_leader $s); lp=${ldr#lsr}
  docker exec "$ldr" "$ADM" submit "shard${s}-val" --host "$(aport $lp $s)" >/dev/null 2>&1
  ref=""; ok=1
  for p in $(seq 1 $N); do
    log=$(docker exec "lsr$p" "$ADM" status --host "$(aport $p $s)" 2>/dev/null | head -1 | sed -n 's/.*log=//p')
    [ -z "$ref" ] && ref="$log"; [ "$log" = "$ref" ] || ok=0
  done
  [ "$ok" = 1 ] && [ -n "$ref" ] && echo "  shard $s: all $N containers committed == $ref" \
    || { echo "FAIL: shard $s logs disagree across containers"; exit 1; }
done

echo "--- HA: kill 1 container (minority of $N); surviving quorum must keep each shard ---"
docker rm -f lsr1 >/dev/null 2>&1; echo "  killed lsr1"; sleep 2
for s in $(seq 0 $((M-1))); do
  ldr=""; for _ in $(seq 1 50); do
    for p in 2 3; do docker exec "lsr$p" "$ADM" status --host "$(aport $p $s)" 2>/dev/null | grep -q "role=2" && { ldr="lsr$p"; break; }; done
    [ -n "$ldr" ] && break; sleep 0.4
  done
  [ -z "$ldr" ] && { echo "FAIL: shard $s did not re-elect after losing lsr1"; exit 1; }
  lp=${ldr#lsr}
  docker exec "$ldr" "$ADM" submit "shard${s}-after" --host "$(aport $lp $s)" >/dev/null 2>&1
  log=$(docker exec "$ldr" "$ADM" status --host "$(aport $lp $s)" 2>/dev/null | head -1 | sed -n 's/.*log=//p')
  echo "$log" | grep -q "shard${s}-after" && echo "  shard $s: re-elected $ldr + committed after losing a node" \
    || { echo "FAIL: shard $s could not commit after fault"; exit 1; }
done

echo "### CROSS-MACHINE REPLICATED SHARDS PASS — $M shards each HA across $N containers ###"
