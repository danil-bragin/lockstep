#!/usr/bin/env bash
# tpcc.sh — SQL-vector (vector=sql, workload=tpcc) adapter.
#
# Uses CockroachDB's built-in `cockroach workload` (TPC-C) as the ONE identical SQL txn
# generator. The cockroach binary's `--pgurl` drives BOTH CockroachDB AND PostgreSQL over
# the pg wire, so both SQL competitors run the same TPC-C client — the fairness story.
#
# Lockstep, etcd, tikv do NOT participate in the tpcc vector (handled elsewhere / are KV).
#
# Invocation (env):
#   SYSTEM=postgres|cockroach \
#   CPUS=4 MEM_G=6 WAREHOUSES=1 DURATION=10s CONCURRENCY=8 PASS=0 \
#   OUT=/abs/path/cell.json \
#   adapters/tpcc.sh
#
# Output: SCHEMA-conformant JSON (vector=sql, workload=tpcc). throughput_ops_s = committed
# txn-op/s (ops/sec(cum) of the __total row). On failure: ok=false, throughput null, note.
# Tears the service down on EVERY exit path.

set -u

ADAPTER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPARE_DIR="$(cd "${ADAPTER_DIR}/.." && pwd)"
COMPOSE="docker compose -f ${COMPARE_DIR}/docker-compose.yml"

SYSTEM="${SYSTEM:?SYSTEM required: postgres|cockroach}"
CPUS="${CPUS:-4}"
MEM_G="${MEM_G:-6}"
WAREHOUSES="${WAREHOUSES:-1}"
DURATION="${DURATION:-10s}"
CONCURRENCY="${CONCURRENCY:-8}"
PASS="${PASS:-0}"
OUT="${OUT:?OUT (output json path) required}"

CELL_ID="${SYSTEM}__sql__tpcc__cpus${CPUS}__nodes1__shards1__conc${CONCURRENCY}__p${PASS}"
NET="compare_default"
CLIENT_NAME="tpcccli_${SYSTEM}_p${PASS}_$$"
INIT_LOG="$(mktemp)"
RUN_LOG="$(mktemp)"

export CELL_ID SYSTEM CPUS MEM_G CONCURRENCY PASS
export VECTOR="sql" WORKLOAD="tpcc" NODES=1 SHARDS=1 VALUE_BYTES=0 OP_COUNT=0

emit_json() { python3 "${ADAPTER_DIR}/emit_json.py" "$1" "$2" "$3" "$4" "$5" "$6" > "$OUT"; }
teardown()  {
  docker rm -f "${CLIENT_NAME}_init" "${CLIENT_NAME}_run" >/dev/null 2>&1 || true
  $COMPOSE down -v --remove-orphans >/dev/null 2>&1 || true
}

fail() {
  local reason="$1" raw=""
  [[ -s "$RUN_LOG"  ]] && raw="$(cat "$RUN_LOG")"
  [[ -z "$raw" && -s "$INIT_LOG" ]] && raw="$(cat "$INIT_LOG")"
  emit_json "false" "null" "null" "null" "$raw" "$reason"
  teardown; rm -f "$INIT_LOG" "$RUN_LOG"
  echo "FAIL [$CELL_ID]: $reason" >&2
  exit 0
}
trap 'teardown; rm -f "$INIT_LOG" "$RUN_LOG"' EXIT

export BENCH_CPUS="$CPUS" BENCH_MEM="${MEM_G}g"
teardown

# cockroach-workload requires the pgurl to point at a database literally named "tpcc"
# (it asserts the db name == workload name). cockroach `workload init` creates that db
# itself; for postgres we pre-CREATE it after readiness.
case "$SYSTEM" in
  postgres)  SVC="postgres" ;  PGURL="postgresql://bench:bench@postgres:5432/tpcc?sslmode=disable" ;;
  cockroach) SVC="cockroach" ; PGURL="postgresql://root@cockroach:26257/tpcc?sslmode=disable" ;;
  *) echo "tpcc only supports postgres|cockroach, got $SYSTEM" >&2; exit 2 ;;
esac

$COMPOSE up -d $SVC >/dev/null 2>&1 || fail "compose up failed for $SVC"

ready=0
if [[ "$SYSTEM" == "postgres" ]]; then
  for i in $(seq 1 60); do
    $COMPOSE exec -T postgres pg_isready -U bench -d bench >/dev/null 2>&1 && { ready=1; break; }
    sleep 1
  done
else
  for i in $(seq 1 90); do
    $COMPOSE exec -T cockroach ./cockroach sql --insecure --host=127.0.0.1:26257 -e "SELECT 1" >/dev/null 2>&1 && { ready=1; break; }
    sleep 1
  done
fi
[[ "$ready" == "1" ]] || fail "$SYSTEM not ready in time"

# postgres: the target "tpcc" database must exist before cockroach-workload init connects.
# (cockroach creates its own db during `workload init`.)
if [[ "$SYSTEM" == "postgres" ]]; then
  $COMPOSE exec -T postgres psql -U bench -d bench -tAc \
    "SELECT 1 FROM pg_database WHERE datname='tpcc'" 2>/dev/null | grep -q 1 \
    || $COMPOSE exec -T postgres psql -U bench -d bench -c "CREATE DATABASE tpcc" >/dev/null 2>&1 \
    || fail "could not create the tpcc database on postgres"
fi

# $1 = phase tag (init|run) so the two client containers get distinct names; the rest are
# cockroach args. teardown sweeps "${CLIENT_NAME}*"-style by removing both explicitly below.
crdb() {
  local phase="$1"; shift
  docker run --rm --name "${CLIENT_NAME}_${phase}" --network "$NET" --entrypoint ./cockroach \
    cockroachdb/cockroach:v24.1.5 "$@"
}

# NOTE on postgres: cockroach-workload's tpcc schema DDL contains CockroachDB-specific
# syntax (inline INDEX / column families / NOT VISIBLE) that vanilla PostgreSQL rejects
# ("syntax error at or near NOT"). This is an upstream client<->server DDL incompatibility,
# NOT a fault in this harness. cockroach-as-target works; postgres-tpcc fails honestly with
# the exact pq error captured in `raw`/`notes` (ok=false) rather than fabricating a number.
if ! crdb init workload init tpcc --warehouses "$WAREHOUSES" "$PGURL" > "$INIT_LOG" 2>&1; then
  hint=""
  [[ "$SYSTEM" == "postgres" ]] && hint=" (KNOWN: cockroach-workload tpcc DDL uses CRDB-specific syntax vanilla postgres cannot parse; postgres tpcc needs a hand-rolled pg schema — out of scope for this adapter)"
  fail "tpcc init failed${hint}. $(tail -3 "$INIT_LOG" | tr '\n' ' ')"
fi

# --wait=0 decouples worker count from warehouse count (the default --wait>0 enforces the
# TPC-C 10-workers-per-warehouse think-time rule). For a throughput-focused bench we drive
# unthrottled, so --wait=0 lets CONCURRENCY be the client knob, matched across systems.
if ! crdb run workload run tpcc --warehouses "$WAREHOUSES" --duration "$DURATION" \
        --workers "$CONCURRENCY" --wait=0 --tolerate-errors "$PGURL" > "$RUN_LOG" 2>&1; then
  grep -qE '__result|tpmC|__total' "$RUN_LOG" || fail "tpcc run failed. $(tail -3 "$RUN_LOG" | tr '\n' ' ')"
fi

PARSED="$(python3 "${ADAPTER_DIR}/parse_tpcc.py" "$RUN_LOG")"
TPUT="$(printf '%s' "$PARSED" | cut -f1)"
P50="$(printf  '%s' "$PARSED" | cut -f2)"
P99="$(printf  '%s' "$PARSED" | cut -f3)"

RAW="$(grep -E '__result|__total|tpmC|Audit check' "$RUN_LOG" | tail -20)"
[[ -z "$RAW" ]] && RAW="$(tail -20 "$RUN_LOG")"

if [[ -z "$TPUT" || "$TPUT" == "null" ]]; then
  fail "could not parse committed throughput from cockroach workload tpcc output"
fi

NOTES="cockroach-workload tpcc (warehouses=${WAREHOUSES}, dur=${DURATION}, wait=0) driving ${SYSTEM} over pg-wire; ops/sec(cum) of the __result aggregate = committed txn-op/s; p50/p99 ms->us; server pinned cpus=${CPUS} mem=${MEM_G}g"
emit_json "true" "$TPUT" "$P50" "$P99" "$RAW" "$NOTES"
echo "OK [$CELL_ID]: tput=${TPUT} ops/s p50=${P50}us p99=${P99}us -> $OUT"
