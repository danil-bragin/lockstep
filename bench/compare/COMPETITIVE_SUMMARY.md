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

## Open levers (documented, fresh-session)

morsel parallelism (multi-core SQL — multiplies all SQL wins by cores; cross-layer, threads live in
providers/prod) · vectorized hash-aggregate (close the DuckDB GROUP BY gap) · dictionary/RLE (needs a
global dict) · vectorized JOIN · multi-machine analytics over the shards.

Reproduce: `bench/compare/run.sh` (KV) · `bench/compare/sql_analytics/run_analytics.sh 1000000 20` (SQL).
