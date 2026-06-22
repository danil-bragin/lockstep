#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Lockstep mutation-testing runner — REAL source-mutation engine (Phase 2, M1).

WHY THIS EXISTS
---------------
Per master-plan §6.7 and cardinal rule 5, mutation testing is the *adequacy
meta-gate*: because every line of test code in Lockstep is agent-written, the
ONLY mechanical proof that the tests are not "coverage theater" is that they
KILL deliberately injected bugs (mutants). The universal merge gate (§8)
therefore fails on a mutation-score *regression*. This replaces the Phase-0
vacuous skeleton (0 mutants -> hollow 100%) with a runner that actually mutates
the C++ sources under core/ + providers/sim, rebuilds, and runs the ctest suite.

WHAT IT DOES (end-to-end, per mutant)
-------------------------------------
  1. GENERATE  — apply the documented operator set (see operators.py) to every
                 mutable C++ source under the configured roots. Deterministic.
  2. SELECT    — `--full` runs all mutants; `--sample N` takes a SEEDED,
                 deterministic sample of N (splitmix64 over the sorted id list).
                 The cap is printed LOUDLY — no silent truncation (§6 forbids
                 coverage theater).
  3. EXECUTE   — for each mutant: BACK UP the file to <file>.mutbak, write the
                 one-line edit IN PLACE, rebuild the affected preset, run ctest
                 UNDER A PER-MUTANT WALL-CLOCK TIMEOUT (derived from a baseline
                 clean-suite timing — see TIMEOUT POLICY). Then ALWAYS restore
                 from the backup (even on exception / Ctrl-C / SIGKILL of a child)
                 and delete the backup on success. A leftover *.mutbak from a
                 crashed/killed run is restored on the NEXT startup (self-heal).
                 Classify:
                    KILLED    : build OK and ctest FAILED  (a test caught it).
                                A build/test that EXCEEDS the per-mutant timeout
                                is ALSO killed (killed-by-timeout): a mutant that
                                breaks liveness/termination IS a detected defect —
                                the suite "caught" it by never finishing. On
                                timeout the ENTIRE process tree is killed (ctest +
                                any test binaries it spawned) via the process
                                group, so nothing is left spinning on the host.
                    SURVIVED  : build OK and ctest PASSED  (a TEST GAP)
                    SKIPPED   : build FAILED (mutant did not compile) -- counted
                               separately, NEVER as killed.
  4. SCORE     — mutation_score = killed / (killed + survived). Per-file + overall.
                 Survivors are listed LOUDLY (file:line, operator, orig->mutated).
  5. GATE      — exit 0 if score >= threshold AND (if a baseline exists) score is
                 not a regression vs the stored baseline. Else exit non-zero.

CONTRACT (kept stable for gate.sh)
----------------------------------
  * `python3 tools/mutation/run_mutation.py`           -> exit 0/non-0
  * `--json`                                            -> machine-readable report
  * LOCKSTEP_MUTATION_THRESHOLD env (default below)     -> gate threshold (percent)
  * stdlib only; may shell out to cmake / ctest.

TIMEOUT POLICY (liveness safety — added after a hang corrupted the tree)
------------------------------------------------------------------------
A mutant that breaks termination (e.g. binary-search `mid + 1` -> `mid + 0`,
`lo = mid`) makes a test SPIN FOREVER. Without a bound that pins the host (it
once left ~10 orphaned test processes spinning, load avg 51) AND — because the
hang had to be SIGKILLed externally — left the in-place mutation UNRESTORED,
corrupting the real source tree.

The fix has four teeth:
  * BASELINE-DERIVED PER-MUTANT TIMEOUT. At startup we time the clean ctest suite
    ONCE; the per-mutant test timeout = max(floor, baseline * factor). Env:
      LOCKSTEP_MUTATION_TIMEOUT_FACTOR  (default 8)
      LOCKSTEP_MUTATION_TIMEOUT_FLOOR   (seconds, default 30)
      LOCKSTEP_MUTATION_BUILD_TIMEOUT   (seconds, default 600)
    --test-timeout / --build-timeout still override explicitly.
  * TIMEOUT => KILLED (killed-by-timeout), recorded distinctly in the report.
  * PROCESS-TREE KILL: every build/test runs in its own process group
    (start_new_session); on timeout we SIGKILL the whole group so ctest's
    grandchild test binaries die too. A best-effort sweep then logs any orphan.
  * CRASH-SAFE RESTORE: backup-before-mutate + restore-from-backup in finally +
    delete-on-success, PLUS a restore-on-START guard that heals any *.mutbak left
    by a previously crashed/killed run before doing anything else.

THRESHOLD POLICY
----------------
Default threshold is 0.0 so the gate does not BLOCK before the agent-written
tests catch up — but survivors are always REPORTED. The threshold is meant to
RATCHET UP as the suite hardens (see docs/mutation.md). If a baseline file
(tools/mutation/baseline.json) exists, the gate ALSO fails on a regression vs the
stored score, which is the real §6.7 meta-gate. Write/refresh the baseline with
`--write-baseline` once a tree's survivors are understood.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field

# operators.py sits next to this file.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import operators as ops  # noqa: E402


# ---------------------------------------------------------------------------
# Configuration.
# ---------------------------------------------------------------------------
REPO_ROOT_DEFAULT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Default gate threshold (percent). 0.0 = report-but-don't-block initially.
# DOCUMENTED to ratchet up (docs/mutation.md). Env-overridable.
THRESHOLD_DEFAULT = float(os.environ.get("LOCKSTEP_MUTATION_THRESHOLD", "0.0"))

# Source roots to mutate (master-plan: core/ runtime + providers/sim). Only files
# that are actually compiled into the suite are worth mutating; these roots are.
SOURCE_ROOTS = ("core", os.path.join("providers", "sim"))
SOURCE_EXTS = (".hpp", ".cpp", ".cc", ".h")

# Build/test commands (reuse the existing presets; debug is fine and fast).
DEFAULT_PRESET = "debug"

# ISOLATED MUTATION BUILD DIRECTORY (the shared-build-tree-pollution fix).
# -----------------------------------------------------------------------
# Mutants MUST NOT be built or tested in the SHARED `build/<preset>` directories
# that the merge gate (gate.sh: `compile+test (debug)`) and human developers use.
# A mutation lives in a HEADER; after the source is restored an incremental
# `cmake --build` sometimes does NOT rebuild the dependent target (an mtime /
# dependency miss), leaving a STALE MUTANT-COMPILED test binary in build/debug.
# A later normal `ctest` then runs that stale mutant binary and FAILS with a
# mutation-induced assert — corrupting the gate signal (this misdiagnosed twice
# as a Phase-1 regression). The cure: mutation gets its OWN build directory,
# configured ONCE with debug-like settings, reused across mutants in the run.
# After any mutation run, build/debug and every preset dir are byte-for-byte
# UNTOUCHED. `build/mutation` is NOT a preset binaryDir (presets use
# build/<presetName>: debug/release/asan/...), so it can never collide, and it is
# already covered by the `build/` rule in .gitignore.
MUTATION_BUILD_DIRNAME = "mutation"
MUTATION_BUILD_SUBDIR = os.path.join("build", MUTATION_BUILD_DIRNAME)

# Per-mutant timeout policy (env-overridable; documented above + in docs/mutation.md).
TIMEOUT_FACTOR_DEFAULT = float(os.environ.get("LOCKSTEP_MUTATION_TIMEOUT_FACTOR", "8"))
TIMEOUT_FLOOR_DEFAULT = float(os.environ.get("LOCKSTEP_MUTATION_TIMEOUT_FLOOR", "30"))
BUILD_TIMEOUT_DEFAULT = int(os.environ.get("LOCKSTEP_MUTATION_BUILD_TIMEOUT", "600"))

# Sentinel suffix for the crash-safe backup of a mutated source.
MUTBAK_SUFFIX = ".mutbak"

# A KILLED verdict whose cause was a timeout is recorded with this detail prefix
# so it is visible in the report as a distinct, liveness-breaking kill.
KILLED_BY_TIMEOUT = "killed-by-timeout"

# Prefix the test binaries share (build/<preset>/tests/lockstep_*). Used only for
# the best-effort orphan sweep after a process-tree kill — never load-bearing.
ORPHAN_BINARY_PREFIX = "lockstep_"

# splitmix64 — the SAME engine the runtime uses (providers/sim/SeededRandom.hpp),
# so mutant selection is deterministic and reproducible WITHOUT std::random.
def _splitmix64(state: int) -> tuple[int, int]:
    mask = (1 << 64) - 1
    state = (state + 0x9E3779B97F4A7C15) & mask
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
    z = (z ^ (z >> 31)) & mask
    return state, z


# ---------------------------------------------------------------------------
# Process-tree-safe command runner.
#
# WHY: ctest spawns the actual test binaries; those grandchildren can OUTLIVE
# ctest. If we only kill the direct child on timeout, a hung test binary keeps
# spinning (this once saturated the host, load avg 51). We start each command in
# its OWN process group (start_new_session=True => setsid). On timeout we SIGKILL
# the WHOLE group (os.killpg) so every descendant dies. Returns:
#   (timed_out: bool, returncode: int|None, stdout: str, stderr: str)
# ---------------------------------------------------------------------------
def run_proc_group(cmd, cwd, timeout, env):
    """Run cmd in its own session/process group; kill the whole tree on timeout."""
    # start_new_session=True => child becomes a session+group leader (os.setsid).
    # On Windows this kwarg is unavailable, so fall back to a plain run there
    # (the simulator/test stack is POSIX-only anyway).
    popen_kwargs = dict(
        cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if hasattr(os, "setsid"):
        popen_kwargs["start_new_session"] = True
    proc = subprocess.Popen(cmd, **popen_kwargs)
    try:
        out, err = proc.communicate(timeout=timeout)
        return (False, proc.returncode, out, err)
    except subprocess.TimeoutExpired:
        _kill_tree(proc)
        # Drain whatever the (now-dead) pipes have so the FDs are freed.
        try:
            out, err = proc.communicate(timeout=10)
        except Exception:
            out, err = "", ""
        return (True, None, out or "", err or "")


def _kill_tree(proc) -> None:
    """SIGKILL the entire process group led by proc (then reap proc)."""
    if hasattr(os, "killpg"):
        try:
            pgid = os.getpgid(proc.pid)
            os.killpg(pgid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    else:
        try:
            proc.kill()
        except Exception:
            pass
    try:
        proc.wait(timeout=10)
    except Exception:
        pass


def sweep_orphans(repo_root: str, log: bool = True) -> list:
    """Best-effort: detect leftover lockstep_* test processes from THIS repo tree.

    After a process-tree kill there should be NONE. If any survive (e.g. a kernel
    raced us), we log loudly and try once more to kill them. Returns the list of
    pids we found (empty == clean). Uses pgrep if present; never fatal.
    """
    pgrep = shutil.which("pgrep")
    if not pgrep:
        return []
    try:
        r = subprocess.run([pgrep, "-f", ORPHAN_BINARY_PREFIX],
                           capture_output=True, text=True, check=False)
    except Exception:
        return []
    pids = [int(x) for x in r.stdout.split() if x.strip().isdigit()]
    # Filter to processes whose argv mentions this repo's build tree, so we never
    # touch an unrelated process that merely shares the prefix.
    mine = []
    for pid in pids:
        if pid == os.getpid():
            continue
        try:
            ps = subprocess.run(["ps", "-o", "command=", "-p", str(pid)],
                                capture_output=True, text=True, check=False)
        except Exception:
            continue
        cmdline = ps.stdout.strip()
        if repo_root in cmdline and ORPHAN_BINARY_PREFIX in cmdline:
            mine.append(pid)
    if mine and log:
        print(f"[mutation] !! ORPHAN SWEEP: {len(mine)} lingering {ORPHAN_BINARY_PREFIX}* "
              f"process(es) survived a kill: {mine} — force-killing now.")
        for pid in mine:
            try:
                os.kill(pid, signal.SIGKILL)
            except Exception:
                pass
    return mine


@dataclass
class Verdict:
    mutant: ops.Mutant
    status: str           # "killed" | "survived" | "skipped"
    detail: str = ""
    timed_out: bool = False   # True iff this was a killed-by-timeout (liveness break)


@dataclass
class Report:
    verdicts: list = field(default_factory=list)
    total_generated: int = 0
    sampled: int = 0
    sample_cap: int | None = None
    elapsed_s: float = 0.0

    @property
    def killed(self) -> int:
        return sum(1 for v in self.verdicts if v.status == "killed")

    @property
    def killed_by_timeout(self) -> int:
        return sum(1 for v in self.verdicts if v.status == "killed" and v.timed_out)

    @property
    def survived(self) -> int:
        return sum(1 for v in self.verdicts if v.status == "survived")

    @property
    def skipped(self) -> int:
        return sum(1 for v in self.verdicts if v.status == "skipped")

    @property
    def viable(self) -> int:
        return self.killed + self.survived

    @property
    def score(self) -> float:
        # killed / viable. With ZERO viable mutants we return 0.0 (NOT a vacuous
        # 100%): a run that built+tested nothing has proven nothing. The old
        # skeleton's vacuous-100 was exactly the coverage theater §6.7 forbids.
        if self.viable == 0:
            return 0.0
        return 100.0 * self.killed / self.viable

    @property
    def survivors(self) -> list:
        return [v for v in self.verdicts if v.status == "survived"]


# ---------------------------------------------------------------------------
# Source discovery + mutant generation.
# ---------------------------------------------------------------------------
def discover_sources(repo_root: str, roots: tuple, changed_only: bool, verbose: bool) -> list:
    found = []
    if changed_only:
        try:
            out = subprocess.run(
                ["git", "diff", "--name-only", "HEAD"],
                cwd=repo_root, capture_output=True, text=True, check=False,
            )
            changed = set(line.strip() for line in out.stdout.splitlines() if line.strip())
        except Exception:
            changed = set()
        for rel in sorted(changed):
            if rel.startswith(roots) and rel.endswith(SOURCE_EXTS):
                if os.path.isfile(os.path.join(repo_root, rel)):
                    found.append(rel)
        if verbose:
            print(f"[mutation] --changed-only: {len(found)} changed source(s)")
        return found
    for root in roots:
        base = os.path.join(repo_root, root)
        for dirpath, _dirs, files in os.walk(base):
            for fn in sorted(files):
                if fn.endswith(SOURCE_EXTS):
                    abs_p = os.path.join(dirpath, fn)
                    rel = os.path.relpath(abs_p, repo_root)
                    found.append(rel)
    found.sort()
    return found


def generate_all(repo_root: str, sources: list, verbose: bool) -> list:
    mutants = []
    for rel in sources:
        abs_p = os.path.join(repo_root, rel)
        try:
            text = open(abs_p, "r", encoding="utf-8").read()
        except Exception as e:
            if verbose:
                print(f"[mutation] skip unreadable {rel}: {e}")
            continue
        muts = ops.generate_for_file(rel, text)
        mutants.extend(muts)
        if verbose:
            print(f"[mutation] {rel}: {len(muts)} mutants")
    # Global deterministic order: by id (file:line:col:op:mutated all sortable).
    mutants.sort(key=lambda m: m.mutant_id)
    return mutants


def deterministic_sample(mutants: list, n: int, seed: int) -> list:
    """Pick n mutants deterministically from the sorted list using splitmix64.

    We assign each mutant a splitmix64 key derived from (seed, index), sort by
    that key, and take the first n. Pure function of (mutants, n, seed) — no
    system randomness, fully replayable (cardinal rule: no std::random equiv).
    """
    if n >= len(mutants):
        return list(mutants)
    keyed = []
    state = seed & ((1 << 64) - 1)
    for idx, m in enumerate(mutants):
        # Mix the index into the stream so the order is a stable permutation.
        state, k = _splitmix64(state ^ (idx * 0x9E3779B97F4A7C15))
        keyed.append((k, idx, m))
    keyed.sort(key=lambda t: (t[0], t[1]))
    chosen = [m for _k, _i, m in keyed[:n]]
    # Return in canonical id order for stable, readable reporting.
    chosen.sort(key=lambda m: m.mutant_id)
    return chosen


# ---------------------------------------------------------------------------
# Build + test one mutant.
# ---------------------------------------------------------------------------
def _read_lines(path: str) -> list:
    with open(path, "r", encoding="utf-8") as f:
        return f.read().split("\n")


def _write_lines(path: str, lines: list) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def _mutbak_for(abs_p: str) -> str:
    return abs_p + MUTBAK_SUFFIX


def apply_mutant(repo_root: str, mutant: ops.Mutant) -> str:
    """Apply the one-line edit IN PLACE — but FIRST copy the file to <file>.mutbak
    so the original bytes survive even if THIS PROCESS is SIGKILLed before its
    finally runs. Returns the ORIGINAL line text (kept for a fast in-memory
    restore). The on-disk .mutbak is the crash-safe ground truth; the next run's
    restore-on-start guard heals from it if we die. Raises if the file/line no
    longer matches (defensive)."""
    abs_p = os.path.join(repo_root, mutant.file_path)
    # Crash-safe backup: write the pristine bytes to .mutbak BEFORE mutating.
    # copy2 preserves mode/mtime; fsync the backup so it is durable before we
    # touch the real file (a power-loss/SIGKILL window otherwise corrupts).
    shutil.copy2(abs_p, _mutbak_for(abs_p))
    try:
        with open(_mutbak_for(abs_p), "rb") as bf:
            os.fsync(bf.fileno())
    except OSError:
        pass
    lines = _read_lines(abs_p)
    idx = mutant.line - 1
    if idx < 0 or idx >= len(lines):
        # File/line drifted: do not mutate; drop the (now-useless) backup.
        _drop_backup(abs_p)
        raise RuntimeError(f"line {mutant.line} out of range in {mutant.file_path}")
    original = lines[idx]
    lines[idx] = mutant.new_line_text
    _write_lines(abs_p, lines)
    return original


def _drop_backup(abs_p: str) -> None:
    bak = _mutbak_for(abs_p)
    try:
        if os.path.isfile(bak):
            os.remove(bak)
    except OSError:
        pass


def restore_mutant(repo_root: str, mutant: ops.Mutant) -> None:
    """Restore the file from its .mutbak (the crash-safe ground truth) and delete
    the backup. Idempotent: if no backup exists (already restored) it is a no-op."""
    abs_p = os.path.join(repo_root, mutant.file_path)
    bak = _mutbak_for(abs_p)
    if os.path.isfile(bak):
        # os.replace is atomic on POSIX — the real file is never left half-written.
        os.replace(bak, abs_p)


def restore_line(repo_root: str, mutant: ops.Mutant, original_line: str) -> None:
    """Back-compat shim (used by selftest's older call sites): prefer the .mutbak
    restore (crash-safe ground truth); fall back to the in-memory line if no
    backup is present."""
    abs_p = os.path.join(repo_root, mutant.file_path)
    if os.path.isfile(_mutbak_for(abs_p)):
        restore_mutant(repo_root, mutant)
        return
    lines = _read_lines(abs_p)
    idx = mutant.line - 1
    if 0 <= idx < len(lines):
        lines[idx] = original_line
        _write_lines(abs_p, lines)


def restore_on_start(repo_root: str, roots: tuple) -> int:
    """RESTORE-ON-START GUARD (self-heal). Before doing anything else, scan the
    source roots for any leftover *.mutbak from a previously CRASHED or KILLED run
    and restore each one (the backup IS the pristine original). Log loudly when it
    fires — a leftover backup means a prior run died mid-mutant. Returns the count
    restored."""
    restored = 0
    for root in roots:
        base = os.path.join(repo_root, root)
        for bak in glob.glob(os.path.join(base, "**", "*" + MUTBAK_SUFFIX),
                             recursive=True):
            target = bak[: -len(MUTBAK_SUFFIX)]
            try:
                os.replace(bak, target)
                rel = os.path.relpath(target, repo_root)
                print(f"[mutation] !! RESTORE-ON-START: healed leftover mutation in "
                      f"{rel} from {os.path.basename(bak)} (a prior run crashed/was "
                      f"killed mid-mutant). Source restored.")
                restored += 1
            except OSError as e:
                print(f"[mutation] !! RESTORE-ON-START: FAILED to heal {bak}: {e}")
    if restored:
        print(f"[mutation] restore-on-start healed {restored} leftover mutation(s).")
    return restored


def mutation_build_dir(repo_root: str) -> str:
    """Absolute path of the ISOLATED mutation build directory (build/mutation).
    This is the ONLY build tree mutants touch — never build/<preset>."""
    return os.path.join(repo_root, MUTATION_BUILD_SUBDIR)


def _debug_cache_vars(repo_root: str, preset: str) -> dict:
    """Read CMakePresets.json and resolve the named preset's effective
    cacheVariables (walking `inherits`), so the isolated dir is configured with
    the SAME debug-like flags the gate's `debug` preset uses (CMAKE_BUILD_TYPE,
    C++ standard, export-compile-commands, etc.) — minus the binaryDir, which we
    override to build/mutation. Falls back to a minimal Debug config if the
    presets file can't be parsed (the goal is debug-like, not byte-identical)."""
    fallback = {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_STANDARD": "23",
        "CMAKE_CXX_STANDARD_REQUIRED": "ON",
        "CMAKE_CXX_EXTENSIONS": "OFF",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
    }
    presets_path = os.path.join(repo_root, "CMakePresets.json")
    try:
        data = json.load(open(presets_path, "r", encoding="utf-8"))
    except Exception:
        return fallback
    by_name = {p.get("name"): p for p in data.get("configurePresets", [])}
    if preset not in by_name:
        return fallback
    # Resolve inheritance chain (child overrides parent).
    chain = []
    seen = set()
    cur = preset
    while cur and cur in by_name and cur not in seen:
        seen.add(cur)
        chain.append(by_name[cur])
        inh = by_name[cur].get("inherits")
        cur = inh if isinstance(inh, str) else (inh[0] if isinstance(inh, list) and inh else None)
    merged: dict = {}
    for p in reversed(chain):  # base first, child last (child wins)
        for k, v in (p.get("cacheVariables") or {}).items():
            merged[k] = v.get("value") if isinstance(v, dict) else v
    return merged or fallback


def configure_isolated_build_dir(repo_root: str, preset: str, build_timeout: int,
                                 verbose: bool) -> str:
    """Configure the ISOLATED mutation build directory ONCE, with the same
    debug-like cache variables as the `preset` (default: debug), but with its
    binaryDir overridden to build/mutation so it can NEVER touch the shared
    build/<preset> dirs the gate + humans use. Returns the absolute build dir.

    We do a plain `cmake -S <repo> -B build/mutation -D...` rather than
    `cmake --preset` because the preset hard-codes binaryDir=build/<presetName>;
    a `-D` override of binaryDir is not honoured by `--preset`. Mirroring the
    cache vars gives debug-equivalent settings without the shared dir."""
    build_dir = mutation_build_dir(repo_root)
    os.makedirs(build_dir, exist_ok=True)
    cache = _debug_cache_vars(repo_root, preset)
    cmd = ["cmake", "-S", repo_root, "-B", build_dir]
    for k, v in cache.items():
        cmd.append(f"-D{k}={v}")
    env = dict(os.environ)
    env.pop("LOCKSTEP_SWEEP_SEEDS", None)
    print(f"[mutation] configuring ISOLATED build dir {MUTATION_BUILD_SUBDIR} "
          f"(debug-like, mirrors preset '{preset}'); build/{preset} and other "
          f"preset dirs are NEVER touched.")
    if verbose:
        print("[mutation]   + " + " ".join(cmd))
    ct, rc, out, err = run_proc_group(cmd, cwd=repo_root, timeout=build_timeout, env=env)
    if ct or rc != 0:
        tail = (err or out or "").strip().splitlines()
        msg = tail[-1] if tail else "configure error"
        raise RuntimeError(
            f"failed to configure isolated mutation build dir {build_dir}: {msg}")
    return build_dir


def build_and_test(repo_root: str, build_dir: str, build_timeout: int,
                   test_timeout: int) -> tuple[bool, bool, str, bool]:
    """Build the ISOLATED mutation build dir, then run ctest UNDER A BOUNDED
    PER-MUTANT TIMEOUT.

    `build_dir` is the ISOLATED mutation directory (build/mutation) — NEVER the
    shared build/<preset>. It must already be configured (see
    configure_isolated_build_dir). This is the shared-tree-pollution fix: every
    mutant compile + ctest happens here, so build/debug is left untouched.

    Returns (built_ok, tests_passed, detail, timed_out).

    built_ok False  -> mutant did not compile -> SKIPPED.
    built_ok True, tests_passed False -> KILLED (incl. killed-by-timeout).
    built_ok True, tests_passed True  -> SURVIVED.

    Both the build and the test step run in their OWN process group; on timeout
    the WHOLE tree (ctest + any test binaries it spawned) is SIGKILLed and a
    best-effort orphan sweep verifies nothing is left spinning. A test timeout is
    a KILL (timed_out=True): a mutant that breaks termination IS a detected defect.
    """
    env = dict(os.environ)
    # Keep mutation runs fast: do NOT crank seed-sweep / fuzz sizes.
    env.pop("LOCKSTEP_SWEEP_SEEDS", None)

    # --- BUILD (process-group bounded). A build that hangs is a SKIP, not a kill:
    #     we cannot attribute a build hang to a test-suite property. ---
    bt, brc, bout, berr = run_proc_group(
        ["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4)],
        cwd=repo_root, timeout=build_timeout, env=env,
    )
    if bt:
        sweep_orphans(repo_root)
        return (False, False, "build timeout (process tree killed)", False)
    if brc != 0:
        tail = (berr or bout or "").strip().splitlines()
        return (False, False, "did not compile: " + (tail[-1] if tail else "build error"), False)

    # --- TEST (process-group bounded). A test that hangs => KILLED-BY-TIMEOUT. ---
    tt, trc, tout, _terr = run_proc_group(
        ["ctest", "--test-dir", build_dir, "--output-on-failure"],
        cwd=repo_root, timeout=test_timeout, env=env,
    )
    if tt:
        # The mutant broke liveness: the suite never finished within the bound.
        # The whole ctest process tree has been SIGKILLed; verify no orphans.
        orphans = sweep_orphans(repo_root)
        note = f"{KILLED_BY_TIMEOUT}: suite did not finish within {test_timeout}s " \
               f"(process tree killed)"
        if orphans:
            note += f"; WARNING {len(orphans)} orphan(s) needed a second kill"
        return (True, False, note, True)
    if trc == 0:
        return (True, True, "all tests passed", False)
    # Distinguish "suite failed" from "ctest infra error". Either way the mutant
    # produced a non-green suite => killed.
    failline = ""
    for ln in (tout or "").splitlines():
        if "failed out of" in ln.lower() or "tests passed" in ln.lower():
            failline = ln.strip()
    return (True, False, failline or "ctest non-zero", False)


def time_clean_suite(repo_root: str, build_dir: str, build_timeout: int) -> float:
    """Time the CLEAN ctest suite ONCE in the ISOLATED build dir (build first so
    the timing is test-only). `build_dir` is the isolated mutation directory —
    NEVER build/<preset>. Returns the wall-clock seconds of the clean ctest run,
    or a conservative fallback if the clean suite itself does not finish (which
    would be a real problem independent of mutation)."""
    env = dict(os.environ)
    env.pop("LOCKSTEP_SWEEP_SEEDS", None)
    # Ensure the tree is built so we measure test time, not a cold build.
    run_proc_group(["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4)],
                   cwd=repo_root, timeout=build_timeout, env=env)
    t0 = time.time()
    tt, _rc, _o, _e = run_proc_group(
        ["ctest", "--test-dir", build_dir, "--output-on-failure"],
        cwd=repo_root, timeout=build_timeout, env=env,
    )
    if tt:
        sweep_orphans(repo_root)
        # Clean suite itself hung — fall back to the floor; surface loudly.
        print("[mutation] !! WARNING: the CLEAN suite did not finish within "
              f"{build_timeout}s; using the timeout floor for per-mutant bounds.")
        return float(build_timeout)
    return time.time() - t0


def derive_test_timeout(baseline_s: float, factor: float, floor_s: float) -> int:
    """per-mutant test timeout = max(floor, baseline * factor), rounded up."""
    return int(max(floor_s, baseline_s * factor) + 0.999)


# ---------------------------------------------------------------------------
# Drive all selected mutants.
# ---------------------------------------------------------------------------
def evaluate(repo_root: str, build_dir: str, mutants: list, build_timeout: int,
             test_timeout: int, verbose: bool) -> list:
    verdicts = []
    n = len(mutants)
    for i, m in enumerate(mutants, start=1):
        applied = False
        timed_out = False
        try:
            apply_mutant(repo_root, m)   # creates the crash-safe .mutbak first
            applied = True
            built, passed, detail, timed_out = build_and_test(
                repo_root, build_dir, build_timeout, test_timeout
            )
        finally:
            # ALWAYS restore from the .mutbak (the crash-safe ground truth) and
            # delete the backup. Even if build_and_test raised, the source is
            # healed here; and even if THIS process is killed before this runs,
            # the next run's restore-on-start guard heals the leftover .mutbak.
            if applied:
                restore_mutant(repo_root, m)
        if not built:
            status = "skipped"
        elif passed:
            status = "survived"
        else:
            status = "killed"
        verdicts.append(Verdict(m, status, detail, timed_out=timed_out))
        tag = {"killed": "KILLED  ", "survived": "SURVIVED", "skipped": "skipped "}[status]
        if timed_out:
            tag = "KILLED* "  # killed-by-timeout (liveness break)
        line = f"[{i}/{n}] {tag} {m.mutant_id}  ({m.describe()})"
        if timed_out:
            line += "  [killed-by-timeout]"
        print(line)
    return verdicts


# ---------------------------------------------------------------------------
# Reporting.
# ---------------------------------------------------------------------------
def per_file_breakdown(verdicts: list) -> dict:
    files: dict = {}
    for v in verdicts:
        f = v.mutant.file_path
        d = files.setdefault(f, {"killed": 0, "survived": 0, "skipped": 0})
        d[v.status] += 1
    return files


def print_human(report: Report, threshold: float, baseline: float | None,
                passed: bool) -> None:
    print("=" * 72)
    print("Lockstep mutation runner — REAL source-mutation engine (§6.7 meta-gate)")
    print("=" * 72)
    if report.sample_cap is not None and report.sampled < report.total_generated:
        print(f"  !! SAMPLED {report.sampled}/{report.total_generated} mutants "
              f"— NOT EXHAUSTIVE (seeded sample). Run --full for the whole set.")
    else:
        print(f"  EXHAUSTIVE: all {report.total_generated} generated mutants run.")
    print(f"  mutants built+tested (viable) : {report.viable}")
    if report.killed_by_timeout:
        print(f"    killed   : {report.killed}   "
              f"(of which {report.killed_by_timeout} killed-by-timeout — liveness breaks)")
    else:
        print(f"    killed   : {report.killed}")
    print(f"    survived : {report.survived}   <- TEST GAPS")
    print(f"    skipped  : {report.skipped}   (did not compile; NOT scored)")
    print(f"  mutation score : {report.score:.1f}%   (killed / viable)")
    print(f"  threshold      : {threshold:.1f}%")
    if baseline is not None:
        print(f"  baseline       : {baseline:.1f}%   (gate fails on regression)")
    print(f"  elapsed        : {report.elapsed_s:.1f}s")
    print("-" * 72)
    files = per_file_breakdown(report.verdicts)
    print("  per-file (killed/survived/skipped):")
    for f in sorted(files):
        d = files[f]
        print(f"    {f}: {d['killed']}/{d['survived']}/{d['skipped']}")
    if report.survivors:
        print("-" * 72)
        print(f"  !! {len(report.survivors)} SURVIVED MUTANT(S) — real test gaps:")
        for v in report.survivors:
            m = v.mutant
            print(f"    SURVIVED  {m.file_path}:{m.line}  [{m.op}]  "
                  f"'{m.original}' -> '{m.mutated}'")
    print("=" * 72)
    print(f"  result : {'PASS' if passed else 'FAIL'}")
    print("=" * 72)


def build_json(report: Report, threshold: float, baseline: float | None,
               passed: bool) -> dict:
    return {
        "mutation_score": round(report.score, 4),
        "threshold": threshold,
        "baseline": baseline,
        "total_generated": report.total_generated,
        "sampled": report.sampled,
        "exhaustive": report.sample_cap is None or report.sampled >= report.total_generated,
        "viable": report.viable,
        "killed": report.killed,
        "killed_by_timeout": report.killed_by_timeout,
        "survived": report.survived,
        "skipped": report.skipped,
        "elapsed_s": round(report.elapsed_s, 2),
        "survivors": [
            {
                "id": v.mutant.mutant_id,
                "file": v.mutant.file_path,
                "line": v.mutant.line,
                "op": v.mutant.op,
                "original": v.mutant.original,
                "mutated": v.mutant.mutated,
            }
            for v in report.survivors
        ],
        "passed": passed,
    }


# ---------------------------------------------------------------------------
# Baseline (regression gate).
# ---------------------------------------------------------------------------
def baseline_path(repo_root: str) -> str:
    return os.path.join(repo_root, "tools", "mutation", "baseline.json")


def load_baseline(repo_root: str) -> float | None:
    p = baseline_path(repo_root)
    if not os.path.isfile(p):
        return None
    try:
        return float(json.load(open(p))["mutation_score"])
    except Exception:
        return None


def write_baseline(repo_root: str, report: Report) -> None:
    p = baseline_path(repo_root)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(
            {
                "mutation_score": round(report.score, 4),
                "viable": report.viable,
                "killed": report.killed,
                "note": "Stored mutation baseline. Gate fails on regression vs this. "
                        "Refresh with run_mutation.py --full --write-baseline once "
                        "survivors are understood; ratchet up as tests harden.",
            },
            f, indent=2,
        )
        f.write("\n")
    print(f"[mutation] wrote baseline -> {p}  (score {report.score:.1f}%)")


# ---------------------------------------------------------------------------
# Main.
# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Lockstep REAL mutation runner (§6.7 adequacy meta-gate)."
    )
    parser.add_argument("--repo-root", default=REPO_ROOT_DEFAULT)
    parser.add_argument("--threshold", type=float, default=THRESHOLD_DEFAULT,
                        help="Min mutation score (percent). Gate fails below this. "
                             "Env LOCKSTEP_MUTATION_THRESHOLD overrides the default.")
    parser.add_argument("--preset", default=DEFAULT_PRESET,
                        help="CMake/ctest preset to (re)build per mutant (default: debug).")
    parser.add_argument("--full", action="store_true",
                        help="Run ALL generated mutants (exhaustive; slow).")
    parser.add_argument("--sample", type=int, default=None, metavar="N",
                        help="Run a deterministic seeded sample of N mutants (default cap "
                             "if neither --full nor --sample given: see --default-cap).")
    parser.add_argument("--seed", type=int, default=0xA1F00D,
                        help="Seed for deterministic mutant sampling (no system randomness).")
    parser.add_argument("--changed-only", action="store_true",
                        help="Only mutate sources changed vs HEAD (git diff).")
    parser.add_argument("--list", action="store_true",
                        help="List the selected mutants and exit (no build/test).")
    parser.add_argument("--build-timeout", type=int, default=BUILD_TIMEOUT_DEFAULT,
                        help="Per-mutant build timeout (s). Env LOCKSTEP_MUTATION_BUILD_TIMEOUT.")
    parser.add_argument("--test-timeout", type=int, default=None,
                        help="Per-mutant TEST timeout (s). Default: derived from a baseline "
                             "clean-suite timing = max(floor, baseline*factor). Set explicitly "
                             "to override the derivation.")
    parser.add_argument("--timeout-factor", type=float, default=TIMEOUT_FACTOR_DEFAULT,
                        help="Per-mutant timeout = max(floor, baseline*FACTOR). "
                             "Env LOCKSTEP_MUTATION_TIMEOUT_FACTOR (default 8).")
    parser.add_argument("--timeout-floor", type=float, default=TIMEOUT_FLOOR_DEFAULT,
                        help="Lower bound (s) on the per-mutant timeout. "
                             "Env LOCKSTEP_MUTATION_TIMEOUT_FLOOR (default 30).")
    parser.add_argument("--write-baseline", action="store_true",
                        help="After running, store the score as the regression baseline.")
    parser.add_argument("--no-baseline", action="store_true",
                        help="Ignore any stored baseline for the gate verdict.")
    parser.add_argument("--default-cap", type=int, default=8,
                        help="Sample size used when neither --full nor --sample is given "
                             "(keeps the gate fast: each mutant is a full rebuild + ctest). "
                             "Printed loudly. Use --full / --sample for deeper runs.")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--selftest", action="store_true",
                        help="Run the non-vacuous self-test (planted-killed + weak-survivor) "
                             "and exit. See selftest.py.")
    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)

    # RESTORE-ON-START GUARD (self-heal). Before ANYTHING else, heal any *.mutbak
    # a previously crashed/killed run left behind, so a hang+SIGKILL can never
    # leave the tree corrupted across invocations.
    restore_on_start(repo_root, SOURCE_ROOTS)

    if args.selftest:
        import selftest
        return selftest.run(repo_root, preset=args.preset, verbose=args.verbose)

    sources = discover_sources(repo_root, SOURCE_ROOTS, args.changed_only, args.verbose)
    all_mutants = generate_all(repo_root, sources, args.verbose)

    # Selection policy.
    sample_cap = None
    if args.full:
        selected = all_mutants
    elif args.sample is not None:
        sample_cap = args.sample
        selected = deterministic_sample(all_mutants, args.sample, args.seed)
    else:
        # Neither flag: apply the default cap so the gate stays fast. LOUD.
        sample_cap = args.default_cap
        selected = deterministic_sample(all_mutants, args.default_cap, args.seed)

    report = Report(
        total_generated=len(all_mutants),
        sampled=len(selected),
        sample_cap=sample_cap,
    )

    if args.list:
        for m in selected:
            print(f"{m.mutant_id}  | {m.describe()}")
        print(f"\n[mutation] {len(selected)}/{len(all_mutants)} selected "
              f"(seed={args.seed}, cap={sample_cap}).")
        return 0

    if not selected:
        print("[mutation] no mutants generated — nothing to test. This is NOT a "
              "passing state (score 0.0). Check SOURCE_ROOTS / operators.")
        # Honest: no viable mutants proves nothing. But don't block an empty tree
        # solely on this — fall through to threshold logic (0.0 vs threshold).

    # ISOLATED BUILD DIR (shared-tree-pollution fix). Configure build/mutation
    # ONCE, with debug-like settings mirroring the preset, BEFORE any mutant runs.
    # Every mutant build + ctest below targets THIS dir; build/<preset> is never
    # touched, so a stale mutant binary can never poison the gate's debug stage.
    try:
        build_dir = configure_isolated_build_dir(
            repo_root, args.preset, args.build_timeout, args.verbose)
    except RuntimeError as e:
        print(f"[mutation] !! FATAL: {e}")
        return 1

    # Derive the per-mutant TEST timeout from a baseline clean-suite timing, unless
    # explicitly overridden. This bounds every mutant's test step so a liveness-
    # breaking mutant can never spin the host (the bug this hardening prevents).
    if args.test_timeout is not None:
        test_timeout = args.test_timeout
        print(f"[mutation] per-mutant test timeout: {test_timeout}s (explicit --test-timeout)")
    else:
        print("[mutation] timing the CLEAN suite once (in the isolated dir) to derive "
              "the per-mutant timeout ...")
        baseline_s = time_clean_suite(repo_root, build_dir, args.build_timeout)
        test_timeout = derive_test_timeout(baseline_s, args.timeout_factor, args.timeout_floor)
        print(f"[mutation] clean suite = {baseline_s:.1f}s -> per-mutant test timeout "
              f"= max({args.timeout_floor:.0f}, {baseline_s:.1f}*{args.timeout_factor:g}) "
              f"= {test_timeout}s. A mutant exceeding this is KILLED-by-timeout "
              f"(its tree is SIGKILLed).")

    t0 = time.time()
    verdicts = evaluate(
        repo_root, build_dir, selected,
        args.build_timeout, test_timeout, args.verbose,
    )
    report.verdicts = verdicts
    report.elapsed_s = time.time() - t0

    baseline = None if args.no_baseline else load_baseline(repo_root)

    # Gate verdict: score >= threshold, AND no regression vs baseline (if any).
    passed = report.score >= args.threshold
    if baseline is not None and report.viable > 0:
        # Allow a tiny epsilon for float noise.
        if report.score + 1e-9 < baseline:
            passed = False

    if args.json:
        print(json.dumps(build_json(report, args.threshold, baseline, passed), indent=2))
    else:
        print_human(report, args.threshold, baseline, passed)

    if args.write_baseline:
        write_baseline(repo_root, report)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
