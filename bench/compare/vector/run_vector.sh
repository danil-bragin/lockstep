#!/usr/bin/env bash
# K1.5 — vector k-NN comparison: Lockstep IVFFLAT vs PostgreSQL 16 + pgvector, identical
# deterministic data (integer formula shared with lockstep_vector.cpp), identical queries,
# BOTH pinned to the SAME single CPU. Measures brute-force k-NN, index build, indexed k-NN,
# and recall@k of each system's index result against its OWN brute-force reference.
#
# Usage: bench/compare/vector/run_vector.sh [N] [DIM] [K] [QUERIES] [LISTS] [PROBES]
# Requires docker: the lockstep-lite build image + pgvector/pgvector:pg16.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
N="${1:-100000}"
DIM="${2:-64}"
K="${3:-10}"
Q="${4:-20}"
LISTS="${5:-200}"
PROBES="${6:-10}"
PIN="0"
OUT=$(mktemp -d /tmp/vecbench.XXXXXX)

echo "== vector k-NN: Lockstep IVFFLAT vs pgvector | N=$N dim=$DIM k=$K q=$Q lists=$LISTS probes=$PROBES cpu=$PIN =="

# ---- Lockstep: build + run pinned ------------------------------------------------------------
echo "-- building lockstep vector bench --"
docker run --rm -v "$ROOT":/work -w /work lockstep-lite bash -c \
  "mkdir -p build-linux && clang++ -std=c++23 -O2 -stdlib=libc++ -rtlib=compiler-rt \
   \$(for d in core storage txn consensus query harness providers/sim; do echo -I \$d/include; done) \
   bench/compare/vector/lockstep_vector.cpp -o build-linux/vector_bench 2>&1 | tail -5"
echo "-- running lockstep (pinned cpu $PIN) --"
docker run --rm --cpuset-cpus="$PIN" -v "$ROOT":/work -w /work lockstep-lite \
  taskset -c "$PIN" /work/build-linux/vector_bench "$N" "$DIM" "$K" "$Q" "$LISTS" "$PROBES" \
  | tee "$OUT/lockstep.jsonl"

# ---- pgvector: container pinned, load the SAME data, same phases -----------------------------
echo "-- starting pgvector (pinned cpu $PIN) --"
docker rm -f pg_vector_bench >/dev/null 2>&1 || true
docker run -d --name pg_vector_bench --cpuset-cpus="$PIN" \
  -e POSTGRES_PASSWORD=bench -e POSTGRES_USER=bench -e POSTGRES_DB=bench \
  pgvector/pgvector:pg16 -c fsync=off -c synchronous_commit=off >/dev/null
for i in $(seq 1 60); do
  docker exec pg_vector_bench pg_isready -U bench >/dev/null 2>&1 && break
  sleep 1
done
PSQL="docker exec pg_vector_bench psql -U bench -d bench -qtA"

echo "-- loading pgvector (same deterministic formula) --"
$PSQL -c "
  CREATE EXTENSION IF NOT EXISTS vector;
  DROP TABLE IF EXISTS docs;
  CREATE TABLE docs (id INT PRIMARY KEY, emb vector($DIM) NOT NULL);
  INSERT INTO docs
  SELECT i, (SELECT array_agg( ((i::bigint % 100) * 31 + d * 7) % 20
                               + (((i::bigint * 2654435761 + d * 40503) % 1000)::float8 / 5000.0) )
             FROM generate_series(0, $DIM - 1) d)::vector
  FROM generate_series(0, $N - 1) i;
  ANALYZE docs;" >/dev/null

# The query vector for query j (same formula as query_text() in lockstep_vector.cpp).
qvec() {
  local j="$1"
  $PSQL -c "SELECT '[' || string_agg( (((($j::bigint * 13) % 100) * 31 + d * 7) % 20 + 0.05)::text, ',' ORDER BY d) || ']'
            FROM generate_series(0, $DIM - 1) d"
}
for j in $(seq 0 $((Q - 1))); do qvec "$j" > "$OUT/q$j.txt"; done

knn_ms() {  # $1 = extra SET, prints min-of-3 EXPLAIN ANALYZE execution time over all queries' mean
  local total=0
  for j in $(seq 0 $((Q - 1))); do
    local v best=""
    v=$(cat "$OUT/q$j.txt")
    for r in 1 2 3; do
      local ms
      ms=$($PSQL -c "$1 EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) SELECT id FROM docs ORDER BY emb <-> '$v' LIMIT $K" \
           | grep -oE 'Execution Time: [0-9.]+' | grep -oE '[0-9.]+')
      [ -z "$ms" ] && continue
      if [ -z "$best" ] || awk "BEGIN{exit !($ms < $best)}"; then best="$ms"; fi
    done
    total=$(awk "BEGIN{print $total + ${best:-0}}")
  done
  awk "BEGIN{printf \"%.2f\", $total / $Q}"
}
knn_ids() {  # $1 = extra SET, $2 = outfile prefix
  for j in $(seq 0 $((Q - 1))); do
    $PSQL -c "$1 SELECT id FROM docs ORDER BY emb <-> '$(cat "$OUT/q$j.txt")' LIMIT $K" > "$OUT/$2$j.ids"
  done
}

echo "-- pgvector brute force (no index) --"
BR=$(knn_ms "")
knn_ids "" "pgbrute"
echo "{\"sys\":\"pgvector\",\"q\":\"knn_brute\",\"ms_each\":$BR}" | tee -a "$OUT/pg.jsonl"

echo "-- pgvector ivfflat build --"
T0=$(date +%s%N)
$PSQL -c "CREATE INDEX docs_ivf ON docs USING ivfflat (emb vector_l2_ops) WITH (lists = $LISTS)" >/dev/null
T1=$(date +%s%N)
echo "{\"sys\":\"pgvector\",\"q\":\"ivfflat_build\",\"ms_each\":$(( (T1 - T0) / 1000000 ))}" | tee -a "$OUT/pg.jsonl"

echo "-- pgvector ivfflat k-NN (probes=$PROBES) --"
AN=$(knn_ms "SET ivfflat.probes = $PROBES;")
knn_ids "SET ivfflat.probes = $PROBES;" "pgann"
echo "{\"sys\":\"pgvector\",\"q\":\"knn_ivfflat\",\"ms_each\":$AN}" | tee -a "$OUT/pg.jsonl"

# recall@k: |ann ∩ brute| / (K * Q) per system's own reference.
REC=$(python3 - "$OUT" "$Q" <<'PY'
import sys
out, q = sys.argv[1], int(sys.argv[2])
hit = tot = 0
for j in range(q):
    ref = set(open(f"{out}/pgbrute{j}.ids").read().split())
    ann = set(open(f"{out}/pgann{j}.ids").read().split())
    hit += len(ref & ann)
    tot += len(ref)
print(f"{hit/tot:.4f}" if tot else "0")
PY
)
echo "{\"sys\":\"pgvector\",\"q\":\"recall_at_k\",\"ms_each\":$REC}" | tee -a "$OUT/pg.jsonl"
docker rm -f pg_vector_bench >/dev/null 2>&1 || true

echo ""
echo "== results (ms/query; recall as fraction) =="
cat "$OUT/lockstep.jsonl" "$OUT/pg.jsonl"
echo "raw: $OUT"
