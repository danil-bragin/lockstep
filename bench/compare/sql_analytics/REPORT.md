# SQL analytics — Lockstep columnar vs PostgreSQL 16

Head-to-head on analytical SQL: identical deterministic data + identical queries, **both pinned
to the same single CPU** (cpu 0), in-memory (Postgres `fsync=off`, table fully cached after
`ANALYZE`), **no secondary indexes** on either side (raw analytical scan/aggregate). Run via
`bench/compare/sql_analytics/run_analytics.sh [N] [iters]`.

## Result (N = 200,000 rows, ms per query, lower = faster)

| query            | Lockstep columnar | Postgres 16 | speedup |
|------------------|------------------:|------------:|--------:|
| scan_agg         |            2.45   |     13.29   | **5.4×** |
| groupby_cat      |            5.90   |     19.42   | **3.3×** |
| groupby_region   |            7.78   |     22.36   | **2.9×** |
| filtered_agg     |            1.32   |      8.82   | **6.7×** |
| zone_skip        |            0.17   |      8.37   | **50×**  |

Lockstep columnar beats Postgres on **every** analytical shape, **2.9×–50×**. The win grows with
N (scan_agg was 4.3× at 50k, 5.4× at 200k) — the SoA vectorized scan scales better than Postgres's
row-at-a-time executor. `zone_skip` (a range predicate on a monotonic column) is **50×**: the
zone-map data skipping reads only the ~5% of chunks that can match, while Postgres full-scans.

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
