#!/usr/bin/env python3
# Tests for the Lockstep forbidden-call lint (Phase 0, spec C0.2).
#
# Runnable as: python3 tools/lint/test_forbidden_calls.py
#   exit 0 on success, non-zero on any failed assertion.
#
# Strategy:
#   - Each dirty fixture is scanned directly by path and asserted to be flagged,
#     with the SPECIFIC forbidden category present in the findings.
#   - The clean fixtures are asserted to produce ZERO findings.
#   - The providers/ fixture is asserted exempt (skipped by the file walker)
#     even though it contains forbidden constructs.
#   - The whole-repo run is asserted to skip tools/lint/fixtures/ (so the real
#     gate stays green while tests target fixtures by path).

from __future__ import annotations

import io
import os
import sys
from contextlib import redirect_stdout

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(THIS_DIR, "fixtures")
sys.path.insert(0, THIS_DIR)

import forbidden_calls as fc  # noqa: E402

_failures = []


def check(cond, msg):
    if cond:
        print(f"  PASS: {msg}")
    else:
        print(f"  FAIL: {msg}")
        _failures.append(msg)


def labels_for(rel_under_fixtures):
    """Scan one fixture file directly and return the set of hit labels."""
    path = os.path.join(FIX, rel_under_fixtures)
    return {label for _line, label in fc.scan_file(path)}


def run_root(root):
    """Run the full linter over `root`; return (exit_code, stdout_text)."""
    buf = io.StringIO()
    with redirect_stdout(buf):
        code = fc.run(root)
    return code, buf.getvalue()


def main():
    # --- Dirty fixtures: every category must be caught. -------------------
    print("[dirty] each forbidden category is flagged")

    clock = labels_for("dirty/clock.cpp")
    check("std::chrono" in clock, "std::chrono flagged (canonical system_clock::now)")

    clock_h = labels_for("dirty/clock_header.hpp")
    check("std::chrono" in clock_h, "std::chrono flagged in .hpp header")

    rnd = labels_for("dirty/random.cpp")
    check("std::rand" in rnd, "std::rand flagged")
    check("std::random_device" in rnd, "std::random_device flagged")
    check("std::mt19937" in rnd, "std::mt19937 engine flagged")
    check("std::default_random_engine" in rnd, "std::default_random_engine flagged")
    check("<random>" in rnd, "<random> include flagged")

    thr = labels_for("dirty/threads.cpp")
    check("std::thread" in thr, "std::thread flagged")
    check("std::jthread" in thr, "std::jthread flagged")
    check("<thread>" in thr, "<thread> include flagged")

    sock = labels_for("dirty/sockets.cpp")
    for fn in ("socket", "bind", "connect", "accept", "listen",
               "send", "recv", "sendto", "recvfrom"):
        check(f"{fn}(" in sock, f"raw socket syscall {fn}( flagged")
    check("<sys/socket.h>" in sock, "<sys/socket.h> include flagged")

    fio = labels_for("dirty/fileio.cpp")
    check("::open(" in fio, "::open( flagged")
    check("::read(" in fio, "::read( flagged")
    check("::write(" in fio, "::write( flagged")
    check("::close(" in fio, "::close( flagged")
    check("lseek(" in fio, "lseek( flagged")
    check("fsync(" in fio, "fsync( flagged")
    check("<fcntl.h>" in fio, "<fcntl.h> include flagged")
    check("<unistd.h>" in fio, "<unistd.h> include flagged")

    atom = labels_for("dirty/atomics.cpp")
    check("std::memory_order_" in atom, "std::memory_order_* flagged")
    check("std::atomic_thread_fence" in atom, "std::atomic_thread_fence flagged")

    # --- Declaration guard must NEVER hide a real call (no false negatives).
    print("[stress] declaration-guard does not mask real syscall calls")
    stress = labels_for("dirty/syscall_calls_stress.cpp")
    for need in ("send(", "recv(", "connect(", "accept(",
                 "::write(", "fsync(", "lseek("):
        check(need in stress, f"real call {need} still flagged (guard safe)")

    # --- Clean fixtures: zero findings. -----------------------------------
    print("[clean] allowed-only fixtures produce no findings")
    check(labels_for("clean/clean_ok.cpp") == set(),
          "clean_ok.cpp has zero findings (comments/strings/member-calls ignored)")
    check(labels_for("clean/clean_header.hpp") == set(),
          "clean_header.hpp has zero findings")

    # --- Directory-level runs --------------------------------------------
    print("[dirs] directory runs behave correctly")

    code_dirty, out_dirty = run_root(os.path.join(FIX, "dirty"))
    check(code_dirty != 0, "running on dirty/ exits non-zero")
    check("dirty/clock.cpp:" in out_dirty.replace(os.sep, "/")
          or "clock.cpp:" in out_dirty,
          "dirty/ run prints machine-readable path:line: hits")
    # providers/ inside dirty/ must NOT appear in findings (exempt).
    check("providers/ok.cpp" not in out_dirty.replace(os.sep, "/"),
          "providers/ fixture is EXEMPT even though it uses std::chrono etc.")

    code_clean, out_clean = run_root(os.path.join(FIX, "clean"))
    check(code_clean == 0, "running on clean/ exits 0")
    check("OK" in out_clean, "clean/ run prints OK summary")

    # --- Whole-repo run excludes the fixtures dir ------------------------
    print("[repo] whole-repo run excludes tools/lint/fixtures/")
    repo_root = fc._default_root()
    code_repo, out_repo = run_root(repo_root)
    norm = out_repo.replace(os.sep, "/")
    check("tools/lint/fixtures/" not in norm,
          "no fixture path appears in whole-repo findings (fixtures excluded)")

    # --- Report ----------------------------------------------------------
    print()
    if _failures:
        print(f"RESULT: FAIL — {len(_failures)} assertion(s) failed.")
        return 1
    print("RESULT: PASS — all assertions passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
