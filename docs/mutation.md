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

Once per run the runner first builds a **shadow copy** of the repo source under
`build/mutation-src/` (see *Shadow source tree* below) and configures the
isolated build from it. Then, for each mutant, it:

1. **Generates** a single-edit source change by applying the operator set below
   to the C++ sources under `core/` and `providers/sim/`.
2. **Applies** the one-line edit **in the SHADOW tree** (never the real working
   tree), **rebuilds** in the **isolated mutation build directory**
   (`build/mutation/`, configured from the shadow — see *Isolated build
   directory* below), and **runs the ctest suite under a bounded per-mutant
   timeout** (see *Liveness safety*). The shadow file is restored afterward by
   re-copying it from the **real source** (the ground truth) — even on exception,
   Ctrl-C, or an external SIGKILL of a child. A crash-safe `<shadow-file>.mutbak`
   is also written next to the shadow file as a belt-and-suspenders, but it can
   only ever land under `build/` (gitignored), **never** in the real source tree.
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

## Shadow source tree (the live-tree-corruption fix — the PRIMARY safety)

Mutation **never writes the real working tree.** Once per run the runner copies
the repo's source into a **shadow tree** under **`build/mutation-src/`** and
configures the build *from the shadow* (`cmake -S build/mutation-src -B
build/mutation`). Every mutant's one-line edit is applied to a file **in the
shadow**, built+tested in `build/mutation`, then restored by re-copying that file
from the real source. The real `core/`, `providers/`, … files are **never** a
build input and are **never** written.

**Why this is mandatory.** The earlier design mutated source files **in place**
(back up to `<file>.mutbak` → mutate → restore in a `finally`). When the process
was **killed mid-mutant** (a host freeze did exactly this), the `finally` never
ran: the **live working tree was left CORRUPTED** with a mutant in place, with
only the `.mutbak` holding the original. That silently left two core mutants
(Scheduler insertion-sort, SeededRandom range guard) that **nearly got
committed.** The isolated build dir (below) fixed stale *binaries* but not
in-place *source* corruption.

**How the shadow makes corruption impossible.**

- The shadow is **rebuilt fresh every run** (`shutil.rmtree` + `copytree`), so it
  always mirrors the current real source byte-for-byte.
- It excludes the heavy/recursive/non-source dirs — notably **`build`** (which
  *contains* the shadow; excluding it avoids infinite recursion), `.git`, and the
  TLC `states/` scratch — and any stray `*.mutbak`. Everything else (all source +
  `CMakeLists.txt` + `CMakePresets.json` + `cmake/` modules) is copied so the
  shadow configures exactly like the real tree.
- An interruption (SIGKILL / host freeze) can **only ever leave junk under
  `build/`**, which is gitignored and disposable — **never** a corrupted real
  source file. `git status core/ providers/` shows **zero** modifications during
  and after a run.
- The `*.mutbak` + **restore-on-start** machinery (see *Liveness safety*) is
  **kept as belt-and-suspenders** (and still heals any legacy in-place state in
  the real tree), but the shadow tree is the primary mechanism that makes the live
  tree untouchable.

`build/mutation-src/` is covered by the `build/` rule in `.gitignore`.

## Isolated build directory (the shared-tree-pollution fix)

Mutation **never** builds or tests in the shared `build/<preset>` directories the
merge gate (`gate.sh` stage `compile+test (debug)`) and human developers use. It
configures and builds in its **own** directory, **`build/mutation/`**, whose
source (`-S`) is the **shadow tree** (`build/mutation-src/`), not the real repo.

**Why this is mandatory.** A mutation lives in a *header*. After the source is
restored, an incremental `cmake --build` sometimes does **not** rebuild the
dependent target — an mtime / dependency miss — leaving a **stale,
mutant-compiled** test binary in `build/debug`. A later *normal*
`cmake --build --preset debug` + `ctest` then runs that stale mutant binary and
**fails with a mutation-induced assert** (e.g. `set_value on empty Promise`),
corrupting the gate signal. This was misdiagnosed twice as a Phase-1 runtime
regression. Isolating the build dir makes it **impossible**: after any mutation
run, `build/debug` and every preset dir are **byte-for-byte untouched**.

**How it works.**

- `build/mutation` is **not** a preset `binaryDir` (presets use
  `build/<presetName>`: `debug`, `release`, `asan`, …), so it can never collide
  with a gate or developer build tree. It is already covered by the `build/`
  rule in `.gitignore`.
- The dir is configured **once at startup** (`configure_isolated_build_dir`),
  then **reused across all mutants** in the run for speed — the first configure
  is a one-time cost.
- Configuration mirrors the `--preset` (default `debug`) **cacheVariables**
  (`CMAKE_BUILD_TYPE=Debug`, C++ standard, export-compile-commands, …) read from
  `CMakePresets.json`, but with the `binaryDir` overridden to `build/mutation`.
  We use a plain `cmake -S build/mutation-src -B build/mutation -D…` (source =
  the **shadow tree**) rather than `cmake --preset` because `--preset` hard-codes
  `binaryDir=build/<presetName>` and ignores a `binaryDir` override — so the only
  way to keep debug-equivalent flags *and* an isolated dir *and* a shadow source
  is to mirror the cache vars. (A stale `build/mutation` cache pointing at a
  different source dir is detected via `CMAKE_HOME_DIRECTORY` and wiped before
  reconfigure.)
- Every per-mutant `cmake --build` and `ctest` (and the baseline clean-suite
  timing) targets **`build/mutation`** via `--test-dir` — never `build/<preset>`.
- The `--selftest` likewise builds its Part A against the **shadow tree** +
  `build/mutation`, so even the teeth-test never writes the real working tree nor
  pollutes the gate's debug tree.

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

### Same-tick timer ordering — gap closed (`tests/scheduler_timer_order_test.cpp`)

A mutation survived at `core/include/lockstep/core/Scheduler.hpp` (the `due_now[b-1]`
timer insertion-sort, ~line 265), the timer
insertion-sort that orders timers due at the **same virtual tick** by `arm_seq`.
The Phase-1 determinism suite never armed **multiple timers due at the same tick**
and pinned their fire order, so any bug in that ordering went undetected — the
real adequacy hole. `tests/scheduler_timer_order_test.cpp` closes it: it arms
several timers all due at one tick, in a non-trivial arm order, and asserts they
**fire in ascending `arm_seq` order** (observable in the event trace). It KILLS
every *observable* same-tick-ordering mutation of that line — `ROR >`→`<=`,
`LCR &&`→`||`, `AOR -`→`+`, `NEG` — each of which corrupts the fire order or trips
the scheduler's own assertions / the suite's liveness ceiling.

### Documented EQUIVALENT mutant: `Scheduler.hpp due_now[b-1] ABS '1'->'0'`

The specific survivor the suite still reports — `due_now[b - 1]` → `due_now[b -
0]` — is a **proven equivalent mutant** through the scheduler's public surface,
and is therefore **un-killable by any test that does not edit `Scheduler.hpp`**:

- The pending-timer vector `timers_` is **always arm-monotonic**: `delay()`
  appends in arm order, and the firing erase preserves survivors' relative order.
  So the index list the insertion-sort receives is **always already sorted by
  `arm_seq`**. On already-sorted input the correct `b-1` and the no-op `b-0` are
  **byte-for-byte identical** (the inner loop never needs to move anything).
- Feeding the sort an **unsorted** list (the only input on which `b-1` and `b-0`
  differ) is impossible without **also** violating the firing erase loop's
  identical precondition (it erases `due_now[k-1]` descending, assuming ascending
  indices). Verified empirically: white-box injecting **any** non-trivial
  same-tick order corrupts the erase **even on correct code** (out-of-range erase
  / `set_value on empty Promise`). The sort's gather-order invariant and the erase
  loop's precondition are the **same** precondition.

So this mutant guards a branch that is **dead-but-correct under every reachable
state**. It is accepted and documented here as an equivalent mutant — not a test
gap — and the mutation gate's threshold/baseline policy should treat it as such
(it does not, by itself, fail a tree whose threshold is not ratcheted up).

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
  --preset P             preset whose cacheVariables the ISOLATED build/mutation dir
                         mirrors (default: debug); mutants build there, NOT in build/<preset>
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
