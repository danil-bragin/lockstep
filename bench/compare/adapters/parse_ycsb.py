#!/usr/bin/env python3
# parse_ycsb.py — extract committed throughput + p50/p99 from a go-ycsb run log.
#
# go-ycsb (v1.0.x) prints one summary row per op type, e.g.:
#   UPDATE - Takes(s): 1.0, Count: 20000, OPS: 19876.5, Avg(us): 402, Min(us): 90,
#            Max(us): 9000, 50th(us): 380, 90th(us): 700, 95th(us): 820,
#            99th(us): 900, 99.9th(us): 1500, 99.99th(us): 4000
# It does NOT print a TOTAL row in this version, so committed throughput = the SUM of the
# per-op OPS over the real op types (READ/UPDATE/INSERT/SCAN/READ_MODIFY_WRITE). Every
# such op is committed for these stores (pg COMMIT / etcd quorum put / cockroach committed
# txn / tikv committed).
#
# Latency: nearest-rank p50/p99 already provided by go-ycsb per op (microseconds). For a
# mixed workload we report the WORST (max across op types) p50/p99 — the conservative tail.
#
# Usage: parse_ycsb.py <run_log_path>
# Output (tab-separated, one line): <throughput|null>\t<p50|null>\t<p99|null>
import re
import sys

REAL_OPS = ("READ", "UPDATE", "INSERT", "SCAN", "READ_MODIFY_WRITE")
LINE_RE = re.compile(r"^([A-Z_]+)\s*-\s*Takes.*$", re.MULTILINE)


def field(line, name):
    mm = re.search(re.escape(name) + r"\s*:\s*([0-9.]+)", line)
    return float(mm.group(1)) if mm else None


def main():
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    ops, p50_by, p99_by = {}, {}, {}
    for lm in LINE_RE.finditer(text):
        op = lm.group(1)
        line = lm.group(0)
        o = field(line, "OPS")
        if o is not None:
            ops[op] = o  # last occurrence wins (final summary)
        p = field(line, "50th(us)")
        if p is not None:
            p50_by[op] = p
        q = field(line, "99th(us)")
        if q is not None:
            p99_by[op] = q

    real = [o for o in ops if o in REAL_OPS]
    tput = None
    if "TOTAL" in ops:
        tput = ops["TOTAL"]
    elif real:
        tput = sum(ops[o] for o in real)

    def worst(d):
        vals = [d[o] for o in real if o in d]
        return max(vals) if vals else None

    p50 = worst(p50_by)
    p99 = worst(p99_by)

    def s(x):
        return "null" if x is None else repr(x)

    print(f"{s(tput)}\t{s(p50)}\t{s(p99)}")


if __name__ == "__main__":
    main()
