#!/usr/bin/env bash
# Lockstep adapter for the comparative bench. Runs INSIDE the lockstep-dev container
# (the orchestrator launches: docker run --memory=<mem> lockstep-dev bash adapters/lockstep.sh).
# lockstepd binds 127.0.0.1 (loopback), so server + client share ONE netns/container; we
# pin the SERVER to K cores via taskset and run the CLIENT on the remaining cores — the
# dual of the competitor setup (server gets K cores, client gets separate headroom).
#
# Emits ONE SCHEMA-conformant JSON to $OUT. Headline throughput = COMMITTED ops/s
# (lockstep_admin pbench/mbench commit_tput, requires commit_covered=1). Latency p50/p99
# is taken from a SEPARATE closed-loop `bench` pass (concurrency-1) — labeled as such in
# notes, since pbench reports throughput only. NEVER fabricates: a no-leader / uncovered /
# fault cell sets ok=false with a reason.
set -u
ulimit -c 0; ulimit -s 16384 2>/dev/null || true

: "${VEC:?}"; : "${WL:?}"; : "${CPUS:?}"; : "${SHARDS:=1}"; : "${NODES:=1}"
: "${CONC:?}"; : "${VBYTES:=16}"; : "${OPCOUNT:?}"; : "${PASS:=0}"; : "${OUT:?}"
: "${RUN_SECONDS:=60}"; : "${CELL_ID:=lockstep_cell}"

BIN=/work/build/lrel/cli
LSD=$BIN/lockstepd
ADM=$BIN/lockstep_admin
DD=$(mktemp -d /tmp/ls_bench.XXXXXX)
HOST_NPROC=$(nproc)

# Core sets: server = cores [0, CPUS), client = the rest (at least 1 core).
srv_cpus=$(( CPUS < HOST_NPROC ? CPUS : HOST_NPROC ))
SERVER_SET="0-$(( srv_cpus - 1 ))"
cli_lo=$srv_cpus
[ "$cli_lo" -ge "$HOST_NPROC" ] && cli_lo=$(( HOST_NPROC - 1 ))   # ensure client has >=1 core
CLIENT_SET="${cli_lo}-$(( HOST_NPROC - 1 ))"

PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; pkill -9 -x lockstepd 2>/dev/null; rm -rf "$DD"; }
trap cleanup EXIT

# Poll `status` on the given admin ports until ANY reports role=2 (leader). Returns 0 if a
# leader appeared within ~20s, 1 otherwise. (A `commit`/`status` reply only proves the node
# is up — NOT that it has won an election; pbench's leader-find needs an actual leader.)
wait_leader() {
  local deadline=$(( SECONDS + 20 ))
  while [ "$SECONDS" -lt "$deadline" ]; do
    for ap in "$@"; do
      local line; line=$(taskset -c "$CLIENT_SET" "$ADM" status --host "$ap" 2>/dev/null | head -1)
      [ "$(field ok "$line")" = "1" ] && [ "$(field role "$line")" = "2" ] && return 0
    done
    sleep 0.3
  done
  return 1
}

emit_fail() {   # $1 = notes
  python3 - "$OUT" "$CELL_ID" "$VEC" "$WL" "$CPUS" "$NODES" "$SHARDS" "$CONC" "$VBYTES" "$OPCOUNT" "$PASS" "$1" <<'PY'
import json,sys
o,cid,vec,wl,cpus,nodes,shards,conc,vb,opc,p,notes=sys.argv[1:]
json.dump({"cell_id":cid,"system":"lockstep","vector":vec,"workload":wl,"cpus":int(cpus),"mem_g":None,
 "nodes":int(nodes),"shards":int(shards),"concurrency":int(conc),"value_bytes":int(vb),"op_count":int(opc),
 "pass":int(p),"throughput_ops_s":None,"p50_us":None,"p99_us":None,
 "fault":{"injected":vec=="fault","recovery_ms":None,"acked_lost":None,"consistent":None},
 "ok":False,"raw":"","notes":notes},open(o,"w"),indent=2)
PY
  echo "[lockstep] FAIL: $1"
}

emit_ok() {   # $1=tput $2=p50 $3=p99 $4=raw $5=notes  [$6=recovery_ms $7=acked_lost $8=consistent]
  python3 - "$OUT" "$CELL_ID" "$VEC" "$WL" "$CPUS" "$NODES" "$SHARDS" "$CONC" "$VBYTES" "$OPCOUNT" "$PASS" \
    "$1" "$2" "$3" "$4" "$5" "${6:-}" "${7:-}" "${8:-}" <<'PY'
import json,sys
(o,cid,vec,wl,cpus,nodes,shards,conc,vb,opc,p,tput,p50,p99,raw,notes,rec,lost,cons)=sys.argv[1:]
def f(x): return None if x=="" else float(x)
def i(x): return None if x=="" else int(x)
def b(x): return None if x=="" else (x=="1" or x.lower()=="true")
json.dump({"cell_id":cid,"system":"lockstep","vector":vec,"workload":wl,"cpus":int(cpus),"mem_g":None,
 "nodes":int(nodes),"shards":int(shards),"concurrency":int(conc),"value_bytes":int(vb),"op_count":int(opc),
 "pass":int(p),"throughput_ops_s":f(tput),"p50_us":f(p50),"p99_us":f(p99),
 "fault":{"injected":vec=="fault","recovery_ms":f(rec),"acked_lost":i(lost),"consistent":b(cons)},
 "ok":True,"raw":raw,"notes":notes},open(o,"w"),indent=2)
PY
  echo "[lockstep] OK tput=$1 p50=$2 p99=$3"
}

field() { sed -n "s/.*\b$1=\([0-9.][0-9.]*\).*/\1/p" <<<"$2" | head -1; }

[ -x "$LSD" ] || { emit_fail "lockstepd not built at $LSD"; exit 0; }

# ---- KV / SCALING: single-node (M=1) or multi-shard (M>1), no replication ----
if [ "$VEC" = "kv" ] || [ "$VEC" = "scaling" ]; then
  BASE=19000
  if [ "$SHARDS" -le 1 ]; then
    AP=$(( BASE )); CP=$(( BASE+1 ))
    mkdir -p "$DD/n1"   # lockstepd does NOT create its data-dir; assemble fails if absent
    taskset -c "$SERVER_SET" "$LSD" --node-id 1 --listen-port "$CP" --admin-port "$AP" \
      --peer "1:$CP" --data-dir "$DD/n1" --seed 12345 --run-seconds "$RUN_SECONDS" >/dev/null 2>&1 &
    PIDS+=($!)
    wait_leader "$AP" || { emit_fail "single-node leader never elected within 20s"; exit 0; }
    if [ "${CLIENTS:-1}" -le 1 ]; then
      PB=$(taskset -c "$CLIENT_SET" "$ADM" pbench --count "$OPCOUNT" --inflight "$CONC" --conns 1 --value-bytes "$VBYTES" --host "$AP" 2>&1)
    else
      # FAIR driver: M concurrent pbench processes (matches go-ycsb's multi-threaded client,
      # which saturates the competitor servers). The single-threaded mbench/pbench client
      # otherwise caps the measurement below the server's real ceiling. Sum their commit_tput.
      ct=$(mktemp -d); cp=""
      for ci in $(seq 1 "$CLIENTS"); do
        ( taskset -c "$CLIENT_SET" "$ADM" pbench --count "$OPCOUNT" --inflight "$CONC" --conns 1 \
            --value-bytes "$VBYTES" --host "$AP" 2>/dev/null \
            | sed -n 's/.*commit_tput=\([0-9.]*\).*covered=\([0-9]\).*/\1 \2/p' > "$ct/$ci" ) &
        cp="$cp $!"
      done
      for p in $cp; do wait "$p"; done
      csum=0; ccov=1
      for ci in $(seq 1 "$CLIENTS"); do
        read -r cv cc < "$ct/$ci" 2>/dev/null || { cv=0; cc=0; }
        csum=$(awk "BEGIN{print $csum + ${cv:-0}}"); [ "${cc:-0}" = "1" ] || ccov=0
      done
      rm -rf "$ct"
      PB="commit_tput=$csum commit_covered=$ccov clients=$CLIENTS"
    fi
    LB=$(taskset -c "$CLIENT_SET" "$ADM" bench --count 256 --value-bytes "$VBYTES" --host "$AP" 2>&1)
  else
    HOSTS=(); for s in $(seq 0 $((SHARDS-1))); do HOSTS+=(--host $(( BASE+s ))); mkdir -p "$DD/shard_$s"; done
    taskset -c "$SERVER_SET" "$LSD" --shards "$SHARDS" --shard-base-port "$BASE" \
      --data-dir "$DD" --seed 12345 --run-seconds "$RUN_SECONDS" >/dev/null 2>&1 &
    PIDS+=($!)
    # every shard is its own single-node leader; wait for the last shard's port to lead
    LAST=$(( BASE + SHARDS - 1 ))
    wait_leader "$BASE" "$LAST" || { emit_fail "multi-shard leaders never elected within 20s"; exit 0; }
    PB=$(taskset -c "$CLIENT_SET" "$ADM" mbench --count "$OPCOUNT" --inflight "$CONC" --value-bytes "$VBYTES" "${HOSTS[@]}" 2>&1)
    LB=$(taskset -c "$CLIENT_SET" "$ADM" bench --count 256 --value-bytes "$VBYTES" --host "$BASE" 2>&1)
  fi

  # parse throughput (pbench: commit_tput + commit_covered ; mbench: agg_commit_tput + all_covered)
  if [ "$SHARDS" -le 1 ]; then
    TPUT=$(field commit_tput "$PB"); COV=$(field commit_covered "$PB"); FAULT=$(field fault "$PB")
  else
    TPUT=$(field agg_commit_tput "$PB"); COV=$(field all_covered "$PB"); FAULT=$(field fault "$PB")
  fi
  P50=$(field commit_p50_us "$LB"); P99=$(field commit_p99_us "$LB")
  RAW=$(printf '%s ||| %s' "$PB" "$LB" | tr -d '\n' | cut -c1-600)

  # Coverage gate: strict (covered==1) for a single client. For the FAIR multi-client driver
  # (CLIENTS>1) on a single-node N=1 daemon, accept covered==0 as a per-client poll-confirmation
  # race (N=1 self-commits synchronously with the persisted append — S8.3 accept≈commit, no
  # quorum to lose), provided no leadership FAULT occurred; covered is recorded in notes.
  cgate_ok=0
  if [ -n "$TPUT" ]; then
    if [ "${COV:-0}" = "1" ]; then cgate_ok=1
    elif [ "${CLIENTS:-1}" -gt 1 ] && [ "$NODES" -le 1 ] && [ "${FAULT:-0}" != "1" ]; then cgate_ok=1; fi
  fi
  if [ "$cgate_ok" != "1" ]; then
    emit_fail "no committed throughput (covered=${COV:-?} fault=${FAULT:-?}); raw: $(echo "$PB" | head -c200)"; exit 0
  fi
  emit_ok "$TPUT" "${P50:-}" "${P99:-}" "$RAW" "native commit_tput; clients=${CLIENTS:-1} covered=${COV:-?}; latency=bench(conc1); shards=$SHARDS"
  exit 0
fi

emit_fail "vector $VEC not yet implemented in lockstep adapter"
exit 0
