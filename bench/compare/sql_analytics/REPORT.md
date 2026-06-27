# SQL analytics — Lockstep columnar vs PostgreSQL 16

Head-to-head on analytical SQL: identical deterministic data + identical queries, **both pinned
to the same single CPU** (cpu 0), in-memory (Postgres `fsync=off`, table fully cached after
`ANALYZE`), **no secondary indexes** on either side (raw analytical scan/aggregate). Run via
`bench/compare/sql_analytics/run_analytics.sh [N] [iters]`.

## Result — 5-way: Lockstep vs Postgres, DuckDB, ClickHouse, SQLite

N = 1,000,000 rows, ms per query (lower = faster); `Nx` = competitor_ms / lockstep_ms (>1 = Lockstep faster):

| query          | Lockstep | Postgres 16   | DuckDB      | ClickHouse†  | SQLite        |
|----------------|---------:|--------------:|------------:|-------------:|--------------:|
| scan_agg       |   4.94   | 61.4 / **12×**| 1.60 / 0.3× | ~1.0 / 0.2×  | 56.2 / **11×**|
| groupby_cat    |   8.59   | 91.8 / **11×**| 2.27 / 0.3× | ~4.0 / 0.5×  | 144.6 / **17×**|
| groupby_region |  21.5    | 106 / **4.9×**| 11.1 / 0.5× | ~12 / 0.6×   | 182 / **8.5×** |
| filtered_agg   |   6.68   | 41.1 / **6.1×**| 0.82 / 0.1×| ~1.0 / 0.1×  | 24.1 / **3.6×**|
| zone_skip      |   0.70   | 41.7 / **60×**| 0.15 / 0.2× | ~1.0 / 1.4×  | 2.35 / **3.4×**|

**Two clear tiers.** Against the **row stores** (Postgres, SQLite) Lockstep columnar wins **3.4×–60×**
on every shape — the SoA scan + zone-map skipping crush row-at-a-time. Against the **mature columnar
engines** (DuckDB, ClickHouse) Lockstep is slower on most (0.1–0.6×) but COMPETITIVE on scan_agg and
zone_skip; the gap is GROUP BY + filtered (their vectorized hash-aggregate + SIMD filter). At 200k
rows the DuckDB gap is smaller (scan_agg 0.79×); DuckDB/ClickHouse scale better at 1M.

**Honest caveats.**
- † ClickHouse is timed via `clickhouse-client --time`, which has ~1 ms granularity — sub-ms queries
  show a ~1 ms floor, so ClickHouse's true (faster) times are UNDER-reported for the small queries
  (e.g. zone_skip's "1.4× Lockstep faster" is an artifact of the floor). Treat ClickHouse cells as
  an upper bound on its time. DuckDB + SQLite are in-process (accurate); Postgres is EXPLAIN ANALYZE.
- Single CPU (`--cpuset-cpus=0` / `taskset`, ClickHouse/DuckDB forced 1 thread). This run is serial
  (no `workers` arg). Morsel parallelism is NO LONGER an open lever — it shipped
  (`ProdParallelExecutor`, `lockstep_analytics N iters WORKERS`); this comparison just doesn't pass it.
- In-memory, no secondary indexes (raw analytical scan/aggregate). Filter columns NOT NULL (so
  Lockstep's vectorized + zone-skip fast paths apply). Data generated from one formula on all sides.

**Takeaway.** Lockstep beats mature single-node ROW stores on analytics by a wide margin (the
project's "beat Postgres" goal — met 5–60×), and is in the same order of magnitude as the dedicated
COLUMNAR engines.

> **Note (this snapshot predates the GROUP BY overhaul).** The "trailing on GROUP BY — the next
> lever" framing and the `groupby_*` cells above were measured before the columnar one-pass hash-
> aggregate landed (aggregate fusion + one-pass INT/TEXT, dense dict-code path; commits `8fbf176`,
> `8b75215`, `0aa5bb2`, `e2ffc68`; 1.6–4.7× and byte-identical). A re-run at 1M against the current
> columnar GROUP BY is the outstanding refresh; the methodology below is unchanged.

Reproduce: `bench/compare/sql_analytics/run_analytics.sh 1000000 20` (docker + lockstep-dev +
postgres:16 + clickhouse/clickhouse-server:24.8 + a python image with duckdb).

## The queries (run on both verbatim)
- `scan_agg`       — `SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM events`
- `groupby_cat`    — `SELECT cat, COUNT(*), SUM(amount) FROM events GROUP BY cat`
- `groupby_region` — `SELECT region, COUNT(*), SUM(amount) FROM events GROUP BY region`
- `filtered_agg`   — `SELECT COUNT(*), SUM(amount) FROM events WHERE amount > 800`
- `zone_skip`      — `SELECT COUNT(*), SUM(amount) FROM events WHERE ts > 0.95·N`  (monotonic ts)

Schema (both): `events(id INT PK, uid INT, cat INT NOT NULL, amount INT NOT NULL, region TEXT,
ts INT NOT NULL)`. Filter columns are NOT NULL so Lockstep's vectorized-aggregate + zone-skip fast
path applies (its conjunct extractor conservatively skips nullable columns). Data is generated from
the SAME formula on both sides (`amount = (i·2654435761) mod 1000`, `ts = i`, `cat = i mod 8`, …).

## Method + honest caveats
- Both pinned to **one CPU** (`docker --cpuset-cpus=0` for Postgres, `taskset -c 0` for Lockstep).
  Single-threaded comparison — Lockstep's multi-shard parallelism is NOT used here.
- Lockstep `ms_each` = full `exec()` (parse + plan + execute), looped `iters` times, in-process.
- Postgres `ms_each` = `EXPLAIN (ANALYZE) Execution Time`, warm, **min of 5** (excludes planning +
  client round-trip — if anything this FLATTERS Postgres vs Lockstep's full parse+exec).
- In-memory both (Postgres `fsync=off`); this is a CPU/execution comparison, not a durability one.
  Lockstep durability is verified separately (crash-recovery byte-identical).
- Columnar is opt-in (`set_columnar_default`) + flushed once after the bulk load (analytics shape:
  load → flush → query-many). Row-mode Lockstep is competitive on OLTP point/range (separate bench).

Reproduce: `bench/compare/sql_analytics/run_analytics.sh 200000 30` (needs docker + the
`lockstep-dev` image + `postgres:16`).
