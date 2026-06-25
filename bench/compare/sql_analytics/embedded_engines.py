#!/usr/bin/env python3
# Embedded analytics competitors for the SQL-analytics comparison, all in one process so the
# orchestrator pins ONE container: DuckDB + ClickHouse (chdb) + SQLite. Same deterministic data
# formula + same queries as lockstep_analytics.cpp. Each query timed warm over ITERS reps; one
# compact JSON line per (engine,query).  argv: N ITERS
import sys, time, json

N = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
ITERS = int(sys.argv[2]) if len(sys.argv) > 2 else 30
TSHI = N - N // 20

QORDER = ["scan_agg", "groupby_cat", "groupby_region", "filtered_agg", "zone_skip"]
def queries(tbl):
    return {
        "scan_agg":       f"SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM {tbl}",
        "groupby_cat":    f"SELECT cat, COUNT(*), SUM(amount) FROM {tbl} GROUP BY cat",
        "groupby_region": f"SELECT region, COUNT(*), SUM(amount) FROM {tbl} GROUP BY region",
        "filtered_agg":   f"SELECT COUNT(*), SUM(amount) FROM {tbl} WHERE amount > 800",
        "zone_skip":      f"SELECT COUNT(*), SUM(amount) FROM {tbl} WHERE ts > {TSHI}",
    }
def emit(sys_name, name, ms):
    print(json.dumps({"sys": sys_name, "q": name, "ms_each": round(ms / ITERS, 4)},
                     separators=(",", ":")), flush=True)

# ---- DuckDB ---------------------------------------------------------------------------------
try:
    import duckdb
    con = duckdb.connect(); con.execute("PRAGMA threads=1")
    con.execute(f"""CREATE TABLE events AS SELECT i::INT id,(i%10000)::INT uid,(i%8)::INT cat,
        ((i*2654435761)%1000)::INT amount,(['north','south','east','west','central'])[(i%5)+1] region,
        i::INT ts FROM range({N}) t(i)""")
    for q in QORDER:
        sql = queries("events")[q]; con.execute(sql).fetchall()
        t0 = time.perf_counter()
        for _ in range(ITERS): con.execute(sql).fetchall()
        emit("duckdb", q, (time.perf_counter()-t0)*1000)
except Exception as e:
    sys.stderr.write(f"duckdb skipped: {e}\n")

# (ClickHouse is benched separately via the clickhouse-server image — chdb's pip wheel is too
#  heavy to install inline.)

# ---- SQLite (embedded row store) ------------------------------------------------------------
try:
    import sqlite3
    cx = sqlite3.connect(":memory:")
    cx.execute("CREATE TABLE events (id INT, uid INT, cat INT, amount INT, region TEXT, ts INT)")
    regs = ['north','south','east','west','central']
    rows = ((i, i%10000, i%8, (i*2654435761)%1000, regs[i%5], i) for i in range(N))
    cx.executemany("INSERT INTO events VALUES (?,?,?,?,?,?)", rows)
    cx.execute("CREATE INDEX IF NOT EXISTS i_ts ON events(ts)")  # SQLite has no columnar skip
    cx.commit()
    for q in QORDER:
        sql = queries("events")[q]; cx.execute(sql).fetchall()
        t0 = time.perf_counter()
        for _ in range(ITERS): cx.execute(sql).fetchall()
        emit("sqlite", q, (time.perf_counter()-t0)*1000)
except Exception as e:
    sys.stderr.write(f"sqlite skipped: {e}\n")
