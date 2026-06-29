#!/usr/bin/env bash
# Distributed star-JOIN bench: co-located-shuffle pushdown vs gather-the-fact baseline.
# Compiles dist_join_bench.cpp against the in-tree headers and runs it. No external DBs — it is a
# pure A/B of the SAME DistributedSql query with pushdown on vs off (see REPORT.md).
#
# Usage: bench/compare/distributed_join/run.sh [FACT_ROWS=1000000] [SHARDS=8] [DISTINCT_FK=1000] [DIM_GROUPS=8]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$(mktemp -d /tmp/distjoin.XXXXXX)"
CXX="${CXX:-c++}"
INCS=""
for d in core storage txn consensus query harness providers/sim providers/prod; do INCS="$INCS -I$ROOT/$d/include"; done
echo "-- building dist_join_bench --"
# shellcheck disable=SC2086
"$CXX" -std=c++2b -O2 -DNDEBUG $INCS "$ROOT/bench/compare/distributed_join/dist_join_bench.cpp" -o "$OUT/dist_join_bench"
echo "-- running --"
"$OUT/dist_join_bench" "${1:-1000000}" "${2:-8}" "${3:-1000}" "${4:-8}"
rm -rf "$OUT"
