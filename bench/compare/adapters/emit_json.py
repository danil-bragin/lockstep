#!/usr/bin/env python3
# emit_json.py — write ONE SCHEMA-conformant cell JSON to stdout.
#
# All scalar cell fields come from the environment (the adapter exports them); the
# measurement-dependent fields come from argv so a failure path can pass nulls.
#
# Usage:
#   emit_json.py <ok:true|false> <throughput|null> <p50|null> <p99|null> <raw> <notes>
# Reads from env: CELL_ID SYSTEM VECTOR WORKLOAD CPUS MEM_G NODES SHARDS
#                 CONCURRENCY VALUE_BYTES OP_COUNT PASS
import json, os, sys


def num(x):
    if x in ("null", "", None):
        return None
    try:
        return float(x)
    except (ValueError, TypeError):
        return None


def main():
    ok, tput, p50, p99, raw, notes = (sys.argv[1:7] + [""] * 6)[:6]
    obj = {
        "cell_id":     os.environ["CELL_ID"],
        "system":      os.environ["SYSTEM"],
        "vector":      os.environ.get("VECTOR", "kv"),
        "workload":    os.environ["WORKLOAD"],
        "cpus":        int(os.environ["CPUS"]),
        "mem_g":       int(os.environ["MEM_G"]),
        "nodes":       int(os.environ.get("NODES", "1")),
        "shards":      int(os.environ.get("SHARDS", "1")),
        "concurrency": int(os.environ["CONCURRENCY"]),
        "value_bytes": int(os.environ.get("VALUE_BYTES", "0")),
        "op_count":    int(os.environ.get("OP_COUNT", "0")),
        "pass":        int(os.environ["PASS"]),
        "throughput_ops_s": num(tput),
        "p50_us":      num(p50),
        "p99_us":      num(p99),
        "ok":          (ok == "true"),
        "raw":         raw,
        "notes":       notes,
    }
    print(json.dumps(obj, indent=2))


if __name__ == "__main__":
    main()
