# Lockstep — Phase 0 Brief Batch (Scaffolding)

> Source of truth: `lockstep-master-plan.md` §7, `lockstep-phase-specs-all.md` Phase 0, `lockstep-agent-briefs.md`.
> Universal merge gate applies. Phase 0 owner mode: agent-autonomous (no core protocol touched).
> No brief, no work.

## Integration contract (ALL agents conform — guarantees disjoint file ownership + clean integration)

Repo root = `/Users/npden4ik/Projects/lockstep`. Final layout:

```
CMakeLists.txt                      A1   root; add_subdirectory each component
CMakePresets.json                   A1   presets: debug release asan tsan ubsan msan
cmake/Sanitizers.cmake              A1   per-sanitizer flag sets
cmake/Warnings.cmake                A1   -Wall -Wextra -Werror -Wpedantic
cmake/Toolchain.cmake               A1   pin C++23, no compiler extensions
.clang-format                       A1
.clang-tidy                         A1   checks config (CI runs it)
.gitignore                          A1
README.md                           A1   build + gate instructions
docs/coding-standards.md            A1   modern-C++23 rules (cardinal rule 6/7)
tests/CMakeLists.txt                A1
tests/smoke_test.cpp                A1   trivial assert; the "trivial CI green" gate
core/include/lockstep/core/IClock.hpp        A2
core/include/lockstep/core/INetwork.hpp      A2
core/include/lockstep/core/IDisk.hpp         A2
core/include/lockstep/core/IRandom.hpp       A2
core/include/lockstep/core/IScheduler.hpp    A2
core/include/lockstep/core/Error.hpp         A2   shared Error type used by interfaces
core/CMakeLists.txt                          A2   header-only INTERFACE lib `lockstep_core`
providers/sim/CMakeLists.txt        A1   placeholder (empty lib, comment: bodies Phase 1-2)
providers/prod/CMakeLists.txt       A1   placeholder (bodies Phase 7)
storage/CMakeLists.txt              A1   placeholder
log/CMakeLists.txt                  A1   placeholder
txn/CMakeLists.txt                  A1   placeholder
query/CMakeLists.txt                A1   placeholder
harness/CMakeLists.txt              A1   placeholder
bench/CMakeLists.txt                A1   placeholder
tools/lint/forbidden_calls.py       A3
tools/lint/fixtures/                A3   clean + dirty fixtures
tools/lint/test_forbidden_calls.py  A3
tools/mutation/run_mutation.py      A4   skeleton runner (end-to-end, even w/ ~nothing to mutate)
scripts/gate.sh                     A4   gate-as-code, LOCAL runner
.github/workflows/ci.yml            A4   gate-as-code, REMOTE matrix
```

### Frozen names (all agents depend on these — DO NOT diverge)
- C++ namespace: `lockstep::core`. Include path: `<lockstep/core/IClock.hpp>` etc.
- CMake header-only target: `lockstep_core` (INTERFACE). Test links it.
- CMake test target: `lockstep_smoke_test` registered with `add_test(NAME smoke ...)` via CTest.
- CMake presets (names exact): `debug`, `release`, `asan`, `tsan`, `ubsan`, `msan`.
- Standard: C++23, `CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`.
- Generator: prefer Makefiles locally (ninja not installed); CI may use ninja.
- Lint exemption dir: `providers/` only (cardinal rule 1).
- Local-host caveats (must be honored, not hidden): MSan unsupported on macOS/Apple-libc++; clang-tidy + scan-build absent from Apple toolchain. `gate.sh` SKIPS these on this host with an explicit `[SKIP host-limited]` line; CI (ubuntu + llvm) runs them for real.

---

## BRIEF A1 — Build system + repo layout + standards
Phase: 0 · Owner mode: agent-autonomous
Inputs: integration contract above; spec C0.1, C0.4, C0.5; cardinal rules 6,7.
Build: CMake (root + per-component CMakeLists per contract), CMakePresets (6 presets), cmake/ flag modules,
       .clang-format, .clang-tidy, .gitignore, README, docs/coding-standards.md, the trivial smoke test
       (tests/smoke_test.cpp + CTest registration). Placeholder CMakeLists for every component dir so the tree configures.
Invariants: C++23 pinned, extensions OFF, warnings-as-errors. core/ depends on nothing above it. No external dep that
       touches clock/network/disk on core's behalf. Tree configures + builds + `ctest` passes with ZERO real logic.
Forbidden: any std::chrono/std::thread/std::rand etc. in non-provider code (none needed here anyway).
Gate: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` green; same under preset `asan`.
Deliverables: all A1 files in contract. Done when: fresh configure+build+ctest green locally (debug + asan).

## BRIEF A2 — Abstraction-boundary interface headers
Phase: 0 · Owner mode: agent-autonomous
Inputs: contract; spec C0.3; master-plan §5 boundary (Clock/Network/Disk/Random/Scheduler); Phase-1 spec C1.1–C1.5 + Phase-2 C2.1–C2.2 for shape hints ONLY.
Build: pure-virtual INTERFACE headers (no bodies, no impl, no syscalls): IClock, INetwork, IDisk, IRandom, IScheduler, plus Error.hpp.
       Header-only INTERFACE CMake lib `lockstep_core` exposing the include dir.
Invariants: interfaces ONLY — every method pure virtual, virtual dtor, no field state, no implementation, no nondeterministic includes
       (<chrono>, <random>, <thread>, <atomic> coordination, socket/file headers all FORBIDDEN here). Keep minimal + versionable —
       these freeze the boundary; over-specifying now risks Phase 4/5 ABI churn. Document each method contract in comments.
       IClock exposes virtual-time now()+delay()-shaped intent (no wall-clock). IRandom = single seeded PRNG surface.
Forbidden: bodies, concrete types owning resources, <chrono>/<random>/<thread>/raw IO headers.
Gate: headers compile standalone under -std=c++23 -Wall -Wextra -Werror; `lockstep_core` links into the smoke test.
Deliverables: 6 headers + core/CMakeLists.txt. Done when: each header compiles in isolation; smoke test linking lockstep_core builds.

## BRIEF A3 — Forbidden-call lint (Example A, spec C0.2)
Phase: 0 · Owner mode: agent-autonomous
Inputs: contract; cardinal rule 1; the forbidden set.
Build: python3 lint (no pip deps) scanning whole tree EXCEPT `providers/` and failing on any of:
       std::chrono, std::rand, std::random_device, std::thread, raw socket syscalls (socket/bind/connect/accept/send/recv),
       raw file syscalls (open/read/write/close/lseek/fsync as syscalls / <fcntl.h>,<unistd.h> raw IO), coordination atomics
       (std::atomic with memory_order, std::memory_order_*). Skip comments/strings sensibly to cut false positives but ZERO false negatives on the set.
Invariants: zero false negatives on forbidden set; providers/ exempt; deterministic exit code (0 clean, non-0 on hit) + machine-readable findings.
Forbidden: n/a. Gate: a fixture file containing each forbidden call is REJECTED; a clean fixture passes; runnable as `python3 tools/lint/forbidden_calls.py <root>`.
Deliverables: forbidden_calls.py + tools/lint/fixtures/{clean,dirty} + test_forbidden_calls.py. Done when: planted `std::chrono::system_clock::now()` in a core/ fixture fails; clean tree passes.

## BRIEF A4 — Gate-as-code: CI matrix + local gate runner + mutation skeleton (spec C0.2, C0.5)
Phase: 0 · Owner mode: agent-autonomous
Inputs: contract; master-plan §8 merge gate; the toolchain caveats (MSan/tidy/analyzer not local).
Build: (1) `.github/workflows/ci.yml` — the FULL universal gate on ubuntu+llvm: debug+release build, ASan+TSan+UBSan+MSan jobs,
       clang-tidy, clang static analyzer (scan-build), coverage, forbidden-call lint, mutation runner, ctest. Ordered: compile→lint+static→sanitizers→sim battery(placeholder hook)→spec-conformance(placeholder)→linearizability(placeholder)→mutation. Each later-phase stage a clearly-marked no-op hook now.
       (2) `scripts/gate.sh` — local runner reproducing the gate's locally-runnable subset; runs lint + configure/build/ctest under debug,asan,tsan,ubsan + mutation skeleton; prints `[SKIP host-limited]` for msan/clang-tidy/scan-build on darwin; non-zero exit on any real failure; prints a dashboard summary table.
       (3) `tools/mutation/run_mutation.py` — skeleton that runs end-to-end and reports a (trivial) mutation score now; pluggable for real mutants later.
Invariants: gate cannot be bypassed (CI required-checks intent documented); local + CI run the SAME logical battery; skips are LOUD not silent.
Forbidden: n/a. Gate: `bash scripts/gate.sh` exits 0 on the clean scaffold and prints the dashboard; ci.yml is valid YAML.
Deliverables: ci.yml + gate.sh + run_mutation.py. Done when: gate.sh green locally with explicit host-limited skips; ci.yml lints as valid.
