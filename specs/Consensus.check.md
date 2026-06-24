# Consensus.tla — TLC model-check record (Phase 4)

Source of truth for the Lockstep replicated log. The implementation must conform to this
spec; behavior changes require editing the spec and re-running TLC first.

## Exact TLC command

```
cd /Users/npden4ik/Projects/lockstep
java -XX:+UseParallelGC -cp tools/tla/tla2tools.jar tlc2.TLC \
  -workers 4 -config specs/Consensus.cfg specs/Consensus.tla
```

(`-XX:+UseParallelGC` only silences a TLC throughput warning; results are identical without it.)

## Instance (specs/Consensus.cfg)

| Constant   | Value             |
|------------|-------------------|
| Server     | {s1, s2, s3}      |
| Value      | {v1, v2}          |
| Nil        | model value `Nil` |
| Follower / Candidate / Leader | distinct model values |
| MaxTerm    | 3                 |
| MaxLogLen  | 3                 |
| MaxMsgs    | 7                 |

- **CONSTRAINT** `StateConstraint`: `currentTerm[s] <= MaxTerm` ∧ `Len(log[s]) <= MaxLogLen`
  ∧ `Cardinality(messages) <= MaxMsgs` for all servers.
- **SYMMETRY** `Symmetry == Permutations(Server) \cup Permutations(Value)`.
- **SPECIFICATION** `Spec` ( `Init /\ [][Next]_vars` ).
- **INVARIANT** `ElectionSafety`, `LogMatching`, `StateMachineSafety`.
- **PROPERTY** `LeaderAppendOnlyProp == [][LeaderAppendOnly]_vars` (the action invariant,
  checked as a temporal property).

## Result (reproduced twice, identical)

```
Model checking completed. No error has been found.
3130657 states generated, 210329 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 20.
Finished in 09s.
```

- All four safety properties hold: **ElectionSafety, LogMatching, StateMachineSafety,
  LeaderAppendOnly.**
- `0 states left on queue` = the bounded model was **exhaustively explored**.
- **No deadlock reported.** TLC's default deadlock check is on; there is no genuine
  liveness hole. The model reaches deep replicated/committed states (depth 20); terminal
  states under the message/term/log caps are artifacts of the bound (no new message can be
  added once `Cardinality(messages) = MaxMsgs` and no term/log growth remains), not a
  protocol stall. Deadlock checking was left ENABLED.

## N=1 self-commit confirmation (specs/Consensus1.cfg)

`AdvanceCommitIndex(s)` is a standalone, always-enabled action: `commitIndex` advances the
instant a Quorum stores a current-term entry — and for a single server the Quorum is the lone
leader itself. So the spec ALREADY permits a 1-node leader to self-commit with no peer ack.
`specs/Consensus1.cfg` is the 3-server `.cfg` with `Server = {s1}` (everything else identical),
confirming the four safety invariants still hold when the cluster is a single server. This is
the spec-side proof that the C++ N=1 self-commit fix (call `advance_commit_index()` after the
lone leader's own append; a lone candidate self-elects on its own vote) is safe.

```
scripts/tlc.sh -config specs/Consensus1.cfg specs/Consensus.tla   (TLC_WORKERS=2 TLC_XMX=4g)

Model checking completed. No error has been found.
154 states generated, 79 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 9.
```

- All four safety properties hold at N=1: **ElectionSafety, LogMatching, StateMachineSafety,
  LeaderAppendOnly.** The reachable graph shows `Timeout → BecomeLeader → ClientRequest⁺ →
  AdvanceCommitIndex` taking `commitIndex[s1]` from 0 to `Len(log[s1])` with `messages = {}`
  throughout — the lone leader commits its own log with NO peer exchange.
- `CHECK_DEADLOCK FALSE` for this config ONLY: a single peerless node legitimately QUIESCES
  once it has self-committed up to the `MaxTerm`/`MaxLogLen` bounds (no peer traffic can
  generate a successor). That terminal state is expected, not a protocol stall; the 3-server
  config keeps deadlock checking ENABLED (its perpetual message churn never terminates within
  the bound). Both runs reproduce identically (`0 states left on queue` = exhaustive).

## Modeling decisions

- **`messages` is a set** (monotonically growing); re-delivery is idempotent. To keep the
  reachable state space finite and fully explorable, `StateConstraint` caps the in-flight
  set at `MaxMsgs = 7`. This still covers reorder / duplicate / partition scenarios within
  the bound; it only limits how many *distinct* messages are simultaneously in flight.
  (MaxMsgs=5 was complete but shallow; MaxMsgs=10 exploded to >3.5M distinct states without
  closing quickly. 7 gives depth-20 coverage — elections, multi-entry replication, current-
  term commits, and stale-leader truncation — and closes in ~9 s.)
- **Separate `UpdateTerm` step-down action.** A server that observes a strictly higher term
  on any message first steps down to Follower (adopt term, clear vote) in its *own* atomic
  step. Only afterward, as a Follower, does it process the AppendEntries that may truncate
  its log. This is what keeps `LeaderAppendOnly` true (see bug #1 below): a Leader never
  mutates its log; truncation only happens once it is already a Follower.
- **`votedFor` persisted in the vote step.** `HandleRequestVote` writes `votedFor` in the
  same atomic action that emits the response — models the Raft "persist vote before reply"
  requirement (`IDisk`).
- **Raft conflict rule, not blind overwrite.** `HandleAppendEntries` deletes existing
  entries only from the *first conflicting index* (same index, different term) and appends
  the incoming suffix; a stale/short re-delivered AppendEntries is an idempotent no-op and
  never erases entries the follower already holds (see bug #2 below).
- **Commitment rule.** `AdvanceCommitIndex` advances `commitIndex` only when a Quorum stores
  the entry AND that entry's term equals the leader's current term — the Raft rule that
  makes `StateMachineSafety` hold; older-term entries are never committed by replication
  count alone.

## Invariant violations caught during development (the spec caught real protocol bugs)

1. **`LeaderAppendOnly` violated** — a stale Leader (term 1) received a current/newer
   AppendEntries and the handler truncated its log *while it was still Leader in the
   pre-state*. FIX (action, not invariant): added the separate `UpdateTerm` step-down so a
   server adopts the higher term and becomes Follower in one step, and `HandleAppendEntries`
   now only fires when `m.mterm = currentTerm[s]` — so any log truncation happens as a
   Follower, never as a Leader.
2. **`StateMachineSafety` evaluation failed (commitIndex past log end)** — a re-delivered
   stale AppendEntries (`prevLogIndex=0, entries=<<>>`) truncated a follower's whole log back
   to empty while its `commitIndex` stayed at 1, so `log[s][1]` was out of range. FIX
   (action, not invariant): replaced the blind `prefix \o entries` rewrite with the proper
   Raft conflict rule — only truncate at the first genuinely conflicting entry; subsumed /
   matching entries are kept, making re-delivery idempotent and never dropping committed
   entries.

Both fixes were to the ACTIONS. The four invariants are **unchanged from the skeleton**
(ElectionSafety, LogMatching, StateMachineSafety, LeaderAppendOnly — verbatim).

## S8.2b — bounded-batch AppendEntries (spec-before-code refinement)

The impls (RaftNodeA/B) cap each `AppendEntries` to at most `kMaxBatch` (=64) entries
instead of shipping the whole unacked suffix, so a pipelined client burst no longer builds
an O(backlog) per-send payload that blocks the single reactor coroutine and starves the
heartbeat timer (the S8.2a-diagnosed 3-node collapse). For the spec to PERMIT what the code
does (spec-before-code), `AppendEntries(s, d)` was generalized so `entries` is now **any
prefix of the suffix** after `prevLogIndex` — `SubSeq(log[s], prevLogIndex+1, lastIndex)`
for a non-deterministically chosen `lastIndex \in prevLogIndex .. Len(log[s])` — rather than
forced to the whole suffix (`lastIndex = Len`). A bounded batch is the concrete point
`lastIndex = Min(Len(log), prevLogIndex + kMaxBatch)`.

This is a STRICT GENERALIZATION (it only ADDS reachable behaviors; the old whole-suffix
send is still reachable at `lastIndex = Len`), so **no invariant is weakened** — Log
Matching and the `leaderCommit = Max(commitIndex, Min(mleaderCommit, lastNew))` adoption are
sound for ANY contiguous `[prevLogIndex+1 .. lastIndex]` range (exactly Raft Fig.2, which
always carries a chosen range). TLC re-checked the SAME four invariants over the larger
state space and found **no error** (288,361 distinct states; depth 20; the
`MaxMsgs`/`MaxLogLen`/`MaxTerm` bounds keep the extra `\E lastIndex` branching finite). The
change is to the ACTION only; the four invariants remain verbatim.

## S9.2 — N=1 commit-follows-fsync durability fix (spec already permits; more conservative)

The 1-node (peerless) self-commit fast path advanced `commitIndex` at persist-ENQUEUE
time (right after `submit()` handed the entry to the FIFO persist worker), BEFORE that
entry's `fdatasync` completed — so a lone leader could mark an entry committed while it
was still page-cache-only; an abrupt crash (SIGKILL / power loss) in the enqueue→fsync
window lost a COMMITTED entry. Synchronous fsync masked the sub-ms gap; async io_uring
fsync (S9.2) widened + exposed it. N>=2 is durable via QUORUM acks (a follower acks only
after IT persists), so the gap was N=1-ONLY.

FIX (both impls, gated on `quorum()==1`, a strict no-op for N>=2): the lone-leader
self-commit now fires at the persist worker's POST-SYNC point — once `sync()` COMPLETES
for the just-persisted prefix — not at enqueue. `advance_commit_index()` logic is
UNCHANGED; only WHERE/WHEN the N=1 path calls it moved. N>=2 stays ack-driven, BYTE-
IDENTICAL (proven: the A-vs-B cross-check + 5 conformance checkers at N=3/5 are byte-
identical before/after; full output diff EMPTY).

SPEC CONFORMANCE (no change needed). `AdvanceCommitIndex(s)` is a standalone enabled
action: it MAY fire any time a Quorum (for N=1, the lone leader itself) stores a current-
term entry, with no ordering constraint relative to other actions. The fix only DELAYS
the concrete commit point to fsync completion — a SUBSET of the spec-permitted firing
points (it never commits anything the spec forbids; commit still requires the entry on a
quorum). It is strictly MORE conservative (commit-after-durable), so no invariant is
weakened. TLC re-checked BOTH configs with no error:
  - `Consensus1.cfg` (N=1): 79 distinct states, depth 9 — the four safety invariants hold.
  - `Consensus.cfg`  (N=3): 288,361 distinct states, depth 20 — no error.
The four invariants (ElectionSafety, LogMatching, StateMachineSafety, LeaderAppendOnly)
remain verbatim. The prod teeth (`prod_consensus_durability_teeth_test`, io_uring +
sync fallback) prove commit-follows-fsync: an appended-but-un-fsynced entry has
`commit_index == 0`; after the fsync completes, `commit_index == 1` — no committed entry
can be lost to an abrupt crash. (Pre-fix the teeth FAIL: the un-fsynced entry shows
`commit_index == 1` — a committed-yet-un-durable entry.)
