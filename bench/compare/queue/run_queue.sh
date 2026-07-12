#!/usr/bin/env bash
# K3.4 — queue throughput: Lockstep queues vs pgmq (the Supabase queue engine), both
# pinned to cpu 0. pgmq ops run SERVER-SIDE in single statements (its best case).
# Usage: run_queue.sh [N] [BATCH]
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
N="${1:-10000}"; B="${2:-100}"; PIN=0
echo "== queues: Lockstep vs pgmq | N=$N batch=$B cpu=$PIN =="
docker run --rm -v "$ROOT":/work -w /work lockstep-lite bash -c \
  "clang++ -std=c++23 -O2 -stdlib=libc++ -rtlib=compiler-rt \
   \$(for d in core storage txn consensus query harness providers/sim; do echo -I \$d/include; done) \
   bench/compare/queue/lockstep_queue.cpp -o build-linux/queue_bench 2>&1 | tail -3"
docker run --rm --cpuset-cpus="$PIN" -v "$ROOT":/work -w /work lockstep-lite \
  taskset -c "$PIN" /work/build-linux/queue_bench "$N" "$B"

docker rm -f pgmq_bench >/dev/null 2>&1 || true
docker run -d --name pgmq_bench --cpuset-cpus="$PIN" -e POSTGRES_PASSWORD=bench \
  quay.io/tembo/pg17-pgmq:latest -c fsync=off -c synchronous_commit=off >/dev/null
for i in $(seq 1 45); do docker exec pgmq_bench pg_isready -U postgres >/dev/null 2>&1 && break; sleep 1; done
P="docker exec pgmq_bench psql -U postgres -qtA"
$P -c "CREATE EXTENSION IF NOT EXISTS pgmq; SELECT pgmq.create('q'); SELECT pgmq.create('qb')" >/dev/null
ms() { $P -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) $1" | grep -oE 'Execution Time: [0-9.]+' | grep -oE '[0-9.]+' | tail -1; }
# send: N single sends server-side (their tight loop, best case)
T=$(ms "SELECT pgmq.send('q', ('{\"p\":' || i || '}')::jsonb) FROM generate_series(1,$N) i")
echo "{\"sys\":\"pgmq\",\"op\":\"send\",\"msg_s\":$(awk "BEGIN{printf \"%.0f\", $N/($T/1000)}")}"
# send batch in one txn
T=$(ms "SELECT pgmq.send_batch('qb', (SELECT array_agg(('{\"p\":' || i || '}')::jsonb) FROM generate_series(1,$N) i))")
echo "{\"sys\":\"pgmq\",\"op\":\"send_txn\",\"msg_s\":$(awk "BEGIN{printf \"%.0f\", $N/($T/1000)}")}"
# drain: read(vt,qty)+delete loop server-side via DO block, timed client-side
T0=$(date +%s%N)
$P -c "DO \$\$ DECLARE r RECORD; ids BIGINT[]; got INT := 0; BEGIN
  LOOP
    ids := ARRAY(SELECT msg_id FROM pgmq.read('q', 1000000, $B));
    EXIT WHEN ids = '{}';
    PERFORM pgmq.delete('q', ids);
    got := got + array_length(ids, 1);
    EXIT WHEN got >= $N;
  END LOOP;
END \$\$" >/dev/null
T1=$(date +%s%N)
echo "{\"sys\":\"pgmq\",\"op\":\"recv_ack\",\"msg_s\":$(awk "BEGIN{printf \"%.0f\", $N/(($T1-$T0)/1000000000)}")}"
docker rm -f pgmq_bench >/dev/null 2>&1 || true
