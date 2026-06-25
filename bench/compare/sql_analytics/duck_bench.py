#!/usr/bin/env python3
# DuckDB side of the SQL-analytics comparison (the real columnar competitor; Postgres is a
# row store). Same deterministic data formula + same queries as lockstep_analytics.cpp. Each
# query is timed over ITERS reps (warm); emits one JSON line per query.
#   argv: N ITERS
import sys, time, json
import duckdb

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 30
TSHI = N - N // 20

con = duckdb.connect()
con.execute("PRAGMA threads=1")  # single-core, to match the pinned comparison
con.execute(f"""
    CREATE TABLE events AS
    SELECT i::INT AS id, (i%10000)::INT AS uid, (i%8)::INT AS cat,
           ((i*2654435761)%1000)::INT AS amount,
           (['north','south','east','west','central'])[(i%5)+1] AS region,
           i::INT AS ts
    FROM range({N}) t(i)
""")

queries = {
    "scan_agg":       "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM events",
    "groupby_cat":    "SELECT cat, COUNT(*), SUM(amount) FROM events GROUP BY cat",
    "groupby_region": "SELECT region, COUNT(*), SUM(amount) FROM events GROUP BY region",
    "filtered_agg":   "SELECT COUNT(*), SUM(amount) FROM events WHERE amount > 800",
    "zone_skip":      f"SELECT COUNT(*), SUM(amount) FROM events WHERE ts > {TSHI}",
}
for name in ["scan_agg", "groupby_cat", "groupby_region", "filtered_agg", "zone_skip"]:
    q = queries[name]
    con.execute(q).fetchall()  # warm
    t0 = time.perf_counter()
    for _ in range(ITERS):
        con.execute(q).fetchall()
    ms = (time.perf_counter() - t0) * 1000.0
    print(json.dumps({"sys": "duckdb", "q": name, "iters": ITERS,
                      "ms_total": round(ms, 2), "ms_each": round(ms / ITERS, 4)},
                     separators=(",", ":")))
