#!/usr/bin/env bash
# SQL-ANALYTICS comparison: Lockstep COLUMNAR engine vs PostgreSQL 16, identical deterministic
# data + identical analytical SQL, BOTH pinned to the SAME single CPU (cpu 0) for a fair
# single-core comparison. No secondary indexes on either side (raw analytical scan/aggregate).
#
# Usage: bench/compare/sql_analytics/run_analytics.sh [N_ROWS] [ITERS]
# Emits one JSON line per (system,query) + a comparison table. Requires docker (lockstep-dev
# image built + postgres:16 pullable).
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
N="${1:-200000}"
ITERS="${2:-50}"
TSHI=$(( N - N / 20 ))
PIN="0"   # both systems pinned to cpu 0
OUT=$(mktemp -d /tmp/sqlana.XXXXXX)

echo "== SQL analytics: Lockstep columnar vs Postgres | N=$N iters=$ITERS cpu=$PIN =="

# ---- Lockstep: build (if needed) + run pinned ----------------------------------------------
echo "-- building lockstep analytics bench --"
docker run --rm -v "$ROOT":/work -w /work lockstep-dev:latest bash -c \
  "clang++ -std=c++23 -O2 \$(for d in core storage txn consensus query harness providers/sim providers/prod; do echo -I \$d/include; done) \
   bench/compare/sql_analytics/lockstep_analytics.cpp -o /work/build/lrel/sql_analytics 2>&1 | tail -5"
echo "-- running lockstep (pinned cpu $PIN) --"
docker run --rm --cpuset-cpus="$PIN" -v "$ROOT":/work -w /work lockstep-dev:latest \
  taskset -c "$PIN" /work/build/lrel/sql_analytics "$N" "$ITERS" 2>/tmp/ls_err | tee "$OUT/lockstep.jsonl"
cat /tmp/ls_err

# ---- Postgres: container pinned to cpu 0, load + bench --------------------------------------
echo "-- starting postgres (pinned cpu $PIN) --"
docker rm -f pg_analytics >/dev/null 2>&1 || true
docker run -d --name pg_analytics --cpuset-cpus="$PIN" \
  -e POSTGRES_PASSWORD=bench -e POSTGRES_USER=bench -e POSTGRES_DB=bench \
  postgres:16 -c fsync=off -c synchronous_commit=off >/dev/null
echo "-- waiting for postgres --"
for i in $(seq 1 60); do
  if docker exec pg_analytics pg_isready -U bench >/dev/null 2>&1; then break; fi
  sleep 1
done
# Load the SAME deterministic data (formula matches lockstep_analytics.cpp). No secondary index.
docker exec pg_analytics psql -U bench -d bench -q -c "
  DROP TABLE IF EXISTS events;
  CREATE TABLE events (id INT PRIMARY KEY, uid INT, cat INT NOT NULL, amount INT NOT NULL, region TEXT NOT NULL, ts INT NOT NULL);
  INSERT INTO events SELECT i, i%10000, i%8, ((i::bigint*2654435761)%1000)::int,
    (ARRAY['north','south','east','west','central'])[(i%5)+1], i FROM generate_series(0,$N-1) i;
  ANALYZE events;" >/dev/null
echo "-- running postgres (EXPLAIN ANALYZE, warm, min of 5) --"
pgsql_for() {
  case "$1" in
    scan_agg)       echo "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM events";;
    groupby_cat)    echo "SELECT cat, COUNT(*), SUM(amount) FROM events GROUP BY cat";;
    groupby_region) echo "SELECT region, COUNT(*), SUM(amount) FROM events GROUP BY region";;
    filtered_agg)   echo "SELECT COUNT(*), SUM(amount) FROM events WHERE amount > 800";;
    zone_skip)      echo "SELECT COUNT(*), SUM(amount) FROM events WHERE ts > $TSHI";;
  esac
}
: > "$OUT/postgres.jsonl"
for q in scan_agg groupby_cat groupby_region filtered_agg zone_skip; do
  SQLQ="$(pgsql_for "$q")"
  best=""
  for r in 1 2 3 4 5; do
    ms=$(docker exec pg_analytics psql -U bench -d bench -tA -c \
      "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $SQLQ" 2>/dev/null \
      | grep -oE 'Execution Time: [0-9.]+' | grep -oE '[0-9.]+')
    [ -z "$ms" ] && continue
    if [ -z "$best" ] || awk "BEGIN{exit !($ms < $best)}"; then best="$ms"; fi
  done
  [ -z "$best" ] && best="0"
  echo "{\"sys\":\"postgres\",\"q\":\"$q\",\"ms_each\":$best}" | tee -a "$OUT/postgres.jsonl"
done
docker rm -f pg_analytics >/dev/null 2>&1 || true

# ---- Embedded competitors: DuckDB + SQLite, pinned cpu 0, 1 thread -------------------------
echo "-- running embedded competitors (duckdb + sqlite, pinned cpu $PIN) --"
docker run --rm --cpuset-cpus="$PIN" -v "$ROOT":/work -w /work python:3.12-slim \
  bash -c "pip install -q duckdb 2>/dev/null; taskset -c $PIN python3 bench/compare/sql_analytics/embedded_engines.py $N $ITERS" \
  2>/dev/null | grep -oE '\{"sys":"[a-z]+","q":"[a-z_]+","ms_each":[0-9.]+\}' | tee "$OUT/embedded.jsonl"

# ---- ClickHouse: columnar OLAP leader, server image, pinned cpu 0 --------------------------
echo "-- running clickhouse (pinned cpu $PIN) --"
docker rm -f ch_analytics >/dev/null 2>&1 || true
docker run -d --name ch_analytics --cpuset-cpus="$PIN" -e CLICKHOUSE_SKIP_USER_SETUP=1 \
  clickhouse/clickhouse-server:24.8 >/dev/null 2>&1
for i in $(seq 1 60); do
  if docker exec ch_analytics clickhouse-client --query "SELECT 1" >/dev/null 2>&1; then break; fi
  sleep 1
done
docker exec ch_analytics clickhouse-client --query "
  CREATE TABLE events ENGINE=MergeTree ORDER BY id AS
  SELECT toInt32(number) id, toInt32(number%10000) uid, toInt32(number%8) cat,
         toInt32((number*2654435761)%1000) amount,
         (['north','south','east','west','central'])[(number%5)+1] region, toInt32(number) ts
  FROM numbers($N)" 2>/dev/null
chq() {
  case "$1" in
    scan_agg)       echo "SELECT COUNT(*),SUM(amount),MIN(amount),MAX(amount) FROM events";;
    groupby_cat)    echo "SELECT cat,COUNT(*),SUM(amount) FROM events GROUP BY cat";;
    groupby_region) echo "SELECT region,COUNT(*),SUM(amount) FROM events GROUP BY region";;
    filtered_agg)   echo "SELECT COUNT(*),SUM(amount) FROM events WHERE amount>800";;
    zone_skip)      echo "SELECT COUNT(*),SUM(amount) FROM events WHERE ts>$TSHI";;
  esac
}
for q in scan_agg groupby_cat groupby_region filtered_agg zone_skip; do
  SQLQ="$(chq "$q")"
  # 5 reps in ONE client session (connection overhead amortized); --time prints per-query
  # Elapsed (server-side execution). FORMAT Null avoids result transfer. Take the min.
  SCRIPT=""; for r in 1 2 3 4 5; do SCRIPT="$SCRIPT$SQLQ FORMAT Null;"; done
  best=$(printf '%s' "$SCRIPT" | docker exec -i ch_analytics clickhouse-client --max_threads 1 \
           --time --multiquery 2>&1 | grep -oE '^[0-9.]+$' \
           | awk 'NR==1||$1<m{m=$1} END{print m*1000}')
  [ -z "$best" ] && best="0"
  echo "{\"sys\":\"clickhouse\",\"q\":\"$q\",\"ms_each\":$best}" | tee -a "$OUT/embedded.jsonl"
done
docker rm -f ch_analytics >/dev/null 2>&1 || true

# ---- compare (N-way) ------------------------------------------------------------------------
echo ""
echo "== COMPARISON (ms per query; >1x = Lockstep faster than that competitor) =="
python3 - "$OUT/lockstep.jsonl" "$OUT/postgres.jsonl" "$OUT/embedded.jsonl" <<'PY'
import json, sys
def load(*paths):
    d={}
    for p in paths:
        try: f=open(p)
        except: continue
        for line in f:
            line=line.strip()
            if not line: continue
            try: o=json.loads(line)
            except: continue
            d.setdefault(o["sys"], {})[o["q"]]=float(o["ms_each"])
    return d
data=load(*sys.argv[1:])
ls=data.get("lockstep",{})
comps=["postgres","duckdb","clickhouse","sqlite"]
hdr=f"{'query':<16}{'lockstep':>9}"+"".join(f"{c:>11}" for c in comps)
print(hdr); print("-"*len(hdr))
for q in ["scan_agg","groupby_cat","groupby_region","filtered_agg","zone_skip"]:
    l=ls.get(q)
    row=f"{q:<16}{(f'{l:.3f}' if l is not None else '?'):>9}"
    for c in comps:
        v=data.get(c,{}).get(q)
        if v is None: row+=f"{'-':>11}"
        elif l: row+=f"{v:.2f}/{v/l:.1f}x".rjust(11)
        else: row+=f"{v:.3f}".rjust(11)
    print(row)
print("\n(cell = competitor_ms / speedup; speedup>1 = Lockstep faster. N-way, 1 CPU, in-memory.)")
PY
rm -rf "$OUT"
