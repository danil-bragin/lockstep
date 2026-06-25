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
  CREATE TABLE events (id INT PRIMARY KEY, uid INT, cat INT NOT NULL, amount INT NOT NULL, region TEXT, ts INT NOT NULL);
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

# ---- compare --------------------------------------------------------------------------------
echo ""
echo "== COMPARISON (ms per query, lower = faster) =="
python3 - "$OUT/lockstep.jsonl" "$OUT/postgres.jsonl" <<'PY'
import json, sys
def load(p):
    d={}
    for line in open(p):
        line=line.strip()
        if not line: continue
        try: o=json.loads(line)
        except: continue
        d[o["q"]]=float(o["ms_each"])
    return d
ls=load(sys.argv[1]); pg=load(sys.argv[2])
print(f"{'query':<18}{'lockstep_ms':>13}{'postgres_ms':>13}{'speedup':>10}")
for q in ["scan_agg","groupby_cat","groupby_region","filtered_agg","zone_skip"]:
    l=ls.get(q); p=pg.get(q)
    if l is None or p is None:
        print(f"{q:<18}{'?' if l is None else f'{l:.3f}':>13}{'?' if p is None else f'{p:.3f}':>13}{'':>10}")
        continue
    sp = p/l if l>0 else 0
    print(f"{q:<18}{l:>13.3f}{p:>13.3f}{sp:>9.2f}x")
PY
rm -rf "$OUT"
