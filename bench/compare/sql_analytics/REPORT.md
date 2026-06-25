# SQL analytics — Lockstep columnar vs PostgreSQL 16

Head-to-head on analytical SQL: identical deterministic data + identical queries, **both pinned
to the same single CPU** (cpu 0), in-memory (Postgres `fsync=off`, table fully cached after
`ANALYZE`), **no secondary indexes** on either side (raw analytical scan/aggregate). Run via
`bench/compare/sql_analytics/run_analytics.sh [N] [iters]`.

## Result (N = 200,000 rows, ms per query, lower = faster) — 3-way: Lockstep vs Postgres vs DuckDB

| query          | Lockstep | Postgres | DuckDB | vs Postgres | vs DuckDB |
|----------------|---------:|---------:|-------:|------------:|----------:|
| scan_agg       |    0.58  |   12.94  |  0.46  | **22.5×**   | 0.79×     |
| groupby_cat    |    5.86  |   18.53  |  0.51  | **3.2×**    | 0.09×     |
| groupby_region |    7.85  |   22.50  |  2.25  | **2.9×**    | 0.29×     |
| filtered_agg   |    1.34  |    8.97  |  0.22  | **6.7×**    | 0.16×     |
| zone_skip      |    0.16  |    8.35  |  0.089 | **52×**     | 0.55×     |

(`vs Postgres` / `vs DuckDB` = competitor_ms / lockstep_ms; >1 = Lockstep faster.)

**vs Postgres 16 (row store):** Lockstep wins on EVERY shape, **2.9×–52×**. scan_agg jumped to 22×
after the SIMD contiguous-fold fast path; zone_skip is 52× (zone-map skips ~95% of chunks while
Postgres full-scans). The Postgres win grows with N (SoA scales better than row-at-a-time).

**vs DuckDB (the columnar leader, single-thread):** an HONEST baseline — DuckDB is faster on every
query. Lockstep is COMPETITIVE on scan_agg (0.79×, ~1.3× behind — the SIMD fold closed most of the
gap) and zone_skip (0.55×). The big gap is GROUP BY (0.09× on groupby_cat — DuckDB's mature
vectorized hash-aggregate vs Lockstep's ordered-map + per-column fold). GROUP BY is the clear next
lever to approach DuckDB; a proper vectorized hash-aggregate (the naive map-swap + running-
accumulator both measured slower — see commit 29cbd34) is the real fix.

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
