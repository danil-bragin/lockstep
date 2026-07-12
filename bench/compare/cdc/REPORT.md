# K4 CDC vs Kafka — measured head-to-head (2026-07-12)

Both sides at their server-side best case, one laptop (arm64, Docker 4 CPUs for Kafka).
Kafka 3.7 (KRaft, single broker, replication 1): its OWN perf tools inside the broker
container — loopback, page cache, batched producer (the same methodology that favored
pgmq in bench/compare/queue). Lockstep: in-process engine over the sim disk
(bench/cdc_bench_main.cpp). Neither side pays a network round trip per record; both
reports are "engine ceiling", not cross-machine end-to-end.

## Numbers

| axis | Kafka 3.7 (64B records) | Lockstep CHANGES |
|---|---|---|
| produce, 1 partition/shard | 604k rec/s acks=1 · 725k acks=all | 148k ops/s (durable SQL INSERT, PK-checked row) |
| produce, 8 partitions/shards | 1.52M rec/s | 605k ops/s (ingest+deliver COMBINED, thread-per-shard) |
| consume, 1 feed | 513k msg/s end-to-end (3.68M/s pure fetch after a 3.3s group rebalance) | 2.37M ops/s live tail · 4.7M/s cold drain |
| per-record latency at full throttle | 231–250 ms avg (batching) | sub-ms (single-row commit) |
| per-record latency, paced 100/s | 2 ms p50 / 4 ms p95 | sub-ms |

## Honest reading

**Kafka wins raw produce.** A batched blob append (no parse, no PK check, no row
codec) is 4–5x our durable SQL INSERT per partition, and its 8-partition producer
(1.5M/s) outruns our 8-shard combined loop (605k/s). If the workload is "firehose of
opaque events", Kafka is the right hammer — we do not contest it.

**Lockstep wins consume.** One feed delivers 2.37M decoded ROWS/s while ingest runs —
4.6x Kafka's end-to-end consumer on one partition; even Kafka's pure-fetch rate
(3.68M blob msgs/s, rebalance excluded, offsets never committed in its perf tool) only
reaches parity-ish, and those are raw bytes, not decoded rows with schema.

**Lockstep wins per-record latency.** Kafka's throughput comes from deep batching:
231 ms average producer latency at full throttle; even paced it is ~2 ms p50 through
the broker. A Lockstep INSERT commits and is FETCHable in well under a millisecond.

**Semantics are not the same weight class.** Kafka's consumer-perf never commits
offsets; real exactly-once needs idempotent producers + transactions + the offsets
topic + group coordination. Lockstep's number already includes: total order per shard,
transactional capture (a rolled-back txn never surfaces), durable server-side cursors,
and exactly-once resume gated by a 12-storm crash oracle (sql_cdc_storm_test).

**Positioning unchanged** (docs/KILLER_FEATURES.md K4): we do not replace the
million-msg/s streaming backbone; we delete the DB → Debezium → Kafka → consumer stack
where Kafka exists only to ship database changes — which is where per-record latency,
row semantics, and exactly-once-by-construction dominate, and where our consume-side
advantage lands.

Repro: `bench/compare/cdc/run_cdc.sh` (Kafka side) + the two Lockstep commands it
prints. Raw outputs preserved in the shell transcript of 2026-07-12.
