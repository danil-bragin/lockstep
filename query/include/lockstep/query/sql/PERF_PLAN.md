# SQL engine — performance roadmap (beat Postgres ≥3× on target workloads)

> **North star.** Go from a rule-based, row-at-a-time SQL SUBSET to a **cost-based,
> vectorized, sharded** engine that beats mature single-node SQL on the workloads where the
> MODERN approach wins (analytical scans/joins/aggregates first — where DuckDB beats Postgres
> — then OLTP). Every phase stays under the **verification gate** (SQL == independent reference
> model, byte-identical determinism, teeth) — correctness is never traded. Our structural edges
> vs Postgres: (a) **vectorization amortizes our coroutine-frame per-row cost** (the standing
> ceiling) — batching is a DIRECT hit on our bottleneck; (b) **horizontal shards** = native
> multi-core/multi-machine parallelism Postgres lacks; (c) **determinism** = a reproducible,
> seed-stable EXPLAIN ANALYZE that finds algorithmic bottlenecks without wall-clock noise.

## Why the modern approach wins (the design we copy + adapt)
- **Vectorized execution** (MonetDB/X100, Vectorwise, DuckDB, Velox): each operator processes a
  BATCH of ~1–2048 values in a columnar (struct-of-arrays) layout, not one row at a time. Wins
  10–100× on scan/filter/aggregate from cache locality, branch elimination, SIMD, and — for us —
  amortizing the coroutine-frame / per-call overhead across a whole batch. **This is the lever.**
- **Cost-based optimization** (System-R DP join ordering; Cascades framework — Cockroach/SQL
  Server): statistics (row counts, n-distinct, min/max, light histograms, MCV) → cardinality
  estimation → access-path + join-order choice. Replaces our rule-based "first usable plan".
- **Data-centric / push pipelines** (Neumann/Hyper): operators PUSH batches up the pipeline;
  pipeline breakers (sort/hash-build) are the only materialization points.
- **Data skipping**: zone maps / min-max per block, dictionary/RLE for TEXT, late materialization
  (carry row-ids, fetch wide columns only for surviving rows).
- **Morsel-driven parallelism** (Hyper/Leis): split a scan into morsels, process in parallel.
  Maps onto our shards + per-shard reactor threads.

## Phases (each: conformance-gated == reference model + byte-identical + a perf receipt)

### Phase 0 — EXPLAIN + EXPLAIN ANALYZE (TRANSPARENCY) — the user's #1, FIRST
On the EXISTING engine (no execution rewrite yet): a `EXPLAIN <select>` statement that prints the
chosen plan (access path: PK point / index range / full scan; the filter/group/sort/limit stages),
and `EXPLAIN ANALYZE` that runs the query with **DETERMINISTIC cost counters** per stage:
rows scanned, rows after filter, predicate evaluations, comparisons, hash probes, groups, sort
elements, bytes decoded, allocations. NOT wall-clock (breaks sim determinism + is noisy) — the
counters are a pure function of the seed, so a bottleneck is reproducible byte-for-byte. Wall-clock
stays a separate prod-only measurement (the bench). This is the instrument every later phase uses.

### Phase 1 — explicit physical PLAN TREE
Refactor exec_select / exec_select_joined from direct-AST-interpretation into an explicit operator
tree: Scan, IndexScan, Filter, Project, HashAggregate, Sort, Limit, HashJoin, NestedLoopJoin. The
planner BUILDS the tree; the executor DRIVES it. Conformance-preserving (output identical). EXPLAIN
then prints the real tree; the optimizer (Phase 2) rewrites it; vectorization (Phase 3) re-implements
the operators. The keystone refactor — natural boundary is between planning (~Engine.hpp:1757) and
execution (~:567).

### Phase 2 — statistics + cost model + cost-based planning
Catalog gains per-table/-column stats (row count, n-distinct, min/max, an equi-depth histogram,
maintained incrementally on write). A cost model (scan = N·c_row; index = log N + matches·c_fetch;
hash-join build/probe; sort = N log N) drives access-path + join-order selection (DP for ≤~10
tables, greedy beyond). Replaces the rule-based "first usable plan". EXPLAIN shows estimated rows.

### Phase 3 — VECTORIZED execution (THE throughput lever)
Re-implement the operators to process **column batches** (struct-of-arrays of ~1024 values) instead
of one `std::vector<Datum>` per row. Vectorized scan-decode, vectorized predicate eval (bitmap
selection vectors), vectorized hash-aggregate + hash-join, vectorized sort. SIMD where the platform
allows (portable + deterministic). This amortizes the per-row coroutine/dispatch cost (our ceiling)
and is where we beat Postgres on analytics. Determinism preserved (batch boundaries are a pure
function of input; results order-identical).

### Phase 4 — data skipping + encoding
Zone maps (min/max per storage block) → skip blocks that can't match a predicate. Dictionary/RLE
for TEXT columns. Late materialization (carry PKs/row-ids through filters/joins, fetch wide columns
only for survivors). Cuts scan + decode work — compounds with vectorization.

### Phase 5 — morsel-driven parallelism (our horizontal edge)
Parallelize a single query's scan/aggregate across shards + per-shard reactor threads (the real
threads already sanctioned in providers/prod). A query splits into morsels; partial aggregates merge
deterministically. This is the multi-core SQL scaling Postgres does via backends — we do it via our
shard threads, under the determinism gate (deterministic partition + merge order).

### Phase 6 — breadth + transactions
Multi-statement interactive txns (BEGIN/COMMIT, savepoints) over the verified txn surface; more
types (BIGINT/DECIMAL/BOOL/TIMESTAMP-as-int/BLOB), constraints, more SQL (CTE, window functions,
more join types). Closes the feature-breadth gap (the biggest "can it even run real SQL" gap).

## Verification discipline (every phase)
- `sql_conformance` / `sql_aggregates` / `sql_join` / `sql_index` / `sql_subquery` == independent
  reference model at elevated seeds; byte-identical determinism (two runs diff EMPTY); teeth catch a
  wrong rewrite. A cost-based plan must return the SAME ROWS as the naive plan (an access path is not
  a semantic change — exactly the sql_index transparency invariant).
- Perf receipt per phase: the deterministic EXPLAIN-ANALYZE counters (load-independent) + the prod
  wall-clock bench (lockstepd --wire-server + lockstep_sqlbench) vs Postgres/Cockroach on the same
  workload + resource pin.

### Phase 3 — MEASURED FINDING (profile-first, the methodology paying off)
A first vectorized increment — a flat-conjunct filter (`not_null_col <op> literal` AND-chain,
reusing apply_cmp/cmp_datum, byte-identical, interpreter fallback for OR/NULL/subquery) — was
landed AND A/B-measured (set_vectorize toggle): VECTORIZED 2156 ms vs interpreter 2116 ms over a
50k-row scan — IDENTICAL. So the predicate EVAL is NOT the bottleneck; the per-row DECODE is (each
scanned row mints a fresh `std::vector<Datum>` — 50k allocations per query). EXPLAIN ANALYZE shows
row COUNTS, not where the CPU goes — the real hot spot is decode/alloc, not filter. So Phase 3's
true lever is COLUMNAR BATCH DECODE: decode a batch of rows' needed columns into reused
struct-of-arrays buffers (a few allocations per batch, not one vector per row), then run the
(already-extracted) conjuncts as raw column passes (SIMD-friendly) over those arrays. The flat
conjunct extractor is kept as the reusable substrate for that columnar filter. NEXT: columnar
decode + columnar materialization (the actual speedup), measured the same way.
