---------------------------- MODULE Consensus ----------------------------
\* Lockstep — Phase 4 consensus spec (SKELETON).
\*
\* Purpose: the model-checked source of truth for the replicated log. The implementation
\* (Phase 4, core-no-freelance, dual-built) MUST conform to this spec. Behavior changes
\* require editing this spec and re-running TLC FIRST.
\*
\* This skeleton fully specifies the STATE and the SAFETY INVARIANTS (the deliverable).
\* The message-handling actions are sketched with TODO markers — fill them in, keeping every
\* step within the safety envelope below, then model-check.
\*
\* How to model-check (TLC):
\*   - Small finite instance, e.g. Server = {s1,s2,s3}, Value = {v1,v2}, Nil a model value;
\*     Follower/Candidate/Leader as model values.
\*   - State constraint to bound the search: MaxTerm and MaxLogLen (e.g. <= 3).
\*   - Symmetry sets over Server and Value.
\*   - Invariants to check: ElectionSafety, LogMatching, StateMachineSafety, LeaderAppendOnly.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Server,            \* set of server ids
          Value,             \* set of client command values
          Nil,               \* placeholder for "no vote"
          Follower, Candidate, Leader

----------------------------------------------------------------------------
VARIABLES
    currentTerm,   \* [Server -> Nat]
    state,         \* [Server -> {Follower, Candidate, Leader}]
    votedFor,      \* [Server -> Server \cup {Nil}]
    log,           \* [Server -> Seq([term : Nat, value : Value])]
    commitIndex,   \* [Server -> Nat]
    votesGranted,  \* [Server -> SUBSET Server]
    messages       \* set of in-flight messages (records)

vars == <<currentTerm, state, votedFor, log, commitIndex, votesGranted, messages>>

----------------------------------------------------------------------------
\* Helpers

Min(a, b) == IF a < b THEN a ELSE b

\* Any majority subset of servers.
Quorum == {Q \in SUBSET Server : Cardinality(Q) * 2 > Cardinality(Server)}

LastTerm(l)  == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].term
LastIndex(l) == Len(l)

\* A candidate's log is at least as up-to-date as a voter's (Raft up-to-date rule).
UpToDate(candLog, voterLog) ==
    \/ LastTerm(candLog) > LastTerm(voterLog)
    \/ /\ LastTerm(candLog) = LastTerm(voterLog)
       /\ LastIndex(candLog) >= LastIndex(voterLog)

----------------------------------------------------------------------------
Init ==
    /\ currentTerm  = [s \in Server |-> 0]
    /\ state        = [s \in Server |-> Follower]
    /\ votedFor     = [s \in Server |-> Nil]
    /\ log          = [s \in Server |-> << >>]
    /\ commitIndex  = [s \in Server |-> 0]
    /\ votesGranted = [s \in Server |-> {}]
    /\ messages     = {}

----------------------------------------------------------------------------
\* Actions (skeleton — fill TODOs; keep within the safety envelope)

\* Election timeout: become candidate, bump term, self-vote.
Timeout(s) ==
    /\ state[s] \in {Follower, Candidate}
    /\ currentTerm'  = [currentTerm  EXCEPT ![s] = currentTerm[s] + 1]
    /\ state'        = [state        EXCEPT ![s] = Candidate]
    /\ votedFor'     = [votedFor     EXCEPT ![s] = s]
    /\ votesGranted' = [votesGranted EXCEPT ![s] = {s}]
    /\ UNCHANGED <<log, commitIndex, messages>>
    \* TODO: emit RequestVote(term=currentTerm'[s], candidate=s, lastIdx, lastTerm) to all others.

\* Grant a vote iff term is current and candidate log is up-to-date; persist the vote.
HandleRequestVote(s) == FALSE
    \* TODO: on a RequestVote msg m:
    \*   if m.term > currentTerm[s] -> step down, adopt term;
    \*   grant iff (votedFor[s] \in {Nil, m.candidate}) /\ UpToDate(m.candLog, log[s]);
    \*   votedFor MUST be persisted (IDisk) before replying — model as part of this step.

\* Become leader on a quorum of votes in the current term.
BecomeLeader(s) ==
    /\ state[s] = Candidate
    /\ votesGranted[s] \in Quorum
    /\ state' = [state EXCEPT ![s] = Leader]
    /\ UNCHANGED <<currentTerm, votedFor, log, commitIndex, votesGranted, messages>>
    \* TODO: initialize per-follower nextIndex/matchIndex if modeling replication explicitly.

\* Leader accepts a client command.
ClientRequest(s, v) ==
    /\ state[s] = Leader
    /\ log' = [log EXCEPT ![s] = Append(@, [term |-> currentTerm[s], value |-> v])]
    /\ UNCHANGED <<currentTerm, state, votedFor, commitIndex, votesGranted, messages>>

\* Replication + log-matching enforcement.
AppendEntries(s, d)      == FALSE  \* TODO: leader s sends entries to follower d
HandleAppendEntries(s)   == FALSE  \* TODO: follower truncates conflicts, appends, enforces log matching
AdvanceCommitIndex(s)    == FALSE  \* TODO: leader advances commit when a Quorum stores an entry of the CURRENT term

Next ==
    \/ \E s \in Server : Timeout(s)
    \/ \E s \in Server : HandleRequestVote(s)
    \/ \E s \in Server : BecomeLeader(s)
    \/ \E s \in Server, v \in Value : ClientRequest(s, v)
    \/ \E s, d \in Server : AppendEntries(s, d)
    \/ \E s \in Server : HandleAppendEntries(s)
    \/ \E s \in Server : AdvanceCommitIndex(s)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable. TLC must show these always hold.

\* At most one leader per term.
ElectionSafety ==
    \A a, b \in Server :
        (state[a] = Leader /\ state[b] = Leader /\ currentTerm[a] = currentTerm[b]) => a = b

\* If two logs hold an entry with the same index and term, they agree on the whole prefix up to it.
LogMatching ==
    \A a, b \in Server :
        \A i \in 1 .. Min(Len(log[a]), Len(log[b])) :
            (log[a][i].term = log[b][i].term) =>
                SubSeq(log[a], 1, i) = SubSeq(log[b], 1, i)

\* Committed entries never diverge: at any committed index, all servers hold the same entry.
StateMachineSafety ==
    \A a, b \in Server :
        \A i \in 1 .. Min(commitIndex[a], commitIndex[b]) :
            log[a][i] = log[b][i]

\* A leader never overwrites or deletes entries in its own log (append-only while leader).
\* Expressed as an action invariant — check with [][LeaderAppendOnly]_vars.
LeaderAppendOnly ==
    \A s \in Server :
        (state[s] = Leader) =>
            /\ Len(log'[s]) >= Len(log[s])
            /\ \A i \in 1 .. Len(log[s]) : log'[s][i] = log[s][i]

THEOREM Spec => [](ElectionSafety /\ LogMatching /\ StateMachineSafety)
=============================================================================
