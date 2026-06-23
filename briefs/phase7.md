# Lockstep — Phase 7 Brief: Prod Boundary (real transport + server)

> Source of truth: master-plan boundary-provider model + cardinal rule 1 (all
> nondeterminism behind providers). Phases 0–6 + core debts merged; the verified
> CORE is done. Phase 7 makes it RUN as a real distributed process WITHOUT touching
> the sim-proven core.
> Decisions locked (2026-06-24): record-replay bridge IN; Linux-first, correctness-only.
> RESOURCE DISCIPLINE (a freeze happened): heavy/gate agents ONE AT A TIME;
> ulimit -c 0 / -s 16384; -j6; in-gate sweeps bounded; stray *.mutbak = restore first.

## Thesis
The core is verified on sim providers. Phase 7 adds only the UNTRUSTED EDGE:
prod impls of the boundary providers (Clock/Disk/Network/Random) behind the SAME
interfaces, plus a real server daemon. The sim-proven logic rides real hardware
with NO core rewrite — that is the whole point of the boundary architecture.

## Verification philosophy (the hard part — read first)
Real providers introduce real nondeterminism; you cannot model-check epoll. So the
prod edge gets its OWN safety net, not the core's:
- **V-PROD-CONTRACT**: every prod provider passes the SAME contract-conformance
  suite as its sim twin. sim is the reference oracle for the contract; sim↔prod is
  differential. A prod provider that diverges from the contract fails here.
- **V-PROD-CLUSTER**: a REAL multi-process cluster, under REAL faults, has its
  observed history checked strict-serializable by an external linearizability
  checker — the prod analogue of the sim fault-storm oracle.
- **V-PROD-REPLAY**: a prod node records all boundary IO to a trace; any prod
  incident replays byte-identical in sim. Keeps debuggability on prod.
- **V-LINT-INVERT**: `providers/prod/` is the ONE place real syscalls
  (socket/fsync/clock/entropy) are allowed; forbidden_calls lint exempts it exactly
  like `providers/sim/`. Everything else stays pure. Confirm lint config covers prod.

## Stages (ordered; each through the universal gate)

### S1 — Provider contract + conformance harness  [DISPATCH FIRST — foundation]
Extract the boundary-provider interface contracts (Clock/Disk/Network/Random) into
a reusable conformance suite any impl MUST pass. Run vs sim first (validates the
harness — sim already conforms), then it becomes the gate for every prod provider in
S2–S4. Contract examples: Disk = write/read/fsync durability + torn-tail-on-crash +
NO lying fsync; Network = ordered-per-connection send/recv, connect/close,
partition; Clock = monotonic non-decreasing + wall. This is the strap everything
else hangs from — build it well.

### S2 — Prod Clock + Random  (+ record-replay scaffolding)
Real monotonic+wall clock behind the Clock provider; real entropy seed for Random.
Determinism guard: txn execution stays a pure fn of its reads — Random feeds only
non-replay paths (election jitter, backoff). Stand up the record-replay TRACE format
here (the first providers to record); a recorded Clock/Random trace replays
identically in sim. Passes S1 conformance.

### S3 — Prod Disk
Real file IO behind the Disk provider: pwrite/pread, fsync/fdatasync, directory
fsync, atomic rename for the manifest. MUST honor the torn-write / no-lying-fsync
contract that WAL + recovery assume — this is where real durability bugs live.
Verify with crash-injection (kill mid-write) + fsync-ordering tests + a recovery
replay that matches the sim WAL semantics. Records to the replay trace. Passes S1.

### S4 — Prod Network
Real TCP behind the Network provider: length-framed messages, async IO via epoll
(Linux), connection management + reconnect, backpressure. send/recv semantics match
sim Network (ordered per connection). Wire the existing C6.3 wire protocol to a real
socket. Records to the replay trace. Passes S1 + an integration test (two real
processes exchange messages, survive partition/reconnect).

### S5 — Server daemon  `lockstepd`
A real process: parse config (node id, peer list, data dir, listen addr), bootstrap
or join a cluster, listen on a socket, run the consensus + txn + storage stack on
PROD providers, serve the wire protocol. Clean startup/shutdown, crash recovery from
the prod Disk. Deliverable: N real OS processes form a real Raft cluster, commit
txns, survive a process restart. The CLI/driver (C6.4/C6.5) now talks to a real
lockstepd over TCP instead of the in-process LocalCluster.

### S6 — Real-cluster verification (Jepsen-style)  [CAPSTONE]
Spin up a real multi-process lockstepd cluster; real clients submit txns; inject
REAL faults — `kill -9`, iptables/nftables partition, clock skew, disk-full. Record
the observed client history and check strict-serializability with an external
linearizability checker (Elle/Knossos-style). This proves the prod providers did not
break the invariants sim proved. Reuse the txn Oracle's notion of correctness; the
checker is the external judge. Seed/scenario-driven, bounded, must terminate.

### S7 — Perf baseline
Use bench/. Measure throughput + latency of the real cluster (single-node and
3-node, read/write mix). Establish a recorded BASELINE. No optimization yet —
just a number to regress against later.

## Out of scope (explicit — deferred to Phase 8+)
auth / TLS / RBAC · SQL surface · backup/restore tooling · k8s operators /
orchestration · multi-region · perf optimization (S7 only baselines).

## Invariants / contracts
- V-PROD-CONTRACT, V-PROD-CLUSTER, V-PROD-REPLAY, V-LINT-INVERT (above).
- Core code under core/ consensus/ txn/ storage/ query/ stays UNCHANGED — Phase 7
  adds files under providers/prod/ + a server target, and wiring only. If a core
  file must change to support a prod provider, that is a design smell → flag it.
- Determinism of the core is untouched: same seed in sim still byte-identical.
- The universal gate (compile + ASan/TSan/UBSan/MSan + lint + clang-tidy +
  scan-build + ctest + mutation + TLC) applies to every Phase 7 landing.
