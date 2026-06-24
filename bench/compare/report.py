#!/usr/bin/env python3
# report.py — aggregate results/*.json into comparison tables + an honest narrative.
# Reads every cell JSON, groups by (system, vector, workload, cpus, nodes), takes the
# MEDIAN throughput across passes (with min/max spread), and prints Markdown.
#
# Honesty rules: ok=false cells are EXCLUDED from medians and listed separately as
# failures. A group with zero ok passes shows "—". Absolute numbers are flagged RELATIVE
# (laptop Docker, shared host). Usage: python3 report.py [results_dir] > REPORT.md
import json, os, sys, statistics, glob

RES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "results")

cells = []
for f in glob.glob(os.path.join(RES, "*.json")):
    if os.path.basename(f).startswith("smoke_"):
        continue
    try:
        cells.append(json.load(open(f)))
    except Exception as e:
        print(f"<!-- skip {f}: {e} -->", file=sys.stderr)

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def fmt(x, n=0):
    if x is None:
        return "—"
    return f"{x:,.{n}f}"

# group ok cells by (system, vector, workload, cpus)
groups = {}
fails = []
def disp_system(c):
    # lockstep.sh always emits system="lockstep"; the multi-shard series is run under
    # vector="scaling" — surface it as a DISTINCT row so its scaling curve is visible.
    if c["system"] == "lockstep" and c["vector"] == "scaling":
        return "lockstep_sharded"
    return c["system"]

for c in cells:
    if not c.get("ok"):
        fails.append(c)
        continue
    k = (disp_system(c), c["vector"], c["workload"], c["cpus"])
    groups.setdefault(k, []).append(c)

def g_tput(k):
    return [c["throughput_ops_s"] for c in groups.get(k, [])]
def g_p(k, key):
    return [c[key] for c in groups.get(k, [])]

out = []
W = out.append
W("# Lockstep — Comparative Benchmark Report\n")
W("> **RELATIVE numbers only.** Laptop Docker Desktop (LinuxKit VM, Apple Silicon, 14 cores, "
  "23.7 GiB), shared host, real wall-clock. Absolute ops/s are meaningful only re-run on the "
  "SAME setup; the *shape* (scaling curve, win/loss direction, fault behavior) is the result. "
  "Throughput = **committed** ops/s; latency = nearest-rank p50/p99. Median of passes; "
  "[min–max] spread shown. ok=false cells excluded + listed at the end.\n")

systems = sorted({k[0] for k in groups})
cpus_seen = sorted({k[3] for k in groups})

# ---- KV write: throughput vs cpus (the scaling table) --------------------------------
W("## KV write — committed throughput (ops/s) vs server cores\n")
hdr = "| system | " + " | ".join(f"{c} cpu" for c in cpus_seen) + " |"
W(hdr); W("|" + "---|" * (len(cpus_seen) + 1))
for s in systems:
    row = [s]
    for c in cpus_seen:
        ts = g_tput((s, "kv", "write", c)) or g_tput((s, "scaling", "write", c))
        m = med(ts)
        if m is None:
            row.append("—")
        else:
            lo, hi = min(t for t in ts if t), max(t for t in ts if t)
            row.append(f"{fmt(m)} [{fmt(lo)}–{fmt(hi)}]")
    W("| " + " | ".join(row) + " |")
W("")

# ---- scaling factor vs 1 cpu ---------------------------------------------------------
W("## KV write — scaling factor (throughput at K cpu / throughput at 1 cpu)\n")
W("| system | " + " | ".join(f"{c} cpu" for c in cpus_seen) + " |")
W("|" + "---|" * (len(cpus_seen) + 1))
for s in systems:
    base = med(g_tput((s, "kv", "write", 1)) or g_tput((s, "scaling", "write", 1)))
    row = [s]
    for c in cpus_seen:
        m = med(g_tput((s, "kv", "write", c)) or g_tput((s, "scaling", "write", c)))
        row.append(f"{m/base:.2f}×" if (m and base) else "—")
    W("| " + " | ".join(row) + " |")
W("")

# ---- latency at a representative cpu level -------------------------------------------
def lat_table(workload, title):
    W(f"## {title} — latency (µs) @ matched concurrency\n")
    W("| system | cpu | p50 | p99 | throughput |")
    W("|---|---|---|---|---|")
    for s in systems:
        for c in cpus_seen:
            for vec in ("kv", "scaling"):
                k = (s, vec, workload, c)
                if k in groups:
                    W(f"| {s} | {c} | {fmt(med(g_p(k,'p50_us')),0)} | "
                      f"{fmt(med(g_p(k,'p99_us')),0)} | {fmt(med(g_tput(k)))} |")
                    break
    W("")
lat_table("write", "KV write")

# ---- rw5050 + sql if present ---------------------------------------------------------
rw = [k for k in groups if k[2] == "rw5050"]
if rw:
    W("## KV 50/50 read-update — committed throughput + tail\n")
    W("| system | cpu | throughput | p50 | p99 |")
    W("|---|---|---|---|---|")
    for k in sorted(rw):
        W(f"| {k[0]} | {k[3]} | {fmt(med(g_tput(k)))} | "
          f"{fmt(med(g_p(k,'p50_us')),0)} | {fmt(med(g_p(k,'p99_us')),0)} |")
    W("")

sql = [k for k in groups if k[1] == "sql"]
if sql:
    W("## SQL TPC-C-lite — txn throughput (over the wire)\n")
    W("| system | cpu | txn ops/s | p50 µs | p99 µs |")
    W("|---|---|---|---|---|")
    for k in sorted(sql):
        W(f"| {k[0]} | {k[3]} | {fmt(med(g_tput(k)))} | "
          f"{fmt(med(g_p(k,'p50_us')),0)} | {fmt(med(g_p(k,'p99_us')),0)} |")
    W("> NOTE: Lockstep SQL is in-process (not over the wire) — measured separately in "
      "bench/sql_bench (the asymmetry named in SPEC.md). These are the over-socket SQL systems.\n")

# ---- fault vector --------------------------------------------------------------------
flt = [c for c in cells if c.get("vector") == "fault" and c.get("ok")]
if flt:
    W("## Fault / HA — leader kill (recovery + zero-acked-loss)\n")
    W("| system | recovery ms | acked lost | consistent | throughput |")
    W("|---|---|---|---|---|")
    for c in flt:
        fa = c.get("fault", {})
        W(f"| {c['system']} | {fmt(fa.get('recovery_ms'),0)} | {fa.get('acked_lost')} | "
          f"{fa.get('consistent')} | {fmt(c.get('throughput_ops_s'))} |")
    W("")

# ---- failures ------------------------------------------------------------------------
if fails:
    W("## Failed cells (ok=false — excluded from medians)\n")
    seen = set()
    for c in fails:
        key = (c.get("system"), c.get("vector"), c.get("workload"), c.get("cpus"))
        if key in seen:
            continue
        seen.add(key)
        W(f"- `{c.get('system')}/{c.get('vector')}/{c.get('workload')}/cpu{c.get('cpus')}` — "
          f"{c.get('notes','')[:160]}")
    W("")

W(f"\n<!-- {len(cells)} cells, {len(groups)} ok groups, {len(fails)} failed -->")
print("\n".join(out))
