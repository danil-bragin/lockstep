#!/usr/bin/env python3
# parse_tpcc.py — extract committed txn throughput + p50/p99 from `cockroach workload run
# tpcc` output.
#
# cockroach-workload ends a run with per-txn-type "__total" blocks AND one overall aggregate
# block tagged "__result". The label lives in the HEADER line; the numeric data is the NEXT
# line (its trailing label column is blank). Example:
#
#   _elapsed__errors__ops(total)__ops/sec(cum)__avg(ms)__p50(ms)__p95(ms)__p99(ms)_pMax(ms)__result
#      10.0s     0       1924        192.4      41.4    14.7    151.0    251.7    469.8
#
#   throughput := ops/sec(cum)   (committed txn-ops/s; tolerated errors are in the _errors
#                                  column and excluded from ops(total))
#   p50_us     := p50(ms) * 1000   ;   p99_us := p99(ms) * 1000
#
# Fallback: the per-type "__total" header+row, else a tpmC line.
#
# Usage: parse_tpcc.py <run_log_path>
# Output (tab): <throughput|null>\t<p50_us|null>\t<p99_us|null>
import re
import sys


def _nums(line):
    # Columns: elapsed is "10.0s" (trailing unit), the rest are bare numbers. Strip a single
    # trailing alpha unit (s) so the elapsed column is captured in position.
    out = []
    for tok in line.split():
        t = re.sub(r"s$", "", tok)            # "10.0s" -> "10.0"
        if re.fullmatch(r"[0-9.]+", t):
            out.append(float(t))
    return out


def _row_after_header(lines, label):
    # Find the LAST header line whose label column == `label`; return numbers from the next
    # non-blank line.
    idx = None
    for i, ln in enumerate(lines):
        if ln.rstrip().endswith(label) and "ops/sec(cum)" in ln:
            idx = i
    if idx is None:
        return None
    for ln in lines[idx + 1:]:
        if ln.strip():
            return _nums(ln)
    return None


def main():
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    tput = p50 = p99 = None

    for label in ("__result", "__total"):
        nums = _row_after_header(lines, label)
        # [elapsed, errors, ops(total), ops/sec, avg, p50, p95, p99, pMax]
        if nums and len(nums) >= 8:
            tput = nums[3]
            p50 = nums[5] * 1000.0
            p99 = nums[7] * 1000.0
            break

    if tput is None:
        m = re.search(r"([0-9.]+)\s+tpmC", text)
        if m:
            tput = float(m.group(1)) / 60.0

    def s(x):
        return "null" if x is None else repr(x)

    print(f"{s(tput)}\t{s(p50)}\t{s(p99)}")


if __name__ == "__main__":
    main()
