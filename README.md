# Lockstep

[![CI](https://github.com/danil-bragin/lockstep/actions/workflows/ci.yml/badge.svg)](https://github.com/danil-bragin/lockstep/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)

A C++23 **deterministic distributed database**. The entire system is a pure
function of `(seed, initial tasks)`: every source of nondeterminism — clock,
network, disk, randomness, thread scheduling — flows through an abstraction
boundary, so any run reproduces **byte-for-byte** from its seed under simulation.
That property is the foundation for the verification strategy: exhaustive fault
injection, differential testing against independent reference models, formal
TLA+ model-checking, and a two-implementation consensus cross-check.

> **Status: a working end-to-end stack, built as a research/learning project.**
> Storage, consensus, distributed transactions, and a SQL surface are all
> implemented and gated. The benchmarks are honest baselines on a developer
> laptop — *not* production numbers. Not intended for production use.

## What's built

- **Deterministic runtime** — a coroutine scheduler over virtual time; all async
  I/O is a coroutine on the scheduler. One `(seed, tasks)` → one byte-identical
  run. (`core/`)
- **Simulation + production providers** — `sim/` injects virtual time, network
  partitions, and disk faults (torn writes, lying fsync, latency); `prod/` is the
  real epoll reactor + sockets + io_uring `fdatasync` (Linux). Verified core code
  runs unchanged on both. (`providers/`)
- **MVCC storage engine** — WAL + memtable + SSTable LSM with size-tiered
  compaction, bloom filters, sparse index, WiscKey value-log separation, and
  crash-consistent recovery (CRC'd records, atomic manifest install). A
  namespace-aware *selective flush* lets a columnar layer keep its blocks resident
  while the LSM bounds the row/index memtable. (`storage/`)
- **Raft consensus, twice** — two independent Raft implementations (`raft_a`,
  `raft_b`) run the same workloads and are **cross-checked** for per-client
  committed-order consistency; single-server membership change; batching +
  pipelining. A sequencer orders cross-shard work. (`consensus/`)
- **Distributed transactions** — a deterministic executor cross-checked against an
  independent one and an oracle; multi-shard with Calvin-style cross-shard atomic
  commit (all-or-nothing, no 2PC), proven under fault storms. (`txn/`)
- **SQL surface** — a from-scratch SQL engine over the verified storage/txn layers:
  joins (hash + index), aggregates, `GROUP BY` / `HAVING`, subqueries, window
  functions, set ops, constraints (PK/FK/UNIQUE/CHECK/DEFAULT), secondary + GIN +
  expression indexes, transactions, a rich type system (DECIMAL, INT128/UINT256,
  DATE/TIME/INTERVAL, UUID, JSON, ARRAY, ENUM), and a row + columnar dual layout
  proven byte-identical. Vectorized + morsel-parallel analytic execution.
  (`query/`)
- **Distributed SQL** — scatter/gather across shards with a **co-located-shuffle**
  star-JOIN pushdown (the large fact is never gathered): `WHERE` / `AVG` / `HAVING`
  pushdown, multi-dimension stars, a global `COUNT(DISTINCT)` shuffle, and a
  broadcast-dim (replicated dimension) join — each proven byte-identical to the
  single-node result. (`query/include/lockstep/query/sql/DistributedSql.hpp`)
- **Security** — mutual-TLS transport (OpenSSL memory-BIO sessions over the prod
  reactor) and certificate-CN → role RBAC (default-deny). (`providers/prod/`)
- **Formal specs** — TLA+ models for consensus, commit ordering, snapshots,
  membership, the sequencer, and cross-shard commit, checked by TLC. (`specs/`)

## Verification

Determinism is what makes the testing exhaustive and reproducible:

- **Differential / metamorphic testing** — the storage engine, executor, and SQL
  engine are each checked against an independent reference model on every seed.
- **Consensus cross-check** — two Raft impls must agree (a sound per-client
  relative-order predicate), so a bug has to occur identically in both to slip.
- **Fault storms** — Jepsen-style crash / partition / torn-write / lying-fsync
  injection; recovered state is always a valid committed prefix (no loss, no
  fabrication).
- **Formal model-checking** — TLC over the TLA+ specs in `specs/`.
- **Sanitizers** — ASan / TSan / UBSan / MSan (MSan over an instrumented libc++).
- **Mutation testing**, **clang-tidy**, **scan-build**, and a **forbidden-call
  lint** (no determinism leaks: no wall-clock, no raw `<random>`, no naked threads
  outside `providers/prod/`).

## Layout

```
core/         deterministic runtime (coroutine scheduler, virtual clock, I/O seams)
providers/    sim/ (virtual time + fault injection) and prod/ (epoll, sockets, io_uring, TLS)
storage/      MVCC engine: WAL, memtable, SSTable LSM, compaction, value-log, recovery
consensus/    two Raft impls (raft_a / raft_b) + cross-check + sequencer
txn/          deterministic transaction executor, cross-shard commit, oracle + checkers
query/        SQL engine (parser, planner, row + columnar execution) + distributed SQL
harness/      sim driver, fault injectors, conformance checkers, seed infrastructure
bench/        benchmark harnesses (storage, SQL surface, competitive comparison)
specs/        TLA+ specifications (+ .check.md companions)
cli/          lockstep_cli, lockstepd (cluster daemon), lockstep_admin, bench drivers
tests/        property / differential / integration / conformance tests
tools/        forbidden-call lint, mutation runner
cmake/        Warnings / Sanitizers / toolchain modules
docs/         coding standards + design notes
```

## Build & test

Requirements: **CMake ≥ 3.24**, a **C++23** compiler (developed on Apple clang;
CI uses upstream LLVM on Ubuntu), **Python 3** for the lint/mutation tooling. No
external C++ dependencies (OpenSSL only for the optional TLS transport).

The project uses **CMake presets**: `debug`, `release`, `asan`, `tsan`, `ubsan`,
`msan`.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Substitute `release` / `asan` / `tsan` / `ubsan` the same way. Build trees go to
`build/<preset>/` (git-ignored).

**Host notes.** MSan, clang-tidy, and scan-build are Linux/LLVM-only and are
skipped on macOS (Apple clang ships no MSan runtime / instrumented libc++); CI
runs them for real. The production reactor (`lockstepd` and friends) is epoll-based
and **Linux-only** — those CLI targets are not built on macOS.

## Running the gate

`scripts/gate.sh` reproduces the locally-runnable subset of the merge gate
(forbidden-call lint + configure/build/ctest under debug/asan/tsan/ubsan + the
mutation skeleton). It prints `[SKIP host-limited]` for CI-only stages, exits
non-zero on any real failure, and prints a dashboard.

```sh
bash scripts/gate.sh
```

## Try it

```sh
# Single-process SQL / KV demo
cmake --build --preset release --target lockstep_cli
./build/release/cli/lockstep_cli

# A 3-process Raft cluster over real TCP (Linux)
cmake --build --preset release --target lockstepd lockstep_admin
# ...launch three lockstepd processes, then drive them with lockstep_admin

# Benchmarks
./build/release/bench/lockstep_bench_driver       # storage write/read sweep
./build/release/bench/lockstep_sql_bench_driver    # SQL surface micro-bench
```

## Standards

See [`docs/coding-standards.md`](docs/coding-standards.md): RAII, no raw owning
pointers, no naked `new`/`delete`, coroutines for async, and no determinism leaks.
These are enforced mechanically by the lint + sanitizer gates, not by review.

## License

[Apache License 2.0](LICENSE).
