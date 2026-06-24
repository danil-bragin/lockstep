#!/usr/bin/env bash
# ycsb.sh — unified YCSB adapter for the 4 competitors (postgres, etcd, cockroach, tikv).
#
# ONE workload generator (go-ycsb in the lockstep-ycsb:latest image) drives every system
# via its native binding, so the workload is byte-identical across systems. The SERVER
# carries the BENCH_CPUS/BENCH_MEM pin (from this adapter); the go-ycsb CLIENT is left with
# host headroom so the server is the bottleneck under test.
#
# Invocation (all via env, single command):
#   SYSTEM=postgres|etcd|cockroach|tikv \
#   WORKLOAD=write|rw5050|read \
#   CPUS=4 MEM_G=6 CONCURRENCY=64 VALUE_BYTES=16 OP_COUNT=100000 \
#   RECORD_COUNT=100000 PASS=0 \
#   OUT=/abs/path/to/cell.json \
#   adapters/ycsb.sh
#
# RECORD_COUNT defaults to OP_COUNT if unset (keyspace == measured op budget).
#
# Output: a SCHEMA-conformant JSON object at $OUT. On any bring-up / parse failure:
#   ok=false, throughput_ops_s=null, a notes reason. NEVER fabricates a number.
#
# Always tears the target service down (compose down -v) on EVERY exit path.

set -u

ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPARE_DIR="$(cd "${ADAPTER_DIR}/.." && pwd)"
WORKLOADS_DIR="${COMPARE_DIR}/workloads"
COMPOSE="docker compose -f ${COMPARE_DIR}/docker-compose.yml"

SYSTEM="${SYSTEM:?SYSTEM required: postgres|etcd|cockroach|tikv}"
WORKLOAD="${WORKLOAD:?WORKLOAD required: write|rw5050|read}"
CPUS="${CPUS:-4}"
MEM_G="${MEM_G:-6}"
CONCURRENCY="${CONCURRENCY:-64}"
VALUE_BYTES="${VALUE_BYTES:-16}"
OP_COUNT="${OP_COUNT:-100000}"
RECORD_COUNT="${RECORD_COUNT:-$OP_COUNT}"
PASS="${PASS:-0}"
OUT="${OUT:?OUT (output json path) required}"

WL_FILE="${WORKLOADS_DIR}/${WORKLOAD}"
if [[ ! -f "$WL_FILE" ]]; then
  echo "no such workload file: $WL_FILE" >&2
  exit 2
fi

CELL_ID="${SYSTEM}__kv__${WORKLOAD}__cpus${CPUS}__nodes1__shards1__conc${CONCURRENCY}__p${PASS}"
NET="compare_default"          # compose project 'compare' default network
CLIENT_NAME="ycsbcli_${SYSTEM}_${WORKLOAD}_p${PASS}_$$"   # deterministic client name to sweep
RUN_LOG="$(mktemp)"
LOAD_LOG="$(mktemp)"

# Cell-scalar fields consumed by emit_json.py via env.
export CELL_ID SYSTEM WORKLOAD CPUS MEM_G CONCURRENCY VALUE_BYTES OP_COUNT PASS
export VECTOR="kv" NODES=1 SHARDS=1

emit_json() {  # ok tput p50 p99 raw notes
  python3 "${ADAPTER_DIR}/emit_json.py" "$1" "$2" "$3" "$4" "$5" "$6" > "$OUT"
}

teardown() {
  # Force-remove the ycsb client first (a --rm container can linger if the network is torn
  # out from under it mid-retry), then bring the service stack down + drop its volumes.
  docker rm -f "$CLIENT_NAME" >/dev/null 2>&1 || true
  $COMPOSE down -v --remove-orphans >/dev/null 2>&1 || true
}

fail() {
  local reason="$1" raw=""
  [[ -s "$RUN_LOG"  ]] && raw="$(cat "$RUN_LOG")"
  [[ -z "$raw" && -s "$LOAD_LOG" ]] && raw="$(cat "$LOAD_LOG")"
  emit_json "false" "null" "null" "null" "$raw" "$reason"
  teardown; rm -f "$RUN_LOG" "$LOAD_LOG"
  echo "FAIL [$CELL_ID]: $reason" >&2
  exit 0
}

trap 'teardown; rm -f "$RUN_LOG" "$LOAD_LOG"' EXIT

export BENCH_CPUS="$CPUS" BENCH_MEM="${MEM_G}g"
teardown    # ensure no stale containers/volumes from a prior cell

case "$SYSTEM" in
  postgres)   SVCS="postgres" ;;
  etcd)       SVCS="etcd" ;;
  cockroach)  SVCS="cockroach" ;;
  tikv)       SVCS="pd tikv" ;;
  *) echo "unknown SYSTEM: $SYSTEM" >&2; exit 2 ;;
esac

if ! $COMPOSE up -d $SVCS >/dev/null 2>&1; then
  fail "compose up failed for: $SVCS"
fi

wait_ready() {
  local i stores
  case "$SYSTEM" in
    postgres)
      for i in $(seq 1 60); do
        $COMPOSE exec -T postgres pg_isready -U bench -d bench >/dev/null 2>&1 && return 0
        sleep 1
      done ;;
    etcd)
      for i in $(seq 1 60); do
        $COMPOSE exec -T etcd etcdctl --endpoints=http://127.0.0.1:2379 endpoint health >/dev/null 2>&1 && return 0
        sleep 1
      done ;;
    cockroach)
      for i in $(seq 1 90); do
        $COMPOSE exec -T cockroach ./cockroach sql --insecure --host=127.0.0.1:26257 -e "SELECT 1" >/dev/null 2>&1 && return 0
        sleep 1
      done ;;
    tikv)
      # PD up first, then TiKV must register as an "Up" store before the bench can run.
      for i in $(seq 1 90); do
        stores="$($COMPOSE exec -T pd /pd-ctl -u http://127.0.0.1:2379 store 2>/dev/null)"
        echo "$stores" | tr -d ' \n' | grep -q '"state_name":"Up"' && return 0
        sleep 1
      done ;;
  esac
  return 1
}

if ! wait_ready; then
  fail "$SYSTEM did not become ready within the readiness window"
fi

# Per-system go-ycsb binding + connection props. The client container joins the bench net
# and dials the service by its compose name (in-network ports, NOT the host-published ones).
DB=""
declare -a CONN_P=()
case "$SYSTEM" in
  postgres)
    DB="postgresql"
    CONN_P=( -p "pg.host=postgres" -p "pg.port=5432"
             -p "pg.user=bench" -p "pg.password=bench" -p "pg.db=bench"
             -p "pg.sslmode=disable" ) ;;
  cockroach)
    DB="postgresql"   # cockroach speaks the pg wire => same binding, port 26257, user root
    CONN_P=( -p "pg.host=cockroach" -p "pg.port=26257"
             -p "pg.user=root" -p "pg.password="
             -p "pg.db=defaultdb" -p "pg.sslmode=disable" ) ;;
  etcd)
    DB="etcd"
    CONN_P=( -p "etcd.endpoints=etcd:2379" ) ;;
  tikv)
    DB="tikv"
    CONN_P=( -p "tikv.pd=pd:2379" -p "tikv.type=raw" ) ;;
esac

COMMON_P=(
  -p "workload=core"
  -p "recordcount=${RECORD_COUNT}"
  -p "fieldcount=1"
  -p "fieldlength=${VALUE_BYTES}"
  -p "fieldlengthdistribution=constant"
  -p "threadcount=${CONCURRENCY}"
  -p "requestdistribution=uniform"
  -p "measurementtype=histogram"
)

ycsb() {
  docker run --rm --name "$CLIENT_NAME" \
    --network "$NET" \
    -v "${WORKLOADS_DIR}:/workloads:ro" \
    lockstep-ycsb:latest \
    go-ycsb "$@"
}

# LOAD (populate). For SQL the pg binding auto-creates the usertable schema on load.
if ! ycsb load "$DB" -P "/workloads/${WORKLOAD}" "${COMMON_P[@]}" "${CONN_P[@]}" \
       > "$LOAD_LOG" 2>&1; then
  fail "go-ycsb load failed (see raw). $(tail -3 "$LOAD_LOG" | tr '\n' ' ')"
fi

# RUN (measured). operationcount == OP_COUNT.
if ! ycsb run "$DB" -P "/workloads/${WORKLOAD}" "${COMMON_P[@]}" \
       -p "operationcount=${OP_COUNT}" "${CONN_P[@]}" \
       > "$RUN_LOG" 2>&1; then
  fail "go-ycsb run failed (see raw). $(tail -3 "$RUN_LOG" | tr '\n' ' ')"
fi

PARSED="$(python3 "${ADAPTER_DIR}/parse_ycsb.py" "$RUN_LOG")"
TPUT="$(printf '%s' "$PARSED" | cut -f1)"
P50="$(printf  '%s' "$PARSED" | cut -f2)"
P99="$(printf  '%s' "$PARSED" | cut -f3)"

RAW="$(grep -E '^[A-Z_]+[[:space:]]*-[[:space:]]*Takes' "$RUN_LOG" | tail -20)"
[[ -z "$RAW" ]] && RAW="$(tail -20 "$RUN_LOG")"

if [[ -z "$TPUT" || "$TPUT" == "null" ]]; then
  fail "could not parse a committed throughput from go-ycsb output"
fi

NOTES="go-ycsb ${DB} binding; workload=${WORKLOAD}; committed ops (load+run); server pinned cpus=${CPUS} mem=${MEM_G}g; client unpinned headroom"
emit_json "true" "$TPUT" "$P50" "$P99" "$RAW" "$NOTES"
echo "OK [$CELL_ID]: throughput=${TPUT} ops/s p50=${P50}us p99=${P99}us -> $OUT"
