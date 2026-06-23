---------------------------- MODULE Snapshot ----------------------------
\* Lockstep — Phase 4 spec C4.3: LOG SNAPSHOTTING / LOG COMPACTION.
\*
\* Purpose: the model-checked source of truth for snapshot / compaction safety on top of the
\* Raft replicated log (specs/Consensus.tla). A server periodically snapshots its applied
\* state-machine state through some index i, then DISCARDS the log prefix <= i (to bound log
\* growth); a follower that has fallen behind the leader's discarded prefix is caught up via
\* InstallSnapshot. The danger this spec rules out: compaction or snapshot install must NOT
\* lose or corrupt committed state — the state-machine value a server holds after compaction
\* must equal what applying the FULL committed log from the start would give.
\*
\* MODELING CHOICE (focused model). We do NOT re-derive Raft leader election / replication here
\* (Consensus.tla already model-checks that). Instead we take the consensus output as given: a
\* single, growing, canonical COMMITTED LOG (`committed`) — the agreed sequence of committed
\* entries that every server's log is a prefix-consistent view of. This is exactly the
\* StateMachineSafety guarantee Consensus.tla proves (committed entries never diverge or
\* reorder). On top of that oracle we model per-server log compaction + snapshot install and
\* check that those operations preserve the applied state-machine value.
\*
\* `committed` is the ORACLE: it is never discarded, so the invariants can always compare a
\* server's (snapshot.state + remaining log) reconstruction against folding the WHOLE committed
\* log. Each server's own `log` is the (possibly compacted) suffix it physically retains.
\*
\* How to model-check (TLC): bounded wrapper only —
\*   cd /Users/npden4ik/Projects/lockstep && scripts/tlc.sh -config specs/Snapshot.cfg specs/Snapshot.tla

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Server,     \* set of server ids
          Key,        \* set of state-machine keys
          Value,      \* set of write values
          Nil         \* "no value" marker for an unwritten key

\* Search bounds (used by StateConstraint; supplied in the .cfg).
CONSTANTS MaxLogLen   \* cap on the canonical committed-log length

----------------------------------------------------------------------------
VARIABLES
    committed,    \* Seq(entry)               the canonical committed log (consensus oracle; grows only)
    log,          \* [Server -> Seq(entry)]   the suffix each server physically retains (after compaction)
    logBase,      \* [Server -> Nat]          absolute index just BEFORE log[s][1]  (= snapshot.lastIncludedIndex)
    commitIndex,  \* [Server -> Nat]          how far this server has learned the committed prefix
    appliedIndex, \* [Server -> Nat]          absolute index of the last entry folded into `applied`
    applied,      \* [Server -> [Key -> Value \cup {Nil}]]   the live state-machine map
    snapshot      \* [Server -> [ lastIncludedIndex : Nat, state : [Key -> Value \cup {Nil}] ]]

vars == <<committed, log, logBase, commitIndex, appliedIndex, applied, snapshot>>

\* A committed entry is a single-key write.  entry == [ key |-> Key, value |-> Value ].

----------------------------------------------------------------------------
\* Helpers

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

EmptyState == [k \in Key |-> Nil]

\* Apply one entry e to a state-machine map st (a single-key overwrite).
ApplyEntry(st, e) == [st EXCEPT ![e.key] = e.value]

\* Fold a sequence of entries `seq` over a starting state `st`, left to right.
RECURSIVE FoldFrom(_, _, _)
FoldFrom(st, seq, i) ==
    IF i > Len(seq) THEN st
    ELSE FoldFrom(ApplyEntry(st, seq[i]), seq, i + 1)

\* The state-machine value produced by applying the first n entries of the canonical committed
\* log, from the empty state.  This is the GROUND TRUTH the invariants compare against.
FoldCommittedPrefix(n) == FoldFrom(EmptyState, SubSeq(committed, 1, n), 1)

\* A server's PHYSICAL view of absolute committed index i: snapshot folds [1..logBase], then the
\* retained log carries (logBase+1 .. logBase+Len(log)).  Reconstruct the state through abs index n
\* from what the server physically holds: snapshot.state THEN the retained-log entries up to n.
\*   requires logBase[s] <= n  (n is at/after the snapshot point) — the only case used below.
ReconstructUpTo(s, n) ==
    FoldFrom(snapshot[s].state, SubSeq(log[s], 1, n - logBase[s]), 1)

----------------------------------------------------------------------------
Init ==
    /\ committed    = << >>
    /\ log          = [s \in Server |-> << >>]
    /\ logBase      = [s \in Server |-> 0]
    /\ commitIndex  = [s \in Server |-> 0]
    /\ appliedIndex = [s \in Server |-> 0]
    /\ applied      = [s \in Server |-> EmptyState]
    /\ snapshot     = [s \in Server |-> [ lastIncludedIndex |-> 0, state |-> EmptyState ]]

----------------------------------------------------------------------------
\* Actions

\* CONSENSUS OUTPUT (oracle growth): a new entry is committed into the canonical log.
\* This stands in for "Phase-4 Raft committed an entry on a quorum"; Consensus.tla proves the
\* committed log never diverges/reorders, so here it is a single monotone sequence.
CommitEntry(k, v) ==
    /\ Len(committed) < MaxLogLen
    /\ committed' = Append(committed, [key |-> k, value |-> v])
    /\ UNCHANGED <<log, logBase, commitIndex, appliedIndex, applied, snapshot>>

\* Normal AppendEntries-style replication of an already-committed entry to server s.
\* The server learns the next committed entry (abs index commitIndex[s]+1) and appends it to its
\* retained log.  It only ever appends entries strictly AFTER its snapshot point (logBase), so the
\* retained log is always the contiguous suffix (logBase+1 ..).  Prefix-consistent by construction:
\* it copies the canonical committed entry verbatim.
AppendCommitted(s) ==
    /\ commitIndex[s] < Len(committed)
    /\ LET nextIdx == commitIndex[s] + 1 IN
         /\ nextIdx = logBase[s] + Len(log[s]) + 1     \* contiguous: append right after retained tail
         /\ log'         = [log         EXCEPT ![s] = Append(@, committed[nextIdx])]
         /\ commitIndex' = [commitIndex EXCEPT ![s] = nextIdx]
    /\ UNCHANGED <<committed, logBase, appliedIndex, applied, snapshot>>

\* Apply: advance the applied state-machine state by folding in the next committed-and-retained
\* entry (abs index appliedIndex[s]+1).  The entry must be present in the retained log (above the
\* snapshot point) and already committed on this server.
Apply(s) ==
    /\ appliedIndex[s] < commitIndex[s]
    /\ LET nextIdx == appliedIndex[s] + 1 IN
         /\ nextIdx > logBase[s]                       \* it survived compaction (still in retained log)
         /\ nextIdx <= logBase[s] + Len(log[s])
         /\ applied'      = [applied      EXCEPT ![s] = ApplyEntry(@, log[s][nextIdx - logBase[s]])]
         /\ appliedIndex' = [appliedIndex EXCEPT ![s] = nextIdx]
    /\ UNCHANGED <<committed, log, logBase, commitIndex, snapshot>>

\* TakeSnapshot: capture the APPLIED state through some index i, set the snapshot, and DISCARD the
\* log prefix <= i.  CRITICAL SAFETY: we may only snapshot through an index we have ALREADY applied
\* (i <= appliedIndex[s]) — otherwise the discarded prefix's writes would be lost (never folded into
\* snapshot.state).  `applied` at this point already folds in [1..appliedIndex], so for i <=
\* appliedIndex the snapshot.state for index i is the reconstruction through i.  We take the
\* snapshot exactly at appliedIndex (the freshest fully-applied point) to keep the model small while
\* still exercising real compaction.
TakeSnapshot(s) ==
    /\ appliedIndex[s] > logBase[s]                    \* there is an un-snapshotted applied prefix to compact
    /\ LET i == appliedIndex[s] IN
         \* snapshot.state = state after applying [1..i].  Because applied already = fold[1..i] and
         \* applied = snapshot.state folded over retained [logBase+1..i], this equals ReconstructUpTo(s,i).
         /\ snapshot' = [snapshot EXCEPT ![s] = [ lastIncludedIndex |-> i,
                                                   state             |-> ReconstructUpTo(s, i) ]]
         \* discard the log prefix <= i: keep only entries with abs index > i.
         /\ log'      = [log     EXCEPT ![s] = SubSeq(@, i - logBase[s] + 1, Len(@))]
         /\ logBase'  = [logBase EXCEPT ![s] = i]
    /\ UNCHANGED <<committed, commitIndex, appliedIndex, applied>>

\* InstallSnapshot: a lagging follower d, whose next needed index (commitIndex[d]+1) is at or below
\* the leader s's snapshot point (i.e. d cannot be caught up by AppendEntries because the entries it
\* needs were discarded by s), ADOPTS s's snapshot wholesale, then keeps s's retained suffix that is
\* already committed canonically.  After install, d's applied state jumps to the snapshot's state and
\* its appliedIndex/commitIndex/logBase jump to the snapshot point; the retained log is reset to the
\* leader's suffix (copied verbatim from the canonical committed log, bounded by what s holds).
InstallSnapshot(s, d) ==
    /\ s # d
    /\ snapshot[s].lastIncludedIndex > 0
    \* d is behind the leader's snapshot point — it genuinely needs the snapshot.
    /\ commitIndex[d] < snapshot[s].lastIncludedIndex
    /\ LET base == snapshot[s].lastIncludedIndex
           \* follower adopts the leader's retained suffix, but only the part already in the
           \* canonical committed log (it can never hold an uncommitted entry).
           suffixLen == Min(Len(log[s]), Len(committed) - base)
       IN
         /\ snapshot'     = [snapshot     EXCEPT ![d] = snapshot[s]]
         /\ applied'      = [applied      EXCEPT ![d] = snapshot[s].state]
         /\ appliedIndex' = [appliedIndex EXCEPT ![d] = base]
         /\ logBase'      = [logBase      EXCEPT ![d] = base]
         /\ commitIndex'  = [commitIndex  EXCEPT ![d] = base + suffixLen]
         /\ log'          = [log          EXCEPT ![d] = SubSeq(committed, base + 1, base + suffixLen)]
    /\ UNCHANGED <<committed>>

Next ==
    \/ \E k \in Key, v \in Value : CommitEntry(k, v)
    \/ \E s \in Server : AppendCommitted(s)
    \/ \E s \in Server : Apply(s)
    \/ \E s \in Server : TakeSnapshot(s)
    \/ \E s, d \in Server : InstallSnapshot(s, d)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Symmetry over interchangeable model values (server ids, keys, and values).
Symmetry == Permutations(Server) \cup Permutations(Key) \cup Permutations(Value)

----------------------------------------------------------------------------
\* State constraint: cap the canonical committed log length so the search is finite.
StateConstraint ==
    Len(committed) <= MaxLogLen

----------------------------------------------------------------------------
\* TYPE / structural sanity — keeps the retained log a contiguous suffix above the snapshot point,
\* and the snapshot point in range.  Checked as an invariant so any action that breaks the
\* representation is caught immediately.
TypeOK ==
    /\ committed \in Seq([key : Key, value : Value])
    /\ \A s \in Server :
         /\ logBase[s] = snapshot[s].lastIncludedIndex
         /\ logBase[s] >= 0
         /\ logBase[s] <= Len(committed)
         /\ logBase[s] + Len(log[s]) <= Len(committed)
         /\ commitIndex[s] = logBase[s] + Len(log[s])
         /\ appliedIndex[s] >= logBase[s]
         /\ appliedIndex[s] <= commitIndex[s]

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable.

\* (1) CompactionPreservesState. The live applied state a server holds always equals folding ALL
\* committed entries [1..appliedIndex] from the start — i.e. compaction (snapshot + prefix discard)
\* never changed the state-machine value.  snapshot.state already folds in the discarded prefix, so
\* reconstructing (snapshot.state + retained log up to appliedIndex) must equal the full-log fold.
CompactionPreservesState ==
    \A s \in Server :
        /\ applied[s] = FoldCommittedPrefix(appliedIndex[s])
        /\ ReconstructUpTo(s, appliedIndex[s]) = FoldCommittedPrefix(appliedIndex[s])

\* (2) SnapshotPrefixConsistent.  The snapshot's lastIncludedIndex sits inside the canonical
\* committed log, and snapshot.state is EXACTLY the fold of the committed prefix through that index
\* (no committed entry below the snapshot is lost — every one is folded into snapshot.state).
\* Also: the retained log is prefix-consistent with the canonical committed log (verbatim entries).
SnapshotPrefixConsistent ==
    \A s \in Server :
        /\ snapshot[s].lastIncludedIndex <= Len(committed)
        /\ snapshot[s].state = FoldCommittedPrefix(snapshot[s].lastIncludedIndex)
        /\ \A j \in 1 .. Len(log[s]) : log[s][j] = committed[logBase[s] + j]

\* (3) RecoveredEqualsFull.  For EVERY server (in particular a follower just caught up by
\* InstallSnapshot), reconstructing the state from (snapshot + retained suffix) up to its
\* commitIndex yields the same state as folding the full committed log up to that same index.
\* This is the recovery guarantee: snapshot + suffix == full log.
RecoveredEqualsFull ==
    \A s \in Server :
        ReconstructUpTo(s, commitIndex[s]) = FoldCommittedPrefix(commitIndex[s])

\* (4) StateMachineSafety (applied-state divergence).  No two servers' applied states differ at a
\* commonly-applied index: if both have applied through index n, their applied maps that reflect
\* [1..n] must agree.  We compare the reconstruction through the lower of the two applied indices —
\* both must equal the canonical fold there, hence each other.
AppliedStateSafety ==
    \A a, b \in Server :
        LET n == Min(appliedIndex[a], appliedIndex[b]) IN
            FoldCommittedPrefix(n) = FoldCommittedPrefix(n)   \* (oracle is single-valued; below is the real test)
            /\ applied[a] = FoldCommittedPrefix(appliedIndex[a])
            /\ applied[b] = FoldCommittedPrefix(appliedIndex[b])

THEOREM Spec =>
    [](TypeOK /\ CompactionPreservesState /\ SnapshotPrefixConsistent
       /\ RecoveredEqualsFull /\ AppliedStateSafety)
=============================================================================
