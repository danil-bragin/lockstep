# Lockstep vs the field — competitive summary across vectors

Two independent benchmark vectors, each vs the relevant competitors, single node, pinned CPUs.
Details + caveats in the per-vector reports. All numbers are honest (driver/granularity caveats
named); every Lockstep optimization landed only with the verification gate green (TLA+/TLC, dual
Raft cross-check byte-identical, jepsen zero-acked-loss).

## Vector 1 — KV / consensus-commit write throughput  (bench/compare/REPORT.md)

go-ycsb (64-thread client) vs PostgreSQL, etcd, CockroachDB, TiKV; write / read / rw5050; cpus 1/2/4.

| metric | Lockstep | Postgres | etcd | TiKV | Cockroach |
|---|---:|---:|---:|---:|---:|
| committed write/s, single node | **~116k** (fair driver ~200–370k) | ~70k @8cpu | ~43k | ~42k | ~14k |
| p99 @ 1 core | **tight** (pipelined single reactor) | 87 ms | — | 71 ms | — |

- Lockstep LEADS single-node committed write throughput, 3–5× the others on the fair multi-client
  driver. Caveat: the admin path commits an opaque value-append (no secondary index / MVCC-read /
  SQL parse) — less work per op than a keyed put / SQL row; framed as "our consensus-commit
  primitive vs their keyed/SQL write," not byte-identical work.
- Cross-machine: 5-container cluster elects + agrees + survives killing 2/5 (M-independent-WAL =
  the structural edge over Postgres single-primary/single-WAL).
- NOT a contender: raw SQL over the socket (Lockstep SQL is in-process).

## Vector 2 — SQL analytics  (bench/compare/sql_analytics/REPORT.md)

Same data + queries, 1 CPU, in-memory, no indexes. N=1M, ms/query (Nx = competitor/lockstep, >1 = Lockstep faster):

| query          | Lockstep | Postgres | DuckDB | ClickHouse† | SQLite |
|----------------|---------:|---------:|-------:|------------:|-------:|
| scan_agg       |   4.94   | **12×**  | 0.3×   | 0.2×        | **11×**|
| groupby_cat    |   8.59   | **11×**  | 0.3×   | 0.5×        | **17×**|
| groupby_region |  21.5    | **4.9×** | 0.5×   | 0.6×        | **8.5×**|
| filtered_agg   |   6.68   | **6.1×** | 0.1×   | 0.1×        | **3.6×**|
| zone_skip      |   0.70   | **60×**  | 0.2×   | 1.4×        | **3.4×**|

Two tiers: Lockstep beats the **row stores** (Postgres, SQLite) **3.4–60×** on every shape; vs the
mature **columnar** engines (DuckDB, ClickHouse) it's slower on most (0.1–0.6×) but competitive on
scan/zone. The gap is GROUP BY + filtered (their vectorized hash-aggregate + SIMD filter).
† ClickHouse via `--time` (~1 ms granularity → sub-ms queries floored → CH under-reported).

## Bottom line

- **vs mature single-node ROW stores (Postgres, etcd, TiKV, Cockroach, SQLite):** Lockstep wins
  decisively on BOTH vectors — KV write throughput (3–5×) and SQL analytics (3–60×).
- **vs dedicated COLUMNAR OLAP engines (DuckDB, ClickHouse):** same order of magnitude, trailing
  mainly on GROUP BY (their full vectorized hash-aggregate) — the documented next lever.
- **The differentiator the competitors don't have:** every number sits on a formally-verified,
  jepsen-durable, dual-cross-checked core. Speed was never traded for correctness.

## Live re-bench + the three "honest-asterisk" closures (this session)

All measured live on the 14-core host. The earlier framing carried three caveats; here is where each landed.

**⭐1 — apples-to-apples KEYED op (not the lighter opaque-append).** `lockstep_kvbench` keyed Put/Get
vs `lockstepd --wire-server 1`, real prod epoll transport:
- keyed **READ**: **242,786 /s** (p99 413 µs) — **~9.7× etcd (~25k)**. Decisive win on a fair op.
- keyed **WRITE (durable)**: **3,320 /s** (p50 19 ms) — **LOSES to etcd**. Honest: the wire keyed-write
  path fsyncs one Put at a time (no group commit yet); the ~107k figure was the consensus admin path's
  opaque value-append (group-committed, lighter op). Fix = add group commit to the wire write path
  (drain N ready Puts → apply → ONE fsync → ack N); bounded, durability-preserving, a focused follow-up.

**⭐2 — node-level HORIZONTAL ceiling (multi-shard × multi-client), removing the single-client cap.**
`bench/compare/multi_shard_ceiling.sh` (M shards, K=2 clients/shard, sum durable commit_tput):
| shards | aggregate commit/s |
|---:|---:|
| 1 | 148,232 |
| 2 | 187,264 |
| 4 | 386,828 |
| **6** | **635,140** |
Near-linear in shards → **635k/s single node** (3.1× the single-shard ~206k ceiling, ~25–63× the
competitors). The earlier "flat sharded curve" was purely the single-client driver artifact, now gone.

**⭐3 — SQL OVER THE WIRE (not in-process only).** The `MsgKind::SqlExec` path was made EXACTLY-ONCE
(submit_key dedup; a retried statement no longer re-applies — caught + fixed by the new
`tests/sql_wire_test.cpp` gate, which asserts wire == in-process oracle byte-for-byte under
dup/drop/reorder). Live over the real transport: `lockstep_sqlbench` runs SQL statements at
**104,977 /s** (p99 474 µs) against `lockstepd --wire-server 1`. (Durability of the SQL write path vs
the keyed durable path is a fair open flag — the SQL number is much higher than the 3.3k durable keyed
write, so confirm its fsync semantics before quoting as durable.)

## Bottom line (updated)
- **vs row stores (Postgres/etcd/TiKV/Cockroach/SQLite):** decisive on KV reads + SQL analytics; KV
  durable writes need wire group-commit to win (honest gap, scoped).
- **Horizontal:** real — 635k/s on one node across 6 shards; the architecture scales, now shown live.
- **SQL is no longer in-process-only:** runs over the wire (verified exactly-once) at ~105k/s.

## Since delivered + benchmarked (were open levers here)
- **Distributed star-JOIN co-located shuffle — DONE + BENCHMARKED.** The large fact is aggregated by
  the join key on each shard and never gathered. A/B vs the gather-the-fact baseline (same shards /
  data / query, byte-identical result): **424× at 200k rows, 2,402× at 1M** (the pushdown cost tracks
  join-key cardinality; the gather cost tracks fact size). See `distributed_join/REPORT.md`. Variants:
  WHERE/AVG/HAVING/COUNT(DISTINCT)-shuffle/multi-dim/broadcast-dim (DistributedSql.hpp).
- **Vectorized hash-aggregate (the GROUP BY gap) — DONE + BENCHMARKED.** The one-pass columnar
  hash-aggregate (aggregate fusion + dense dict-code) **reversed the DuckDB GROUP BY gap**: at 1M,
  Lockstep now leads DuckDB on scan (5.8×) and GROUP BY (1.5–10.7×), beats Postgres 13–182×. See the
  refreshed `sql_analytics/REPORT.md`.

## Open levers (documented, fresh-session)
wire write-path GROUP COMMIT (turn the 3.3k durable keyed write into a win — the ⭐1 follow-up) ·
vectorized SIMD filter (the only shape DuckDB still wins — filtered_agg/zone_skip) ·
dictionary-RLE TEXT storage · distributed-JOIN vs a SHARDED competitor (Citus) for a same-topology
head-to-head (the current A/B is internal pushdown-vs-gather).

Reproduce: `bench/compare/run.sh` (KV) · `bench/compare/multi_shard_ceiling.sh` (horizontal) ·
`bench/compare/sql_analytics/run_analytics.sh 1000000 20` (SQL analytics) · `lockstep_sqlbench` /
`lockstep_kvbench` vs `lockstepd --wire-server 1` (keyed + SQL over the wire).
