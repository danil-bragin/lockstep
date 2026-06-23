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

---

# S8.1 — Concurrent measurement (the REAL throughput ceiling + bottleneck)

> **MEASUREMENT, NOT optimization.** Same container, same honesty caveats as S7. The S7
> table above is **closed-loop, concurrency 1** — throughput == 1/latency *by construction*,
> so it measures latency, NOT the throughput ceiling. S8.1 drives load under **concurrency**
> (many requests in flight) to find the REAL ceiling and the actual bottleneck that S8.2
> batching must target. **No core/sim/consensus/txn/storage/query change** — this is a new
> prod-layer load client + a harness. (The protected dirs diff is EMPTY for S8.1.)

## The pipelined / concurrent client (`lockstep_admin pbench`)

A new verb keeps **K requests IN FLIGHT** on one persistent connection — a sliding WINDOW:
fire the first K submits without waiting, then on each reply fire the next — and can drive
**C concurrent connections** (`--conns`) round-robin-pumped on the client. It reports REAL
throughput = **total ACCEPTED ops / wall time** (NOT 1/latency):

```
PBENCH count=N inflight=K conns=C value_bytes=B elapsed_ms=T accept_tput=ops/s \
       accepted=A replied=R fault=0|1 unfinished=0|1
```

`fault=1` = a `NotLeader` reply was seen (leadership lost mid-run); pbench then STOPS firing
new submits and drains the outstanding window, so the throughput reflects the **stable-leader
window**, not a long doomed tail. Everything is BOUNDED (finite N, finite K, an absolute wall
guard); `pgrep -x lockstepd` is empty after every run.

**Does the admin server accept concurrency, or serialize?** The admin serve-loop processes
frames from one connection **serially** (`recv → handle_admin → send`), but the connection
**buffers queued inbound frames (FIFO)**, so K pipelined submits ARE accepted without K
client↔server round-trips. Crucially, `submit()` returns at **ACCEPT (append) time** — it does
**NOT** block on `fdatasync` (the persist worker syncs in the background) nor on commit — so
`accept_tput` measures the ACCEPT ceiling = reactor/CPU + append + per-frame serve cost.
**The server does NOT fully serialize away the benefit of pipelining** (1-node tput rises
~5.7× with depth), so server-per-connection serialization is NOT the 1-node bottleneck.

## QPS-vs-depth curve (median of 3 passes, fresh cluster per pass; count=4000, value=16B, conns=1)

| config | N | depth K | accept_tput (ops/s) | min | max | fault | note |
|--------|---|---------|---------------------|-----|-----|-------|------|
| 1-node | 1 | 1  | **2940**  | 2834 | 2976 | no  | ≈ S7 single-client (~2800) — same path |
| 1-node | 1 | 4  | **7128**  | 7063 | 7242 | no  | 2.4× over depth 1 |
| 1-node | 1 | 16 | **13021** | 13002| 13039| no  | 4.4× over depth 1 |
| 1-node | 1 | 64 | **16773** | 16678| 16789| no  | 5.7× over depth 1 — still rising |
| 3-node | 3 | 1  | **463**   | 441  | 627  | no  | clean; ≈½–⅓ of S7's ~1100 (real 3-node variance) |
| 3-node | 3 | 4  | **117**   | 102  | 124  | **yes** | leadership LOST (~1200/4000 accepted) — COLLAPSE |
| 3-node | 3 | 16 | **276**   | 256  | 317  | **yes** | leadership LOST (~1280/4000) |
| 3-node | 3 | 64 | **778**   | 721  | 836  | **yes** | leadership LOST (~1400/4000) |

**Saturation knee.** *1-node:* **no knee within K≤64** — throughput climbs monotonically
(2940→16773) and is still rising at 64; the ceiling is ~17k ops/s and pipelining is the only
thing that approaches it (closed-loop concurrency-1 would never exceed ~3k). *3-node:* the knee
is at **K=1** — throughput does NOT rise with depth, it **collapses** (463 → ~100–800 with
`fault=1`): more in-flight makes 3-node WORSE, the opposite of 1-node.

## BOTTLENECK VERDICT + evidence

**1-node → (c) reactor / single-thread CPU-bound (NOT fsync, NOT server-serialization).**
- Evidence: tput RISES 5.7× with pipelining (2940→16773) — so it is NOT (d) server-per-conn
  serialization (that would stay flat) and NOT (a) fsync-at-accept (accept doesn't sync; an
  fsync wall would also be flat vs depth). It scales smoothly with in-flight depth and
  plateaus toward a ~17k ceiling = the single reactor thread saturating on per-request work
  (epoll wakeups, frame decode/encode, log append, promise plumbing). The low-depth number
  (2940) is round-trip/syscall-latency-bound; the high-depth ceiling is reactor-CPU-bound.

**3-node → (b)/(c) hybrid: replication round-trip at depth 1, then single-reactor STARVATION
of the Raft heartbeat/replication path under burst load.**
- Evidence at depth 1 (clean, fault=0): 463 ops/s vs 1-node's 2940 = a **~6× drop** purely
  from adding cross-process TCP replication — the per-accept reactor now also services
  follower `AppendEntries` RTTs + acks + heartbeat timers, so each admin round-trip is delayed.
- Evidence at depth ≥4 (**fault=1, throughput COLLAPSES**): a pipelined burst of client submits
  on the **single leader reactor** delays its own heartbeats past the election timeout → a
  follower starts an election → the leader is **deposed** (`NotLeader`). The cluster thrashes
  leadership and only ~1200–1600 of 4000 submits land before the first deposition. This is
  **single-threaded reactor starvation**: client I/O, follower RPC, the persist worker, and
  heartbeat timers all contend for one thread, and client bursts win, starving consensus.

## What S8.2 batching should target

The bottleneck is **per-operation work on the single reactor thread**, NOT disk fsync at the
accept path and NOT server-per-connection serialization. So S8.2 should **batch to cut the
per-op reactor cost**, in two complementary directions:

1. **Batch the admin/client accept path** — coalesce multiple queued client submits into ONE
   log-append + ONE `broadcast_append` (group-commit at the LEADER): N submits → 1 append
   amortizes the per-op append + replication-kick + frame plumbing. This is the lever that
   moves the 1-node ~17k reactor ceiling AND relieves the 3-node reactor so bursts stop
   starving heartbeats. **Prod-layer-eligible** (the admin serve-loop / submit batching can
   live at the prod surface) — confirm it does not require a consensus-core change.
2. **Batch replication entries** (3-node) — send multiple log entries per `AppendEntries`
   instead of one kick per submit, cutting the follower-RTT count per committed op. This may
   touch the **consensus core** (`broadcast_append` / `AppendEntries` payload sizing). If so,
   that is the **S8.2 core finding — FLAG it**, do not change core in a measurement stage.

> **CORE-CHANGE FLAG for S8.2:** making 3-node survive concurrency almost certainly needs a
> *core* change (entry-batched `AppendEntries`, and/or de-prioritizing client I/O vs heartbeat
> on the reactor). Per S8.1 invariants the core was NOT touched here; this is recorded as the
> S8.2 target.

## Reproduce (S8.1)

```bash
docker run --rm -v "$PWD:/work" -w /work --memory=6g --cpus=4 lockstep-dev:latest bash -lc '
  set -e; ulimit -c 0; ulimit -s 16384
  cmake -S . -B build/lrel -GNinja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/lrel -j6 --target lockstepd lockstep_admin
  to(){ perl -e "my \$t=shift;my \$p=fork();if(\$p==0){setpgrp(0,0);exec @ARGV;exit 127}\$SIG{ALRM}=sub{kill q(KILL),-\$p;exit 124};alarm \$t;waitpid(\$p,0);exit(\$?>>8)" "$@"; }
  LOAD_DEPTHS="1 4 16 64" LOAD_COUNT=4000 LOAD_REPEATS=3 RUN_SECONDS=40 to 600 bash tests/prod_load.sh
  pgrep -x lockstepd && echo LEAKED || echo no-orphans'
```

Tunables (env): `LOAD_COUNT`, `LOAD_VALUE_BYTES`, `LOAD_DEPTHS`, `LOAD_CONNS`, `LOAD_REPEATS`,
`RUN_SECONDS`, `SEED`. Single-shot: `lockstep_admin pbench --count N --inflight K --conns C
--value-bytes B --host PORT [--host PORT ...]`.

## Honest caveats (S8.1)

- **Laptop container, `--cpus=4`, shared host, real wall-clock** — absolute numbers are a
  *relative* regression baseline only; re-run on the SAME setup to compare.
- **3-node numbers at depth ≥4 are a COLLAPSE signal, not a steady throughput** — `fault=1`
  means leadership was lost; the tput there is "ops accepted before deposition / wall", useful
  as evidence of starvation, NOT as a sustained-QPS figure. 3-node depth-1 (463) is the only
  clean steady 3-node number, and it has high run-to-run variance (441–627).
- **`accept_tput` is the ACCEPT (append) ceiling, not commit throughput.** Commit adds the
  background `fdatasync` + (3-node) replication RTT; S7's commit-latency table still describes
  that path. A committed-throughput-under-concurrency measurement is left for after S8.2.
- Measured on the prod path **as built** — no optimization, no hot-path change for S8.1.
