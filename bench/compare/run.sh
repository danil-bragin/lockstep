#!/usr/bin/env bash
# Comparative-bench ORCHESTRATOR. Sweeps the cell matrix (system × vector × cpus × pass),
# dispatching each cell to the right adapter, pinning resources IDENTICALLY, writing one
# SCHEMA JSON per cell to results/. RESUMABLE: a cell whose results JSON already exists is
# skipped, so a killed run resumes where it left off. Bounded: one server up at a time;
# every adapter tears its server down on exit.
#
# Runs on the macOS HOST (bash 3.2). Lockstep cells run inside the lockstep-dev container
# (lockstepd is Linux-only); competitor cells run via the go-ycsb / tpcc adapters which
# docker-compose their target service. Host = 14 cores, 23.7 GiB.
#
# Usage:  bash bench/compare/run.sh [matrix]
#   matrix ∈ { kv, scaling, fault, sql, all }   (default: all)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COMPARE="$ROOT/bench/compare"
RES="$COMPARE/results"
mkdir -p "$RES"
MATRIX="${1:-all}"

# ---- standardized, identical-across-systems workload knobs ----------------------------
VALUE_BYTES="${VALUE_BYTES:-16}"
OP_COUNT="${OP_COUNT:-20000}"      # measured ops per cell (same for every system)
CONC="${CONC:-64}"                 # client concurrency (pbench inflight == go-ycsb threadcount)
PASSES="${PASSES:-3}"              # fresh repeats; report takes the MEDIAN
MEM_G="${MEM_G:-6}"
CPU_LEVELS="${CPU_LEVELS:-1 2 4 8}"   # the 1->N core sweep
KV_SYSTEMS="${KV_SYSTEMS:-lockstep lockstep_sharded postgres etcd cockroach tikv}"

ran=0; skipped=0; failed=0
log(){ printf '\n\033[1m[run]\033[0m %s\n' "$*"; }

# --- dispatch ONE lockstep cell (in the lockstep-dev container) ------------------------
run_lockstep(){  # $1=vec $2=cpus $3=shards $4=nodes $5=conc $6=opcount $7=pass $8=cell $9=out
  local vec=$1 cpus=$2 shards=$3 nodes=$4 conc=$5 oc=$6 pass=$7 cell=$8 out=$9
  docker run --rm --memory="${MEM_G}g" -v "$ROOT:/work" -w /work \
    -e VEC="$vec" -e WL=write -e CPUS="$cpus" -e SHARDS="$shards" -e NODES="$nodes" \
    -e CONC="$conc" -e VBYTES="$VALUE_BYTES" -e OPCOUNT="$oc" -e PASS="$pass" \
    -e RUN_SECONDS=120 -e CELL_ID="$cell" -e OUT="/work/bench/compare/results/$(basename "$out")" \
    lockstep-dev:latest bash -lc 'bash bench/compare/adapters/lockstep.sh' >/dev/null 2>&1
}

# --- dispatch ONE competitor KV cell (go-ycsb) -----------------------------------------
run_ycsb(){  # $1=system $2=workload $3=cpus $4=conc $5=opcount $6=pass $7=out
  SYSTEM="$1" WORKLOAD="$2" CPUS="$3" MEM_G="$MEM_G" CONCURRENCY="$4" \
    VALUE_BYTES="$VALUE_BYTES" OP_COUNT="$5" RECORD_COUNT="$5" PASS="$6" OUT="$7" \
    bash "$COMPARE/adapters/ycsb.sh" >/dev/null 2>&1
}

cell_done(){ [ -f "$1" ] && python3 -c "import json,sys;d=json.load(open('$1'));sys.exit(0 if d.get('ok') else 1)" 2>/dev/null; }

do_cell(){  # $1=label(echo) ; $2=outfile ; shift 2 ; rest = command
  local label=$1 out=$2; shift 2
  if cell_done "$out"; then echo "  skip  $label (done)"; skipped=$((skipped+1)); return; fi
  printf '  run   %s ... ' "$label"
  "$@"
  if cell_done "$out"; then echo "OK"; ran=$((ran+1));
  else echo "FAIL"; failed=$((failed+1)); fi
}

# ======================================================================================
# KV + SCALING: write workload, sweep cpus 1->N, all systems, PASSES repeats.
# lockstep        = single Raft group (apples-to-apples vs etcd)
# lockstep_sharded= M=cpus independent shards (Lockstep's horizontal lever)
# ======================================================================================
matrix_kv(){
  local cpus sys p
  for cpus in $CPU_LEVELS; do
    for sys in $KV_SYSTEMS; do
      for p in $(seq 0 $((PASSES-1))); do
        local cell="${sys}__kv__write__cpus${cpus}__conc${CONC}__p${p}"
        local out="$RES/${cell}.json"
        case "$sys" in
          lockstep)         do_cell "$cell" "$out" run_lockstep kv "$cpus" 1 1 "$CONC" "$OP_COUNT" "$p" "$cell" "$out" ;;
          lockstep_sharded) do_cell "$cell" "$out" run_lockstep scaling "$cpus" "$cpus" 1 "$CONC" "$OP_COUNT" "$p" "$cell" "$out" ;;
          *)                do_cell "$cell" "$out" run_ycsb "$sys" write "$cpus" "$CONC" "$OP_COUNT" "$p" "$out" ;;
        esac
      done
    done
  done
}

# rw5050 read/write mix at a single cpu level (read-path behavior)
matrix_rw(){
  local sys p cpus=4
  for sys in postgres etcd cockroach tikv; do
    for p in $(seq 0 $((PASSES-1))); do
      local cell="${sys}__kv__rw5050__cpus${cpus}__conc${CONC}__p${p}"
      local out="$RES/${cell}.json"
      do_cell "$cell" "$out" run_ycsb "$sys" rw5050 "$cpus" "$CONC" "$OP_COUNT" "$p" "$out"
    done
  done
}

# SQL TPC-C (cockroach drives both; postgres schema is CRDB-incompatible -> ok=false, noted)
matrix_sql(){
  local sys p cpus=4
  for sys in cockroach postgres; do
    for p in $(seq 0 $((PASSES-1))); do
      local cell="${sys}__sql__tpcc__cpus${cpus}__p${p}"
      local out="$RES/${cell}.json"
      do_cell "$cell" "$out" env SYSTEM="$sys" CPUS="$cpus" MEM_G="$MEM_G" WAREHOUSES=1 \
        DURATION=15s CONCURRENCY="$CONC" PASS="$p" OUT="$out" bash "$COMPARE/adapters/tpcc.sh"
    done
  done
}

log "matrix=$MATRIX  op_count=$OP_COUNT conc=$CONC passes=$PASSES cpus='$CPU_LEVELS'"
case "$MATRIX" in
  kv)       matrix_kv ;;
  rw)       matrix_rw ;;
  sql)      matrix_sql ;;
  fault)    echo "fault vector: run adapters/fault.sh separately (3-node leader-kill)";;
  all)      matrix_kv; matrix_rw; matrix_sql ;;
  *)        echo "unknown matrix '$MATRIX'"; exit 2 ;;
esac

log "DONE  ran=$ran skipped=$skipped failed=$failed  -> $RES"
