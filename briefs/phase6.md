# Lockstep — Phase 6 Brief Batch (Query / Protocol Layer + Breadth)

> Source of truth: lockstep-phase-specs-all.md Phase 6 (C6.1-C6.6), master-plan D3/D5.
> The developer-facing surface. Agents go WIDE here (breadth zone) — but the merge gate still applies
> (compile + ASan/TSan/UBSan + forbidden-lint + ctest + mutation; full §8 via scripts/gate-linux.sh).
> RESOURCE DISCIPLINE (a freeze happened): heavy/gate agents ONE AT A TIME; ulimit -c 0 / -s 16384; -j6;
> in-gate sweeps <=64 seeds; bounded; stray *.mutbak = interrupted mutation, restore before trusting.

## Scope (C6.1-C6.6)
- C6.1 transaction-function model (D1/D3): users author ONE-SHOT txn functions (params in / result out, NOT
  interactive). User txn code must itself be DETERMINISTIC — the surface only exposes deterministic read/write
  handles; no ambient clock/random/IO in the body.
- C6.2 read/query language: typed, composable, NON-SQL surface; a planner mapping queries to versioned storage
  reads; the D5 selector exposed safely (call-site-visible / type-encoded, can't be silently misused).
- C6.3 wire protocol: the client network protocol (new; Postgres-wire shim explicitly deferred).
- C6.4 drivers/SDKs: one reference-language driver first, then fan out.
- C6.5 CLI + admin tooling.
- C6.6 docs, examples, conformance suite for the txn/query semantics.

## Staged build (foundation first, then breadth)
- **Stage F (DISPATCH NOW):** the CLIENT SURFACE — a typed Database/Client API that (a) lets a user author + submit
  one-shot txn functions over the existing txn::Executor seam, (b) exposes a typed read/query surface (get/scan/range)
  over storage::Engine MVCC at a chosen D5 level (call-site-visible), (c) a conformance test that authors example
  txns + queries and checks results vs expected / the strict-serializable oracle. This is what protocol/drivers/CLI
  build on. Build over txn/ (Executor, Transaction, the D5 Level selector) + storage/ (Engine) + the verified stack.
- **Stage B (after F, WIDE):** wire protocol (C6.3) · reference driver (C6.4) · CLI (C6.5) · docs+examples+conformance
  (C6.6). Disjoint-ish — fan out, each through the gate.

## Contracts / invariants
- V-DET-USER: a user txn body is a pure function of its declared reads; the surface gives no nondeterministic handle.
- V-D5-SAFE: a read's consistency level is in the type/call (default strict-serializable); ⊥ silent misuse.
- V-CONFORM: txn/query semantics match the strict-serializable oracle on the default path; relaxed levels match
  their D5 checker. Reuse Phase-5 oracle + D5 checkers where possible.
- Determinism + the universal gate as always. query/ NOT lint-exempt.
