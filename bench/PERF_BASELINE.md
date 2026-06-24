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

> **⚠️ HEADLINE CORRECTED BY S8.3 (see bottom).** The S8.1 `accept_tput` numbers below are
> the **leader's local-APPEND rate**, NOT commit throughput — `submit` returns at APPEND
> (term,index assigned) BEFORE commit. With S8.2b batching, append is cheap, so `accept_tput`
> **inflates far past the real commit ceiling** (measured 3-node depth-64: accept_tput ~68k/s
> but real COMMIT throughput ~15k/s — a ~4.4× gap). The S8.1 collapse-fix is real; only the
> throughput **numbers** here were misleading. **For the honest commit-throughput headline,
> read the S8.3 section at the end of this file.** The S8.1 text is retained as the
> bottleneck/collapse analysis it was; its accept numbers are now clearly secondary.

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

---

# S8.3 — HONEST commit throughput (the corrected headline) + a cheap O(1) commit query

> **MEASUREMENT, NOT optimization.** Same container, same honesty caveats. No
> core/sim/consensus/txn/storage/query change (this is prod-admin + cli + harness only;
> the protected-dirs diff is **EMPTY**). The full-log STATUS path is **UNCHANGED** — the
> jepsen + cluster_smoke safety checkers still depend on it and still pass. This stage
> makes the THROUGHPUT NUMBER honest: it reports real **commit** throughput, backed by a
> new **cheap O(1) commit-index query** so polling commit progress per submit no longer
> pays the O(log) full-STATUS cost.

## Why the S8.1 `accept_tput` was misleading

`submit()` returns at **ACCEPT** (the leader appends the entry, assigns term+index) —
**BEFORE** the entry is committed (replicated to a quorum + `commit_index` advanced). With
S8.2b batching, append is cheap, so `accept_tput` (accepted ops / wall) inflates well past
the real commit ceiling: the leader "finishes" accepting 4000 ops in ~50 ms while only a
fraction have actually committed, the cluster catching up over the next ~250 ms+. The
collapse-fix from S8.2b is real and verified — only the S8.1 throughput *numbers* overstated
the ceiling.

## The cheap O(1) commit-index query (the backing for honest measurement)

A new admin verb / RPC, **`StatCommit`** (request kind 6 → reply kind 7), returns **only
`{role, term, commit_index}`** — `node_->commit_index()` is a member read (O(1)); it does
**NOT** walk or serialize the durable log. The existing full-log **`Status`** path (which
serializes every committed value, O(log size)) is **kept intact** — the `prod_jepsen` and
`prod_cluster_smoke` safety checkers read its committed-log digest to check agreement +
ack-durability, so it must stay. CLI surface:

- `lockstep_admin commit --host PORT [--host PORT ...]` → `COMMIT port=P ok=1 role=R term=T commit=C`
  (the O(1) counterpart of `status`, no `log=` digest).
- `lockstep_admin status …` → unchanged (full `log=v1,v2,…` digest; what the checkers use).

The honest-commit harness polls `StatCommit` (NOT `Status`) per progress-check, so the hot
path is O(1) and does **not** degrade as the log grows. Measured at an 8000-entry log
(container, incl. process-spawn overhead): cheap `commit` ≈ **715 µs/call** vs full `status`
≈ **1981 µs/call** — the ~2.8× delta is exactly the full-log re-serialization the hot path
now avoids (the server-side gap is larger; process spawn is a constant in both).

## Honest commit-throughput method (the `pbench` extension)

1. Drive the pipelined accept load (depth K) and record the highest **accepted index**
   (`commit_target`).
2. **Poll the cheap `StatCommit` on every host** until the cluster's `commit_index` covers
   `commit_target`. The leader only advances `commit_index` once a **quorum has acked**, so
   max(commit_index) across hosts reaching the target ⇒ quorum-committed.
3. `commit_tput = accepted / (wall from first submit until commit covers all accepted)`.
   **HEADLINE = `commit_tput`.** `accept_tput` is kept as a **clearly-labeled secondary**
   (leader-append rate, NOT commit). Everything BOUNDED (finite count, absolute catch-up wall).

`pbench` now prints: `commit_tput … commit_target … commit_reached … commit_covered=0|1 …
accept_tput … accept_elapsed_ms …`. (Per-connection client ids were also made distinct so
the daemon routes each connection's replies correctly — a shared id collapsed concurrent
connections; this fixed the poller never receiving its reply.)

## HONEST commit-throughput numbers (median of 3 passes, fresh cluster per pass; count=4000, value=16B, conns=1)

| config | N | depth K | **commit_tput** (ops/s) | min | max | accept_tput (NOT commit) | accept-vs-commit gap | note |
|--------|---|---------|-------------------------|-----|-----|--------------------------|----------------------|------|
| 1-node | 1 | 1  | **3997**  | 3988 | 4165 | 3998  | ~1.0× | N=1 self-commits at append ⇒ accept≈commit (no gap) |
| 1-node | 1 | 4  | **9068**  | 8944 | 9272 | 9072  | ~1.0× | accept≈commit |
| 1-node | 1 | 16 | **14446** | 13814| 14484| 14500 | ~1.0× | accept≈commit |
| 1-node | 1 | 64 | **17114** | 16081| 17605| 17146 | ~1.0× | accept≈commit; ceiling ~17k, still rising |
| 3-node | 3 | 1  | **1166**  | 949  | 1741 | 1243  | ~1.07× | clean (`covered=1`); replication RTT bound; high variance |
| 3-node | 3 | 4  | **~0–234**| 0    | 234  | 244   | n/a   | **UNSTABLE: `fault=1`/`covered=0` in 2 of 3 passes** (collapse persists at depth 4) |
| 3-node | 3 | 16 | **1218**  | 708  | 3898 | 1342  | ~1.1× | clean (`covered=1`) but very high variance (708–3898) |
| 3-node | 3 | 64 | **15412** | 12812| 16325| 68403 | **~4.4×** | clean (`covered=1`); **accept_tput ~68k MASSIVELY overstates the ~15k commit ceiling** |

### The accept-vs-commit gap (the whole point)

- **1-node: NO gap.** N=1 self-commits synchronously at append (gated `quorum()==1`), so
  accept *is* commit — the S8.1 1-node accept numbers were already honest. The 1-node
  commit ceiling is **~17k ops/s** at depth 64 (still rising).
- **3-node depth-64: a ~4.4× gap** — `accept_tput` median **68403** vs honest
  `commit_tput` median **15412**. THIS is the misleading inflation the lead flagged: append
  finishes in ~50 ms, but commit (quorum replication + `commit_index` advance) takes ~250 ms.
  **The real 3-node ceiling is ~15k commits/s at high depth, not ~68k.**
- **3-node depth-4 is still a collapse zone** (`fault=1` / `commit_covered=0` in 2/3 passes):
  S8.2b batching makes high depth (64) survive AND commit cleanly, but a *small* in-flight
  window (4) on 3 nodes still occasionally thrashes leadership before catch-up. Recorded as a
  finding, not a steady number.

> **Counter-intuitive but real:** for 3-node, a LARGER in-flight window (64) commits cleanly
> and far faster than a small one (4). Bigger batches coalesce more per `AppendEntries`,
> relieving the single reactor — the opposite of the S8.1 pre-batching collapse curve.

## STATUS-O(log) hot-path degradation: GONE on the measured path

The S7-flagged degradation (each `STATUS` re-serializes the FULL durable log, so a
per-submit poll re-pays O(log size) and throughput sags as the log grows) is **removed from
the hot path**: the honest-commit harness polls the O(1) `StatCommit`, never the full-log
`Status`. The full-log `Status` itself is intentionally **left O(log)** (the checkers need
the digest) — we just stopped the measurement hot path from paying it. Evidence above:
cheap `commit` ≈715 µs vs full `status` ≈1981 µs at an 8000-entry log.

## Safety re-run (full-log STATUS intact)

- `prod_cluster_smoke`: **ALL PASS** (election → replication AGREEMENT → kill+restart
  CATCH-UP → final agreement), reading the unchanged full-log STATUS digest.
- `prod_jepsen`: TEETH correctly FAILs the fabricated bad history; a clean run has **all
  scenarios PASS** (ALL SAFETY PROPERTIES HELD). **FLAG:** the fault-injection scenarios are
  **flaky run-to-run on this container** (ack-durability occasionally fails) — verified
  **identical on the unchanged baseline tree** (git-stash), so it is **pre-existing
  container-timing flakiness, NOT a regression from S8.3** (S8.3 touches no path jepsen uses;
  it only `submit`s + reads the unchanged full STATUS).

## Reproduce (S8.3)

```bash
docker run --rm -v "$PWD:/work" -w /work --memory=6g --cpus=4 lockstep-dev:latest bash -lc '
  set -e; ulimit -c 0; ulimit -s 16384
  cmake -S . -B build/lrel -GNinja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/lrel -j6 --target lockstepd lockstep_admin
  to(){ perl -e "my \$t=shift;my \$p=fork();if(\$p==0){setpgrp(0,0);exec @ARGV;exit 127}\$SIG{ALRM}=sub{kill q(KILL),-\$p;exit 124};alarm \$t;waitpid(\$p,0);exit(\$?>>8)" "$@"; }
  LOAD_DEPTHS="1 4 16 64" LOAD_COUNT=4000 LOAD_REPEATS=3 RUN_SECONDS=90 to 900 bash tests/prod_load.sh
  pgrep -x lockstepd && echo LEAKED || echo no-orphans'
```

Single-shot honest run: `lockstep_admin pbench --count N --inflight K --conns C --value-bytes B
--host PORT [--host PORT ...]` (headline `commit_tput`; `accept_tput` secondary). Cheap query:
`lockstep_admin commit --host PORT [...]`.

## Honest caveats (S8.3)

- **Laptop container, `--cpus=4`, shared host, real wall-clock** — absolute numbers are a
  *relative* regression baseline only; re-run on the SAME setup to compare.
- **`commit_tput` is the HEADLINE; `accept_tput` is the leader-append rate (NOT commit)** and
  is kept only to show the gap. For 3-node at depth, the gap is large (~4.4×) — trust
  `commit_tput`.
- **3-node has high run-to-run variance** (depth-1: 949–1741; depth-16: 708–3898) and
  **depth-4 is an UNSTABLE collapse zone** (`commit_covered=0`) — treat single 3-node numbers
  as order-of-magnitude; the clean steady points are depth-1 (~1.2k) and depth-64 (~15k).
- The commit-wall is poll-bounded (the cheap poll has its own client reactor); the
  `commit_elapsed_ms` is accept-wall + catch-up-wall, a slight over-count of the true commit
  instant — stated as a baseline approximation.
- Measured on the prod path **as built** — no consensus/core change; prod-admin + cli +
  harness only.

---

# S8.5 — PROFILE the commit path, then group-commit fsync — VERDICT: ALREADY group-committed

> **PROFILE FIRST, then act on the DATA (S8.1/S8.2-style).** The hypothesis was that the
> ~17k(1-node)/~15k(3-node) commit ceiling is bounded by **fsync-per-entry** on the durable
> persist path, and that GROUP COMMIT (batch `fdatasync` across pending appends) would lift
> it. We instrumented the real durable barrier and measured. **The data says the commit path
> is ALREADY group-committed and is NOT fsync-bound at depth — so we did NOT add redundant
> batching** (the brief: "do NOT implement group-commit if the data says it won't help").
> The change is **profiling-only**: `ProdDisk` fdatasync/append counters + a daemon `DISKSTATS`
> shutdown line. **core/sim/consensus/txn/storage/query diff EMPTY.**

## The profiler (fdatasync count + latency, off the durability path)

`ProdDisk` now counts `append()` calls, `sync()` (fdatasync) calls, and the summed `fdatasync`
wall-time (a `steady_clock` delta around the syscall — prod-only, never any deterministic
ordering). `lockstepd` prints on shutdown:

```
DISKSTATS node=N commit_index=C fdatasync_calls=S append_calls=A fsync_total_ms=.. \
          fsync_avg_us=.. fsyncs_per_commit=S/C appends_per_fsync=A/S bytes_appended=..
```

`fsyncs_per_commit` is the headline: ≈1.0 ⇒ one fsync per committed entry (fsync-bound);
≪1.0 ⇒ many appends already coalesced under one fsync (group commit already in effect).

## PROFILE RESULTS (container, Release, `pbench` commit load, count=4000, value=16B)

| config | depth | commit_tput | fdatasync calls | commit_index | **fsyncs / commit** | appends / fsync | fsync_avg µs | fsync % of wall |
|--------|-------|-------------|-----------------|--------------|---------------------|-----------------|--------------|-----------------|
| 1-node | 1     | 4547        | 2002            | 2001         | **1.000**           | 1.11            | 185          | ~84% (371ms/440ms) |
| 1-node | 64    | 16855       | 65              | 4001         | **0.016**           | **68.4**        | 409          | ~11% (27ms/237ms)  |
| 3-node | 1 (leader) | 2930   | 804             | 2001         | **0.40**            | 2.83            | 278          | ~33% |
| 3-node | 64 (leader)| 24223  | 66              | 4001         | **0.016**           | 61.6            | 407          | ~16% (27ms/165ms) |

## BOTTLENECK VERDICT: NOT fsync-bound at depth — group commit ALREADY EXISTS

**The single FIFO persist worker in BOTH Raft impls already group-commits.** Its drain loop
appends EVERY record currently queued (re-checking the queue after each `append` await), and
only when the queue is empty does it issue ONE `sync()`. So any appends that pile up while the
worker is awaiting the disk are swept into the same sync — a **deterministic, scheduler-turn
boundary** ("sync everything pending right now"), exactly the group-commit rule, already in
place since the FIFO worker was introduced. Evidence:

- **At depth 64: 68 appends ride ONE fdatasync** (`fsyncs_per_commit=0.016`). A pipelined burst
  of K in-flight submits is coalesced under a single barrier. fsync is only **~11–16% of the
  commit wall** — the other ~85% is the single-reactor per-op work (epoll, frame decode/encode,
  log append, promise plumbing, replication), **matching the S8.1 reactor-CPU verdict.**
- **1/fsync-latency ≈ 1/410µs ≈ 2440 fsync/s**, but commit throughput is **16.8k commits/s** —
  *7× ABOVE the per-entry fsync ceiling*. You CANNOT commit 16.8k ops/s with one fsync each on a
  410µs fsync; the only way past 2440/s is that fsyncs are already batched (~68:1), which the
  counter confirms. **If commit were fsync-bound, throughput would plateau at ~2.4k, not 17k.**
- **Depth 1 IS fsync-bound** (1.000 fsyncs/commit, fsync = 84% of wall) — but depth 1 has NOTHING
  to batch (one request in flight ⇒ the queue holds one record per sync by definition). The
  depth-1 ceiling is 1/fsync-latency-and-RTT, and is the *closed-loop latency* number, NOT the
  throughput ceiling. Pipelining (the existing batching) is what carries depth 1→64 from 4.5k to
  17k — and it does so precisely BY amortizing the fsync the worker already coalesces.

**Conclusion: adding a second group-commit mechanism would be a NO-OP** (the worker already
coalesces the entire in-flight window under one sync) **and could only add latency** (an explicit
accumulate-then-sync timer would force a wait the deterministic drain doesn't need). The honest,
data-driven move is to NOT change the persist path. The real lever for a higher ceiling is the
single-reactor per-op CPU cost (S8.1's standing finding) — NOT fsync.

## The deterministic batching rule (already in both impls), and persist-before-reply

- **RaftNodeA** (`RaftNodeA.hpp` persist_worker, ~L1694): drains `write_queue_` FIFO, `continue`s
  to re-check the queue after every `append` await, then `if (want_sync_) co_await sync()` once
  the queue is empty. `enqueue_durable` (~L1668) sets `want_sync_ = true` per mutation; ONE
  worker (`worker_running_`) runs at a time, preserving on-disk order == logical order.
- **RaftNodeB** (`RaftNodeB.hpp` persist_worker, ~L1686): same shape — drains the queue, then
  `if (want_sync_) co_await sync()`. `sync_now()` (~L968) requests the barrier after the
  enqueues for a step.
- **Batching boundary is DETERMINISTIC in the sim**: "sync all records pending when the queue
  drains" is a pure function of the single-thread scheduler's turn order, not a wall-clock timer.
  The sim cross-check stays byte-identical per seed (verified: `consensus_crosscheck_test`
  ALL CHECKS PASSED; harness/snapshot/membership OK).
- **Persist-before-reply** is the SAME as before this stage (we changed nothing here): the spec
  (`Consensus.tla`) models durability as an *atomic-within-a-step* precondition (e.g. "persist
  votedFor before reply"), NOT a separately-timed barrier — so a batched physical barrier under
  one fdatasync does not weaken it: each logical action's records are enqueued (in logical order)
  and the durability is established by the worker the deterministic scheduler runs ahead of the
  observable downstream effect. TLC `Consensus.cfg`: **no error, invariants hold** (288361 states,
  depth 20). The brief's no-data-loss gate confirms it end-to-end: `prod_jepsen` (5 nodes, 4 fault
  rounds, SIGKILL + SIGSTOP) — **acked=32 durable=32, ZERO committed-acked entries lost**, ALL 3
  scenarios PASS; `prod_cluster_smoke` ALL PASS (kill+restart catch-up from ProdDisk).

## Honest commit throughput (AFTER == baseline; no path change → no regression, no win)

| config | N | depth | commit_tput (ops/s) | min | max | accept_tput (NOT commit) |
|--------|---|-------|---------------------|-----|-----|--------------------------|
| 1-node | 1 | 1     | 4036                | 3653| 4146| 4037 |
| 1-node | 1 | 16    | 13696               |13654|14505| 13711 |
| 1-node | 1 | 64    | **17245**           |17122|17550| 17270 |
| 3-node | 3 | 1     | 1261                | 1118| 1468| 1278 |
| 3-node | 3 | 16    | 1244 (high var)     | 793 | 3944| 1396 |
| 3-node | 3 | 64    | **23541**           |22444|31988| 90453 |

These match the S8.3 honest-commit baseline within container variance: **the ceiling is
unchanged** (~17k 1-node, ~15–24k 3-node at depth) because the persist path was not touched —
**group commit was already there.** `fsyncs_per_commit` before == after == 0.016 at depth 64
(no per-entry-fsync to remove; it was already removed by the FIFO worker).

## Honest caveats (S8.5)

- **Laptop container, `--cpus=4`, real wall-clock** — relative regression numbers only.
- **The "AFTER" is the unchanged path** — this stage is a PROFILE that *correctly concluded not
  to implement* the proposed optimization, because the optimization already exists. The receipt
  is the fsyncs-per-commit measurement (0.016 at depth) proving the coalescing.
- Profiling counters are prod-only (`ProdDisk`/`lockstepd`); they feed NO ordering and do NOT
  exist in the sim, so determinism + the A-vs-B cross-check are untouched.

---

# S8.7 — POOL the Future SharedState (cut the dominant explicit per-op alloc)

> **OPTIMIZATION (core change), proven no-regression by sim BYTE-IDENTICAL determinism.**
> Touches `core/Future.hpp` + `core/detail/SchedulerSink.hpp` + a new
> `core/detail/SharedStatePool.hpp` — the MOST fundamental async primitive, used by the
> sim Scheduler, the prod ProdReactor, and every consensus/txn/storage/query coroutine.
> The spec is UNCHANGED (no behavior change); the load-bearing proof is that the
> deterministic sim renders BYTE-IDENTICAL traces/committed-logs/fingerprints before vs
> after (a correct memory pool changes only WHERE objects live, never values or order).

## What + why

S8.6 measured ~31 heap allocs per committed op on the single reactor thread. The
tractable, dominant EXPLICIT allocation is `std::make_shared<SharedState<T>>` minted
once per Promise/Future pair (one per disk op, net send, timer, KV op, …). S8.7
RECYCLES that storage with a per-scheduler-sink object pool instead of heap-freeing it.

## The pool (where it lives, the recycle point, the no-UAF argument)

- **Lives in `SchedulerSink`** (the abstract base both `core::Scheduler` and
  `prod::ProdReactor` implement) as a concrete `SharedStatePool` member — so BOTH the
  sim and prod inherit their OWN pool; it is freed with the sink. `make_promise<T>(sink)`
  (the single mint chokepoint, ~40 call sites) routes the SharedState allocation through
  it via `std::allocate_shared<SharedState<T>>(PoolAllocator{&pool}, sink)`.
- **Ownership UNCHANGED.** Still a `std::shared_ptr`; the Promise, the Future, and the
  FutureAwaiter all hold copies exactly as before. The refcount still drives lifetime.
  Only the allocator changed.
- **RECYCLE POINT == last-reference-drop, EXACTLY as today.** `allocate_shared` fuses the
  control block + `SharedState<T>` into ONE block; when the LAST `shared_ptr` reference
  drops, the library destroys the object and calls the pool allocator's `deallocate()`,
  which links the block onto a per-size free list instead of returning it to the OS. So a
  slot is recycled at PRECISELY the instant the old `make_shared` block would have been
  `delete`d: after BOTH the Promise set it AND the awaiter consumed it (whichever
  shared_ptr is last). **No earlier recycle ⇒ no use-after-free, no recycling a slot a
  coroutine still references.** The allocator copy travels with the control block, so a
  block always returns to the pool it came from (no cross-pool corruption). Single-thread
  with the sink (L6): plain vectors, no atomics.
- A dedicated unit test (`shared_state_pool`) locks this: a freed slot IS recycled by the
  next mint, a still-held slot is NEVER aliased, and a set-but-not-consumed state survives
  a dropped Promise (the no-UAF property made executable). ASan/UBSan clean over a
  20k-cycle churn loop.

## BYTE-IDENTICAL determinism (the strongest no-regression proof)

Stash the change, capture the rendered output, restore, capture again, `diff`:

```
runtime_determinism (full event trace incl promise_set/schedule/resume) : IDENTICAL
seed_sweep (multi-seed fingerprints)                                     : IDENTICAL
consensus_crosscheck (A-vs-B committed-log cross-check)                  : IDENTICAL
```

All three diffs are EMPTY — the pool is invisible to output, as a pure memory-reuse
optimization must be (ordering keys are arm/enqueue SEQUENCE numbers, never addresses).

## Safety (the change is used by EVERYTHING)

- **ASan + TSan + UBSan: all 40 ctests PASS under each** (use-after-free / double-free /
  lost-wakeup are the risks; none triggered). Mac `gate.sh` green.
- **Consensus**: A-vs-B cross-check + 5 conformance checkers + N=1 + membership + snapshot
  all PASS, byte-identical. **TLC unaffected** (no spec change).
- **Prod (container, Release)**: `prod_cluster_smoke` **ALL PASS** (replication agreement +
  kill/restart catch-up); `prod_jepsen` **ALL 3 SCENARIOS PASS** — `acked=32 durable=32`,
  ZERO committed-acked entries lost, no split-brain, V-XCHECK order held.
- **clang-tidy clean** on the new pool headers (clang-analyzer owning-memory /
  use-after-free / member-init all pass).

## ALLOC/OP before → after (container, Release, `-DLOCKSTEP_PROFILE_ALLOC`, 1-node depth-16, count=4000)

| | allocs/op | the `<=128` bucket (the SharedState make_shared block) |
|--|-----------|--------------------------------------------------------|
| BEFORE | **31.15** | 13,411 allocs (~3.35/op) |
| AFTER  | **27.89** | **706 allocs (~0.18/op)** |

**−3.26 allocs/op (~10.5%).** The `<=128` bucket — which held the fused
`make_shared<SharedState<T>>` blocks — collapsed from 13,411 to 706 (a drop of 12,705
allocs ≈ **3.18/op**, almost exactly the total −3.26/op gain). That ~12.7k WAS the
SharedState allocation; pooling recycled it away. The 706 still in `<=128` are OTHER
64–128 B allocations on the path (small std::function / string-buffer mints), not
SharedState — SharedState's own contribution to that bucket is now ~0. The remaining ~28
allocs/op live in the `<=32`/`<=64` buckets and ARE the coroutine frames
(compiler-controlled) + small payload strings — the RESIDUAL next cost (secondary; frame
elision is harder and not chased here).

## COMMIT throughput before → after (container, Release, median of 3 fresh passes, count=4000, value=16B)

| config | depth | BEFORE commit_tput | AFTER commit_tput |
|--------|-------|--------------------|-------------------|
| 1-node | 1     | 3795.8             | 3960.6            |
| 1-node | 64    | 16579.7            | **17252.0**       |
| 3-node | 1     | 802.8              | 803.6             |
| 3-node | 64    | 13180.4 (hi var)   | 18191.9 (hi var)  |

**HONEST:** the 1-node depth-64 ceiling moved 16.6k → 17.3k (~+4%), which is WITHIN this
contended container's run-to-run variance band (BEFORE max 17.8k, AFTER min 16.7k). So the
proximate, clean, load-independent WIN is the alloc reduction (−3.3 allocs/op, the
SharedState bucket eliminated); throughput is at-or-slightly-above baseline. Throughput did
not jump because the ~28 residual allocs/op are coroutine frames that still dominate the
per-op reactor CPU (the S8.1 standing finding) — the SharedState alloc was ~10% of the
explicit churn, so removing it is a ~10% explicit-alloc win, not a throughput multiplier.
The ceiling is unchanged-to-slightly-up; the RESIDUAL lever is coroutine-frame elision.

## Honest caveats (S8.7)

- **Laptop container, `--cpus=4`, real wall-clock** — throughput numbers are relative,
  high-variance; the alloc/op COUNT is load-independent and is the firm receipt.
- The pool is **pure memory reuse**: the sim is byte-identical (proven), so the win is
  fewer allocations, not different behavior. Coroutine frames are the next alloc cost and
  are NOT addressed here (compiler-controlled; would need HALO/frame-elision work).

# Phase 9 S9.1 — Multi-shard throughput SCALING (the horizontal lever)

> **The order-of-magnitude lever.** Phase 8 established that a SINGLE single-node Raft
> reactor is bounded by per-op reactor CPU (coroutine frames) — essentially hard for ONE
> group. The only order-of-magnitude lever is HORIZONTAL: run **M fully-independent
> single-node Raft shards** in one process, each on its OWN std::thread (the reactor IS
> that thread), and route load by key-hash to a shard's port. NO cross-shard txns, NO
> shared mutable state on the data path — the clean embarrassingly-parallel first step.

## Design (no shared mutable state on the data path)

- One `lockstepd --shards M --shard-base-port P` spawns **M threads**. Each shard owns its
  OWN `ProdReactor` (own epoll fd) + `ProdNetworkBus` (own listen + admin sockets) +
  `ProdDisk` (`data_dir/shard_<i>/consensus.wal`) + `ProdConsensusNode` (single-node
  cluster `{i+1}`, self-commits via the gated **N=1** path — UNCHANGED consensus surface).
- Shard `i` (0-based): node_id `i+1`, admin port `P+i`, consensus port `P+M+i`,
  dir `data_dir/shard_<i>`. Shards share NOTHING mutable → **no locks on the data path**.
- **Routing is client-side**: `hash(key) % M → shard admin port P+shard`. A request reaches
  a shard on its own port — **no in-process cross-thread request handoff**.
- **Clean lifecycle**: each reactor self-deadlines (`--run-seconds`); the parent **joins
  ALL M threads** unconditionally on every exit path. No detached/leaked threads, no orphans.
- **Why no data race**: the M reactor run loops touch only their own objects. The ONLY
  cross-thread state is read-only startup config + ONE atomic failure counter; shard
  CONSTRUCTION is serialized under a mutex (ProdReactor's ctor reads `getenv` once). The
  thread orchestration lives in `providers/prod/.../ProdShardRunner.hpp` — the prod-provider
  boundary where real threads are sanctioned (the reactor was already the one place real
  threads live). `cli/` stays single-thread (the forbidden-call lint enforces this).

## Scaling curve (FIXED work per shard — the honest unit; aggregate = per_shard × M)

`tests/prod_scale.sh`, driven by `lockstep_admin mbench` (key-routed, per-shard pipelined,
aggregate commit throughput = total accepted / wall until EVERY shard's commit covers its
load). FRESH daemons per pass (empty log → no log-growth confound). Median of 3 passes.

Machine: **nproc=14**, container `--cpus=12 --memory=8g` (Linux `lockstep-dev`). per_shard=8000,
inflight=64, value_bytes=16. `agg_commit_tput` = HONEST aggregate committed ops/s.

| M (shards) | agg_commit_tput (ops/s) | factor vs M=1 |
|-----------:|------------------------:|--------------:|
| 1          | **9 073**               | 1.00×         |
| 2          | **18 231**              | 2.01×         |
| 4          | **34 432**              | 3.80×         |
| 8          | **49 859**              | 5.50×         |
| 12         | **50 062**              | 5.52×         |

**Did it scale?** YES — **near-linear to M≈4** (2.01× / 3.80×), then **flattening to a
~50k ops/s ceiling at M≈8** (the knee). This is the textbook embarrassingly-parallel curve:
linear while spare cores exist, flat once cores + memory bandwidth (and the single-threaded
client, see caveats) saturate. The new **aggregate ceiling ≈ 50k committed ops/s** vs the
~9k single fresh shard on this host — a **~5.5×** real throughput win from sharding alone.

## Correctness + per-shard durability

`tests/prod_shard_smoke.sh` (M=4): every shard independently **commits** a distinct single-key
value (`durable=1`) and **reads it back** from its committed-log digest; then SIGKILL the whole
daemon, **restart on the same data dir**, and every shard **recovers its value from its
ProdDisk** — per-shard durable crash/restart, the smoke pattern applied per shard. PASS.
Even key-routing confirmed: 2000 ops over 4 shards split 501/499/501/499 (hash-uniform).

## Data-race safety (the FIRST real threads — the load-bearing check)

The TSan build (`-DLOCKSTEP_SANITIZER=tsan`) of `lockstepd`/`lockstep_admin` links + runs
the multi-shard daemon under load (4 concurrent shard threads) AND through a clean
self-deadline JOIN of all threads: **NO ThreadSanitizer data race**, `daemon_exit=0` (clean
join), no orphan threads/processes. `clang-tidy` (`concurrency-*` = ERRORS) on the threaded
TUs: **0 errors, 0 concurrency findings**. forbidden-call lint clean (real threads confined
to the `providers/` boundary; `cli/` stays single-thread). Protected dirs
(core/consensus/txn/storage/query/providers-sim) diff EMPTY.

## Honest caveats

- **Laptop container, real wall-clock** — absolute numbers are relative, run-to-run variant;
  only meaningful re-run on the SAME setup. The SCALING FACTOR (relative) is the result.
- **The client is single-threaded**: `mbench` drives all M shard reactors round-robin on ONE
  client thread, so past ~8 shards the CLIENT becomes a bottleneck — part of the flattening
  at M=8/12 is client-side, not a server-shard limit. The server-side per-shard parallelism
  is genuine (each shard is a real independent OS thread). A multi-process / multi-thread
  client would push the measured aggregate higher.
- **per_shard vs fixed-total**: with a FIXED TOTAL split across shards (instead of fixed
  per-shard) the curve looks SUPER-linear at small M, because a lone shard's fsync/commit
  catch-up dominates its wall and sharding parallelizes independent disks. The fixed-per-shard
  table above is the honest "constant work per shard" unit.
- **No cross-shard transactions** (Phase 9 later) and **no cross-process replication** of a
  shard yet — this stage is the pure throughput-scaling proof of independent shards.

---

# Phase 9 S9.2 — async io_uring for the prod IO path (ASYNC fdatasync overlap)

> **PROFILE FIRST, then implement only where the data justifies — honest verdict: KEEP.**
> The premise (from S8.5/S8.6) was that the single-thread ceiling is coroutine-frame CPU,
> NOT IO-blocking, so io_uring buys only a MODEST per-shard gain. We profiled the syscall
> fraction, then converted the ONE high-value, durability-correct, buffer-lifetime-safe
> path — **the durable fdatasync** — to async io_uring, and measured honestly. The gain is
> real and bigger than the raw syscall fraction implied (because async fsync removes the
> reactor STALL inside fdatasync, not just syscall overhead), and it COMPOUNDS across
> shards. **core/sim/consensus/txn/storage/query diff EMPTY** — prod-provider-layer only.

## io_uring availability in the container (STEP 0)

- Kernel **6.10.14-linuxkit** (Docker Desktop LinuxKit VM on an Apple-Silicon Mac host) —
  io_uring fully supported (`io_uring_setup` returns `features=0x7fff`).
- **Docker's DEFAULT seccomp profile BLOCKS io_uring** (`io_uring_setup` → `EPERM`). The
  ring is only usable with **`docker run --security-opt seccomp=unconfined`**. Probed both
  ways: default → EPERM, unconfined → OK.
- **No liburing in the image**, but the kernel header `linux/io_uring.h` IS present. We use
  the **RAW io_uring syscalls** (`io_uring_setup`/`io_uring_enter` + `mmap` of the SQ/CQ
  rings) — zero new dependency, no Dockerfile change. (`providers/prod/ProdUring.hpp`.)
- **GRACEFUL FALLBACK:** if `io_uring_setup` fails (seccomp-blocked / old kernel), the ring
  is `!valid()` and every disk transparently uses the SYNCHRONOUS fdatasync — correctness
  NEVER depends on the ring. The default-seccomp prod gate still passes (sync path).

## The syscall-fraction PROFILE (STEP 1 — the io_uring ceiling)

`strace -f -c -w` (wall-time-IN-syscall, all threads), Release, container, pbench commit
load (count=4000, value=16B). NOTE: strace's ptrace heavily slows the daemon (commit_tput
~5.5k under trace vs ~16k un-traced), so we use it ONLY for the relative syscall MIX. The
`epoll_pwait` time is the reactor BLOCKING IDLE (waiting for work), NOT reclaimable CPU; the
io_uring ceiling is the NON-idle IO-WORK syscall fraction.

| depth | fdatasync | send+recv | epoll_ctl | pwrite | **IO-syscall fraction of busy time** |
|-------|-----------|-----------|-----------|--------|--------------------------------------|
| 1     | ~4.6%     | ~0.7%     | ~0.4%     | ~0.3%  | dominated by the SERIAL per-op chain (1 send + 2 recv + 1 fsync + 1 pwrite) |
| 64    | ~0.2%     | ~0.3%     | ~0.2%     | ~0.3%  | **~1% of busy time** (fsync already coalesced ~68:1, `fsyncs_per_commit=0.016`) |

**Ceiling implied:** at the steady commit ceiling (depth 64) the IO-syscall fraction is ~1%
of busy reactor time — io_uring's syscall-batching can reclaim almost nothing there; the
rest is single-reactor coroutine-frame CPU (the S8.1/S8.6 standing finding), which io_uring
CANNOT touch. At depth 1 the per-op fsync is on the CRITICAL PATH (the reactor BLOCKS inside
fdatasync ~370µs/op), so async overlap can reclaim a real, large fraction there. So the
honest prediction: **small gain at the high-depth ceiling, large gain at low depth/latency.**

## The integration (STEP 2 — what, where, and the load-bearing safety reasoning)

**WHAT:** only the **durable `fdatasync`** is async (`IORING_OP_FSYNC | FSYNC_DATASYNC`).
`pwrite`/`send`/`recv` stay on the synchronous/epoll path. RATIONALE: making THEM async
would hand the kernel a pointer into a CHURNING buffer (the classic io_uring use-after-free)
for a sub-1% syscall-time win — not worth the lifetime risk per the profile. **fsync submits
NO user buffer (just the fd), so V-RKV1 is satisfied VACUOUSLY** — there is no pinned-buffer
hazard at all.

**REACTOR INTEGRATION (alongside epoll, not a replacement):** the io_uring ring fd is itself
epoll-pollable. The reactor (`ProdReactor`) owns ONE ring and registers its fd on the
EXISTING epoll set; on the ring fd's `EPOLLIN` (a CQE is ready) it reaps completions. The
network path stays on epoll untouched; only disk fdatasync flows through the ring. This is
the minimal clean integration — epoll keeps doing what it does well; the ring is one more
additive fd branch.

**DURABILITY BARRIER (the load-bearing invariant):** an async fsync's COMPLETION (its CQE)
IS the barrier. `ProdDisk::sync()` (reactor-bound ctor) submits the async fdatasync and
returns a Future that resolves ONLY when the CQE is reaped (`res >= 0` ⇒ durable; `res < 0`
⇒ IoFault, NEVER a false ack). The FIFO persist worker's `co_await sync()` does not resume —
and the entry is not treated durable — until the CQE arrives. On completion the promise is
resolved, which SCHEDULES the parked coroutine via the SchedulerSink (L1 — never an inline
resume). Single-thread-per-shard: each shard's reactor owns its OWN ring; NO cross-thread
ring sharing.

**GRACEFUL-SHUTDOWN FLUSH (a real finding — see FLAG below):** `ProdConsensusNode`'s dtor
calls `reactor.flush_uring()`, which PUMPS the reactor (so a persist worker that has appended
but not yet submitted its sync gets to run) then BLOCK-DRAINS the ring until no fsync is in
flight — every durably-intended entry's CQE is reaped while the WAL fd is still open. This
restores the exact clean-exit durability synchronous fdatasync gave for free. An ABRUPT crash
(SIGKILL) skips the dtor — its un-completed fsyncs are honestly lost (page-cache only), the
correct crash semantics; jepsen confirms quorum-acked entries are durable regardless.

## HONEST before/after — per-shard commit throughput (STEP 3)

A/B on ONE Release binary via `LOCKSTEP_NO_URING=1` (SYNC "before") vs the ring (URING
"after"); container `--cpus=4`, median of 3 fresh passes, count=4000, value=16B, all
`commit_covered=1`:

| config | depth | BEFORE (sync) | AFTER (io_uring) | gain | note |
|--------|-------|---------------|------------------|------|------|
| 1-node | 1     | 2883          | **15918**        | **~5.5×** | depth-1 is closed-loop LATENCY: async removes the per-op fsync STALL from the critical path |
| 1-node | 16    | 12184         | **17945**        | **~1.47×** | fsync partly overlapped; reactor no longer blocks inside fdatasync |
| 1-node | 64    | 16191         | **18275**        | **~1.13×** | the steady CEILING: small but REAL (slightly above the ~1% syscall fraction because async also frees the reactor from BLOCKING in fdatasync) |

**Honest read:** the high-depth ceiling moves ~16.2k → ~18.3k (**~13%**) — a modest, real
win, exactly the "fsync-overlap" the brief named, and a touch above the raw syscall fraction
because the win is not syscall-batching but **removing the reactor stall inside the blocking
fdatasync** (each sync was ~370µs of dead reactor time). The depth-1/16 gains are larger
because there fsync is on the critical path. `fsyncs_per_commit` drops 0.016 → ~0.008 at
depth (async lets MORE appends pile up per barrier: ~139 appends/fsync vs 68).

### Multi-shard scale (S9.1 re-run) — does the per-shard win COMPOUND? YES

`mbench` key-routed aggregate, container `--cpus=12 --memory=8g`, per-shard 6000, inflight
64, all `all_covered=1 fault=0`:

| M (shards) | BEFORE (sync) agg | AFTER (io_uring) agg | gain |
|-----------:|------------------:|---------------------:|-----:|
| 1          | 12 292            | 12 744               | 1.04× |
| 2          | 23 424            | 26 611               | 1.14× |
| 4          | 42 550            | 49 700               | 1.17× |
| 8          | 55 737            | **65 791**           | **1.18×** |

The per-shard async-fsync gain **compounds across shards** — the M=8 aggregate ceiling moves
~55.7k → ~65.8k commits/s (+18%). **Rings do NOT contend at high M**: each shard owns its
own independent ring (no shared ring), so there is no cross-shard ring contention; TSan is
clean on the 4-shard daemon under real commit load.

## SAFETY re-run (STEP 4 — io_uring touches the durability + completion paths)

- **prod_jepsen** (5 nodes, 4 fault rounds, SIGKILL + SIGSTOP, 3 scenarios, ring ON under
  `seccomp=unconfined`): **ALL 3 PASS** — `acked=32 durable=32`, **ZERO committed-acked
  entries lost**, no split-brain, V-XCHECK order held. The async-fsync barrier holds under
  real fault injection.
- **prod_cluster_smoke**: **ALL PASS** — election → replication AGREEMENT → SIGKILL a
  follower → restart on the SAME data dir → recover from ProdDisk → CATCH-UP → final
  agreement (the async-fsync'd WAL recovers correctly).
- **prod_consensus_test** (durable crash/restart): **ALL PASS** on BOTH the ring path and the
  forced-sync path — appended+ASYNC-synced entries survive a clean teardown + reopen
  byte-identical (the graceful-shutdown flush guarantees the platter has them).
- **prod_uring_test** (new): ring setup/availability, async-fsync CQE completion, and the
  durability barrier through `ProdDisk`'s reactor ctor (append → async sync → crash → reopen
  → synced prefix survives). PASS on the async path AND the forced-sync-fallback path.
- **prod_network_test** (record-replay): PASS (network path untouched).
- **TSan**: 4-shard daemon under real commit load (4 independent rings) — **NO data race**,
  clean thread join, no orphans.
- **ASan/LSan**: uring + disk + consensus tests — **clean** (no buffer UAF, no leak in the
  ring mmap / completion-callback / blocking-drain path).
- **clang-tidy**: clean on the new io_uring headers (checked transitively via the non-provider
  test TU; clang-analyzer / bugprone / concurrency / member-init all pass).
- **forbidden-call lint**: OK (`ProdUring` is under `providers/` = exempt; the test TU does
  NO raw file IO — all real disk IO stays in `providers/prod/ProdDisk`).
- **Mac host build**: green (`prod_disk_test` builds + passes; io_uring is `#ifdef __linux__`
  and the uring/consensus targets are Linux-only-guarded, simply absent on macOS).
- **Protected-dirs diff** (core/sim/consensus/txn/storage/query/providers-sim): **EMPTY.**

## VERDICT: KEEP io_uring (modest-but-real, durability-correct, compounds across shards)

The async fdatasync is a **real, honest win** — ~13% at the single-shard steady ceiling,
~1.5×–5.5× at lower depth/latency, and ~18% at the M=8 aggregate ceiling, with the durability
barrier intact (jepsen `durable=32`, zero loss) and zero buffer-lifetime risk (fsync carries
no user buffer). It is NOT a throughput multiplier at the high-depth ceiling — the profile
correctly predicted the syscall fraction is ~1% there and the residual is coroutine-frame CPU
— but it is comfortably above noise and worth keeping. We did NOT async-ify pwrite/send/recv
(sub-1% syscall-time for a real use-after-free risk) — the data did not justify it.

## FLAG (a real finding worth recording)

**The N=1 self-commit fast-path advances `commit_index` at persist-ENQUEUE time, not at
fsync-COMPLETION time.** In `RaftNodeA::submit()` the lone-leader branch calls
`advance_commit_index()` immediately after `persist_entry()` (which only ENQUEUES the durable
record + sets `want_sync_`), BEFORE the FIFO persist worker issues the fdatasync. With the old
SYNCHRONOUS fsync this was masked (the worker drained inline within the test's pump, so durable
≈ committed). The ASYNC fsync EXPOSED it: an entry could be committed + in `durable_entries()`
yet still only in the page cache if the process exits before the fsync CQE is reaped. This is a
**pre-existing consensus-core property** (commit-before-physical-barrier on the N=1 path), NOT
introduced by S9.2 — and core is FROZEN, so we did NOT change it. We restored clean-exit
durability in the PROD LAYER via the graceful-shutdown ring flush (`flush_uring()` in
`ProdConsensusNode`'s dtor). Quorum-replicated (N≥2) entries are durable via the normal
ack-driven path (jepsen `durable=32`). **Recorded for a later core review** — whether the N=1
self-commit should gate on the sync barrier completing rather than on enqueue.

## Reproduce (S9.2)

```bash
# NOTE the seccomp flag — default Docker seccomp BLOCKS io_uring_setup.
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/work" -w /work \
  --memory=6g --cpus=4 lockstep-dev:latest bash -lc '
  set -e; ulimit -c 0; ulimit -s 16384
  cmake -S . -B build/lrel -GNinja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/lrel -j6 --target lockstepd lockstep_admin lockstep_prod_uring_test
  ./build/lrel/tests/lockstep_prod_uring_test         # async path
  LOCKSTEP_NO_URING=1 ./build/lrel/tests/lockstep_prod_uring_test  # forced sync fallback
  pgrep -x lockstepd && echo LEAKED || echo no-orphans'
```

A/B a daemon on one binary: `LOCKSTEP_NO_URING=1 lockstepd …` (sync) vs `lockstepd …` (ring).

## Honest caveats (S9.2)

- **Laptop container (Docker Desktop LinuxKit VM on Apple Silicon), `--cpus=4/12`, real
  wall-clock** — absolute numbers are RELATIVE regression baselines only; re-run on the SAME
  setup. The relative SYNC-vs-URING gain is the result.
- **io_uring requires `--security-opt seccomp=unconfined` in this container** (default seccomp
  blocks `io_uring_setup`). On the default-seccomp prod gate the ring is unavailable and the
  daemon runs the SYNCHRONOUS fsync path (the "before" numbers) — still correct, just no win.
- **Only fdatasync is async.** pwrite/send/recv stay synchronous/epoll (the profile did not
  justify the buffer-lifetime risk). So this is a fsync-overlap win, NOT a full async-IO rewrite.
- **The high-depth ceiling win (~13%) is modest** and within ~1 variance band of the heaviest
  contended passes — it is real (consistent across 3 passes + compounding across shards) but it
  is NOT a multiplier. The depth-1/low-depth gains are large because fsync is on the critical
  path there.

---

# SQL surface (v2 SELECT) micro-bench — parse / plan / execute BASELINE

> **BASELINE — NOT OPTIMIZED.** A number to *regress against* later. The SQL layer is
> SUGAR over the verified `query::Database` scan/get/range + an in-memory
> filter/group/aggregate/sort pipeline. No core/sim/consensus/txn/storage and no
> `query::Database`/`Query` code was touched for the SQL v2 surface — this measures the
> *new SQL layer's CPU cost as built*, in memory, over the existing read surface.

## What was measured

`bench/sql_bench_main.cpp` (`lockstep_sql_bench_driver`). A FIXED, deterministic amount
of work per query shape over an **N=500-row** table `emp(id INT PK, dept TEXT, sal INT,
age INT)`, in memory (the engine primes a read store; **no disk I/O on the read path**).
So this is the SQL pipeline's CPU cost — tokenize + recursive-descent parse + plan
(PK fast-path vs full-scan decision) + decode rows + filter / group / aggregate / sort /
slice — **NOT** storage throughput (that is the storage / prod sections above).

The driver TU is **wall-clock-free / forbidden-lint clean** (no `std::chrono`): it runs a
fixed iter count per shape and prints a result **checksum** (proves the work is REAL — the
optimizer cannot elide it — and DETERMINISTIC: same build ⇒ same checksum, run to run).
Time is taken EXTERNALLY (`/usr/bin/time -p`, or the per-shape `steady_clock` harness used
to produce the table below). Two runs of the driver print byte-identical output.

## Numbers (Apple M4 Pro, macOS host, -O2 release, in-memory)

Per-op latency = (total wall for `iters` repetitions) / `iters`. Table N = 500 rows.

| query shape                              | iters   | per-op    | what dominates |
|------------------------------------------|---------|-----------|----------------|
| PARSE-only (rich GROUP BY+HAVING+ORDER)  | 100 000 | **~1.1 µs**   | tokenize + recursive descent (no exec) |
| point  — `WHERE id = <pk>` (point get)   | 20 000  | **~2.9 µs**   | parse + a single encoded-key `Query.get` |
| range  — `WHERE id BETWEEN a AND b`      | 2 000   | **~0.46 ms**  | PK range scan + decode ~250 rows |
| filter — full scan + ANY-col predicate   | 2 000   | **~1.05 ms**  | scan + decode 500 rows + per-row predicate eval |
| order  — full scan + ORDER BY + LIMIT    | 1 000   | **~1.10 ms**  | scan/decode 500 + stable_sort + slice |
| groupby— full scan + GROUP BY + 5 aggs   | 1 000   | **~1.06 ms**  | scan/decode 500 + grouped fold (COUNT/SUM/MIN/MAX/AVG) |
| having — GROUP BY + HAVING + ORDER BY    | 1 000   | **~1.05 ms**  | as groupby + HAVING filter + group sort |
| distinct— full scan + DISTINCT + ORDER   | 1 000   | **~1.05 ms**  | scan/decode 500 + de-dup + sort |
| join   — 2-table equi-join (hash join)   | 1 000   | **~1.14 ms**  | scan/decode 500 emp + 5 dpt + build hash index + probe (500 matched rows) |
| joingrp— join + GROUP BY + AVG + ORDER   | 1 000   | **~1.17 ms**  | as join + grouped fold over 5 groups + group sort |

Stable across two runs (variance < ~5%). The point-get is ~3 orders of magnitude cheaper
than a full-scan query: the **PK fast-path** (a single `Query.get` of the order-preserving
encoded key) avoids decoding the whole table, while any full-scan filter/aggregate is
linear in the row count + the decode cost. Parse is ~1 µs and is NOT the bottleneck for any
executing query — the row decode + pipeline dominates.

The **2-table equi-join** (`emp.deptid = dpt.did`, 500 emp × 5 dpt) lands near the other
full-scan shapes (~1.14 ms): it takes the **hash-join** path — scan+decode both tables, build
an ordered hash index on the (tiny) right table's key, probe with each left row. The cost is
still dominated by the same per-row KV decode of the 500-row left table, not the join itself;
the build/probe over 500 + 5 rows is cheap on top. Adding GROUP BY + AVG + ORDER over the 5
joined groups adds little (~+0.03 ms). The join is sugar over the SAME `Query.scan` reads —
the combine is a pure in-memory step (V-RKV1 deterministic; `std::map` index, no rng).

## Honest caveats

- **In-memory, single host, real wall-clock** — absolute µs/ms are RELATIVE regression
  baselines only; re-run on the SAME machine. The *shape* of the result (point ≪ range ≪
  full-scan; parse ≪ exec) is the finding.
- **N = 500 by default**, because building the table is **O(N²)**: the v1 write path
  (`Engine::submit_write`) re-submits the WHOLE accumulated write-log as one batch per
  INSERT (so a read-modify-write body observes prior committed state through the verified
  executor). That is a WRITE-path cost unrelated to the SELECT pipeline measured here —
  FLAGGED, out of scope (Database/Engine write semantics UNCHANGED). A future incremental
  prime would remove it.
- **Full-scan cost is row-decode-bound**, not pipeline-bound: filter/order/groupby/distinct
  over the same 500 rows all land near ~1 ms because each first decodes 500 KV rows back to
  typed tuples. The aggregate fold / sort / de-dup add little on top. This is the obvious
  first optimization target (lazy / projected decode), but this stage is a BASELINE, not an
  optimization.
- **Not an optimization task.** No SQL fast-path beyond the existing PK point/range lowering
  was added; the in-memory pipeline is the straightforward O(rows·predicate) implementation.
