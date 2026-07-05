# Lockstep — Product Vision, Gap Analysis & Go-to-Market Roadmap

*Status: living planning document. Owner: danil-bragin. Created 2026-07-05.*

This document answers three questions:

1. **What do we have?** — an honest inventory of assets and their competitive weight.
2. **What's missing?** — a gap analysis across engineering, operations, ecosystem, and trust.
3. **How do we win?** — positioning, a phased step-by-step roadmap (M0–M8), and a
   distribution / go-to-market playbook.

The framing throughout is honest. "Monopolize the database market" is not a plan;
**owning a defensible wedge and expanding from it** is. This document defines that wedge.

---

## 1. What we have (asset inventory)

| Asset | Competitive weight | Who else has it |
|---|---|---|
| **Deterministic simulation testing** — whole system a pure function of `(seed, tasks)`; byte-identical replay; exhaustive fault injection | ★★★★★ unique | FoundationDB (dead as OSS momentum), TigerBeetle, Antithesis (sells it as a service) |
| **Dual-Raft cross-check** — two independent consensus impls must agree per-client | ★★★★★ unique | Nobody ships this |
| **TLA+ specs checked by TLC** for consensus, snapshots, membership, sequencer, cross-shard commit | ★★★★ rare | MongoDB/CockroachDB partially, not end-to-end |
| **PostgreSQL wire protocol** — real psql/ORMs connect; extended protocol, SCRAM-SHA-256, auth+RBAC; **replicated over Raft** | ★★★★★ market-critical | CockroachDB, Yugabyte — this is the table stakes they proved |
| **HA via Raft, multi-shard, Calvin-style cross-shard atomic commit (no 2PC)** | ★★★★ | CockroachDB (2PC+HLC), TiDB (Percolator) — ours is architecturally simpler |
| **Row + columnar dual layout, byte-identical; vectorized + morsel-parallel analytics** — beats Postgres 3.4–60× on analytics at 1 CPU | ★★★★ | Nobody in the "HA Postgres-wire" category ships columnar analytics (Cockroach/Yugabyte are row-only OLTP) |
| **KV write throughput** — group-committed keyed write beats etcd; keyed read ~9.7× etcd; multi-shard 635k agg | ★★★ | etcd is the incumbent to beat and we do |
| **PITR + backup/restore + recovery toolkit + cross-replica hash check + force-new-cluster** | ★★★ operational maturity signal | Everyone mature has it; most young DBs don't |
| **mTLS + cert-CN RBAC default-deny** | ★★ | table stakes, done |
| **CI fully green, public repo, Apache 2.0, sanitizers, mutation testing, forbidden-call lint** | ★★★ trust signal | Rare at this rigor |

**The one-sentence asset summary:** a Postgres-wire-compatible, Raft-replicated HTAP
database whose every layer is deterministically simulated, differentially tested,
formally specified, and dual-implementation cross-checked — a verification story no
shipping database matches, attached to honest benchmark wins over Postgres/etcd on
both OLTP-write and analytics vectors.

---

## 2. Honest gap analysis

Severity: 🔴 blocks adoption · 🟡 blocks scale/expansion · 🟢 polish.

### 2.1 Engineering gaps (core)

| # | Gap | Severity | Notes |
|---|---|---|---|
| E1 | **Single-thread coroutine ceiling ~17k commits/s per shard** — suspend/resume + shared_ptr refcount CPU-bound; real fix = flatten coroutines → state machines (big rewrite, deferred) | 🟡 | Horizontal sharding masks it (635k agg) but per-shard ceiling is visible in any single-hot-key workload |
| E2 | **No dynamic shard rebalancing / splitting** — shard count fixed at boot; key→shard is static routing | 🔴 for scale-out story | Cockroach's core feature; ours is the biggest architectural gap vs them |
| E3 | **Membership: single-server add/remove only; no joint consensus, no learners/non-voting replicas** | 🟡 | Learners needed for zero-downtime scale-out and backup replicas |
| E4 | **No rolling-upgrade story** — no versioned wire/state-machine compatibility contract | 🔴 for production | A DB you cannot upgrade in place is a demo |
| E5 | **Columnar GROUP BY trails DuckDB/ClickHouse (0.1–0.6×)** — need vectorized hash-aggregate + SIMD filter kernels | 🟡 | We beat row stores everywhere; closing this makes the analytics claim unconditional |
| E6 | **Query optimizer is heuristic** — no statistics (ANALYZE), no cost model, no join reordering; EXPLAIN exists but costs are absent | 🟡 | Fine at current SQL surface; wall for real workloads with 3+ joins |
| E7 | **SQL surface gaps**: TRIGGER, stored procedures/functions, sequences beyond AUTO_INCREMENT, COPY protocol, cursors, LISTEN/NOTIFY, prepared-statement plan cache | 🟡 | ORM compat mostly OK without them; COPY matters for data loading speed |
| E8 | **PG-user → own user table** — RBAC still keyed to cert-CN; PG-native CREATE USER/GRANT partial | 🟡 | Needed for "drop-in Postgres" claim |
| E9 | **Distributed SQL residuals** — SUM/AVG(DISTINCT) shuffle, true snowflake (dim→dim) joins | 🟢 | Documented in SQL_FEATURES_PLAN.md |
| E10 | **No resource governance** — no memory limits, no admission control/backpressure under overload, no per-query timeout/cancellation | 🔴 for production | First OOM in prod kills trust permanently |
| E11 | **N=1 commit-before-fsync window** (documented; N≥2 safe via quorum) | 🟡 | Fix properly before claiming single-node durability |

### 2.2 Operations gaps

| # | Gap | Severity |
|---|---|---|
| O1 | **No config file** — everything is CLI flags on lockstepd | 🔴 |
| O2 | **No Grafana dashboards / alert rules** shipped alongside Prometheus metrics | 🟡 |
| O3 | **No slow-query log, no query cancellation, no `pg_stat_activity` equivalent** | 🟡 |
| O4 | **No multi-machine, multi-day soak test** — all verification on one laptop + Docker; no real-network long-haul run | 🔴 for trust |
| O5 | **No official TPC-C / TPC-H style runs** — current benches are custom (honest, but not the lingua franca) | 🟡 |
| O6 | **No upgrade/downgrade & data-format versioning tests** | 🔴 (pairs with E4) |

### 2.3 Ecosystem & distribution gaps

| # | Gap | Severity |
|---|---|---|
| D1 | **README says "Not intended for production use"** — correct today, but the plan must define the exit criteria for removing it | 🔴 |
| D2 | **No 5-minute experience**: no Docker Hub image, no `docker compose up` 3-node cluster, no brew/deb/rpm packages | 🔴 — this is the #1 adoption blocker |
| D3 | **No docs site** — knowledge lives in repo .md files; no website, no architecture pages, no getting-started | 🔴 |
| D4 | **No ORM/driver compatibility matrix** — psql works; SQLAlchemy/Prisma/Hibernate/pgx/npgsql untested systematically | 🟡 |
| D5 | **No content presence** — zero blog posts, no HN/Reddit/Twitter presence, no conference talks; the verification story (the single most linkable asset) is unpublished | 🔴 for distribution |
| D6 | **No release process** — no tags, no semver, no changelog, no binaries | 🔴 |
| D7 | **Solo maintainer** — bus factor 1; no CONTRIBUTING.md, no good-first-issues | 🟡 |
| D8 | **No external audit** — self-reported Jepsen-style results; an official Jepsen engagement (or Antithesis run) converts skeptics | 🟡 (💰) |

---

## 3. Positioning — where we can actually win

### The market map

- **HA Postgres-compatible OLTP** (CockroachDB, Yugabyte, Neon): billion-dollar funded, mature. Head-on assault loses.
- **Embedded/edge consistent SQL** (rqlite, dqlite, Turso/libSQL): active niche, weaker players, no PG wire + columnar combo.
- **OLAP engines** (DuckDB, ClickHouse): mature, but **not replicated/transactional** — different animal.
- **Verified/simulation-tested systems** (TigerBeetle, FoundationDB, Antithesis): the credibility niche; TigerBeetle proved the *story itself* drives adoption.

### The wedge (recommended)

> **"The HTAP Postgres alternative you can actually trust":** a Raft-replicated,
> Postgres-wire database with built-in columnar analytics 3–60× faster than Postgres —
> and the only one whose every layer is deterministically simulated, formally specified,
> and dual-implementation cross-checked.

Why this combination is defensible:

1. **Nobody in the "HA Postgres" category has columnar analytics.** Cockroach/Yugabyte
   users who want analytics bolt on a warehouse. We do both in one engine, proven byte-identical.
2. **Nobody in any category has the verification story end-to-end.** TigerBeetle showed
   that "we test harder than anyone" is a *distribution engine* — every blog post about
   the methodology is marketing that competitors cannot copy without years of work.
3. **The honest-benchmark culture is itself differentiation.** Every report in
   `bench/compare/` names its caveats. In a market poisoned by benchmarketing, that earns
   the developer audience's trust.

Target early adopters (in order):
1. **Infra engineers who read HN** — reached via the verification blog series (Phase M4).
2. **Teams outgrowing single-primary Postgres HA pain** (Patroni/failover fatigue) who
   don't want Cockroach's operational weight or pricing.
3. **Edge/embedded teams needing replicated SQL with real consistency** (rqlite's users, but wanting PG wire + real SQL).
4. **Correctness-obsessed domains** — fintech ledgers, control planes, billing (TigerBeetle's audience, but general-purpose SQL).

### What we do NOT chase (explicit non-goals for 24 months)

- Multi-region geo-replication with latency-aware placement (Cockroach's moat; 5+ engineer-years).
- MySQL wire protocol.
- Serverless/branching storage (Neon's moat).
- Beating ClickHouse at petabyte OLAP.

---

## 4. The roadmap — M0 → M8, step by step

Each milestone has an **exit criterion** — a falsifiable statement that must be true
before moving on. Order within a milestone is the recommended execution order.
Effort keys: S ≤ 1 session · M = 2–5 sessions · L = 5–15 sessions · XL = a phase of its own.

### M0 — Truth baseline: "we know exactly what we are" *(1–2 weeks)*

Goal: freeze an honest definition of current state so every later claim is checkable.

- [ ] **M0.1 (S)** Version scheme: adopt semver, tag current HEAD as `v0.1.0`, add CHANGELOG.md (Keep-a-Changelog format).
- [ ] **M0.2 (S)** Write `docs/PRODUCTION_READINESS.md`: the explicit checklist of what must be true to remove the "not for production" banner (tracks E4, E10, E11, O4, O6). This document *is* the banner-removal exit criterion.
- [ ] **M0.3 (M)** Data-format & wire versioning contract: stamp WAL/SSTable/manifest/snapshot/catalog records with a format version; refuse mismatches loudly. Prereq for every upgrade story.
- [ ] **M0.4 (S)** Add `CONTRIBUTING.md` + issue templates + 10 curated good-first-issues (docs, small SQL functions, bench harness).
- [ ] **M0.5 (S)** LICENSE audit: confirm all deps Apache-2.0-compatible; add NOTICE if needed.

**Exit criterion:** a stranger can state, from repo docs alone, exactly what Lockstep guarantees today and what it does not.

### M1 — The 5-minute experience *(2–4 weeks)* — **highest-leverage milestone**

Goal: `docker compose up` → psql → replicated SQL in under 5 minutes. Everything downstream (blog posts, HN, docs) links here.

- [ ] **M1.1 (M)** Publish multi-arch (amd64+arm64) Docker image to Docker Hub/GHCR on every tag: `lockstepd` + `lockstep_admin` + `lockstep_pgd`, distroless-ish, sane defaults.
- [ ] **M1.2 (M)** `deploy/docker-compose.yml`: 3-node Raft cluster + PG port exposed, health checks, one env var each. Include a `demo` profile that loads a sample dataset.
- [ ] **M1.3 (S)** Config file support in lockstepd (`--config lockstep.toml`), flags override file (fixes O1).
- [ ] **M1.4 (M)** `curl -fsSL https://…/install.sh` single-binary install + Homebrew tap + .deb (nfpm). Static-link musl build if feasible; else document glibc floor.
- [ ] **M1.5 (S)** Rewrite README top: 30-second pitch, animated GIF of the 3-node demo surviving a `docker kill`, quickstart block, then the verification story.
- [ ] **M1.6 (M)** `lockstep demo` subcommand: spins an in-process 3-node sim cluster, opens a SQL prompt, offers a scripted "kill a node, watch it keep serving" tour. Zero infra needed — the deterministic sim is uniquely able to do this.

**Exit criterion:** a person who has never seen the repo reaches a replicated psql prompt in ≤ 5 minutes on Mac and Linux, measured with a stopwatch, no support.

### M2 — Docs site & the story *(2–4 weeks, parallelizable with M1)*

- [ ] **M2.1 (M)** Docs site (Docusaurus or mkdocs-material) on GitHub Pages: Getting Started · Architecture · SQL reference (generate the supported-surface table from `SQL_FEATURES_PLAN.md`) · Operations (backup/PITR/recovery toolkit/hashcheck) · Verification methodology.
- [ ] **M2.2 (M)** Architecture deep-dive pages, one per layer (runtime, storage, consensus, txn, SQL, distributed SQL) — largely distilled from existing specs/briefs; each page ends with "how this layer is verified".
- [ ] **M2.3 (S)** "Limitations" page — the gap table from §2, public. Honesty as brand.
- [ ] **M2.4 (S)** Benchmarks page: methodology, reproduction scripts (`bench/compare` already has them), all caveats inline.

**Exit criterion:** every claim in the README links to a docs page that substantiates or reproduces it.

### M3 — Postgres compatibility, systematically *(4–8 weeks)*

Goal: convert "psql works" into a tested compatibility contract — the practical adoption gate for target audience #2.

- [ ] **M3.1 (M)** ORM compatibility CI matrix: SQLAlchemy, Prisma, Hibernate, pgx, npgsql, ActiveRecord — each running its smoke suite against `lockstep_pgd` in CI; publish the pass/fail matrix on the docs site (fixes D4).
- [ ] **M3.2 (M)** PG-native users: CREATE USER / ALTER USER / GRANT on an own user table, mapped onto existing RBAC (closes E8).
- [ ] **M3.3 (M)** `COPY FROM/TO` (text + binary) — bulk loading is the first thing every evaluator does (from E7).
- [ ] **M3.4 (M)** Prepared-statement plan cache + portal suspension correctness under the extended protocol.
- [ ] **M3.5 (S)** `pg_catalog` shims sufficient for `\d`, `\dt`, ORM introspection (information_schema subset).
- [ ] **M3.6 (L, stretch)** Sequences, cursors; TRIGGER + stored functions decided by demand signals from M3.1 failures, not speculatively.

**Exit criterion:** ≥ 4 of 6 ORMs pass their smoke suites in CI, and the matrix is public.

### M4 — Distribution launch *(6–10 weeks, overlaps M3)*

Goal: spend the unique asset — the verification story — as content. TigerBeetle playbook.

- [ ] **M4.1 (M)** Blog series (docs-site blog or personal), one post ≈ every 2 weeks:
  1. "A database that is a pure function of its seed" (determinism + replay)
  2. "We wrote Raft twice so bugs have to happen twice" (dual cross-check + the real bugs it caught — backprop memory files are ready-made material)
  3. "Torn writes, lying fsyncs: the fault storms" (WAL fabrication bug story)
  4. "Beating Postgres 3–60× on analytics while formally verified" (bench + methodology)
  5. "Calvin, not 2PC: cross-shard transactions" (XShardCommit.tla)
- [ ] **M4.2 (S)** Show HN with post #1 + the M1 demo link. Prereq: M1 done, M2 landing pages done. HN wants: honest status, reproducible claims, a runnable thing.
- [ ] **M4.3 (S)** Submit talks: CppCon / CppNow (deterministic C++23 coroutine runtime), HYTRADBOI / P99 CONF (verification story), local meetups first for reps.
- [ ] **M4.4 (S)** Comparison pages on docs site: "Lockstep vs rqlite/dqlite", "vs etcd", "vs Cockroach" — factual, caveated, linking bench reproductions. These rank on search and frame the evaluation.
- [ ] **M4.5 (S)** Community channel (Discord or GitHub Discussions) + a public roadmap board mirroring this document.

**Exit criterion:** ≥ 1000 GitHub stars, ≥ 3 external contributors, ≥ 5 "we tried it" reports from strangers. (Stars are a vanity proxy, but they gate the next audience.)

### M5 — Production hardening *(2–4 months)* — the banner-removal milestone

Goal: satisfy `docs/PRODUCTION_READINESS.md` (M0.2). The gaps: E4, E10, E11, O4, O6, E3.

- [ ] **M5.1 (L)** Resource governance: per-query memory accounting + limit, admission control / backpressure under overload, statement timeout + cancellation (client `Cancel` on PG wire) (closes E10).
- [ ] **M5.2 (M)** Fix N=1 commit-before-fsync properly in core (commit_index advances on fsync completion; both impls; TLC + cross-check byte-identical stays green) (closes E11).
- [ ] **M5.3 (L)** Rolling upgrades: version-negotiated wire + state-machine, mixed-version cluster test in CI, documented upgrade procedure (closes E4, O6; builds on M0.3).
- [ ] **M5.4 (M)** Raft learners / non-voting replicas → zero-downtime node replacement and scale-out (closes E3). Spec first (Membership.tla extension), teeth, both impls, additive.
- [ ] **M5.5 (L)** Multi-machine soak: 5 real machines (cloud spot instances), 72h continuous fault storm (network partitions via tc/iptables, kill -9, disk-full), invariant checkers running throughout; publish the report (closes O4).
- [ ] **M5.6 (M)** Ship Grafana dashboards + alert rules; slow-query log; `lockstep_admin top` (activity view) (closes O2, O3).
- [ ] **M5.7 (💰, optional but high-value)** External audit: official Jepsen engagement or an Antithesis run. Converts self-reported rigor into third-party proof (closes D8).

**Exit criterion:** the production-readiness checklist is green; the README banner changes to "production-ready for single-region deployments; see limitations".

### M6 — Performance leadership *(2–4 months, parallel with M5)*

Goal: make both benchmark claims unconditional.

- [ ] **M6.1 (L)** Vectorized hash-aggregate + SIMD filter kernels for columnar GROUP BY — close the DuckDB/ClickHouse gap to ≥ 0.8× on all five query shapes (E5). Profile-first (the proven discipline), byte-identical conformance gate throughout.
- [ ] **M6.2 (M)** Statistics + ANALYZE + a minimal cost model: row-count + NDV per column, join reordering for ≥ 3-way joins, cost-annotated EXPLAIN (E6).
- [ ] **M6.3 (XL, gated on profiling)** Coroutine flattening spike on ONE hot path (the commit pipeline): measure the real ceiling gain on a branch before committing to the rewrite (E1). Go/no-go decision documented.
- [ ] **M6.4 (M)** TPC-C (OLTP) and TPC-H SF1 (analytics) unofficial runs, published with methodology — the lingua-franca numbers evaluators ask for (O5).

**Exit criterion:** analytics ≥ 0.8× DuckDB on the standard shapes; published TPC-C/TPC-H numbers with reproduction scripts.

### M7 — Scale-out story *(3–6 months)*

Goal: close the biggest architectural gap vs Cockroach for the target segment (single-region scale-out).

- [ ] **M7.1 (XL)** Dynamic shard split/merge + rebalancing: spec first (new TLA+ model for range movement atomicity), Calvin-sequencer-aware routing updates, verified movement (no lost/duplicated keys under fault storm) (E2).
- [ ] **M7.2 (M)** Online `ALTER TABLE` at scale (backfill as background deterministic work).
- [ ] **M7.3 (M)** Distributed-SQL residuals: SUM/AVG(DISTINCT) shuffle, snowflake dim→dim (E9).
- [ ] **M7.4 (L)** Kubernetes operator (or at minimum a Helm chart + StatefulSet recipe) — the deployment mode the scale-out audience expects.

**Exit criterion:** a 3→6 node live expansion under load with zero errors, demonstrated in the docs with a script.

### M8 — Monetization & sustainability *(begins after M4 traction, runs parallel)*

Realistic options, in order of fit:

1. **Open-core stays 100% open; sell a managed service** (the Neon/PlanetScale path). Heavy lift; only viable after M5+M7 and real traction. Decide at ≥ 5 production users.
2. **Support/consulting + priority features** for early production adopters (fintech/control-plane niche pays for correctness). Viable immediately after M5.
3. **The verification harness as a product** — "Antithesis-lite": deterministic-simulation testing for *other people's* distributed systems, extracted from `core/` + `providers/sim/`. This is a *separate product* with its own market; spike it only if the DB wedge stalls, but document the extraction seam now (S).
4. **Sponsorship/grants** (GitHub Sponsors, NLnet, Sovereign Tech Fund) — bridge funding for the OSS phase; apply during M4 (S).

**Exit criterion:** a funding source covers continued development before month 18.

---

## 5. Sequencing summary & dependencies

```
M0 truth baseline ──► M1 five-minute experience ──► M4 launch (needs M1+M2)
                 └──► M2 docs site ────────────────┘
M3 PG compat matrix ──────────────────────────────► (feeds M4 credibility)
M4 traction ──► M5 production hardening ──► banner removal ──► M8 monetize
            └─► M6 perf leadership (parallel)
M5 + M6 ──► M7 scale-out ──► managed-service option
```

The critical path to first external users is **M0 → M1 → M2 → M4.2 (Show HN)**:
roughly 6–10 weeks of focused work, none of it deep-engine work. The engine is
already the strong part; **distribution is the bottleneck**, so the early milestones
are deliberately packaging and story, not features.

## 6. Risks

| Risk | Mitigation |
|---|---|
| **Bus factor 1** | M0.4 contributor funnel; docs deep-dives (M2.2) double as onboarding; keep the verification gate so contributions can't silently break correctness |
| **Trust barrier for a new DB** ("nobody got fired for Postgres") | Lead with verification story + external audit (M5.7); position as *complement* first (analytics replica / edge tier), *replacement* second |
| **HN launch flops** | The launch is a repeatable series (5 blog posts), not one shot; each post is independently linkable |
| **Cockroach/Neon ship columnar** | Their engines aren't byte-identically dual-layout; our verification moat stays; speed of the M6 window matters |
| **C++23 toolchain friction for contributors** | Docker dev image is already the documented path; keep it working |
| **Scope creep back into engine work** (the comfortable zone) | This document. M1/M2/M4 are packaging milestones; hold the line |

## 7. KPIs per phase

| Phase | Metric |
|---|---|
| M1–M2 | time-to-first-psql ≤ 5 min; docs cover 100% of README claims |
| M4 | 1000 stars; 3 external contributors; 5 stranger trial reports |
| M5 | production-readiness checklist green; 72h soak report published; ≥ 1 external production user |
| M6 | ≥ 0.8× DuckDB on standard shapes; TPC-C/H published |
| M7 | live 3→6 node expansion demo |
| M8 | funding covers development by month 18 |

---

*Non-negotiables carried through every milestone: durability and determinism are
sacrosanct; the consensus cross-check stays byte-identical; every consensus change
is strictly additive; benchmarks stay honest with named caveats.*
