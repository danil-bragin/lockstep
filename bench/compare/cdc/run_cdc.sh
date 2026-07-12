#!/usr/bin/env bash
# K4 CDC vs Kafka — head-to-head, both sides at their server-side best case.
# Kafka: official perf tools INSIDE the broker container (loopback, page cache) —
# the same "his best case" methodology as bench/compare/queue vs pgmq.
# Lockstep: bench/cdc_bench_driver (in-process engine, sim disk) — our best case.
# Numbers land in REPORT.md; this script reproduces the Kafka side.
set -euo pipefail

KIMG=apache/kafka:3.7.0
K=lockstep-kafka-bench
docker rm -f "$K" 2>/dev/null || true
docker run -d --name "$K" --cpus=4 "$KIMG"
sleep 15
kbin() { docker exec "$K" /opt/kafka/bin/"$@"; }

kbin kafka-topics.sh --bootstrap-server localhost:9092 --create --topic t1 --partitions 1 --replication-factor 1
kbin kafka-topics.sh --bootstrap-server localhost:9092 --create --topic t8 --partitions 8 --replication-factor 1

echo "== produce, 1 partition, 64B, acks=1 =="
kbin kafka-producer-perf-test.sh --topic t1 --num-records 1000000 --record-size 64 \
  --throughput -1 --producer-props bootstrap.servers=localhost:9092 acks=1 | tail -1
echo "== produce, 1 partition, 64B, acks=all =="
kbin kafka-producer-perf-test.sh --topic t1 --num-records 1000000 --record-size 64 \
  --throughput -1 --producer-props bootstrap.servers=localhost:9092 acks=all | tail -1
echo "== produce, 8 partitions, 64B, acks=all =="
kbin kafka-producer-perf-test.sh --topic t8 --num-records 2000000 --record-size 64 \
  --throughput -1 --producer-props bootstrap.servers=localhost:9092 acks=all | tail -1
echo "== consume, 1 partition =="
kbin kafka-consumer-perf-test.sh --bootstrap-server localhost:9092 --topic t1 --messages 2000000 | tail -1
echo "== consume, 8 partitions =="
kbin kafka-consumer-perf-test.sh --bootstrap-server localhost:9092 --topic t8 --messages 2000000 | tail -1
echo "== paced 100 rec/s (per-record latency, batching amortization removed) =="
kbin kafka-producer-perf-test.sh --topic t1 --num-records 2000 --record-size 64 \
  --throughput 100 --producer-props bootstrap.servers=localhost:9092 acks=all | tail -1

echo "== Lockstep side (run from the repo root) =="
echo "  cmake --build build/release --target lockstep_cdc_bench_driver -j 8"
echo "  ./build/release/bench/lockstep_cdc_bench_driver 200000     # drain + live tail"
echo "  for m in 1 2 4 8; do ./build/release/bench/lockstep_cdc_bench_driver --dist \$m 100000; done"
docker rm -f "$K"
