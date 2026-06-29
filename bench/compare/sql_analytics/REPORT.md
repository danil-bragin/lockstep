# SQL analytics — Lockstep columnar vs Postgres · DuckDB · ClickHouse · SQLite

Head-to-head on analytical SQL: identical deterministic data + identical queries, **both pinned
to the same single CPU** (cpu 0), in-memory (Postgres `fsync=off`, table fully cached after
`ANALYZE`), **no secondary indexes** on either side (raw analytical scan/aggregate). Run via
`bench/compare/sql_analytics/run_analytics.sh [N] [iters]`.

## Result — 5-way: Lockstep vs Postgres, DuckDB, ClickHouse, SQLite

N = 1,000,000 rows, ms per query (lower = faster), re-measured after the one-pass columnar GROUP BY
overhaul (aggregate fusion + dense dict-code hash-aggregate). `Nx` = competitor_ms / lockstep_ms
(>1 = Lockstep faster):

| query          | Lockstep | Postgres 16    | DuckDB        | ClickHouse†   | SQLite          |
|----------------|---------:|---------------:|--------------:|--------------:|----------------:|
| scan_agg       |  0.300   | 54.6 / **182×**| 1.73 / **5.8×**| ~1.0 / **3.3×**| 58.3 / **194×** |
| groupby_cat    |  1.681   | 90.1 / **54×** | 2.52 / **1.5×**| ~4.0 / **2.4×**| 150.6 / **90×** |
| groupby_region |  1.062   | 108.8 / **102×**| 11.3 / **10.7×**| ~12 / **11×** | 186.0 / **175×**|
| filtered_agg   |  3.071   | 39.7 / **13×** | 0.85 / 0.28×  | ~1.0 / 0.33×  | 25.5 / **8.3×** |
| zone_skip      |  0.264   | 35.9 / **136×**| 0.14 / 0.52×  | ~1.0 / 3.8×†  | 2.44 / **9.2×** |

**Two clear tiers.** Against the **row stores** (Postgres, SQLite) Lockstep columnar wins **8×–194×**
on every shape — the SoA scan + zone-map skipping crush row-at-a-time. Against the **mature columnar
engines** (DuckDB, ClickHouse) Lockstep now **WINS scan_agg and GROUP BY** (vs DuckDB: scan 5.8×,
groupby_cat 1.5×, groupby_region 10.7×) and is competitive on the rest; it trails **only** on
`filtered_agg` and `zone_skip` (DuckDB's vectorized SIMD filter — 0.28×/0.52×). **The GROUP BY gap
reversed:** the one-pass columnar hash-aggregate (aggregate fusion + dense dict-code) turned the
previous "trailing on GROUP BY" into a 1.5–10.7× lead over DuckDB. ClickHouse's sub-ms cells hit its
~1 ms timing floor (see caveat), so the real ClickHouse gaps on scan/groupby are at most this.

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
project's "beat Postgres" goal — **met 13–182×**), and now leads the dedicated columnar engine DuckDB
on scan + GROUP BY (the columnar overhaul's payoff), trailing it only on SIMD-filtered scans. The
remaining lever is a vectorized SIMD filter for `filtered_agg` / `zone_skip`.

> Numbers from one developer-laptop run (Docker, both sides pinned to cpu 0); relative, not a
> production benchmark. Re-run with `bench/compare/sql_analytics/run_analytics.sh 1000000 50`
> (needs the `lockstep-dev` image + pullable postgres:16 / clickhouse-server / python+duckdb).

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
