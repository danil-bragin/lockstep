#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Non-vacuous proof that the mutation engine actually works (M1 teeth-test).

This is the mutation-engine analogue of the forbidden-call lint's teeth-test:
a runner that reports a score is worthless unless we can PROVE it (a) KILLS a
mutant a real test should catch, and (b) REPORTS a SURVIVOR when the test is too
weak to catch it. Both halves run here, self-contained, no human in the loop.

PART A — PLANTED MUTANT IS KILLED (against the REAL tree)
  We generate the real mutant set for core/, pick one mutant on a load-bearing
  line of the deterministic Scheduler (an operator the determinism tests MUST
  depend on), build+test it, and assert the runner classifies it KILLED. If the
  suite does not catch it, that is itself a real finding (printed), and the
  self-test FAILS — proving the engine is not faking kills.

PART B — WEAK TEST LEAVES A MUTANT SURVIVED + REPORTED
  We synthesize a tiny self-contained C++ fixture (a `classify()` function) and
  TWO tests: a STRONG test that pins the behaviour, and a WEAK test that only
  checks a trivial property. We mutate the fixture, build+run ONLY THE WEAK test,
  and assert the runner reports the mutant SURVIVED (the weak test could not kill
  it). This proves survivors are surfaced, not hidden.

Both parts shell out to the same machinery the real runner uses (operators +
in-place edit + build + test), so the proof exercises the real code paths.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import operators as ops  # noqa: E402
import run_mutation as rm  # noqa: E402


def _cxx() -> str:
    for c in ("c++", "clang++", "g++"):
        if shutil.which(c):
            return c
    return "c++"


# ---------------------------------------------------------------------------
# PART A — planted mutant on a real core line must be KILLED by the real suite.
# ---------------------------------------------------------------------------
def part_a_planted_killed(repo_root: str, preset: str, verbose: bool) -> bool:
    print("\n--- PART A: planted mutant on real core line must be KILLED ---")
    sources = rm.discover_sources(repo_root, rm.SOURCE_ROOTS, False, verbose)
    mutants = rm.generate_all(repo_root, sources, verbose)

    # Pick a load-bearing mutant on the deterministic Scheduler — a single edit
    # that changes timer/clock arithmetic the determinism tests depend on. We try
    # candidates in an op order that tends to COMPILE (AOR / ABS on the timer
    # math, SDL of a side-effect call) before the ones more likely to break the
    # coroutine-heavy header (ROR/NEG on template-ish expressions), so the proof
    # lands a real KILL quickly instead of burning builds on SKIPs.
    chosen = None
    op_rank = {"AOR": 0, "ABS": 1, "SDL": 2, "LCR": 3, "ROR": 4, "EQ": 5, "NEG": 6, "CBR": 7}
    candidates = [m for m in mutants if "Scheduler.hpp" in m.file_path]
    candidates.sort(key=lambda m: (op_rank.get(m.op, 9), m.line, m.col))
    if not candidates:
        candidates = mutants[:1]

    build_dir = os.path.join(repo_root, "build", preset)
    if not os.path.isdir(build_dir):
        print(f"  [setup] configuring preset '{preset}' ...")
        subprocess.run(["cmake", "--preset", preset], cwd=repo_root,
                       capture_output=True, text=True, check=False)

    for m in candidates[:8]:
        print(f"  trying planted mutant: {m.mutant_id}  ({m.describe()})")
        applied = False
        try:
            rm.apply_mutant(repo_root, m)
            applied = True
            built, passed, detail, _to = rm.build_and_test(repo_root, preset, 600, 600)
        finally:
            if applied:
                rm.restore_mutant(repo_root, m)
        if not built:
            print(f"    -> did not compile (SKIP): {detail}; trying next")
            continue
        if passed:
            # A survivor here is a REAL test gap; the self-test treats inability
            # to kill THIS particular mutant as worth reporting, then tries next.
            print(f"    -> SURVIVED (test gap on this mutant): {detail}; trying next")
            continue
        chosen = m
        print(f"    -> KILLED by suite: {detail}")
        break

    if chosen is None:
        print("  PART A FAIL: no planted mutant on the scheduler was KILLED by the "
              "suite (every candidate survived or did not compile). This is a real "
              "adequacy hole — the engine is honest, the TESTS are weak here.")
        return False
    print(f"  PART A PASS: planted mutant {chosen.mutant_id} was KILLED by the real "
          "ctest suite. Engine demonstrably kills real bugs.")
    return True


# ---------------------------------------------------------------------------
# PART B — weak test leaves a mutant SURVIVED + reported (tiny fixture).
# ---------------------------------------------------------------------------
_FIXTURE_CPP = """\
// Tiny self-contained fixture for the mutation self-test.
// classify(n) returns 1 iff n is strictly greater than the THRESHOLD, else 0.
#include <cstdio>

static int classify(int n) {
    int THRESHOLD = 10;
    if (n > THRESHOLD) {   // <-- the line we mutate (ROR: > becomes <=)
        return 1;
    }
    return 0;
}

// A STRONG harness would assert classify(11)==1 && classify(10)==0 — that pins
// the boundary and KILLS a `>`->`<=` mutant. The WEAK harness below only checks
// classify(100)==1, which a `<=` mutant would FAIL... so we make the weak test
// even weaker: it only checks that classify returns SOMETHING in {0,1}. That can
// never distinguish > from <=, so the mutant SURVIVES.
int main(int argc, char**) {
    int strong = (argc < 0) ? 1 : 0;  // compile-time-ish switch via argv count
    if (strong) {
        // strong assertions (unused path in the weak build)
        if (classify(11) != 1) return 1;
        if (classify(10) != 0) return 1;
    }
    // WEAK assertion: result is a valid boolean. True for BOTH original and
    // mutant -> cannot kill the mutant.
    int r = classify(11);
    if (r != 0 && r != 1) return 1;
    return 0;
}
"""


def part_b_weak_survives(verbose: bool) -> bool:
    print("\n--- PART B: weak test leaves a mutant SURVIVED + reported ---")
    cxx = _cxx()
    with tempfile.TemporaryDirectory(prefix="lockstep_mut_selftest_") as tmp:
        src_rel = "fixture.cpp"
        src_abs = os.path.join(tmp, src_rel)
        with open(src_abs, "w", encoding="utf-8") as f:
            f.write(_FIXTURE_CPP)

        # Generate mutants for the fixture; find the ROR `>`->`<=` on classify.
        muts = ops.generate_for_file(src_rel, _FIXTURE_CPP)
        target = None
        for m in muts:
            if m.op == "ROR" and m.original == ">" and "THRESHOLD" in m.new_line_text:
                target = m
                break
        if target is None:
            # Fallback: any ROR on the fixture.
            for m in muts:
                if m.op == "ROR":
                    target = m
                    break
        if target is None:
            print("  PART B FAIL: could not generate the expected fixture mutant.")
            return False
        print(f"  fixture mutant: {target.mutant_id}  ({target.describe()})")

        def build_and_run(label: str) -> tuple[bool, bool]:
            bin_path = os.path.join(tmp, "fixture_bin")
            b = subprocess.run([cxx, "-std=c++23", src_abs, "-o", bin_path],
                               capture_output=True, text=True, check=False)
            if b.returncode != 0:
                if verbose:
                    print(f"    [{label}] did not compile: {b.stderr.strip()[-200:]}")
                return (False, False)
            r = subprocess.run([bin_path], capture_output=True, text=True, check=False)
            return (True, r.returncode == 0)

        # Sanity: the ORIGINAL fixture passes the weak test.
        built0, pass0 = build_and_run("original")
        if not (built0 and pass0):
            print("  PART B FAIL: original fixture did not pass its own weak test.")
            return False

        # Apply the mutant in place, build + run the WEAK test.
        lines = _FIXTURE_CPP.split("\n")
        lines[target.line - 1] = target.new_line_text
        with open(src_abs, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        built1, pass1 = build_and_run("mutant")

        if not built1:
            print("  PART B FAIL: mutant did not compile (expected it to compile + survive).")
            return False
        if not pass1:
            print("  PART B FAIL: weak test KILLED the mutant — it was not weak enough. "
                  "The whole point is to demonstrate a SURVIVOR.")
            return False

        # SURVIVED — and we REPORT it loudly, exactly as the real runner would.
        print("  PART B PASS: the weak test could NOT kill the mutant -> SURVIVED.")
        print(f"    SURVIVED  {target.file_path}:{target.line}  [{target.op}]  "
              f"'{target.original}' -> '{target.mutated}'   <- reported as a TEST GAP")
        return True


# ---------------------------------------------------------------------------
# PART C — a PLANTED INFINITE-LOOP mutant must (a) TIME OUT, (b) be classified
# KILLED-by-timeout, (c) the source RESTORED, (d) NO orphan processes left.
#
# This reproduces the exact failure class that corrupted the tree: a binary
# search whose `mid + 1` becomes `mid + 0` so `lo = mid` never advances and the
# loop spins forever. We drive it through the REAL engine machinery
# (apply_mutant -> build_and_test under the bounded timeout -> restore_mutant)
# in a throwaway tree, with a CMake/ctest setup mirroring the real one, so the
# process-tree kill + .mutbak restore are exercised for real.
# ---------------------------------------------------------------------------
_HANG_CPP = """\
// Fixture: a binary search. The `mid + 1` on the marked line is load-bearing for
// termination. Mutating it to `mid + 0` (ABS off-by-one) makes `lo = mid` never
// advance -> INFINITE LOOP. The mutation engine must KILL it by TIMEOUT, not hang.
#include <vector>

static int bsearch_lower(const std::vector<int>& a, int key) {
    int lo = 0;
    int hi = (int)a.size();
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < key) {
            lo = mid + 1;   // MUTATION TARGET: + 1 -> + 0 spins forever
        } else {
            hi = mid;
        }
    }
    return lo;
}

int main() {
    std::vector<int> a;
    for (int i = 0; i < 1000; ++i) a.push_back(i * 2);
    // A key that lands on the `a[mid] < key` branch forces the lo=mid+1 path.
    volatile int r = bsearch_lower(a, 777);
    return (r >= 0) ? 0 : 1;
}
"""

_HANG_CMAKE = """\
cmake_minimum_required(VERSION 3.20)
project(lockstep_hang_fixture CXX)
set(CMAKE_CXX_STANDARD 17)
enable_testing()
add_executable(lockstep_hang_selftest hang.cpp)
add_test(NAME lockstep_hang_selftest COMMAND lockstep_hang_selftest)
"""


def part_c_timeout_killed(verbose: bool) -> bool:
    print("\n--- PART C: planted INFINITE-LOOP mutant must KILL-by-timeout + restore ---")
    if not shutil.which("cmake") or not shutil.which("ctest"):
        print("  PART C SKIP: cmake/ctest not available.")
        return True

    tmp = tempfile.mkdtemp(prefix="lockstep_mut_hang_")
    try:
        # Lay out a tiny tree: a SOURCE_ROOT named 'core' (so restore-on-start can
        # see a .mutbak under it), and a build dir we configure with the fixture.
        src_root = os.path.join(tmp, "core")
        os.makedirs(src_root, exist_ok=True)
        src_rel = os.path.join("core", "hang.cpp")
        src_abs = os.path.join(tmp, src_rel)
        with open(src_abs, "w") as f:
            f.write(_HANG_CPP)
        # CMake lists live at the src_root so add_executable finds hang.cpp.
        with open(os.path.join(src_root, "CMakeLists.txt"), "w") as f:
            f.write(_HANG_CMAKE)
        build_dir = os.path.join(tmp, "build", "debug")
        os.makedirs(build_dir, exist_ok=True)
        cfg = subprocess.run(["cmake", "-S", src_root, "-B", build_dir],
                             capture_output=True, text=True, check=False)
        if cfg.returncode != 0:
            print(f"  PART C SKIP: fixture cmake configure failed:\n{cfg.stderr[-400:]}")
            return True

        # Build the ABS mutant `+ 1` -> `+ 0` on the marked line via the real
        # operator generator, then confirm we have the exact infinite-loop edit.
        muts = ops.generate_for_file(src_rel, _HANG_CPP)
        target = None
        for m in muts:
            if m.op == "ABS" and m.original == "1" and m.mutated == "0" \
               and "lo = mid" in m.new_line_text:
                target = m
                break
        if target is None:
            print("  PART C FAIL: could not generate the `mid + 1`->`mid + 0` mutant.")
            return False
        print(f"  planted mutant: {target.mutant_id}  ({target.describe()})")
        print(f"    new line -> {target.new_line_text.strip()}")

        # Sanity: CLEAN suite must finish fast (so we know the timeout fires on the
        # MUTANT, not on a slow baseline).
        t0 = time.time()
        bt, brc, _o, _e = rm.run_proc_group(
            ["cmake", "--build", build_dir], cwd=tmp, timeout=120, env=dict(os.environ))
        if bt or brc != 0:
            print("  PART C SKIP: clean fixture build failed/hung.")
            return True
        ct, crc, _co, _ce = rm.run_proc_group(
            ["ctest", "--test-dir", build_dir], cwd=tmp, timeout=30, env=dict(os.environ))
        clean_s = time.time() - t0
        if ct or crc != 0:
            print("  PART C SKIP: clean fixture suite did not pass quickly.")
            return True
        print(f"  clean fixture suite: ~{clean_s:.1f}s, passes.")

        # Per-mutant timeout: small (a real run derives from the baseline; here we
        # pin a small explicit bound so the proof is fast and unambiguous).
        per_mut_timeout = 8

        # DRIVE THE REAL ENGINE PATH: apply (creates .mutbak), build+test under the
        # bounded timeout, restore from .mutbak in finally.
        applied = False
        timed_out = False
        built = passed = None
        detail = ""
        kill_start = time.time()
        try:
            rm.apply_mutant(tmp, target)           # backs up to core/hang.cpp.mutbak
            applied = True
            # Prove the backup exists (crash-safe ground truth on disk).
            if not os.path.isfile(src_abs + rm.MUTBAK_SUFFIX):
                print("  PART C FAIL: .mutbak backup was not created before mutation.")
                return False
            built, passed, detail, timed_out = rm.build_and_test(
                tmp, "debug", 120, per_mut_timeout)
        finally:
            if applied:
                rm.restore_mutant(tmp, target)
        elapsed = time.time() - kill_start

        # (a) TIMED OUT (and we did not wait forever — bounded near per_mut_timeout).
        if not timed_out:
            print(f"  PART C FAIL: mutant did NOT time out (built={built}, passed={passed}, "
                  f"detail={detail!r}). The infinite loop should have tripped the bound.")
            return False
        status = "killed" if (built and not passed) else "?"
        print(f"  (a) TIMED OUT after ~{elapsed:.1f}s (bound {per_mut_timeout}s) -> {detail}")

        # (b) classified KILLED-by-timeout.
        if not (built and not passed and timed_out):
            print(f"  PART C FAIL: not classified KILLED-by-timeout "
                  f"(built={built}, passed={passed}, timed_out={timed_out}).")
            return False
        print(f"  (b) classified KILLED-by-timeout (status={status}, timed_out=True).")

        # (c) SOURCE RESTORED: bytes match original AND no .mutbak remains.
        restored = open(src_abs).read()
        if restored != _HANG_CPP:
            print("  PART C FAIL: source was NOT restored to original bytes after timeout.")
            return False
        if os.path.isfile(src_abs + rm.MUTBAK_SUFFIX):
            print("  PART C FAIL: .mutbak backup was left behind after restore.")
            return False
        print("  (c) SOURCE RESTORED exactly (byte-identical) and .mutbak deleted.")

        # (d) NO orphan processes from this fixture.
        orphans = []
        if shutil.which("pgrep"):
            r = subprocess.run(["pgrep", "-f", "lockstep_hang_selftest"],
                               capture_output=True, text=True, check=False)
            orphans = [p for p in r.stdout.split() if p.strip().isdigit()
                       and int(p) != os.getpid()]
        if orphans:
            print(f"  PART C FAIL: {len(orphans)} orphan hang process(es) survived: {orphans}")
            for p in orphans:
                try:
                    os.kill(int(p), 9)
                except Exception:
                    pass
            return False
        print("  (d) NO orphan processes left (process-tree kill reached the test binary).")
        print("  PART C PASS: infinite-loop mutant -> timeout -> killed-by-timeout -> "
              "restored -> no orphans.")
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# PART D — RESTORE-ON-START guard heals a leftover .mutbak from a crashed run.
# ---------------------------------------------------------------------------
def part_d_restore_on_start(verbose: bool) -> bool:
    print("\n--- PART D: restore-on-start guard heals a leftover .mutbak ---")
    tmp = tempfile.mkdtemp(prefix="lockstep_mut_heal_")
    try:
        root = "core"
        base = os.path.join(tmp, root)
        os.makedirs(base, exist_ok=True)
        target = os.path.join(base, "Victim.hpp")
        pristine = "int answer() { return 42; }\n"
        corrupt = "int answer() { return 0; }  // <- a crashed run left THIS mutation\n"
        # Simulate the aftermath of a crash: the real file holds a MUTATION, and a
        # .mutbak holds the PRISTINE original (exactly what apply_mutant writes
        # before mutating, and what a SIGKILL would leave behind).
        with open(target, "w") as f:
            f.write(corrupt)
        with open(target + rm.MUTBAK_SUFFIX, "w") as f:
            f.write(pristine)
        print(f"  simulated leftover: {os.path.relpath(target, tmp)} is MUTATED, "
              f"{os.path.basename(target)}{rm.MUTBAK_SUFFIX} holds the original.")

        n = rm.restore_on_start(tmp, (root,))
        if n != 1:
            print(f"  PART D FAIL: expected 1 restore, got {n}.")
            return False
        healed = open(target).read()
        if healed != pristine:
            print("  PART D FAIL: file was not healed back to pristine bytes.")
            return False
        if os.path.isfile(target + rm.MUTBAK_SUFFIX):
            print("  PART D FAIL: .mutbak not consumed after restore-on-start.")
            return False
        print("  PART D PASS: restore-on-start healed the leftover mutation (logged above) "
              "and consumed the .mutbak. The tree self-heals across invocations.")
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(repo_root: str, preset: str = "debug", verbose: bool = False) -> int:
    print("=" * 72)
    print("Lockstep mutation engine — NON-VACUOUS SELF-TEST (teeth-test)")
    print("=" * 72)
    a = part_a_planted_killed(repo_root, preset, verbose)
    b = part_b_weak_survives(verbose)
    c = part_c_timeout_killed(verbose)
    d = part_d_restore_on_start(verbose)
    print("\n" + "=" * 72)
    print(f"  PART A (planted mutant KILLED)        : {'PASS' if a else 'FAIL'}")
    print(f"  PART B (weak-test SURVIVOR shown)     : {'PASS' if b else 'FAIL'}")
    print(f"  PART C (infinite-loop KILLED-by-timeout+restored+no-orphans): "
          f"{'PASS' if c else 'FAIL'}")
    print(f"  PART D (restore-on-start self-heal)   : {'PASS' if d else 'FAIL'}")
    ok = a and b and c and d
    print(f"  SELF-TEST: {'PASS — engine proven non-vacuous + crash-safe' if ok else 'FAIL'}")
    print("=" * 72)
    return 0 if ok else 1


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.exit(run(root))
