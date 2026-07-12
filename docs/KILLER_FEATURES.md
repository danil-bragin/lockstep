# Lockstep — Killer Features: Full Plan

*Status: living engineering plan. Owner: danil-bragin. Created 2026-07-05.
Expands Part B of `docs/TECH_PLAN.md` into a complete, market-researched catalog.
Gap workstreams W1–W9 live in TECH_PLAN.md; K-numbers here supersede the short list there.*

## Selection methodology

A feature makes this list only if **all three** hold:

1. **Market pull** — externally verifiable demand signal (cited per feature), not our guess.
2. **Unfair advantage** — our architecture (determinism, MVCC+Seq total order, Calvin
   sequencing, dual row/columnar layout, LSM-immutable files, verified core) makes it
   *structurally cheaper or better for us* than for Postgres/CockroachDB/DuckDB to copy.
3. **Verifiable** — it can be gated the Lockstep way (differential model, teeth, TLC where
   protocol-shaped, byte-identical conformance).

Features failing (2) are commodity checklist items — tracked, but never prioritized over
features passing all three.

**The macro-signal all research converges on:** the 2026 default is *consolidation onto one
database*. "Just use Postgres" is now the mainstream advice — Postgres passed 55% developer
usage; teams actively replace Elasticsearch (BM25-in-PG), Kafka (queues-in-PG), and Pinecone
(pgvector) with one engine. Simultaneously three ecosystems exploded around what single-node
Postgres *cannot* do well: HA without Patroni/pgbouncer footguns, instant CoW branching
(Neon), and AI-agent infrastructure (MCP at ~97M monthly SDK downloads, agent memory as a
first-class category). **Lockstep's play: be the consolidation target that is also
distributed, verified, and agent-native.** Every feature below serves that sentence.

---

## Theme A — "One database instead of five" (the consolidation wedge)

### K1 — HA vector search (pgvector-compatible) *(L)* — TIER: BET

**Market signal.** Vector search is table stakes for AI apps; guidance converged on
"below ~50M vectors, Postgres+pgvector wins on TCO" — but pgvector rides single-primary
Postgres, and standalone vector-DB adoption is *declining* while hybrid retrieval intent
tripled (10.3%→33.3% in one quarter). The demand is "vectors inside my main database, HA,
hybrid with keyword search."

**Unfair advantage.** (a) Raft-replicated strongly-consistent vectors — pgvector can't;
(b) seeded determinism → **byte-reproducible HNSW** (level assignment via SeededRandom),
so replica index equality is *checkable* with the existing keyspace-hash machinery — a
publishable novelty ("deterministic ANN"); (c) columnar SoA float blocks are already the
ideal SIMD distance layout.

**Steps.**
- [x] K1.1 `vector(n)` type (dim-checked, logical 15 over the ARRAY codec with REAL elements; pgvector `'[x,y,z]'` text form). *Remaining: pgvector BINARY format on the PG wire (text works today).*
- [x] K1.2 Distance ops `<->` / `<#>` / `<=>` (+ `l2_distance`/`cosine_distance`/`inner_product` fns); exact scan row + columnar; ORDER BY-expression k-NN idiom. *Remaining: SIMD kernels (scalar loops today).*
- [x] K1.3 `USING ivfflat` (deterministic k-means — PK-ordered seeding, no rng; `lists`/`probes` knobs; entries carry the payload so probes skip row fetches).
- [x] K1.4 `USING hnsw` — deterministic build (integer-geometric hash-of-PK levels, no rng/libm; byte-identical replica graphs, attestable via keyspace-hash). *Honest caveat: correct but point-get-bound (~130 µs/hop) — ivfflat is the fast path today; batch-get optimization pending.*
- [ ] K1.5 Recall/latency bench vs pgvector @1M×768d; honest report.
- [x] K1.6 Gate: index==scan differential (probes=lists must EQUAL brute force — in sql_vector_test) *; recall bounds vs exact oracle at scale pending K1.5.*

**Deps:** W3 arena (index build memory). **GTM hook:** "pgvector, but HA and reproducible."

### K2 — Hybrid full-text search: BM25 + vectors + RRF *(L)* — TIER: BET

**Market signal.** The single loudest 2026 replacement story: BM25-in-Postgres
(pg_search / pg_textsearch, Tiger Data's "Elasticsearch's hybrid search, now in Postgres")
"removes the technical justification for keeping a separate cluster"; practitioners report
Postgres covering ~85% of search needs and killing the sync-pipeline ("why is my search
stale?") class of bugs. Hybrid (BM25+vector+RRF) is the winning relevance recipe.

**Unfair advantage.** Search index updated **in the same total order as the data** — zero
staleness *by construction*, with HA. Elasticsearch's whole failure mode (sync lag) cannot
exist. GIN index machinery + expression indexes (J-series) already give the inverted-index
skeleton; dict-encoded columnar TEXT gives cheap term storage.

**Steps.**
- [ ] K2.1 Tokenizer/analyzer set (simple, whitespace, stemmer per language; deterministic — no ICU locale drift, pinned tables).
- [ ] K2.2 Inverted index as a GIN extension: postings with term frequencies + doc lengths (BM25 inputs), WAL-logged like existing indexes.
- [ ] K2.3 `@@` match operator + `bm25_score()`; top-k via existing ORDER BY/LIMIT path, then a dedicated top-k heap operator.
- [ ] K2.4 RRF: `rrf(rank1, rank2)` helper over CTEs first (works today with K1); a fused hybrid operator later.
- [ ] K2.5 Relevance parity harness vs Elasticsearch BM25 on a public corpus (BEIR subset); honest report.
- [ ] K2.6 Gate: index==scan differential (score equality vs a naive reference scorer).

**Deps:** none hard; K1 for hybrid story. **GTM hook:** "Fire your Elasticsearch cluster; search is never stale again."

### K3 — SQL queues & exactly-once messaging *(M)* — TIER: BET, cheapest of the bets

**Market signal.** "Postgres as a queue" (pgmq, River, SKIP LOCKED patterns) is a
mainstream 2026 pattern — teams drop Kafka/SQS for moderate volumes to kill infra sprawl.
Known ceiling: Postgres queues degrade under bloat/vacuum and offer no exactly-once.

**Unfair advantage.** This is *the* Calvin/determinism sweet spot: the Seq total order
makes **exactly-once delivery a primitive, not a protocol** (consumer offset = Seq cursor —
same mechanism as K5 CDC); deterministic dispatch means competing consumers get
reproducible assignment; LSM has no vacuum-bloat failure mode; queue ops replicate with
the same durability as data — and an enqueue can be **transactional with a data write**
(the outbox pattern collapses into a single commit).

**Steps.**
- [ ] K3.1 `CREATE QUEUE q` (catalog object) + `SEND q, payload` / `RECEIVE q [BATCH n] [VISIBILITY t]` / `ACK` / `DLQ` semantics — sugar over a keyed namespace + Seq cursors.
- [ ] K3.2 Transactional enqueue: SEND inside a txn commits atomically with data writes (outbox killer).
- [ ] K3.3 Competing consumers: deterministic visibility-timeout redelivery (virtual-time in sim → exhaustively testable).
- [ ] K3.4 Throughput bench vs pgmq + SQS latency profile; honest report.
- [ ] K3.5 Gate: exactly-once teeth under fault storm (kill consumers/leader mid-flight; oracle counts every message delivered-and-acked exactly once).

**Deps:** none (PITR/Seq machinery done). **GTM hook:** "Transactional outbox in one line. Exactly-once, verified under fault injection."

### K4 — Change Data Capture *(M–L)* — TIER: BET *(unchanged from TECH_PLAN K2)*

Seq-cursor changefeeds over `export_ops`; globally-ordered (vs Cockroach's per-range
resolved-timestamp complexity); exactly-once resume free. Steps as in TECH_PLAN
(CREATE CHANGEFEED → retention interaction → sinks → storm teeth). Feeds K10 realtime.

**K4.1 SHIPPED:** `CHANGES t SINCE <seq> [LIMIT n]` pull changefeed — rows
`(_seq, _op PUT|DELETE, <all columns>)` in the engine's TOTAL commit order; the client's
cursor is just the last `_seq` it processed → exactly-once resume BY CONSTRUCTION (no
consumer-group protocol). Storage: `export_ops(..., include_flushed=true)` also reads
live SSTables (entries keep their original seqs), so a flush no longer truncates the
feed; the retention horizon = COMPACTION (like Kafka's segment retention), and a
compacted-past cursor gets a clean refusal, never a silent gap. Gates: replay-from-0 ==
live table, split-cursor == single pass, restart byte-stable, cross-table filtering,
compaction-horizon tooth (storage_pitr_test §9). Throughput (release, one core):
full drain 4.7M ops/s; cold chunked cursor 860k ops/s; LIVE TAIL (ingest+consume
interleaved, 800k rows) 2.37M ops/s delivered. **K4.2 SHIPPED:** per-shard feeds — `CHANGES t SHARD <i> SINCE <s>` on the distributed
coordinator routes to shard i's own Seq line: M shards = M independent, internally
totally-ordered feeds = Kafka's partition model exactly (no cross-shard order, same as
no cross-partition order; one cursor per shard). Cross-shard `CHANGES` without SHARD
gets a clean teaching error. Scaling (thread-per-shard, engines share nothing,
TSan-clean; release, laptop): aggregate ingest+deliver 128k → 199k → 394k → 605k ops/s
at 1/2/4/8 shards (~linear to the physical perf cores, honest efficiency-core rolloff
at 8). Gate: union of per-shard replays == the whole distributed table + per-shard
exactly-once resume (sql_cdc_test §8). **K4.3 SHIPPED:** retention knob + wire transport. `SET cdc.retain_seq = <h>` (engine:
`set_cdc_retain_from`) clamps compaction's version GC below a retained feed cursor —
the op-log suffix [h..] stays consumable through compaction. Kafka's retention horizon,
but CURSOR-EXACT instead of time-guessed: advance h as consumers acknowledge, disk cost
tracks actual lag. Gate: identical overwrite workload — control engine GC refuses a
from-0 cursor, retained engine replays all 40 ops contiguously (storage_pitr_test §9).
And CHANGES t SHARD i runs unchanged over the WIRE shard transport (statement routes to
the remote engine): union of per-shard wire feeds == the whole distributed table
(sql_distributed_wire_test) — the prod consumer topology, one wire cursor per shard.
**K4.4 SHIPPED:** named changefeeds — `CREATE CHANGEFEED cf FOR t` / `FETCH cf [LIMIT
n]` / `ACK CHANGEFEED cf AT <seq>` / `DROP CHANGEFEED cf`. The cursor registry is a
hidden row table written through the ordinary SQL path (the K3-queue trick), so cursors
are durable, replicated, and restart-surviving like user data. FETCH never advances;
ACK does (monotone-only) — a crash between them refetches the same batch: exactly-once
EFFECT with zero consumer-group machinery. Every ACK/DROP recomputes min(acked) across
feeds and hands compaction the retention horizon automatically — disk pays exactly for
the slowest consumer's lag (Kafka: guessed retention.ms + a separate offsets topic +
group coordinators; here: one table and one min()). Named feeds are per-shard (the
coordinator refuses, cursors ARE partition offsets). Gates: fetch/ack loop, crash-safe
refetch, backwards-ACK tooth, two-feed min() horizon + DROP recompute, restart cursor
survival (sql_cdc_test §11-12). **K4.5 SHIPPED:** the PG push surface — `LISTEN <changefeed>` / `UNLISTEN` in the wire
session (PG semantics are per-connection; the SQL core is untouched). At every
ReadyForQuery boundary the session probes each listened feed (FETCH LIMIT 1 — the
cursor never moves) and emits NotificationResponse 'A' (channel = feed, payload = the
first unacked _seq) when an unannounced batch exists; de-duped per batch, an ACK
re-arms. Push-wake + pull-batch over exactly-once cursors — a stock PG driver's
LISTEN/NOTIFY loop becomes a Kafka consumer with durable offsets, no client library.
Gates: unknown-feed error, backlog announce on LISTEN, de-dup, acked silence, new-write
announce with exact payload, UNLISTEN silence (pg_wire_test). **K4.6 SHIPPED:** the fault-storm oracle — 12 deterministic seeded storms (160 rounds
each): writer (rows, txns, ~1/3 rolled back) × consumer (FETCH, partial ACK,
idempotent apply) × 8-17 crash/recover cycles per storm, including between FETCH and
ACK and mid-transaction. Oracle: V1 replayed shadow == SELECT *; V2 delivered seq-set
== the exact op-log (no loss, no rollback leak); V3 delivery always past the durable
cursor. Mutation-verified (ACK+1 kills V1+V2). Exactly-once through crashes is a GATE
(sql_cdc_storm_test). **K4.7 MEASURED** (bench/compare/cdc/REPORT.md): Kafka 3.7
head-to-head, both at server-side best case. Kafka wins raw produce (725k
rec/s/partition batched blobs vs our 148k durable SQL rows; 1.52M vs 605k at 8-way).
We win consume (2.37M decoded rows/s vs 513k msg/s e2e) and per-record latency
(sub-ms vs 231ms-avg batching / 2ms p50 paced) — and our numbers carry durable
cursors + storm-gated exactly-once, which his perf tool does not attempt. Open:
server-initiated mid-idle push (poll_notifications hook exists), prod replicated
shards.

### K5 — Incrementally-maintained materialized views *(L)* — TIER: BET *(TECH_PLAN K3)*

Materialize/pg_ivm demand; deterministic total order → *exact* deltas, no
eventual-consistency caveats; pairs with columnar for always-fresh dashboards. Steps as in
TECH_PLAN (single-table aggregates → differential replay gate → delta-joins → REFRESH).

**K5.1 SHIPPED:** `CREATE INCREMENTAL MATERIALIZED VIEW mv AS SELECT g..., COUNT(*),
SUM(x)... FROM t [WHERE p] GROUP BY g...` — maintained LAZILY from the K4 CDC op-log:
a read pulls the base table's committed ops past the view's durable cursor, folds
per-group deltas (old row image = MVCC read at the cursor snapshot; new = the feed's
final image), and applies changed rows + the cursor advance in ONE commit batch —
crash-safe by construction (partial apply impossible; re-derivation idempotent).
Compaction past the cursor = honest full-REFRESH fallback, never silently wrong.
v1 shape: single row-mode table, INT/TEXT group columns, COUNT(*) required (detects
group death) + any number of SUM(col); AVG/MIN/MAX/DISTINCT/HAVING/joins are clean
teaching errors (AVG: project SUM and COUNT(*), divide in a view; MIN/MAX need a
heap under deletes). REFRESH on an incremental view re-bases the cursor. Gate:
incremental == full recompute, byte-for-byte, sampled through a 220-round seeded
storm (inserts/updates/deletes, txn commits AND rollbacks, group birth+death,
filter-boundary rows), across a restart, and after REFRESH (sql_ivm_test, 146th).
**K6.1 SHIPPED (the composition):** FETCH/CHANGES on a feed over an incremental view
drive its catch-up first — so base-table commits flow into the view's changefeed with
NOBODY ever SELECTing the view. The full live-dashboard loop over a stock PG driver:
`LISTEN dash` (a changefeed over the view) → another session commits to the BASE
table → the server pump delivers 'A' → `FETCH dash` returns the maintained AGGREGATE
deltas (group updates as PUT rows, group death as DELETE) → apply, `ACK`. Strongly
consistent live queries — Supabase-Realtime-shaped UX with Materialize-shaped
semantics, from one engine. Gates: SQL-level loop incl. group-death DELETE delta +
DROP retiring the registry and durable cursor (sql_ivm_test), wire-level push on a
parked listener (pg_wire_test). Open: delta-joins, SUM over REAL, eager per-write
mode, dist/columnar bases.

### K6 — Realtime live queries (subscriptions) *(M after K4/K5)* — TIER: FAST-FOLLOW

**Market signal.** Supabase Realtime / LISTEN-NOTIFY / Zero/Electric client-side sync —
"live UI without polling" is a default expectation in 2026 app stacks; Postgres
LISTEN/NOTIFY is lossy (no replay) and unordered across failover.

**Unfair advantage.** K4 changefeed + K5 IVM compose into **strongly-consistent live
queries with resume**: subscribe to a view; deltas arrive in commit order with a Seq token;
reconnect resumes without a snapshot diff. Nobody offers *consistent* live queries over HA.

**Steps.**
- [ ] K6.1 `LISTEN`/`NOTIFY` PG-compat (ordered, replayable — strictly better than PG's).
- [ ] K6.2 `SUBSCRIBE TO VIEW v FROM SEQ n` on the wire protocol → initial state + IVM delta stream.
- [ ] K6.3 WebSocket gateway endpoint in lockstepd (prod boundary only).
- [ ] K6.4 Gate: client-observed stream == replay oracle at every Seq; failover mid-subscription resumes gapless.

## Theme B — Developer-workflow killers

### K7 — Instant copy-on-write database branching *(L)* — TIER: BET

**Market signal.** Neon's defining feature and a top evaluation criterion in 2026 platform
comparisons: "every PR gets an isolated database with production-like data in under a
second." PlanetScale had to answer with schema-branches; demand is validated at
funding-round scale.

**Unfair advantage.** The LSM is *already* copy-on-write: SSTables and vlog segments are
immutable; a branch = a manifest fork + shared file references + its own memtable/WAL —
we don't need Neon's custom page-server storage layer, the storage engine was born
branchable. MVCC Seq gives "branch at any historical Seq" (pairs with K11 time travel).

**Steps.**
- [ ] K7.1 Refcounted file ownership in the manifest (compaction/GC respects cross-branch references) — the one real engine change; spec note + torture test.
- [ ] K7.2 `CREATE BRANCH b [FROM SEQ n]` / `DROP BRANCH` — catalog + manifest fork; O(metadata), target < 100 ms.
- [ ] K7.3 Wire routing: connect string selects branch (`dbname=main/b`); branches replicate as part of cluster state.
- [ ] K7.4 Diverged-branch writes (own WAL line per branch); no merge in v1 (Neon doesn't merge either — branches are ephemeral test envs).
- [ ] K7.5 CI story: `lockstep branch create pr-123 && run tests && drop` recipe.
- [ ] K7.6 Gate: branch == PITR-restore-to-Seq oracle byte-identical; fault storm during branch create/drop; compaction never reclaims a file a live branch references (teeth: broken refcount variant must be caught).

**Deps:** W2 (manifest format version bump). **GTM hook:** "A database per pull request, in 100 ms, on your own hardware."

### K8 — Deterministic testing-as-a-feature: `lockstep sim` for user apps *(M)* — TIER: BET, uniquely ours

**Market signal.** Antithesis sells deterministic-simulation testing as a service;
TigerBeetle's simulator is its loudest marketing asset (200% YoY community growth, $24M
Series A validating "correctness sells"). Meanwhile every backend team fights flaky
integration tests against Docker databases.

**Unfair advantage.** Only we can hand users the *actual production engine* running in a
deterministic simulator **as a test dependency**: in-process cluster, virtual time
(timeouts run instantly), seeded fault injection (partitions, crashes, torn writes),
byte-reproducible failures. "Your integration test found a bug at seed 1337 — every rerun
reproduces it" — no other database can offer this, because their engines aren't
deterministic. This is the verification investment sold as a *developer product*.

**Steps.**
- [ ] K8.1 `lockstep sim serve --seed S --nodes 3 [--faults storm.toml]`: PG wire on localhost backed by the sim cluster, virtual time auto-advancing.
- [ ] K8.2 Fault-profile DSL (TOML): partition/crash/disk-fault schedules or seeded randomness — the existing harness knobs, exposed.
- [ ] K8.3 Test-framework adapters: a testcontainers-style module + pytest/Jest fixtures (`LOCKSTEP_SIM_SEED` env → reproducible CI).
- [ ] K8.4 `--record` → on failure, print the seed + minimal repro command (bridges to K9 flight recorder).
- [ ] K8.5 Gate: same seed + same client-request order → byte-identical run incl. injected faults, cross-platform (portable-mul128 discipline already holds).

**Deps:** none — the harness exists; this is packaging it behind one binary. **GTM hook:** "Test your app against a database that breaks deterministically."

### K9 — Flight recorder: replay production incidents *(L)* — TIER: BET *(TECH_PLAN K5)*

Record the provider boundary in prod → byte-replay the node in sim offline; divergence
bisector doubles as our own debugging tool. Steps as in TECH_PLAN. **Deps:** W2 (recording
format). Together K8+K9 form the **"deterministic lifecycle"** story: test deterministically →
run in prod → replay any incident deterministically. No competitor can tell it.

### K10 — Time travel: `AS OF` *(S–M)* — TIER: FAST-FOLLOW, quick win *(TECH_PLAN K4)*

MVCC+PITR make it a surface feature; audit/debug/compliance demand; demo pairs with K7
(branch from the past) and the PITR story. Steps as in TECH_PLAN.

## Theme C — AI-agent infrastructure (the 2026-native wedge)

### K11 — The agent-memory database story *(M composition + S pieces)* — TIER: BET (narrative-level)

**Market signal.** Agent memory is now a first-class category (its own benchmarks,
20+ vector stores integrated, dedicated frameworks); MCP hit ~97M monthly SDK downloads
and Linux Foundation governance; clouds ship "agentic AI database" offerings (Oracle 26ai,
Aurora DSQL MCP). Agents need: semantic recall (vectors), keyword recall (BM25), episodic
history (time travel), session state (transactions), isolation (branches/tenancy) — today
that's 3–4 systems glued together.

**Unfair advantage.** K1+K2+K10+K3+K7 *compose* into the complete agent-memory stack in
one consistent engine — plus two agent-specific properties nobody has: **reproducible
memory** (deterministic ANN + AS OF = "what did the agent know at step N?" answerable
exactly — an auditability story for agent incidents) and **branchable memory** (fork an
agent's memory for an experiment, throw it away).

**Steps.**
- [x] K11.1 SHIPPED: `lockstep_mcpd` — the agent-memory MCP server (stdio JSON-RPC 2.0,
      newline-delimited; register the binary in any MCP client). Tools: `query`,
      `schema`, `remember` (auto-provisions the memory store: TEXT + VECTOR(dim) +
      IVFFLAT + BM25 on first use), `recall` (RRF-fused hybrid when an embedding is
      given, BM25 otherwise — DETERMINISTIC ranking, replayable in incident review),
      `history` (SELECT at statement-version n = "the agent's world at step N",
      exact). Durable over ProdDisk WALs; a restart recovers the store byte-identically
      (verified end-to-end over stdio). Gate: mcp_server_test — handshake, recall ==
      the documented SQL recipe, AS-OF step audit, protocol/tool teeth. FOUND ALONG THE
      WAY: `AS OF SEQ` counts committed WRITE STATEMENTS while `CHANGES _seq` counts
      storage ops — two version lines; documented in the tool, unification = open item.
      Remaining from the original K11.1 scope: RBAC-scoped per-agent tokens.
- [x] K11.2 Hybrid-recall recipe (shipped INSIDE the recall tool; the SQL it runs):
      `SELECT id, kind, content FROM agent_memory ORDER BY rrf_score(embedding, '[q]',
      content, 'query text') DESC LIMIT k` — recency re-weighting composes by fusing
      with `ORDER BY id DESC` ranks client-side; view template + leg weights = open.
- [x] K11.3 Episodic audit (shipped as the `history` tool): any SELECT + `AS OF SEQ n`
      where n = committed-statement step; each `remember` is exactly one step.
- [ ] K11.4 Per-agent isolation: cheap namespaces (schema-per-agent exists via `qualify`) + K7 branch-per-experiment.
- [x] K11.5 MEASURED (bench/compare/mcp/REPORT.md): vs mem0 2.0.11 + FAISS, fully
      local, identical deterministic embeddings/corpus, no LLM either side. Quality
      decisively ours — recall@5 0.935 vs 0.620 clean, 0.230 vs 0.050 noisy (hybrid
      RRF vs vector-only; mem0's faiss leg admits no keyword search). Throughput to
      mem0 with stated causes (in-process + non-durable vs subprocess + fsync'd WAL).
      LOCOMO-style LLM-judged QA = open (needs keys).

**Deps:** K1, K2, K10 (K3, K7 enrich). **GTM hook:** "Your agents' memory: searchable, transactional, auditable to the step, forkable."

### K12 — Deterministic Wasm UDFs / stored procedures *(L)* — TIER: FAST-FOLLOW *(TECH_PLAN K6)*

Fuel-metered Wasm = user code that cannot break determinism; solves stored procedures
(hardest E7 item) and gives agents a safe server-side compute hook. Steps as in TECH_PLAN
(engine spike → scalar UDFs → gas/memory via W3 arena → TVFs later).

## Theme D — Correctness verticals & ops pain-killers

### K13 — Ledger primitives (double-entry accounting in SQL) *(M)* — TIER: FAST-FOLLOW

**Market signal.** TigerBeetle: $24M Series A (May 2026), 200%+ YoY community growth,
paying customers from day one — proof that "correctness for money movement" is a market,
not a niche. But TigerBeetle is a fixed-schema KV-ish ledger — teams still bolt a SQL
database next to it for everything else.

**Unfair advantage.** We can offer **the ledger inside the SQL database**: a
`CREATE LEDGER` table class with debit/credit columns, a declarative
`CHECK SUM(debits)=SUM(credits)` *engine-enforced per transaction* (not per row), balance
materialization via K5 IVM, and the same verification pedigree (deterministic, dual
cross-checked, fault-storm proven) that is TigerBeetle's entire pitch — plus joins,
constraints, and the rest of SQL around it.

**Steps.**
- [ ] K13.1 Transfer-level invariant: multi-row insert groups validated atomically (piggybacks on existing CHECK machinery + txn boundary).
- [ ] K13.2 Balance-tracking materialized aggregates (K5) with overdraft CHECK.
- [ ] K13.3 Idempotency keys (unique + typed conflict return — "already applied" not "error").
- [ ] K13.4 Two-phase transfers (pending/posted/voided state machine) as SQL sugar.
- [ ] K13.5 Bench vs TigerBeetle honest framing (they win raw TPS; we win "ledger + the rest of your schema in one place, same safety").
- [ ] K13.6 Gate: oracle = independent accounting model; fault-storm — books balance at every recovered prefix (teeth: partial transfer must be caught).

**Deps:** K5 for balances. **GTM hook:** "TigerBeetle-grade safety, but it speaks SQL and holds the rest of your data too."

### K14 — Native many-connection support (kill pgbouncer) *(S–M)* — TIER: FAST-FOLLOW

**Market signal.** The #1 documented operational Postgres pain: process-per-connection →
mandatory pgbouncer with "three modes, configuration full of footguns, subtle bugs";
pooler as SPOF; no failover awareness; Patroni+PgBouncer+HAProxy stacks just to serve
connections.

**Unfair advantage.** The epoll reactor is connection-cheap *by construction* — a
connection is a coroutine + socket, not a process. We don't build a pooler; we make the
problem not exist and prove it.

**Steps.**
- [ ] K14.1 Bench: 10k/50k idle + active PG-wire connections, memory per connection, p99 under churn; publish vs raw PG + PgBouncer stack.
- [ ] K14.2 Per-connection memory cap + idle timeout (W3 machinery).
- [ ] K14.3 Session-state correctness audit under high connection count (prepared stmts, SET vars — per-session isolation holds).
- [ ] K14.4 Leader-aware client redirect (typed NOT_LEADER + hint) so no external proxy is needed for failover either.

**GTM hook:** "50,000 connections, no pgbouncer, no footgun matrix."

### K15 — Edge read replicas / local-first sync *(XL)* — TIER: WATCH (design doc only)

**Market signal.** Local-first is "the hottest architecture trend in 2026"
(ElectricSQL/PowerSync/Zero/Turso offline-writes all racing); Turso's embedded replicas +
offline writes define the shape: local reads, queued writes, server authority.

**Why WATCH not BET.** Real demand, but the client-side half (embedded lib, conflict UX,
per-platform SDKs) is a second product line; the space is crowded and fast-moving. Our
entry when ready: **learners (W6) as edge replicas** + K4 changefeed as the sync stream —
the server half we get almost free. Revisit after W6+K4 land; write the design doc then,
ship a read-only "edge replica" mode first (bounded-staleness reads with a Seq floor —
consistent, unlike most of the field).

### K16 — Bottomless storage (S3 tiering) *(XL)* — TIER: WATCH *(TECH_PLAN K7)*

Neon/Turso demand curve; WiscKey immutable files = natural objects; blocked behind W7
(movement machinery overlaps). Design doc after W7. No code before.

---

## Priority matrix

| K | Feature | Demand | Advantage | Effort | Tier |
|---|---|---|---|---|---|
| K3 | SQL queues, exactly-once | high | **unique** (Seq) | M | **BET — do first** |
| K1 | HA vector search | very high | strong | L | **BET** |
| K2 | BM25 hybrid search | very high | strong (no-staleness) | L | **BET** |
| K7 | CoW branching | very high | strong (LSM born-COW) | L | **BET** |
| K8 | `lockstep sim` for apps | med-high | **unique** | M | **BET — cheap** |
| K4 | CDC | high | strong (global order) | M–L | **BET** |
| K5 | Incremental matviews | high | strong (exact deltas) | L | **BET** |
| K11 | Agent-memory story | very high | composition + unique audit | M (composes) | **BET (narrative)** |
| K9 | Flight recorder | med (trust: high) | **unique** | L | fast-follow |
| K10 | AS OF time travel | med | strong (free) | S–M | fast-follow, quick win |
| K6 | Live queries | high | strong (consistent) | M | fast-follow (after K4/K5) |
| K13 | Ledger primitives | med (proven $) | strong | M | fast-follow |
| K14 | Kill pgbouncer | high (pain) | strong (born-cheap) | S–M | fast-follow, mostly bench+polish |
| K12 | Wasm UDFs | med | strong | L | fast-follow |
| K15 | Edge/local-first | high | partial | XL | watch (doc after W6+K4) |
| K16 | S3 tiering | high | strong | XL | watch (doc after W7) |

## Recommended sequence (interleaved with TECH_PLAN W-track)

```
Now      W1 → W2 → W3.1–3.4            (safety first — unchanged)
Wave 1   K10 AS OF → K3 queues → K8 sim-for-apps → K14 bench
         (each S–M; four shipped killer features while W-track continues)
Wave 2   K1 vectors → K2 BM25 → hybrid RRF        (the consolidation core)
Wave 3   K4 CDC → K5 IVM → K6 live queries        (the realtime layer)
Wave 4   K7 branching (needs W2) → K11 agent story (composes waves 2–3)
Wave 5   K9 flight recorder → K13 ledger → K12 Wasm
Docs-only gates: K15 after W6+K4 · K16 after W7
```

Rationale for Wave 1: three of the four are S–M efforts with *unique*-tier advantage —
maximum differentiation per session, and each is independently demo-able. The heavy
consolidation bets (K1/K2) start once governance (W3) protects the engine from the very
workloads they attract.

## The three flagship narratives (what the features add up to)

1. **"One verified database instead of five"** — K1+K2+K3+K4+K5+K6: vectors, search,
   queues, CDC, matviews, realtime — each replacing an external system, all in one
   Raft-replicated engine where nothing is ever stale or lost. (Rides the strongest
   documented market current.)
2. **"The deterministic lifecycle"** — K8+K9+K10+K7: test against seeded faults → branch
   per PR → replay any prod incident byte-for-byte → query the past. (Cannot be copied
   without rebuilding on determinism; converts the verification investment into product.)
3. **"The agent-native database"** — K11 composing waves 2–3 + MCP: memory that is
   searchable, transactional, auditable-to-the-step, forkable. (The 2026-native audience.)

## Research sources

- Tiger Data — "It's 2026, Just Use Postgres"; "Elasticsearch's Hybrid Search, Now in Postgres" (BM25+vector+RRF)
- SoftwareSeni — Replacing Elasticsearch with Postgres (BM25 hybrid, RRF); Medium — PG FTS vs Elasticsearch 6-month report (~85% coverage)
- VB Pulse Q1 2026 via VentureBeat — hybrid retrieval intent 10.3%→33.3%; standalone vector-DB adoption declining; "below 50M vectors Postgres wins TCO" (Data Science Collective)
- Neon/PlanetScale 2026 comparisons (CompareStacks, Bytebase, getautonoma) — CoW branching as a top evaluation criterion
- mem0 "State of AI Agent Memory 2026"; MCP ~97M monthly SDK downloads, Linux Foundation governance (Lushbinary trends); Oracle 26ai Private Agent Factory; AWS agentic-AI databases
- TigerBeetle — $24M Series A May 2026 (FinTech Futures), 200% YoY community growth, deterministic simulator as core pitch
- PgBouncer pain documentation (rivestack, Percona, PlanetScale engineering) — mode footguns, SPOF, no failover awareness
- pg_duckdb 1.0 (MotherDuck) — 10–100× analytics-in-Postgres demand; HTAP/zero-ETL trend (Rapydo)
- Turso offline writes / embedded replicas; ElectricSQL/PowerSync/Zero 2026 comparisons — local-first momentum
