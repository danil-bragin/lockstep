#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Lockstep mutation-testing runner (Phase 0 skeleton).

WHY THIS EXISTS
---------------
Per master-plan §6.7 and cardinal rule 5, mutation testing is the *adequacy
meta-gate*: because every line of test code in Lockstep is agent-written, the
only mechanical proof that the tests are not "coverage theater" is that they
*kill deliberately injected bugs (mutants)*. The universal merge gate
(master-plan §8) therefore fails on a mutation-score regression.

In Phase 0 there is almost nothing to mutate (the tree is scaffolding plus an
empty smoke test). This runner is intentionally a complete, end-to-end skeleton:
it runs now, reports a mutation score, and exits 0 -- while leaving clearly
marked function boundaries (`generate_mutants` / `run_suite_against`) where the
real mutation operators and per-mutant test runs plug in for Phase >= 2, when
the simulation harness and checkers (the things actually worth mutating) exist.

CONTRACT
--------
* stdlib only (no pip deps) -- runs in CI and on a bare dev host identically.
* Deterministic: a mutation score is a pure function of (mutants, suite result).
* Exit 0 when score >= THRESHOLD; non-zero on regression -> fails the gate.

USAGE
-----
    python3 tools/mutation/run_mutation.py [--repo-root DIR] [--threshold F]
                                           [--json] [-v]

A "mutation score" is killed_mutants / viable_mutants. With zero mutants the
runner reports a vacuously-passing 100.0% ("0 mutants, runner OK"), which is the
correct Phase-0 state: the machinery is wired and green, ready for real mutants.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Gate parameter. The merge gate fails when the achieved score drops below this.
# Phase 0: nothing to mutate, so 100.0 is trivially met. Raise/lower per phase as
# the real mutant population comes online (Phase >= 2). Keep in sync with CI.
# ---------------------------------------------------------------------------
MUTATION_SCORE_THRESHOLD = float(os.environ.get("LOCKSTEP_MUTATION_THRESHOLD", "100.0"))


@dataclass
class Mutant:
    """A single injected bug: a description of what was changed and where."""

    mutant_id: str
    file_path: str
    description: str
    # The actual mutated source text would be carried here once real operators
    # exist. Left intentionally empty in the Phase 0 skeleton.
    patched_source: Optional[str] = None


@dataclass
class SuiteResult:
    """Outcome of running the full test suite against one (possibly mutated) tree."""

    passed: bool
    detail: str = ""


@dataclass
class MutationReport:
    total: int = 0
    killed: int = 0
    survived: int = 0
    # Mutants that could not be built/run (e.g. did not compile) are not counted
    # against the score -- only *viable* mutants form the denominator.
    non_viable: int = 0
    survivors: list = field(default_factory=list)

    @property
    def viable(self) -> int:
        return self.killed + self.survived

    @property
    def score(self) -> float:
        # Vacuous-true with zero viable mutants: the runner is wired and green.
        if self.viable == 0:
            return 100.0
        return 100.0 * self.killed / self.viable


# ---------------------------------------------------------------------------
# PLUGGABLE BOUNDARY #1 -- mutant generation.
#
# TODO(phase>=2): replace this Phase-0 stub with real mutation operators that
# walk the C++ AST / source of the harness + checkers + storage and emit
# semantically-meaningful mutants (flip a comparison, drop a statement, swap
# +/-, weaken an invariant assert, etc.). The whole point is that the test
# suite must KILL these. Return an empty list to mean "nothing to mutate yet".
# ---------------------------------------------------------------------------
def generate_mutants(repo_root: str, verbose: bool = False) -> list:
    """Phase 0 skeleton: produce zero mutants (nothing meaningful exists yet)."""
    if verbose:
        print(f"[mutation] scanning {repo_root} for mutable targets ...")
        print("[mutation] Phase 0: no real mutation operators wired -> 0 mutants.")
    return []  # 0 mutants -> vacuously-passing score; runner proven end-to-end.


# ---------------------------------------------------------------------------
# PLUGGABLE BOUNDARY #2 -- run the test suite against a (mutated) tree.
#
# TODO(phase>=2): apply `mutant.patched_source`, build, and run the deterministic
# simulation battery + ctest. Return SuiteResult(passed=True) IFF the suite
# stays green. A mutant is "killed" when the suite goes RED on it (passed=False);
# a mutant that the suite still passes is a SURVIVOR -> evidence of weak tests.
# ---------------------------------------------------------------------------
def run_suite_against(repo_root: str, mutant: Optional[Mutant], verbose: bool = False) -> SuiteResult:
    """Phase 0 skeleton: never invoked (no mutants). Wired for Phase >= 2."""
    return SuiteResult(passed=True, detail="phase0-skeleton: suite-run not yet wired")


def evaluate_mutants(
    repo_root: str,
    mutants: list,
    runner: Callable[[str, Optional[Mutant], bool], SuiteResult] = run_suite_against,
    verbose: bool = False,
) -> MutationReport:
    """Drive every mutant through the suite and tally the report.

    A mutant is KILLED when the suite fails on it (the bug was caught), and a
    SURVIVOR when the suite still passes (a hole in the tests).
    """
    report = MutationReport(total=len(mutants))
    for mutant in mutants:
        result = runner(repo_root, mutant, verbose)
        if result.passed:
            report.survived += 1
            report.survivors.append(mutant.mutant_id)
            if verbose:
                print(f"[mutation] SURVIVED {mutant.mutant_id}: {mutant.description}")
        else:
            report.killed += 1
            if verbose:
                print(f"[mutation] killed   {mutant.mutant_id}: {mutant.description}")
    return report


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description="Lockstep mutation runner (Phase 0 skeleton).")
    parser.add_argument(
        "--repo-root",
        default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        help="Repository root to mutate (default: inferred from this script's location).",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=MUTATION_SCORE_THRESHOLD,
        help="Minimum acceptable mutation score (percent). Gate fails below this.",
    )
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON report.")
    parser.add_argument("-v", "--verbose", action="store_true", help="Per-mutant logging.")
    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)

    mutants = generate_mutants(repo_root, verbose=args.verbose)
    report = evaluate_mutants(repo_root, mutants, verbose=args.verbose)
    score = report.score
    passed = score >= args.threshold

    if args.json:
        print(
            json.dumps(
                {
                    "mutation_score": round(score, 4),
                    "threshold": args.threshold,
                    "total_mutants": report.total,
                    "viable_mutants": report.viable,
                    "killed": report.killed,
                    "survived": report.survived,
                    "non_viable": report.non_viable,
                    "survivors": report.survivors,
                    "passed": passed,
                },
                indent=2,
            )
        )
    else:
        if report.viable == 0:
            summary = "0 mutants, runner OK"
        else:
            summary = f"{report.killed}/{report.viable} mutants killed"
        print("=" * 60)
        print("Lockstep mutation runner (Phase 0 skeleton)")
        print(f"  mutation score : {score:.1f}%  ({summary})")
        print(f"  threshold      : {args.threshold:.1f}%")
        print(f"  result         : {'PASS' if passed else 'FAIL (regression)'}")
        if report.survivors:
            print(f"  survivors      : {', '.join(report.survivors)}")
        print("=" * 60)

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
