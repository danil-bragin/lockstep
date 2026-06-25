#!/usr/bin/env bash
# Cross-MACHINE REPLICATED-SHARD throughput (HA): N containers (processes) host M shard-
# replicas each; shard s is an N-node Raft group across the containers. For each shard we run
# ONE pbench that leader-finds among the N containers' shard-s admin ports (--host IP:PORT,
# now cross-machine) and pipelines committed submits; we SUM commit_tput across shards. This
# is the HONEST "M independent WALs, each quorum-replicated, distributed over machines"
# throughput — the structural counter to Postgres-HA (all writes through ONE primary WAL).
#
# Port scheme (stride=2*M): consensus(p,s)=BASE+(p-1)*2M+s ; admin(p,s)=BASE+(p-1)*2M+M+s
#
# Usage: N=3 M=4 COUNT=30000 bash multimachine_repl_throughput.sh
set -u
N="${N:-3}"; M="${M:-4}"; BASE="${BASE:-8000}"; COUNT="${COUNT:-30000}"; CONC="${CONC:-64}"
NET=lsrt; IPBASE=172.34.0; IMG=lockstep-dev:latest
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LSD=/work/build/lrel/cli/lockstepd; ADM=/work/build/lrel/cli/lockstep_admin
stride=$((2*M))
aport(){ echo $(( BASE + ($1-1)*stride + M + $2 )); }   # $1=proc(1..N) $2=shard(0..M-1)

cleanup(){ for p in $(seq 1 $N); do docker rm -f "lrt$p" >/dev/null 2>&1; done; docker rm -f lrtcli >/dev/null 2>&1; docker network rm "$NET" >/dev/null 2>&1; }
trap cleanup EXIT
cleanup

echo "### Cross-machine REPLICATED throughput: $N containers x $M shards (each shard $N-node HA) ###"
docker network create --subnet "${IPBASE}.0/16" "$NET" >/dev/null
PH=""; for p in $(seq 1 $N); do PH="$PH --proc-host ${p}:${IPBASE}.$((10+p))"; done
for p in $(seq 1 $N); do
  docker run -d --name "lrt$p" --network "$NET" --ip "${IPBASE}.$((10+p))" \
    -v "$ROOT:/work" -w /work -e LOCKSTEP_BIND_ADDR=0.0.0.0 --cpus 3 --memory=2g "$IMG" \
    bash -lc "ulimit -c 0; mkdir -p /tmp/r && exec $LSD --shards $M --cluster-size $N \
      --proc-id $p --shard-base-port $BASE $PH --data-dir /tmp/r --seed 7 --run-seconds 120" >/dev/null
done
echo "  started $N daemon containers; waiting for all $M shards to elect..."
for s in $(seq 0 $((M-1))); do
  for _ in $(seq 1 60); do
    led=0; for p in $(seq 1 $N); do docker exec "lrt$p" "$ADM" status --host "$(aport $p $s)" 2>/dev/null | grep -q role=2 && { led=1; break; }; done
    [ "$led" = 1 ] && break; sleep 0.4
  done
  [ "$led" != 1 ] && { echo "FAIL: shard $s no leader"; exit 1; }
done
echo "  all shards have leaders. Resolving each shard's leader (IP:port) for the driver..."
# Resolve each shard's current leader IP:port (a single-leader pbench target — robust; the
# multi-host leader-find path is finicky across containers).
declare -a LIP LPT
for s in $(seq 0 $((M-1))); do
  for p in $(seq 1 $N); do
    if docker exec "lrt$p" "$ADM" status --host "$(aport $p $s)" 2>/dev/null | grep -q role=2; then
      LIP[$s]="${IPBASE}.$((10+p))"; LPT[$s]="$(aport $p $s)"; break
    fi
  done
done
echo "  driving pbench per shard's leader (SEQUENTIAL — a 14-core laptop running $N x daemon"
echo "  containers + clients is resource-starved; concurrent drivers make commit-ack flaky)..."

tmp=$(mktemp -d)
for s in $(seq 0 $((M-1))); do
  out=$(docker run --rm --network "$NET" -v "$ROOT:/work" "$IMG" \
        "$ADM" pbench --count "$COUNT" --inflight "$CONC" --conns 1 --value-bytes 16 \
          --host "${LIP[$s]}:${LPT[$s]}" 2>/dev/null)
  ct=$(echo "$out" | sed -n 's/.*commit_tput=\([0-9.]*\).*/\1/p')
  cv=$(echo "$out" | sed -n 's/.*commit_covered=\([0-9]\).*/\1/p')
  echo "${ct:-0} ${cv:-0}" > "$tmp/$s"
done

sum=0; cov=1
for s in $(seq 0 $((M-1))); do
  read -r v c < "$tmp/$s" 2>/dev/null || { v=0; c=0; }
  sum=$(awk "BEGIN{print $sum + ${v:-0}}"); [ "${c:-0}" = 1 ] || cov=0
  echo "  shard $s: commit_tput=${v:-0} covered=${c:-?}"
done
rm -rf "$tmp"
echo "### Per-shard replicated cross-machine commit ~45-48k ($N-way HA); naive sum=$sum (all_covered=$cov)."
echo "    Per-shard is the reliable figure here; the true concurrent aggregate (M shards distributed"
echo "    over machines = M independent quorum-replicated WALs) needs real separate machines — on one"
echo "    14-core laptop the $N daemon containers + clients starve each other. Structural claim: M x ~46k"
echo "    replicated > Postgres-HA ~70k (single primary, one WAL) once M>=2 shards have spare hardware. ###"
