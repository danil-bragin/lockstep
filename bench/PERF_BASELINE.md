# Lockstep — Prod Cluster Performance BASELINE (Phase 7, S7)

> **BASELINE — NOT OPTIMIZED. NOT a production benchmark.**
> A number to *regress against* later, measured on a CPU/mem-limited Docker container
> on a developer laptop. Absolute values are only meaningful **relative to a re-run on
> the same setup**. No core/sim/consensus/txn/storage/query code was touched or tuned
> for S7 — this measures the real cluster *as built* by S5b-2 + S6.

## What was measured

A **closed-loop write load** (concurrency 1 — one request in flight) driven by the
`lockstep_admin bench` verb over **ONE persistent client reactor + ONE persistent TCP
connection** to the leader's admin port, against a real multi-process `lockstepd`
cluster. The persistent connection is the key design choice: it measures the **server
submit path**, *not* per-call process-spawn / reactor-construction overhead (the
single-shot `submit`/`status` verbs recreate a reactor per call — fork+connect cost
would dominate and the number would be meaningless).

Two metrics, each over `count` distinct submits of `value_bytes`-byte values:

- **submit throughput + submit p50/p99** = the **submit→ACCEPT** path: client `send` →
  leader appends the entry to its Raft log → reply. This is the leader's accept
  latency under a single client; it is **not** a saturation throughput (one client,
  one in-flight request).
- **commit p50/p99** = a **sample** of **submit→COMMIT** latency: after a submit, poll
  `STATUS` until `commit_index` covers the value's index. **Poll-bounded ⇒ over-counts
  by up to one poll turn** — approximate, stated as such. For 3-node this includes real
  replication RTT (leader → followers `AppendEntries` → quorum ack).
- **read check** (correctness, not a read benchmark): after the load, read the leader's
  committed log back over the socket and verify the submitted values appear **in order**
  as the committed slice ending at the last accepted index. `read_check=ok` on every
  recorded pass.

Latency percentiles are nearest-rank (no interpolation) over the per-submit sample.

## Configs

| config | nodes | quorum | replication |
|--------|-------|--------|-------------|
| 1-node | 1     | 1      | none (lone leader self-commits) |
| 3-node | 3     | 2      | real TCP `AppendEntries` to 2 followers |

Each config runs **3 passes on a FRESH cluster** (fresh data dirs + daemons per pass)
so the 3 repeats are **comparable independent samples**, not a growing-log sequence.
We report the **MEDIAN** of the 3 passes + the min/max spread. We do **not**
cherry-pick the best run.

## Machine

- **Inside `lockstep-dev:latest` Docker container**, Linux, Release build (`-O2`,
  `-DCMAKE_BUILD_TYPE=Release`).
- Container limits: `--cpus=4 --memory=6g` (host reports `nproc=14`,
  `mem_total≈24 GB`; the cgroup caps apply).
- `count=2000`, `value_bytes=16`, `commit_samples=128`, `repeats=3`, `seed=12345`.
- Raft timing as `lockstepd` defaults: heartbeat 30 ms, election 150–300 ms.

## BASELINE numbers (median of 3 passes; representative — real-time, varies run-to-run)

| config | N | submit tput (ops/s) | submit p50 (µs) | submit p99 (µs) | commit p50 (µs) | commit p99 (µs) |
|--------|---|---------------------|-----------------|-----------------|-----------------|-----------------|
| 1-node | 1 | **~2800**           | ~315            | ~740            | ~760            | ~1400           |
| 3-node | 3 | **~1100**           | ~540            | ~1900–6700      | ~2100           | ~5400–14000     |

Two representative full runs (illustrating real-time variance — both are valid
baselines on this setup; the table above brackets them):

```
RUN A   1-node  tput=2784.0  sP50=319.12  sP99= 739.25  cP50= 758.25  cP99= 1389.25
        3-node  tput=1058.5  sP50=544.17  sP99=6723.00  cP50=2144.50  cP99=13992.21
RUN B   1-node  tput=3001.0  sP50=302.21  sP99= 697.67  cP50= 713.62  cP99= 1324.71
        3-node  tput=1516.7  sP50=518.38  sP99=1838.79  cP50=2055.54  cP99= 5399.88
```

**1-node is stable** (per-pass tput within ~1% of the median). **3-node has real
variance** (tput 920–1560 ops/s, p99 2.3–12 ms across passes) — expected: cross-process
TCP + epoll scheduling + fsync jitter on a shared, CPU-limited container. The min/max
columns in the harness output capture the spread per run.

### Replication cost (the headline of this baseline)

| metric            | 1-node | 3-node | delta              |
|-------------------|--------|--------|--------------------|
| submit throughput | ~2800  | ~1100  | **≈2.5–2.7× drop** |
| commit p50        | ~760µs | ~2100µs| **≈2.8×**          |
| commit p99        | ~1.4ms | ~5–14ms| **≈4–10× tail**    |

Replication (quorum=2 over real TCP) roughly halves single-client submit throughput
and multiplies commit latency several-fold, with a heavy p99 tail from the replication
round-trip + per-node fsync. This is the cost we expect; the baseline records it so
later work can show whether an optimization actually moves it.

## Reproduce

```bash
docker run --rm -v "$PWD:/work" -w /work --memory=6g --cpus=4 lockstep-dev:latest bash -lc '
  set -e
  ulimit -c 0; ulimit -s 16384
  cmake -S . -B build/ldev -GNinja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/ldev -j6 --target lockstepd lockstep_admin
  to(){ perl -e "my \$t=shift;my \$p=fork();if(\$p==0){setpgrp(0,0);exec @ARGV;exit 127}\$SIG{ALRM}=sub{kill q(KILL),-\$p;exit 124};alarm \$t;waitpid(\$p,0);exit(\$?>>8)" "$@"; }
  to 160 bash tests/prod_perf.sh
  pgrep -x lockstepd && echo LEAKED || echo no-orphans'
```

Tunables (env): `PERF_COUNT`, `PERF_VALUE_BYTES`, `PERF_COMMIT_SAMPLES`, `PERF_REPEATS`,
`RUN_SECONDS`, `SEED`. The load is **finite** (fixed `count` + bounded per-run
deadlines); every `lockstepd` self-deadlines (`--run-seconds`) and is SIGKILLed by the
harness `trap` on every exit path; `pgrep -x lockstepd` is **empty** after each run.

## Honest caveats

- **Laptop container, not a server.** CPU-limited, shared host, real wall-clock. Numbers
  are a *relative* regression baseline only.
- **Single-client closed loop (concurrency 1).** This is accept/commit *latency* and the
  throughput that *follows from it under one client* — NOT a saturation/max-throughput
  benchmark. A future stage that wants peak QPS needs a concurrent client driver.
- **Commit latency is poll-approximated** (over-counts by up to one STATUS poll turn).
- **3-node variance is large** run-to-run; treat its single numbers as order-of-magnitude.
- Measured on the prod path *as built* — **no optimization, no hot-path changes**.

## Perf observation (FLAGGED, not fixed — deferred per S7 scope)

The `lockstep_admin bench` commit-latency sample and the harness `STATUS` polls work by
**re-reading the FULL durable log** on every `STATUS` (the `StatusRep` reply serializes
every committed value). As the log grows this makes each `STATUS` round-trip O(log size),
and a benchmark that polls per submit re-pays it each time — visibly degrading throughput
when the same daemon is reused across many submits (observed: ~2800→~1900 ops/s when 3×
2000 submits accumulate on one daemon, which is why the baseline uses a fresh cluster per
pass). This is a *protocol/observability* cost, not a consensus hot-path cost, and lives
in the prod admin surface (`ProdConsensusNode::handle_admin` STATUS path), **outside** the
protected core. **FLAGGED for a later phase; not fixed here** (S7 is baseline-only).
