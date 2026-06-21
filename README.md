# Lockstep

A C++23 deterministic distributed database. The entire system is a pure function
of `(seed, initial tasks)`: all nondeterminism (clock, network, disk, randomness,
scheduling) flows through an abstraction boundary, so any run reproduces
byte-for-byte from its seed under simulation.

This repo is currently at **Phase 0 — Scaffolding**: build, CI, verification
skeleton, and the abstraction-boundary interface headers. No functional logic
yet — but the gates that govern everything else exist and pass.

## Requirements

- **CMake ≥ 3.24**
- A **C++23** compiler. Developed on **Apple clang 17** (macOS); CI also uses
  upstream LLVM on Ubuntu.
- **Generator:** Unix Makefiles locally (ninja is not assumed installed). CI may
  use ninja.
- **Python 3** for the lint / mutation tooling under `tools/`.
- No external C++ dependencies.

## Layout

```
core/         deterministic runtime + abstraction-boundary interface headers
providers/    sim/ (virtual time, fault injection) and prod/ (io_uring/NVMe/sockets)
storage/      MVCC key-value engine (WAL, memtable, SSTable, compaction)
log/          replicated log / consensus + sequencer
txn/          distributed transactions + consistency selector
query/        query / protocol layer
harness/      sim driver, fault injectors, checkers, seed infrastructure
bench/        benchmark harness
specs/        TLA+ specifications
tests/        smoke + (later) property/integration tests
tools/        forbidden-call lint, mutation runner
cmake/        Toolchain / Warnings / Sanitizers flag modules
docs/         coding standards
```

Most component directories are placeholders in Phase 0 (empty INTERFACE libs)
and gain bodies in their owning phase.

## Configure / build / test

The project uses **CMake presets**. Available presets:
`debug`, `release`, `asan`, `tsan`, `ubsan`, `msan`.

```sh
# Debug
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

# AddressSanitizer
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

Substitute `tsan` / `ubsan` / `release` the same way.

Build trees go to `build/<preset>/` (git-ignored).

### MemorySanitizer (`msan`) — host note

MSan is **Linux/LLVM-only**: Apple clang + macOS libc++ ship no MSan runtime and
no instrumented libc++. On macOS the `msan` preset still *configures* (with a
loud warning) but builds **without** MSan instrumentation. The real MSan job runs
in CI on ubuntu + upstream llvm. See `cmake/Sanitizers.cmake`.

Likewise **clang-tidy** and **clang static analyzer (scan-build)** are absent from
the Apple command-line toolchain and are skipped locally — CI runs them for real.

## Running the gate

`scripts/gate.sh` is the local runner that reproduces the locally-runnable subset
of the universal merge gate (forbidden-call lint + configure/build/ctest under
debug/asan/tsan/ubsan + the mutation skeleton). It prints `[SKIP host-limited]`
for anything that can only run in CI (msan, clang-tidy, scan-build), exits
non-zero on any real failure, and prints a dashboard summary.

```sh
bash scripts/gate.sh
```

> `scripts/gate.sh`, the CI workflow, and the lint/mutation tooling are authored
> by other Phase 0 agents (A3/A4). Until they land, use the per-preset commands
> above.

## Standards

See [`docs/coding-standards.md`](docs/coding-standards.md): RAII, no raw owning
pointers, no naked `new`/`delete`, coroutines for async, and no determinism leaks
(cardinal rules 6 & 7). These are enforced mechanically, not by review.
