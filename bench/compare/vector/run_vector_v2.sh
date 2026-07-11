#!/usr/bin/env bash
# K1.5 dataset v2 — SEPARATED points, probes SWEEP: the honest recall-vs-latency curve,
# Lockstep IVFFLAT vs pgvector, identical deterministic data, both pinned to cpu 0.
# Usage: run_vector_v2.sh [N] [DIM] [K] [QUERIES] [LISTS] [PG_LISTS]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
N="${1:-100000}"; DIM="${2:-64}"; K="${3:-10}"; Q="${4:-20}"; LISTS="${5:-200}"; PG_LISTS="${6:-100}"
PIN="0"; OUT=$(mktemp -d /tmp/vecv2.XXXXXX)
echo "== v2 sweep | N=$N dim=$DIM k=$K q=$Q lists=$LISTS cpu=$PIN =="

echo "-- lockstep --"
docker run --rm -v "$ROOT":/work -w /work lockstep-lite bash -c \
  "clang++ -std=c++23 -O2 -stdlib=libc++ -rtlib=compiler-rt \
   \$(for d in core storage txn consensus query harness providers/sim; do echo -I \$d/include; done) \
   bench/compare/vector/lockstep_vector_v2.cpp -o build-linux/vector_v2 2>&1 | tail -3"
docker run --rm --cpuset-cpus="$PIN" -v "$ROOT":/work -w /work lockstep-lite \
  taskset -c "$PIN" /work/build-linux/vector_v2 "$N" "$DIM" "$K" "$Q" "$LISTS" | tee "$OUT/ls.jsonl"

echo "-- pgvector --"
docker rm -f pg_v2 >/dev/null 2>&1 || true
docker run -d --name pg_v2 --cpuset-cpus="$PIN" -e POSTGRES_PASSWORD=bench \
  -e POSTGRES_USER=bench -e POSTGRES_DB=bench pgvector/pgvector:pg16 \
  -c fsync=off -c synchronous_commit=off >/dev/null
for i in $(seq 1 60); do docker exec pg_v2 pg_isready -U bench >/dev/null 2>&1 && break; sleep 1; done
PSQL="docker exec pg_v2 psql -U bench -d bench -qtA"
$PSQL -c "
  CREATE EXTENSION IF NOT EXISTS vector;
  CREATE TABLE docs (id INT PRIMARY KEY, emb vector($DIM) NOT NULL);
  INSERT INTO docs SELECT i,
    (SELECT array_agg( ((i::bigint % 1000) * 37 + d * 11) % 8
                       + (((i::bigint * 2654435761 + d * 40503) % 2000)::float8 / 1000.0) )
     FROM generate_series(0, $DIM - 1) d)::vector
  FROM generate_series(0, $N - 1) i;
  ANALYZE docs;" >/dev/null
qvec() { $PSQL -c "SELECT '[' || string_agg( ((((($1::bigint * 37) % 1000) * 37 + d * 11) % 8)
                 + ((($1::bigint * 13 + d * 7) % 100)::float8 / 100.0))::text, ',' ORDER BY d) || ']'
                 FROM generate_series(0, $DIM - 1) d"; }
for j in $(seq 0 $((Q - 1))); do qvec "$j" > "$OUT/q$j"; done
# brute reference ids + timing — ONE session (per-query docker exec is flaky at volume)
{
  for j in $(seq 0 $((Q - 1))); do
    echo "SELECT '##Q$j';"
    echo "SELECT id FROM docs ORDER BY emb <-> '$(cat "$OUT/q$j")' LIMIT $K;"
    echo "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) SELECT id FROM docs ORDER BY emb <-> '$(cat "$OUT/q$j")' LIMIT $K;"
  done
} | docker exec -i pg_v2 psql -U bench -d bench -qtA > "$OUT/brute" 2>/dev/null
python3 - "$OUT" "$Q" <<'PY2'
import re, sys
o, q = sys.argv[1], int(sys.argv[2])
cur, times = None, []
ids = {}
for ln in open(f"{o}/brute").read().splitlines():
    if ln.startswith("##Q"):
        cur = int(ln[3:]); ids[cur] = []; continue
    m = re.search(r"Execution Time: ([0-9.]+)", ln)
    if m: times.append(float(m.group(1))); cur = None; continue
    if cur is not None and ln.strip().isdigit(): ids[cur].append(ln.strip())
for j in range(q):
    open(f"{o}/ref{j}", "w").write("\n".join(ids.get(j, [])))
ms = sum(times) / len(times) if times else 0.0
print(f'{{"sys":"pgvector","probes":0,"ms_each":{ms:.2f},"recall":1.0}}')
PY2
$PSQL -c "CREATE INDEX dix ON docs USING ivfflat (emb vector_l2_ops) WITH (lists = $PG_LISTS)" >/dev/null
for PRB in 1 2 5 10 20 50 100; do
  # ONE psql session per probes value (per-call docker exec proved flaky at this volume):
  # markers delimit each query's id rows; EXPLAIN supplies the server-side timing.
  {
    echo "SET ivfflat.probes = $PRB;"
    for j in $(seq 0 $((Q - 1))); do
      echo "SELECT '##Q$j';"
      echo "SELECT id FROM docs ORDER BY emb <-> '$(cat "$OUT/q$j")' LIMIT $K;"
      echo "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) SELECT id FROM docs ORDER BY emb <-> '$(cat "$OUT/q$j")' LIMIT $K;"
    done
  } | docker exec -i pg_v2 psql -U bench -d bench -qtA > "$OUT/sweep$PRB" 2>/dev/null
  python3 - "$OUT" "$Q" "$PRB" <<'PY2'
import re, sys
o, q, prb = sys.argv[1], int(sys.argv[2]), sys.argv[3]
txt = open(f"{o}/sweep{prb}").read().splitlines()
ids, times, cur = {}, [], None
for ln in txt:
    if ln.startswith("##Q"):
        cur = int(ln[3:]); ids[cur] = set(); continue
    m = re.search(r"Execution Time: ([0-9.]+)", ln)
    if m: times.append(float(m.group(1))); cur = None; continue
    if cur is not None and ln.strip().isdigit(): ids[cur].add(ln.strip())
hit = tot = 0
for j in range(q):
    ref = set(open(f"{o}/ref{j}").read().split())
    hit += len(ref & ids.get(j, set())); tot += len(ref)
ms = sum(times) / len(times) if times else 0.0
print(f'{{"sys":"pgvector","probes":{prb},"ms_each":{ms:.2f},"recall":{hit/tot if tot else 0:.4f}}}')
PY2
done | tee -a "$OUT/pg.jsonl"
docker rm -f pg_v2 >/dev/null 2>&1 || true
echo ""; echo "== curve =="; cat "$OUT/ls.jsonl" "$OUT/pg.jsonl"; echo "raw: $OUT"
