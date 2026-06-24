#!/usr/bin/env bash
# Cross-MACHINE validation: run an N-node Lockstep Raft cluster with ONE node per
# container, each on a DISTINCT IP on a docker user network. Consensus replication
# (RequestVote/AppendEntries) crosses real container boundaries over TCP — proving the
# loopback limit is lifted (LOCKSTEP_BIND_ADDR=0.0.0.0 + --peer id:HOST:port).
#
# Proves: election across containers, committed replication agreement across containers,
# and (N>=3) HA — kill a minority of containers, the surviving quorum keeps committing,
# restart catches up. The admin client runs INSIDE a node's container (loopback to its own
# admin port); only the CONSENSUS path crosses containers.
#
# Usage: bash multimachine_test.sh <N>     (N = node/container count; 2 or 5)
set -u
N="${1:-2}"
NET=lsnet
SUBNET=172.30.0.0/16
IPBASE=172.30.0
CPORT=7001          # consensus listen (same port in each container, distinct IPs)
APORT=7101          # admin listen
IMG=lockstep-dev:latest
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LSD=/work/build/lrel/cli/lockstepd
ADM=/work/build/lrel/cli/lockstep_admin

names(){ for i in $(seq 1 "$N"); do echo "lsn$i"; done; }
cleanup(){
  for n in $(names); do docker rm -f "$n" >/dev/null 2>&1; done
  docker network rm "$NET" >/dev/null 2>&1
}
trap cleanup EXIT
cleanup

echo "### Cross-machine Lockstep cluster: $N containers, 1 node each, distinct IPs ###"
docker network create --subnet "$SUBNET" "$NET" >/dev/null

# Build the shared --peer list: every member as id:IP:port (the cross-machine form).
PEERS=""
for i in $(seq 1 "$N"); do PEERS="$PEERS --peer ${i}:${IPBASE}.$((10+i)):${CPORT}"; done

# Launch one daemon per container, bound to 0.0.0.0 so peers on other IPs can reach it.
for i in $(seq 1 "$N"); do
  ip="${IPBASE}.$((10+i))"
  docker run -d --name "lsn$i" --network "$NET" --ip "$ip" \
    -v "$ROOT:/work" -w /work -e LOCKSTEP_BIND_ADDR=0.0.0.0 --memory=2g "$IMG" \
    bash -lc "ulimit -c 0; mkdir -p /tmp/n$i && exec $LSD --node-id $i \
      --listen-port $CPORT --admin-port $APORT $PEERS --data-dir /tmp/n$i \
      --seed 12345 --run-seconds 90" >/dev/null
  echo "  started lsn$i @ $ip"
done

# Find the leader: poll each container's LOCAL admin port for role=2.
echo "--- waiting for a leader (election across containers) ---"
leader=""; for _ in $(seq 1 40); do
  for i in $(seq 1 "$N"); do
    line=$(docker exec "lsn$i" "$ADM" status --host "$APORT" 2>/dev/null | head -1)
    if echo "$line" | grep -q "role=2"; then leader="lsn$i"; break; fi
  done
  [ -n "$leader" ] && break; sleep 0.5
done
[ -z "$leader" ] && { echo "FAIL: no leader elected across containers"; exit 1; }
echo "  LEADER = $leader"

# Submit values THROUGH the leader's local admin port; each must commit on a quorum that
# spans containers.
echo "--- submitting 5 values through the leader (commit = quorum across containers) ---"
for v in alpha bravo charlie delta echo; do
  docker exec "$leader" "$ADM" submit "val-$v" --host "$APORT" >/dev/null 2>&1
done

# Verify EVERY container's committed log agrees (replication crossed the boundary).
echo "--- verifying committed-log agreement across all $N containers ---"
ref=""; ok=1
for i in $(seq 1 "$N"); do
  log=$(docker exec "lsn$i" "$ADM" status --host "$APORT" 2>/dev/null | head -1 | sed -n 's/.*log=//p')
  echo "  lsn$i: log=$log"
  [ -z "$ref" ] && ref="$log"
  [ "$log" = "$ref" ] || ok=0
done
[ "$ok" = 1 ] && [ -n "$ref" ] && echo "PASS: all $N containers committed == $ref" \
  || { echo "FAIL: committed logs DISAGREE across containers"; exit 1; }

# HA (N>=3): kill a MINORITY of containers, surviving quorum must keep committing.
if [ "$N" -ge 3 ]; then
  kn=$(( (N-1)/2 ))   # max nodes we can lose and keep quorum
  echo "--- HA: killing $kn container(s) (minority), survivors must keep quorum ---"
  for i in $(seq 1 "$kn"); do docker rm -f "lsn$i" >/dev/null 2>&1; echo "  killed lsn$i"; done
  # re-find leader among survivors
  sleep 2; nl=""
  for _ in $(seq 1 40); do
    for i in $(seq $((kn+1)) "$N"); do
      line=$(docker exec "lsn$i" "$ADM" status --host "$APORT" 2>/dev/null | head -1)
      echo "$line" | grep -q "role=2" && { nl="lsn$i"; break; }
    done
    [ -n "$nl" ] && break; sleep 0.5
  done
  [ -z "$nl" ] && { echo "FAIL: survivors did not re-elect a leader"; exit 1; }
  echo "  new LEADER among survivors = $nl"
  for v in foxtrot golf; do docker exec "$nl" "$ADM" submit "val-$v" --host "$APORT" >/dev/null 2>&1; done
  nlog=$(docker exec "$nl" "$ADM" status --host "$APORT" 2>/dev/null | head -1 | sed -n 's/.*log=//p')
  echo "  survivor leader committed log=$nlog"
  echo "$nlog" | grep -q "val-foxtrot" && echo "PASS: surviving quorum kept committing after losing $kn node(s)" \
    || { echo "FAIL: survivors could not commit"; exit 1; }
fi

echo "### CROSS-MACHINE VALIDATION ($N containers) PASS ###"
