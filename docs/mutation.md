# Mutation testing — the adequacy meta-gate (§6.7)

Every line of test code in Lockstep is agent-written. The only *mechanical* proof
that those tests are not coverage theater is that they **kill deliberately
injected bugs (mutants)**. Mutation score is therefore THE adequacy metric, and
the universal merge gate (master-plan §8, §6.7) fails on a mutation-score
**regression**.

This document describes the real source-mutation engine under `tools/mutation/`.
It replaces the Phase-0 vacuous skeleton (which generated 0 mutants and reported a
hollow 100%).

## What it does

For each mutant the runner:

1. **Generates** a single-edit source change by applying the operator set below
   to the C++ sources under `core/` and `providers/sim/`.
2. **Backs up** the file to `<file>.mutbak`, **applies** the one-line edit in
   place, **rebuilds** the affected preset (`debug` by default), and **runs the
   ctest suite under a bounded per-mutant timeout** (see *Liveness safety*). The
   original bytes are always restored **from the backup** afterward — even on
   exception, Ctrl-C, or an external SIGKILL of a child — and the backup is
   deleted on success.
3. **Classifies** the mutant:
   - **KILLED** — build OK and ctest **FAILED** → a test caught the bug. Good.
     A build/test that **exceeds the per-mutant timeout** is **also KILLED**
     (recorded distinctly as **killed-by-timeout**): a mutant that breaks
     termination/liveness *is* a detected defect — the suite "caught" it by never
     finishing. Its whole process tree is SIGKILLed (see below).
   - **SURVIVED** — build OK and ctest **PASSED** → a **test gap**. Reported loudly.
   - **SKIPPED** — the mutant **did not compile** (or the *build* itself timed
     out — a build hang can't be attributed to a test property). Counted
     separately, **never** as killed (an uncompilable mutant proves nothing about
     the tests).
4. **Scores**: `mutation_score = killed / (killed + survived)`. Per-file + overall.
   killed-by-timeout mutants count as **killed**.

A run with **zero viable mutants scores 0.0, not 100%** — a run that built and
tested nothing has proven nothing. (The old skeleton's vacuous 100% was exactly
the coverage theater §6.7 forbids.)

## Operator set

Mutation is **lexical**, not AST-based: we tokenize each source line, skip
comments / string & char literals / preprocessor directives, and apply each
operator at every safe site. Documented operators (code → meaning):

| Code | Operator | Mutation |
|------|----------|----------|
| `ROR` | relational-operator replacement | `<` `<=` `>` `>=` swapped (boundary-flipping) |
| `EQ`  | equality-operator replacement   | `==` ↔ `!=` |
| `LCR` | logical-connector replacement   | `&&` ↔ `\|\|` |
| `AOR` | arithmetic-operator replacement | `+` ↔ `-`, `*` ↔ `/` |
| `ABS` | integer-literal boundary / off-by-one | `N` → `N+1` and `N` → `N-1` |
| `CBR` | constant / boolean replacement  | `true` ↔ `false` |
| `NEG` | negate condition                | `if (X)` / `while (X)` → `if (!(X))` |
| `SDL` | statement deletion (safe subset)| a side-effect call statement `f(...);` → `;` |

Operators that produce a longer C++ token (`<<`, `>>`, `->`, `++`, `+=`, `&&` from
`Type&&`, etc.) are skipped at generation time so we don't emit nonsense; the few
that slip through simply fail to compile and are counted as **SKIPPED**.

## Determinism (cardinal rule: no system randomness)

- Mutant **generation** is a pure function of the source bytes: files sorted by
  path, sites scanned line-then-column, operators applied in a fixed registry
  order. Same tree → same mutant set, byte for byte.
- Mutant **selection** (`--sample N`) is seeded with **splitmix64** — the same
  hand-rolled engine the runtime PRNG uses (`providers/sim/SeededRandom.hpp`) —
  never `std::random` / system entropy. Same `(--sample N, --seed S)` → same
  mutant subset and same verdicts on every run.

## Liveness safety & crash-safe restore (the hang-that-corrupted-the-tree fix)

A mutant can break **termination**: e.g. a binary-search `mid + 1` → `mid + 0`
makes `lo = mid` never advance, so a test **spins forever**. Without bounds this
once left ~10 orphaned test processes pinning the host (load avg 51) and — because
the hang had to be SIGKILLed externally — left the in-place mutation **unrestored**,
**corrupting the real source tree**. The runner now has four teeth against this:

1. **Per-mutant timeout, derived from a baseline.** At startup the runner times
   the **clean** ctest suite **once**, then sets
   `per-mutant test timeout = max(floor, baseline × factor)`. Configurable:

   | Knob | Env | `--flag` | Default |
   |------|-----|----------|---------|
   | factor | `LOCKSTEP_MUTATION_TIMEOUT_FACTOR` | `--timeout-factor` | `8` |
   | floor (s) | `LOCKSTEP_MUTATION_TIMEOUT_FLOOR` | `--timeout-floor` | `30` |
   | build timeout (s) | `LOCKSTEP_MUTATION_BUILD_TIMEOUT` | `--build-timeout` | `600` |

   `--test-timeout N` overrides the derivation entirely with a fixed bound.

2. **Timeout ⇒ KILLED-by-timeout.** A test step that exceeds the bound is a KILL,
   recorded distinctly (`killed_by_timeout` in `--json`, a `KILLED*  … [killed-by-timeout]`
   line and a per-summary count in the human report). It counts as killed — never
   survived, never skipped.

3. **Process-tree kill.** Every build and test step runs in **its own process
   group** (`start_new_session` / `setsid`). On timeout the runner SIGKILLs the
   **whole group** (`os.killpg`), so ctest's **grandchild test binaries** die too —
   they are exactly what saturated the host before. A best-effort **orphan sweep**
   (`pgrep`/`ps` for this repo's `lockstep_*` binaries) then logs loudly and
   re-kills anything that somehow survived.

4. **Crash-safe restore (two layers).**
   - **Backup + restore + delete-on-success.** Before mutating, the file is copied
     (and fsync'd) to `<file>.mutbak`; the `finally` restores from it (atomic
     `os.replace`) and deletes it on success. The on-disk backup — not an
     in-memory string — is the ground truth, so even a SIGKILL of *this* process
     can't lose the original.
   - **Restore-on-start guard.** At the very start of **every** run, the runner
     scans the source roots for any leftover `*.mutbak` (the fingerprint of a
     previously crashed/killed run) and **restores them first**, logging loudly,
     before doing anything else. The tree **self-heals** on the next invocation.

The self-test (`--selftest`) proves all of this end-to-end:
- **Part C** plants the exact infinite-loop mutant (a binary search where
  `lo = mid + 1` becomes `lo = mid + 0`), drives it through the real engine, and
  asserts it **(a)** times out, **(b)** is classified **killed-by-timeout**,
  **(c)** restores the source byte-for-byte (and deletes the `.mutbak`), and
  **(d)** leaves **no orphan** processes.
- **Part D** simulates a leftover `.mutbak` from a crashed run and asserts the
  **restore-on-start** guard heals it and consumes the backup.

## Runtime bound — no silent truncation

Full mutation is expensive (one rebuild + full ctest per mutant). The runner
**always prints the cap loudly**; it never silently truncates (master-plan §6
forbids coverage theater):

```
!! SAMPLED 12/707 mutants — NOT EXHAUSTIVE (seeded sample). Run --full for the whole set.
```

Selection modes:

- `--full` — run **all** generated mutants (exhaustive, slow; the periodic /
  CI-nightly mode).
- `--sample N` — run a deterministic seeded sample of N (fast; for inner-loop / PR).
- *(neither flag)* — apply a small **default cap** (`--default-cap`, 8) so the
  merge gate stays fast (each mutant is a full rebuild + ctest). The cap is
  printed loudly as above.
- `--changed-only` — scope mutation to sources changed vs `HEAD` (`git diff`),
  for cheap PR-scoped runs.

## Gate verdict & threshold ratchet

The runner exits **0** iff `score >= threshold` **and** (if a baseline exists)
the score is **not a regression** vs the stored baseline.

- `LOCKSTEP_MUTATION_THRESHOLD` (env) or `--threshold` sets the floor. **Default
  is `0.0`**: the gate *reports* survivors but does not block before the
  agent-written tests catch up.
- **The threshold is meant to RATCHET UP** as the suite hardens. Once survivors
  are understood and the genuine gaps are closed, raise the floor (e.g. 60 → 80 →
  90) so regressions below it fail the gate. Do not leave it at 0 forever — a
  permanent 0 floor is itself coverage theater.
- **Baseline regression gate** (the real §6.7 meta-gate): if
  `tools/mutation/baseline.json` exists, the gate also fails when the score drops
  below the stored baseline, independent of the absolute threshold. Refresh it
  with `--write-baseline` once a tree's survivors are understood:

  ```
  python3 tools/mutation/run_mutation.py --full --write-baseline
  ```

## Survivors are test gaps — never hidden

Every **SURVIVED** mutant is printed with `file:line`, operator, and
`original -> mutated`, both in the human summary and in `--json` (`survivors[]`).
A survivor means a real, mechanically-proven hole in the agent-written tests:
fix the test (or accept + document the gap), then re-run.

## Non-vacuous proof (the teeth-test)

A runner that reports a score is worthless unless it can prove it actually kills
mutants. `selftest.py` (run via `--selftest`) proves both halves, end to end:

- **Part A** — plants a real mutant on a load-bearing line of the deterministic
  `Scheduler`, builds + runs the **real** ctest suite, and asserts the runner
  classifies it **KILLED**. If the suite can't catch it, that is itself a real
  finding and the self-test FAILS (the engine never fakes a kill).
- **Part B** — synthesizes a tiny `classify()` fixture with a **deliberately weak
  test**, mutates it (`>` → `<=`), and asserts the runner reports the mutant
  **SURVIVED** — proving survivors are surfaced, not hidden.
- **Part C** — plants the exact **infinite-loop** mutant (binary-search
  `mid + 1` → `mid + 0`) and proves it **times out → killed-by-timeout →
  source restored → no orphan processes** (the hang/corruption fix).
- **Part D** — proves the **restore-on-start** guard heals a leftover `.mutbak`.

```
python3 tools/mutation/run_mutation.py --selftest
```

## CLI

```
python3 tools/mutation/run_mutation.py [options]

  --full                 run all generated mutants (exhaustive)
  --sample N             deterministic seeded sample of N mutants
  --seed S               sample seed (default fixed; no system randomness)
  --changed-only         only mutate sources changed vs HEAD
  --preset P             cmake/ctest preset to rebuild per mutant (default: debug)
  --threshold F          min score percent (env LOCKSTEP_MUTATION_THRESHOLD)
  --test-timeout N       fixed per-mutant test timeout (s); overrides derivation
  --timeout-factor F     per-mutant timeout = max(floor, baseline*F) (env LOCKSTEP_MUTATION_TIMEOUT_FACTOR)
  --timeout-floor S      lower bound on the per-mutant timeout (env LOCKSTEP_MUTATION_TIMEOUT_FLOOR)
  --build-timeout S      per-mutant build timeout (env LOCKSTEP_MUTATION_BUILD_TIMEOUT)
  --write-baseline       store the score as the regression baseline
  --no-baseline          ignore any stored baseline for the verdict
  --list                 list the selected mutants and exit (no build/test)
  --json                 machine-readable report
  --selftest             run the non-vacuous teeth-test and exit
  -v                     per-mutant verbose logging
```

`gate.sh` invokes `python3 tools/mutation/run_mutation.py` with no flags, so the
merge gate runs the fast default-capped sample and fails only on a threshold /
baseline breach — survivors are reported but do not, by themselves, fail a tree
whose threshold is not yet ratcheted up.
