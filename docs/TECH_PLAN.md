# Lockstep — Technical Master Plan

*Status: living engineering plan. Owner: danil-bragin. Created 2026-07-05.
Companion to `docs/PRODUCT_ROADMAP.md` (M0–M2 packaging milestones deliberately deferred;
this document is the engineering-first track).*

Two parts:

- **Part A — Gap closure (W1–W9):** every known technical gap, as a workstream with
  concrete code seams, step-by-step subtasks, and a verification gate.
- **Part B — Killer features (K1–K7):** features chosen because they (a) have proven
  market demand and (b) are *structurally cheaper for us* than for competitors thanks
  to determinism / MVCC / Calvin / the dual-layout engine — i.e. features where the
  architecture is the moat.

Effort units: **S** ≤ 1 session · **M** 2–5 · **L** 5–15 · **XL** phase-of-its-own.

**Non-negotiables (every workstream):** durability and determinism sacrosanct; the
dual-Raft cross-check stays byte-identical; every consensus change strictly additive;
SQL changes gated by the conformance suite (byte-identical vs the reference model);
new invariants get teeth (a deliberately broken variant must be caught); ASan/UBSan
(and TSan where threads) green; profile before optimizing.

---

# Part A — Gap closure

## W1 — Durability truth: N=1 commit-before-fsync fix *(M)* 🔴

**Problem.** At N=1, self-commit advances `commit_index` when the persist is
*enqueued*, not when the fsync *completes* — a committed-but-not-durable window.
The async io_uring path exposed it; sync fsync masked it. N≥2 is safe via quorum.
Documented in the backprop ledger; the proper core fix was deferred. Single-node is
the first deployment every evaluator runs, so this is the first gap to close.

**Design.** The self-vote for an entry must not count toward commitment until the
local persist completion for that entry's index has fired. Concretely: track
`durable_index` (highest index whose fsync completed) per node; the commit rule uses
`min(match_index[self], durable_index)` for the self contribution. At N≥2 this is a
no-op in practice (quorum already covers it) — the change must be *provably* inert
there: cross-check byte-identical.

**Steps.**
- [ ] W1.1 Spec first: extend the consensus TLA+ model with a `durable` per-node
      variable and the invariant `commitIndex ≤ SelfDurableOrQuorum`; add the teeth
      variant (commit-on-enqueue) and confirm TLC catches it.
- [ ] W1.2 Implement in `raft_a`: persist-completion callback advances
      `durable_index`; commit rule change. Strictly additive.
- [ ] W1.3 Implement identically in `raft_b` (independent code, same contract).
- [ ] W1.4 Cross-check: full AGREE run — byte-identical at N=1,3,5.
- [ ] W1.5 Fault-storm proof: N=1 with async io_uring fsync + kill-between
      enqueue-and-completion; recovered state must never contain an acked-but-not-fsynced
      write. Remove the clean-exit mitigation note from the backprop entry.

**Gate.** TLC green incl. teeth · cross-check byte-identical · jepsen N=1 io_uring
storm zero acked-loss · perf: N=1 commit throughput regression ≤ 5% (group commit
should absorb the latency).

## W2 — On-disk & wire format versioning *(M)* 🔴

**Problem.** No `format_version` anywhere (grep confirms): WAL records (KV, query,
catalog), SSTable header/footer, manifest, value-log segments, snapshots, backup
archives, raft persisted state, and the wire hello are all unversioned. Any layout
change silently corrupts existing data; a mixed-version cluster is undefined
behavior. Cheap now, impossible later — prerequisite for every upgrade story.

**Steps.**
- [ ] W2.1 Inventory: one table in `specs/storage-engine.md` listing every persisted
      record type, its current magic (if any), and its owner file.
- [ ] W2.2 Stamp: `magic (u32) + format_version (u16)` on every file-level header
      (SSTable, manifest, vlog segment, snapshot, backup, raft state) and a version
      byte in the WAL stream header. Current version = 1; readers refuse `> known`
      with a loud, named error; `< known` reserved for future migration hooks.
- [ ] W2.3 Wire: version field in the connect/hello frame of the internal RPC and
      admin protocols; mismatch → refuse with both versions in the message.
      (PG wire already carries its own protocol version — untouched.)
- [ ] W2.4 Fixture tests: commit version-1 fixture files under `tests/fixtures/`;
      a test opens each fixture forever after (backward-compat canary). A
      hand-corrupted version=99 fixture must be refused (teeth).
- [ ] W2.5 Policy doc: `docs/FORMAT_VERSIONS.md` — what bumps the version, what the
      compat contract is (N reads N and N−1, once migration exists).

**Gate.** All existing tests green (format change is additive header) · fixture
canaries in CI · recovery/PITR/backup round-trips still byte-exact.

## W3 — Resource governance *(L)* 🔴

**Problem.** Zero limits anywhere. Concretely: PG `CancelRequest` (code 80877102)
unhandled — a hung `SELECT` cannot be killed and psql Ctrl+C is ignored; no
`statement_timeout` (grep empty repo-wide); `SqlEngine` materializes intermediates
(derived tables / CTEs / views → ephemeral tables, hash-join builds, GROUP BY
tables, sort buffers) with **no memory accounting** — one bad cross join = OOM =
node death; the wire server's submit queue has no backpressure — one fast client
inflates memory unboundedly. First prod OOM kills trust permanently.

**Design constraints.** Wall-clock is forbidden in core (forbidden-call lint), so
*timeouts are a boundary concern*: deadlines come from `IClock` (virtual in sim,
real at the prod reactor) and arrive in the engine as a **cancellation flag** —
core stays deterministic. Memory limits are deterministic by construction (byte
counters, not allocator introspection): the same query + same limit → the same
deterministic error on every replica, so replicated statement execution stays
byte-identical across nodes.

**Steps.**
- [ ] W3.1 (M) Memory accounting in `SqlEngine`: a per-statement `QueryArena`
      counter charged by the ephemeral-table materializer, hash-join build side,
      aggregation hash table, and sort buffers; `SET lockstep.max_query_memory = N`
      (session + server default). Exceeding → deterministic
      `query memory limit exceeded (N bytes)` error, statement aborted, txn intact
      (SAVEPOINT machinery already handles partial rollback).
- [ ] W3.2 (M) Cancellation seam: an atomic cancel flag on the session, checked at
      morsel boundaries, per-K-rows in row loops, and between pipeline stages.
      In sim it is driven by virtual-time tests (deterministic); in prod by the
      reactor deadline timer.
- [ ] W3.3 (S) `statement_timeout` (session + server default) implemented on the
      W3.2 seam at the prod boundary.
- [ ] W3.4 (M) PG `CancelRequest`: issue `BackendKeyData` (pid, secret) at startup;
      accept the 80877102 packet on a fresh connection; route to the session's
      cancel flag. Verify psql Ctrl+C aborts a long query and the connection
      survives.
- [ ] W3.5 (M) Wire backpressure: bounded per-connection and global submit queues;
      overflow → deterministic park (flow control) or typed reject, never
      unbounded growth. Fault-storm with a firehose client: memory stays bounded.
- [ ] W3.6 (S) Admission control: `max_concurrent_statements`; excess queued FIFO
      with queue-depth metric.
- [ ] W3.7 (S) Observability: per-query peak-memory + duration in the slow-query
      log (structured-log extension); `lockstep_admin top` shows active statements
      with cancel capability.

**Gate.** Conformance byte-identical (limits off by default in the conformance
run) · deterministic-error test: same seed + limit → identical error on every
replica · psql Ctrl+C e2e in Docker · firehose fault-storm bounded-memory check ·
ASan/TSan green.

## W4 — Vectorized hash aggregation: close the DuckDB gap *(L)* 🟡

**Problem.** Columnar GROUP BY trails DuckDB/ClickHouse 2–10× (groupby_cat 8.59 ms
vs their ~2.5 ms at 1M×1cpu) — they run vectorized hash aggregation with SIMD
filters; our grouped fold is per-row. We already have the two prerequisites:
dictionary encoding for low-cardinality TEXT blocks (83ccf35) and a morsel-parallel
executor. This closes the analytics claim to unconditional.

**Design.** Three tiers by group-key shape, chosen per block from block metadata:
1. **Dict-coded TEXT key, low cardinality:** group id = dict code → *direct-index
   accumulator arrays* (no hashing at all); merge per-morsel arrays by index.
2. **Small-domain INT key (min/max from zone map fits a window):** offset-index
   accumulators, same as (1).
3. **General:** open-addressing hash table with batched probing (compute all hashes
   for a morsel, then probe), grow-by-doubling, allocation charged to W3.1 arena.
Filters produce **selection vectors** feeding the aggregator (the vectorized filter
already exists — 2.65× — extend it to emit selections rather than materialize).

**Steps.**
- [ ] W4.1 (S) Profile harness: per-shape flame profiles for the 5 bench queries;
      record the baseline table in `query/include/lockstep/query/sql/PERF_PLAN.md`.
- [ ] W4.2 (M) Selection-vector plumbing: filter kernels emit `sel[]`; aggregation
      consumes `(block, sel)` pairs; conformance byte-identical.
- [ ] W4.3 (M) Tier-1 direct-index aggregation over dict codes (SUM/COUNT/MIN/MAX/AVG);
      morsel-local arrays + index-wise merge, reusing the existing group-count gate
      (2f92f06) to pick tiers.
- [ ] W4.4 (M) Tier-2 small-domain INT keys via zone-map ranges.
- [ ] W4.5 (L) Tier-3 batched-probe hash table; composite keys packed to a single
      u64 code where widths allow, else byte-string keys.
- [ ] W4.6 (S) SIMD pass over the hot kernels (explicit pragmas / intrinsics where
      the autovectorizer fails — check disassembly, keep portable fallback).
- [ ] W4.7 (S) Re-run `bench/compare/sql_analytics` vs DuckDB/ClickHouse; update
      the report honestly.

**Gate.** Conformance byte-identical after *every* step (row vs columnar dual-layout
equality is the strongest guard) · target ≥ 0.8× DuckDB on all five shapes ·
no regression on the row path · ASan/UBSan green.

## W5 — Statistics + cost-based optimization *(L)* 🟡

**Problem.** Joins execute in FROM order; build-side choice is heuristic; no
ANALYZE, no row counts, no NDV. A 3+‑way join in an unlucky order is a 100×
cliff — the wall for the first real workload.

**Design.** Deterministic statistics (hash-based KMV sketch for NDV — hashing is
seeded and deterministic, so stats are byte-identical across replicas), stored in
the catalog store under a new namespace (`0x05`, clear of tables 0x01 / markers
0x02 / views 0x04). Cost model deliberately minimal: cardinality × width for scans,
build+probe estimate for hash joins, selectivity from min/max + NDV. Join
reordering: exhaustive DP up to 8 relations, greedy beyond. **Plans may change;
results may not** — the differential suite is the safety net.

**Steps.**
- [ ] W5.1 (M) `ANALYZE [table]`: row count, per-column min/max/null-fraction/NDV
      (KMV k=256); persisted at `0x05`, recovered in `reprime_catalog_from_store`;
      replicated as DDL-class statement (broadcast in `DistributedSql`).
- [ ] W5.2 (S) Auto-stats trigger: re-ANALYZE when a table's row delta exceeds 20%
      (deterministic counter, not time-based).
- [ ] W5.3 (M) Selectivity estimation for predicates (=, range, IN, AND/OR
      independence assumption); wire into EXPLAIN as `est_rows`.
- [ ] W5.4 (M) Hash-join build-side choice + join reordering (DPccp ≤ 8, greedy
      above); cost-annotated EXPLAIN.
- [ ] W5.5 (S) Regression corpus: a set of multi-join queries where the naive order
      is pathological; assert both the chosen order (EXPLAIN snapshot) and the
      runtime bound.

**Gate.** Conformance + differential model byte-identical on every seed (plans
change, answers don't) · EXPLAIN snapshots deterministic · the pathological corpus
speeds up ≥ 10×.

## W6 — Raft learners (non-voting replicas) *(M)* 🟡

**Problem.** Membership is single-server add/remove of *voters* only. Adding a
voter that starts empty immediately counts toward quorum — a real availability/risk
window during node replacement. No learner state means no safe scale-out, no
read-replica story, and no data-movement primitive for W7.

**Steps.**
- [ ] W6.1 Spec: extend `Membership.tla` with a learner set — replicated to, never
      votes, never counted in any quorum, promotion is a config change gated on
      catch-up. Teeth: a variant that counts learners in quorum must break
      QuorumOverlap in TLC.
- [ ] W6.2 Implement in both impls (additive `ConfigChange` op kinds: AddLearner /
      PromoteLearner / RemoveLearner); replication to learners reuses the existing
      AppendEntries path unchanged.
- [ ] W6.3 Promotion gate: leader promotes only when learner `match_index` is
      within a configured lag of `commit_index`.
- [ ] W6.4 `lockstep_admin add-learner / promote / remove` + status output.
- [ ] W6.5 Fault-storm: kill/partition learners mid-catch-up — no quorum impact,
      no commit stall; promote under storm — linearizability holds.

**Gate.** TLC incl. teeth · cross-check byte-identical (learner traffic excluded
from the committed-order predicate by construction) · storm green.

## W7 — Dynamic shard split / move / rebalance *(XL)* 🟡 — biggest architectural gap

**Problem.** key→shard is static from boot (`txn/CrossShard.hpp` router). A hot
shard cannot split; a node cannot be drained; scale-out = recreate the cluster.
This is the headline feature of CockroachDB/TiDB and our largest structural gap.

**Design — the Calvin advantage.** In a Calvin system, *data movement is just a
transaction*: the sequencer already gives a total order, so a range move is an
atomically-ordered "freeze range → copy → flip ownership" that every shard observes
at the same sequence position. No consensus-level surgery, no 2PC. Ownership lives
in a **meta-shard** (its own Raft group) holding a versioned routing table
(`range → shard, epoch`); routers cache it and retry on epoch mismatch (a typed
`WrongEpoch` redirect, never a wrong answer).

**Steps.**
- [ ] W7.1 (M) Spec: `specs/RangeMove.tla` — invariants: every key owned by exactly
      one shard at every sequence point (NoOrphan, NoDualOwner); a move is
      all-or-nothing; reads/writes during a move are never lost or duplicated.
      Teeth: a variant that applies the flip on the source before the copy
      completes must violate NoLoss.
- [ ] W7.2 (M) Meta-shard: routing table as a replicated state machine; epoch bump
      per change; router cache + `WrongEpoch` retry protocol.
- [ ] W7.3 (M) Range abstraction: replace the static key→shard hash with
      range-partitioned routing (contiguous key ranges), initially 1 range/shard —
      byte-identical behavior to today (canary step).
- [ ] W7.4 (L) Split: leader-local range split at a chosen key (metadata-only,
      data stays); meta-shard transaction publishes the new range pair.
- [ ] W7.5 (L) Move: learner-assisted copy (W6) of the range's data to the target
      shard, then the cutover transaction through the sequencer: freeze (reject
      with WrongEpoch), final delta ship, ownership flip, unfreeze. Bounded freeze
      window; measure it.
- [ ] W7.6 (M) Merge (inverse of split, same machinery).
- [ ] W7.7 (M) Balancer policy: per-range load counters (deterministic op counts,
      not wall-clock rates) → advisory `lockstep_admin rebalance plan` first,
      auto-mode behind a flag later.
- [ ] W7.8 (L) Proof: fault-storm with continuous cross-shard transactions during
      split/move/merge — oracle equality, exactly-once, zero lost/dup keys;
      cross-check byte-identical throughout.

**Gate.** TLC incl. teeth · the W7.3 canary is byte-identical to the static router ·
storm-under-movement green · a live 3→6 shard expansion under load with zero client
errors (scripted, becomes the demo).

## W8 — Coroutine-flattening spike: the per-shard ceiling *(M spike, XL if go)* 🟡

**Problem.** ~17k commits/s/shard single-thread ceiling; profiling already ruled
out heap (frames pooled) and fsync — it is coroutine suspend/resume + `shared_ptr`
atomic refcounts + real work. The real fix (flatten the hot path into explicit
state machines) is a big risky rewrite. Rule: **spike before committing.**

**Steps.**
- [ ] W8.1 Define the measured path: submit → append → replicate → ack → commit →
      apply, N=1 and N=3, group-commit on.
- [ ] W8.2 Branch: flatten *only* this path in `raft_a` (hand-rolled state struct,
      intrusive refcounts / arena ownership instead of `shared_ptr`), sim-only.
- [ ] W8.3 Measure: same bench, same seeds. Record flame-graph deltas.
- [ ] W8.4 Go/no-go doc in `bench/PERF_BASELINE.md`: gain ≥ 2× → schedule the real
      rewrite (both impls, full gate) as its own phase; < 2× → close the question,
      the ceiling is "real work", horizontal scaling remains the answer.

**Gate (spike).** Byte-identical results on the branch (determinism must survive
flattening — this is also the riskiest part and the reason to spike) · honest
numbers either way.

## W9 — SQL / PG surface completion *(M each, demand-ordered)* 🟢

- [ ] W9.1 **COPY FROM/TO** (text, CSV, binary): the extended-protocol CopyIn/
      CopyOut messages; batched through the existing group-commit write path.
      Bulk load is the first thing every evaluator does; today it's row-at-a-time
      INSERT with a full parse each.
- [ ] W9.2 **Prepared-statement plan cache**: statements are stored as text and
      re-parsed per Execute (`PgWire.hpp:442`); cache parsed AST (and later, the
      W5 plan) keyed on (sql, catalog epoch); invalidate on DDL.
- [ ] W9.3 **PG-native users**: CREATE USER / ALTER / GRANT on an own catalog table
      (namespace alongside 0x04/0x05), SCRAM verifiers stored, mapped onto the
      existing default-deny RBAC; cert-CN remains for internal/admin auth.
- [ ] W9.4 **pg_catalog / information_schema shims** sufficient for `\d`, `\dt`,
      and ORM introspection (fixed virtual tables over the catalog).
- [ ] W9.5 **Sequences** (`CREATE SEQUENCE`, `nextval`) — deterministic, replicated
      (a sequence bump is a tiny keyed write).
- [ ] W9.6 Distributed-SQL residuals: SUM/AVG(DISTINCT) shuffle; snowflake
      (dim→dim) join pushdown.
- [ ] W9.7 Cursors (`DECLARE/FETCH`) — portal suspension already half-exists in the
      extended protocol; finish it.

**Gate.** Each lands with its own conformance additions; ORM smoke tests (psql,
SQLAlchemy, pgx) against `lockstep_pgd` in Docker.

---

# Part B — Killer features

Selection rule: proven external demand **and** an architectural unfair advantage —
each of these is materially harder for CockroachDB/Postgres/DuckDB to copy than for
us to build.

## K1 — HA vector search (pgvector-compatible) *(L)* — demand: AI applications

**Why us.** pgvector is the de-facto AI-app store but rides single-primary
Postgres; the HA options are expensive managed services. A **Raft-replicated,
strongly-consistent vector store speaking the pgvector SQL dialect** fills a real
gap. Bonus nobody else has: HNSW construction uses random level assignment — under
our seeded determinism the index is **reproducible byte-for-byte**, so replicas can
verify index equality (the keyspace-hash machinery extends to index blocks) and a
rebuild is provably identical. "Deterministic ANN" is a publishable novelty.

**Steps.**
- [ ] K1.1 `vector(n)` type (float32 array, dimension-checked), literals, casts —
      slots into the existing type system (logical id next after UINT256).
- [ ] K1.2 Distance operators `<->` (L2), `<#>` (IP), `<=>` (cosine) with SIMD
      kernels; exact scan first (row + columnar; columnar SoA float blocks are
      already the ideal layout).
- [ ] K1.3 `CREATE INDEX ... USING ivfflat` (k-means lists; deterministic seeded
      init) — simpler than HNSW, ships first; `probes` session knob.
- [ ] K1.4 `CREATE INDEX ... USING hnsw` — seeded level assignment (SeededRandom),
      deterministic build; WAL-logged like existing secondary indexes.
- [ ] K1.5 Recall/latency bench vs pgvector at 1M×768d; honest report.
- [ ] K1.6 Index == scan differential gate (the J-series discipline) extended with
      recall bounds for ANN (exact-scan oracle).

## K2 — Change Data Capture *(M–L)* — demand: universal (search/cache/warehouse sync)

**Why us.** The op-log export already exists (`Pitr.hpp` / `WalEngine::export_ops`)
with a total order and a stable cursor: **Seq**. CDC is then a streaming read of an
existing verified structure — exactly-once resume comes free from the Seq cursor,
and the Calvin total order means cross-shard changefeeds have a *global* order
(Cockroach's changefeeds are per-range with resolved-timestamp complexity; ours is
structurally simpler).

**Steps.**
- [ ] K2.1 `CREATE CHANGEFEED FOR TABLE t [FROM SEQ n]` → wire-protocol stream of
      committed row changes (old/new, JSON envelope), resume token = Seq.
- [ ] K2.2 Backpressure + retention interaction (a changefeed pins the op-log
      horizon; PITR archive already manages retention — reuse).
- [ ] K2.3 Sink adapters: stdout/webhook first; Kafka-protocol later.
- [ ] K2.4 Teeth: exactly-once under storm — kill the consumer, resume from token,
      no gap/dup (oracle-checked).

## K3 — Incrementally-maintained materialized views *(L)* — demand: Materialize/pg_ivm/ClickHouse-MV space

**Why us.** IVM's hard part is ordering/consistency of the delta stream; our apply
path is a deterministic total order already, so view deltas are exact by
construction — no eventual-consistency caveats. Combined with the columnar layer,
a materialized aggregate view is an always-fresh dashboard table. HTAP synergy:
this is the feature that makes "one engine for OLTP+analytics" concrete.

**Steps.**
- [ ] K3.1 `CREATE MATERIALIZED VIEW ... WITH (incremental)` — start with
      single-table aggregate views (COUNT/SUM/AVG/MIN-with-recount GROUP BY):
      delta maintenance hooked into the commit apply.
- [ ] K3.2 Differential gate: materialized result byte-identical to re-running the
      view query at every Seq (checked exhaustively in sim).
- [ ] K3.3 Join views (fact⋈dim) via delta-join (dim updates re-probe fact side —
      bounded by the existing shuffle machinery in distributed mode).
- [ ] K3.4 `REFRESH MATERIALIZED VIEW` for the non-incremental fallback.

## K4 — Time-travel queries: `AS OF` *(S–M)* — demand: audit/debug/compliance; quick win

**Why us.** MVCC snapshots keyed by Seq and PITR already exist; this is surface,
not machinery. `SELECT ... AS OF SEQ n` (and `AS OF TIMESTAMP` mapped through the
op-log's seq↔time index) + a retention knob. Cockroach charges operational
complexity for this; for us it's a weekend feature with a great demo
("query the database as of just before the bad deploy" — pairs with the PITR story).

- [ ] K4.1 Parser + snapshot pin per statement; refuse beyond retention horizon.
- [ ] K4.2 Seq↔wall-time mapping table maintained at the prod boundary (op-log
      already timestamps at archive time).
- [ ] K4.3 Differential gate: `AS OF n` == replay-to-n oracle (PITR test reuse).

## K5 — Flight recorder: deterministic production replay *(L, research-grade differentiator)*

**Why us — and only us.** The core is a pure function of its inputs; prod differs
from sim only through the provider boundary. Record the boundary (received frames
+ order, timer fires, fsync completions, connection events) into a ring buffer →
**replay the exact node execution in sim, byte-for-byte, offline**. "Attach the
flight-recorder file to the bug report; we step through your production incident
in a debugger." No shipping database can do this; it is the determinism investment
paying out as a *user-facing* feature, and the strongest possible proof of the
verification story.

- [ ] K5.1 Boundary event schema + ring-buffer recorder in `providers/prod`
      (bounded, format-versioned per W2, off by default, ~zero cost when off).
- [ ] K5.2 `lockstep_replay <recording>`: reconstructs the sim harness from the
      recording, replays, asserts state-hash equality with the recorded outcome.
- [ ] K5.3 Self-test: record a prod fault-storm run in Docker, replay on the Mac
      host, byte-identical state hash (cross-platform determinism is already held
      by the portable-mul128 discipline).
- [ ] K5.4 Divergence detector: replay hash ≠ recorded hash → flag the first
      diverging event (bisect over the event stream) — this is also *our* debugging
      tool for any future prod-only bug.

## K6 — Deterministic Wasm UDFs / stored procedures *(L)*

**Why us.** Stored procedures are gap E7's hardest item *because* arbitrary user
code threatens determinism. A fuel-metered Wasm sandbox (no syscalls, no clock, no
float-nondeterminism modes, gas-bounded) makes user code deterministic by
construction — so `CREATE FUNCTION ... LANGUAGE wasm` executes identically on every
replica and inside the sim. Competitors bolt on PL/pgSQL interpreters; we get
any-language UDFs *plus* replay-safety. (Interpreter choice: embed wasm3 or
wasmtime with determinism config; evaluate size/licensing at K6.1.)

- [ ] K6.1 Runtime spike: embed, measure call overhead, confirm bit-exact float
      behavior across platforms; pick the engine.
- [ ] K6.2 `CREATE FUNCTION f(args) RETURNS t LANGUAGE wasm AS <blob>` — blob in
      the catalog (new namespace), scalar UDFs usable in expressions.
- [ ] K6.3 Gas + memory limits charged to the W3 arena; deterministic
      out-of-fuel error.
- [ ] K6.4 Later: table-valued functions; procedures with SQL callbacks (needs a
      reentrancy story — explicitly out of first scope).

## K7 — Tiered / bottomless storage (SSTables on object store) *(XL, later)*

**Why us.** WiscKey separation means cold value-log segments and deep-level
SSTables are immutable files — natural S3 objects; the LSM manifest already
installs file sets atomically. "Bottomless single-node with S3 behind it" targets
the Neon/Turso demand curve. Needs an async fetch path and a block cache; schedule
after W7 (movement machinery overlaps). Design doc first; no code before then.

---

# Sequencing

```
Phase A — correctness & safety   W1 → W2 → W3            (~1.5–2 months)
Phase B — perf leadership        W4 → W5, W8 spike        (parallel with late A)
Phase C — quick killer wins      K4 → K2 → K1             (each independently shippable)
Phase D — scale-out              W6 → W7                  (starts after A)
Phase E — deep differentiators   K3 → K6 → K5             (after B; K5 after W2)
Phase F — breadth on demand      W9.x interleaved when an evaluator/ORM hits the gap
K7 design doc only until W7 lands.
```

Dependencies: W3 arena ← charged by W4/K6 · W6 learners ← W7 movement · W2 formats
← K5 recording format · W5 stats ← K1 ivfflat costing (optional) · K2/K4 ← PITR
(done).

**Recommended immediate order: W1 → W2 → W3.1–W3.4 → K4 → W4.** Rationale: close
the durability-truth hole first (small, principled), stamp formats while cheap,
make the engine unkillable-by-query, bank the time-travel quick win, then take the
benchmark crown.

# Risk register (technical)

| Risk | Mitigation |
|---|---|
| W7 destabilizes the verified core | Spec-first; W7.3 canary must be byte-identical to the static router before any movement code |
| Flattening (W8) breaks determinism subtly | It's a spike on a branch; byte-identical gate is the go/no-go criterion |
| Optimizer (W5) changes plans → silent wrong results | Impossible-by-construction to ship: differential model gates every seed; plans change, answers can't |
| K6 Wasm engine nondeterminism (NaN bits, SIMD) | K6.1 spike explicitly tests cross-platform bit-exactness before any SQL surface |
| Governance limits (W3) fork replica behavior | Limits are deterministic byte-counters in the statement stream, never allocator-dependent |
| Scope: killer features starve gap closure | Phase order above; K-features gate on their prerequisite W-items |
