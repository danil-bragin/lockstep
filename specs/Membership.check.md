# Membership.tla — TLC model-check record (Phase 4, C4.2 membership change)

Source of truth for Lockstep's **single-server membership change**. Adding/removing servers
from the consensus group must never violate election safety: during a config change two
DISJOINT majorities (one in the old config C_old, one in the new config C_new) must not each
elect a leader in the same term → split brain. The implementation must conform to this spec;
behavior changes require editing the spec and re-running TLC first.

## Exact TLC command (bounded wrapper — never a bare `java`)

```
cd /Users/npden4ik/Projects/lockstep
scripts/tlc.sh -deadlock -config specs/Membership.cfg specs/Membership.tla
```

- `scripts/tlc.sh` caps heap (`-Xmx`), puts the metadir/scratch in `/tmp` (auto-removed on
  exit, so it NEVER writes `states/` into the repo), and bounds workers. A bare TLC run once
  dumped ~15 GB and froze the host; this wrapper is the only sanctioned way to run.
- `-deadlock` disables deadlock checking. Terminal states here are pure search-bound
  artifacts (every candidate has reached `MaxTerm` with split votes and the chain is at
  `MaxConfigs`, so no action is enabled). They are not a protocol stall, exactly as in
  Consensus.check.md. Safety checking is unaffected.

## Model (single-server change)

A FOCUSED model — configs + elections, NOT full log replication (replication is verified
separately in Consensus.tla). The configuration history is a single GLOBAL, totally-ordered
**chain** `configs[1], configs[2], …`, each consecutive pair differing by ≤ `MaxChangeDelta`
servers. Each server holds `cfgIdx[s]` = the latest config index it has adopted (configs live
in the log; a server uses the newest config it has seen — Raft §4.1), and only ever moves
FORWARD along the chain. This chain abstraction is faithful to Raft (config entries append to
ONE log and propagate forward) and forbids the physically-impossible divergence a free
"config is any subset per server" model would admit.

STATE: `currentTerm`, `state` (Follower/Candidate/Leader), `votedFor`, `votesGranted` per
server; the global `configs` chain; per-server `cfgIdx`.

ACTIONS:
- `Timeout` — a current member becomes Candidate, bumps term, self-votes.
- `RequestVote(v,c)` — member `v` grants iff `c`'s term ≥ `v`'s, `v` hasn't already voted away
  this term, **and `cfgIdx[c] ≥ cfgIdx[v]`** (the config-log up-to-date rule — see below).
- `BecomeLeader` — wins on a quorum of EVERY live config: its own config, plus the preceding
  chain config while the transition into it is still straddling (joint-consensus window).
- `ProposeConfigChange` — the **config-change action**: the current-term leader appends ONE
  new config (delta ≤ `MaxChangeDelta`) to the chain, but only when the chain is `Settled`
  (previous change fully propagated) and it holds a live quorum and the max term in its config.
- `AdoptConfig` — any server catches up one step toward the head (async rollout → straddle).
- `StepDown` — adopt a higher term seen anywhere, revert to Follower (mirrors Consensus
  `UpdateTerm`); only the highest-term leader persists.

### The overlap rule that MAKES single-server change safe

Two consecutive chain configs differ by ≤ 1 server, so **any majority of one and any majority
of the other intersect** (`QuorumOverlap`). The discipline that keeps only ADJACENT configs
simultaneously live — hence keeps overlap true — is:
1. **delta ≤ 1 per step** (`ProposeConfigChange` / `TypeOK` chain invariant);
2. **commit-before-next**: a new change may only start from a `Settled` chain (the previous
   one fully propagated). Without this, two single-server changes compose into a net 2-server
   jump whose majorities can be disjoint (Ongaro §4.2.3);
3. **config-log up-to-date voting**: a voter refuses a candidate whose `cfgIdx` is behind its
   own. This is the analogue of Raft's log up-to-date check restricted to config entries — and
   it is ESSENTIAL: it stops a server stranded on a stale, superseded config (e.g. a removed
   server that never learned it was removed) from collecting a quorum under that old config and
   electing a second leader.

## Instance (specs/Membership.cfg)

| Constant       | Value             |
|----------------|-------------------|
| Server         | {s1, s2, s3, s4}  |
| Nil            | model value `Nil` |
| Follower / Candidate / Leader | distinct model values |
| MaxTerm        | 3                 |
| MaxConfigs     | 3 (chain ≤ 3 configs) |
| MaxChangeDelta | 1 (single-server change) |
| InitConfig     | {s1, s2, s3}      |

- Base cluster {s1,s2,s3}; s4 is the add/remove candidate. Chain can e.g. grow
  `{s1,s2,s3} → {s1,s3} → {s1,s3,s4}` (remove one, add one), each step delta 1.
- **CONSTRAINT** `StateConstraint`: `currentTerm[s] ≤ MaxTerm` ∧ `Len(configs) ≤ MaxConfigs`.
- **SPECIFICATION** `Spec` (`Init /\ [][Next]_vars`).
- **INVARIANT** `TypeOK`, `ElectionSafety`, `QuorumOverlap`, `ConfigChangeSafety`.
- **No SYMMETRY**: `InitConfig = {s1,s2,s3}` singles out s4, so server ids are NOT
  interchangeable — `Permutations(Server)` would be UNSOUND. The instance is small enough to
  explore exhaustively without symmetry reduction.

## Result (reproduced twice, identical)

```
Model checking completed. No error has been found.
12183067 states generated, 1669158 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 25.
Finished in 29s.
```

- All four invariants hold: **TypeOK, ElectionSafety, QuorumOverlap, ConfigChangeSafety.**
- `0 states left on queue` = the bounded model was **exhaustively explored**.

### Safety invariants (the deliverable) — all hold

- **ElectionSafety** — at most one leader per term (preserved across the change).
- **QuorumOverlap** — for every adjacent chain pair `configs[i] / configs[i+1]`, every majority
  of one intersects every majority of the other. This is the property that MAKES single-server
  change safe.
- **ConfigChangeSafety** — the headline C4.2 property: across any reachable state during/after a
  membership change, no two servers are leaders in the same term (no two disjoint old/new
  majorities both elect).
- **TypeOK** — includes the single-server CHAIN invariant: every consecutive pair of chain
  configs differs by ≤ `MaxChangeDelta`, making the "configs form a single-server chain"
  assumption an explicitly-checked fact rather than just an action side effect.

### Not vacuous — the dangerous straddle IS reachable

A witness check (a temporary invariant `~(Len(configs) ≥ 2 ∧ ∃ leader at head ∧ ∃ server still
lagging behind)`) was **violated** under the safe instance — i.e. TLC reaches states with a
real multi-config chain, an elected leader on the head config, and other servers still on an
older config (the old/new straddle). Safety holds *across* that straddle; the property is not
trivially true because the unsafe interleaving is never explored.

## TEETH — a 2-server jump DOES violate overlap (specs/Membership2.cfg)

```
scripts/tlc.sh -deadlock -config specs/Membership2.cfg specs/Membership.tla
→ Error: Invariant QuorumOverlap is violated.
```

`Membership2.cfg` is identical except `MaxChangeDelta = 2` (a deliberately-WRONG 2-at-once
change). TLC immediately finds a chain step between two configs whose majorities are DISJOINT,
violating `QuorumOverlap`. This proves the checker has teeth: the single-server instance is not
vacuously safe — relaxing the change rule to two servers at once breaks the very property the
spec exists to guarantee.

## Invariant violations caught during development (the spec caught real protocol bugs)

The deliverable invariants were UNCHANGED throughout; every fix was to an ACTION / the change
RULE, never to the safety property (per the brief: a failing safety invariant means the change
rule is wrong). The chain of real membership bugs the checker exposed:

1. **`ElectionSafety` — composed 2-server jump.** Two back-to-back single-server changes
   (`{s1,s2,s3} → {s1,s3} → {s1,s3,s4}`) composed into a net 2-server change; the original and
   final configs had DISJOINT majorities (`{s1,s2}` vs `{s3,s4}`), and two leaders emerged in
   one term. FIX (action): `ProposeConfigChange` may only start from a `Settled` chain
   (commit-before-next) and the leader must hold a live quorum of its config —
   `LeaderQuorumValid` — so a leader that lost the cluster cannot keep mutating membership.

2. **`ElectionSafety` — stale lower-term leader drove changes.** A term-1 leader kept driving
   config changes while a term-2 leader already existed, tying two delta-2-separated configs to
   the same term. FIX (action): `ProposeConfigChange` requires the proposer to hold the MAXIMUM
   term among its config's members (it is genuinely the current leader), plus the `StepDown`
   action so a server adopts any higher term and reverts to Follower.

3. **`ElectionSafety` — removed server stranded on a stale config self-elected.** A server
   removed by a change never learned it was out, stayed on the superseded config two
   generations back, and collected a quorum there to become a SECOND leader in a term already
   won under the current config. FIX (action): the **config-log up-to-date rule** in
   `RequestVote` — a voter refuses any candidate whose `cfgIdx` is behind its own. A lagging
   candidate is then refused by every server that moved ahead and can never out-vote the
   up-to-date cluster. (This is the abstraction-faithful stand-in for Raft's log up-to-date
   check, which this focused model would otherwise lack.)

4. **Modeling-soundness fix (not an invariant violation).** The first model let each server
   hold an ARBITRARY subset as its config, which TLC drove into physically-impossible
   divergence (e.g. `config[s1] = {s4}`) and a 100 M+ state blowup. Replaced with the global
   single-server **chain** + per-server `cfgIdx` (forward-only). This is both faithful to Raft
   (one log, config entries propagate forward) and keeps the state space exhaustively
   checkable (1.67 M distinct states, 29 s).

## Modeling decisions

- **Global config chain, not free per-server configs.** Faithful to Raft and bounded.
- **`-deadlock` disabled.** Terminal states are search-bound artifacts (all at `MaxTerm`,
  chain at `MaxConfigs`), not protocol stalls — same rationale as Consensus.check.md.
- **No symmetry** (InitConfig breaks server-id interchangeability — see above).
- **Joint-consensus election window.** During a straddle a candidate must win a majority of
  BOTH the head config and the preceding one; single-server overlap makes that a single,
  shared, non-disjoint constraint.
- **Two cfgs share one .tla.** `Membership.cfg` (delta 1, SAFE) and `Membership2.cfg`
  (delta 2, TEETH / expected violation) drive the same module via `MaxChangeDelta`.
