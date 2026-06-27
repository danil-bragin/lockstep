# Comparative Benchmark — Lockstep vs Postgres / etcd / CockroachDB / TiKV

> **HONEST FRAMING FIRST.** This harness does not exist to crown Lockstep. It exists to
> place Lockstep's *real, verified* numbers next to mature systems on **identical hardware
> budgets + identical workloads + identical metric definitions**, and to say plainly where
> Lockstep is competitive, where it is not, and *why* in each case. A benchmark that only
> shows wins is a marketing deck, not an engineering result.

## What is being compared, and why each competitor

| system | category | the apples-to-apples question it answers |
|--------|----------|------------------------------------------|
| **Lockstep** | sim-verified distributed txn KV + SQL-sugar | the system under test |
| **etcd** | single-Raft KV | closest analog to ONE Lockstep replicated shard — Raft commit latency + zero-loss under faults, head-to-head |
| **CockroachDB** | distributed SQL, Raft-per-range, sharded | closest analog to Lockstep's *whole* story — multi-shard scaling + serializable SQL + HA |
| **PostgreSQL** | mature single-node SQL | the raw-SQL maturity baseline — where Lockstep's young SQL layer is honestly behind |
| **TiKV** (+ PD) | distributed txn KV | a second distributed-KV point — Percolator txns vs Lockstep's Calvin-style cross-shard |

## The honest hypothesis (stated BEFORE running — so the data can refute it)

- **Single-box raw SQL throughput → Postgres wins.** Decades of planner/executor/storage
  maturity vs Lockstep's young in-memory SQL pipeline. We expect to *lose* here and report it.
- **Single Raft group KV commit → etcd is the bar.** Lockstep's one replicated shard should be
  in the same order of magnitude (~1–20k commits/s depending on pipeline depth); etcd is the
  reference for "is our Raft commit path competitive".
- **Multi-core throughput SCALING → Lockstep's strength.** Independent thread-per-shard
  (measured ~50–66k commits/s at M=8 on this host, near-linear to M≈4) is the horizontal lever.
  The question: does Lockstep's scaling *curve* (factor vs cores) hold up against Cockroach's
  range-sharded scaling, and how does each degrade past the core count.
- **Strict-serializable commit UNDER FAULTS → the verification story.** Not "who is fastest"
  but "who loses zero acked data + stays consistent when the leader is killed mid-load", with
  recovery time measured. Lockstep's jepsen-proven zero-loss is the claim to put on the same
  axis as etcd/Cockroach.

## Vectors (the 4 workloads)

1. **KV point ops (YCSB-style)** — read / write / mixed (e.g. 50/50, 95/5) over a fixed
   keyspace + value size. Metrics: committed throughput (ops/s), p50/p99 latency
   (nearest-rank), at matched client concurrency / pipeline depth.
2. **Scaling 1→N cores** — the SAME KV workload, server pinned to K ∈ {1, 2, 4, 8, … host-max}
   CPUs. Metric: throughput(K) and the scaling factor vs K=1. This is where Lockstep's
   multi-shard parallelism is the headline.
3. **Fault / HA** — 3-replica cluster, steady load, then SIGKILL the leader. Metrics:
   unavailability window (time-to-new-leader), throughput dip, and **zero-acked-loss**
   verification (every ack'd write still present + consistent after recovery).
4. **SQL transactions (TPC-C-lite)** — multi-statement contended txns. Metrics: txn/s, p50/p99,
   and consistency outcome (do anomalies appear under the claimed isolation level).

## Fairness rules (the load-bearing methodology — identical or it doesn't count)

- **IDENTICAL RESOURCE PIN.** Every server runs in Docker with the SAME `--cpus=K --memory=Mg`
  for a given cell. The core-sweep changes K identically for all systems. The client gets fixed,
  separate headroom so the *server* is the bottleneck under test (and we record client CPU to
  prove the client is not the limit; where it is — e.g. a single-thread client past M=8 — we
  SAY SO, as the existing Lockstep baseline already does).
- **IDENTICAL WORKLOAD.** Same keyspace size, value size, op mix, op count / duration, warmup,
  and seed across systems. Defined ONCE in the driver, not per-adapter.
- **IDENTICAL METRIC DEFINITIONS.** "Throughput" = **committed** ops / wall (NOT accept/append —
  Lockstep's own baseline corrected this; we hold every system to commit, e.g. Postgres COMMIT
  returned, etcd quorum-acked, Cockroach txn committed). Latency = nearest-rank p50/p99 over the
  per-op sample. Same closed-loop vs pipelined concurrency model per cell.
- **EACH SYSTEM AT ITS BEST, not handicapped.** We drive each via its native/optimized client
  (Lockstep `lockstep_admin pbench/mbench`; Postgres/Cockroach pg-wire; etcd clientv3; TiKV
  go-ycsb). We do NOT force a fast system through a slow uniform loop — the EQUALIZERS are the
  resource pin + workload + metric definitions, not a lowest-common-denominator client.
- **MEDIAN of N fresh passes**, with min/max spread reported. Fresh data dirs per pass. No
  cherry-picking; the spread is published.
- **Every honest caveat published** (laptop Docker Desktop LinuxKit VM on Apple Silicon, shared
  host, real wall-clock ⇒ absolute numbers are RELATIVE; the *shape* — scaling curve, fault
  behavior, win/loss direction — is the result).

## Known asymmetry to handle explicitly (do NOT paper over)

- **SQL-over-wire asymmetry — since RESOLVED.** When this charter was written Lockstep SQL ran
  in-process only, so a "SQL txn/s" head-to-head was either unfair or impossible at parity. The
  resolution (expose SQL over `wire::Server` via a SQL verb) was taken: SQL now runs over the
  socket (verified exactly-once, ~105k/s) and across shards (distributed JOIN). A socket-to-socket
  SQL head-to-head against Postgres/Cockroach is now *possible* but **not yet run** — that
  benchmark is the remaining open item, not an architectural blocker. Until it lands, the SQL
  story still leans on correctness/isolation, and any in-process SQL number is labeled a lower
  bound (omits network).
- **etcd/TiKV are KV, not SQL**; they appear only in vectors 1–3, not 4.
- **Postgres is single-node**; in the scaling vector it scales via processes/connections on K
  cores, not via replication — that is the honest single-node-scaling point, labeled as such.

## Layout

```
bench/compare/
  SPEC.md            — this file (methodology + honest framing)
  docker-compose.yml — one service per competitor, parametrized cpus/mem
  run.sh             — orchestrator: for each (system × cpu-level × vector): pin, run, collect, teardown
  adapters/          — one per system (lockstep, postgres, etcd, cockroach, tikv); drive go-ycsb / native
  report.py          — results/*.json → comparison tables + honest narrative
  results/           — per-cell JSON (resumable: skip completed cells)
  sql_analytics/     — the 5-way single-CPU SQL-analytics comparison (vs DuckDB/ClickHouse/PG/SQLite)
```

## Resource discipline (this laptop froze before on heavy builds)

- ONE system up at a time; tear down (compose down -v) before the next ⇒ bounded memory.
- Resumable per-cell JSON; a killed run re-skips done cells.
- Bounded per-cell wall guard; every server self-deadlines or is force-removed.
- Competitor images PULLED once (postgres, etcd, cockroach, tikv+pd); Lockstep uses the
  existing `lockstep-dev` image. Pulling external images = downloading — surfaced to the user.
