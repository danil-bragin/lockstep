---------------------------- MODULE Consensus ----------------------------
\* Lockstep — Phase 4 consensus spec.
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

\* Search bounds (used by StateConstraint; supplied as model values in the .cfg).
CONSTANTS MaxTerm, MaxLogLen, MaxMsgs

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
\* Message schema (messages is a SET; re-delivery is idempotent).
\*
\* RequestVote request:
\*   [ mtype |-> "RequestVote", mterm |-> Nat, msource |-> Server, mdest |-> Server,
\*     mlastLogIndex |-> Nat, mlastLogTerm |-> Nat ]
\*
\* RequestVote response:
\*   [ mtype |-> "RequestVoteResponse", mterm |-> Nat, msource |-> Server, mdest |-> Server,
\*     mgranted |-> BOOLEAN ]
\*
\* AppendEntries request:
\*   [ mtype |-> "AppendEntries", mterm |-> Nat, msource |-> Server, mdest |-> Server,
\*     mprevLogIndex |-> Nat, mprevLogTerm |-> Nat, mentries |-> Seq(entry),
\*     mleaderCommit |-> Nat ]
\*   where entry == [ term |-> Nat, value |-> Value ].
\*
\* AppendEntries response:
\*   [ mtype |-> "AppendEntriesResponse", mterm |-> Nat, msource |-> Server, mdest |-> Server,
\*     msuccess |-> BOOLEAN, mmatchIndex |-> Nat ]

----------------------------------------------------------------------------
\* Helpers

Min(a, b) == IF a < b THEN a ELSE b
Max(a, b) == IF a > b THEN a ELSE b

\* Any majority subset of servers.
Quorum == {Q \in SUBSET Server : Cardinality(Q) * 2 > Cardinality(Server)}

LastTerm(l)  == IF Len(l) = 0 THEN 0 ELSE l[Len(l)].term
LastIndex(l) == Len(l)

\* A candidate's log is at least as up-to-date as a voter's (Raft up-to-date rule).
UpToDate(candLog, voterLog) ==
    \/ LastTerm(candLog) > LastTerm(voterLog)
    \/ /\ LastTerm(candLog) = LastTerm(voterLog)
       /\ LastIndex(candLog) >= LastIndex(voterLog)

\* Term carried by entry at index i of log l (0 if out of range / index 0).
TermAt(l, i) == IF i = 0 \/ i > Len(l) THEN 0 ELSE l[i].term

Send(m)  == messages' = messages \cup {m}

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
\* Actions

\* Election timeout: become candidate, bump term, self-vote, and broadcast RequestVote.
Timeout(s) ==
    /\ state[s] \in {Follower, Candidate}
    /\ currentTerm[s] < MaxTerm                 \* bound the search
    /\ currentTerm'  = [currentTerm  EXCEPT ![s] = currentTerm[s] + 1]
    /\ state'        = [state        EXCEPT ![s] = Candidate]
    /\ votedFor'     = [votedFor     EXCEPT ![s] = s]
    /\ votesGranted' = [votesGranted EXCEPT ![s] = {s}]
    /\ messages' = messages \cup
         { [ mtype        |-> "RequestVote",
             mterm        |-> currentTerm[s] + 1,
             msource      |-> s,
             mdest        |-> d,
             mlastLogIndex |-> LastIndex(log[s]),
             mlastLogTerm  |-> LastTerm(log[s]) ]
           : d \in Server \ {s} }
    /\ UNCHANGED <<log, commitIndex>>

\* Step down on a higher-term message: adopt the term, revert to Follower, clear the vote.
\* Modeled as its OWN action so that any log mutation (truncation) performed by a later
\* AppendEntries handler happens while the server is ALREADY a Follower — this is what keeps
\* LeaderAppendOnly (a Leader's log is append-only) true under stale-leader truncation.
\* The triggering message is left in `messages` to be (re)processed at the adopted term.
UpdateTerm(s) ==
    \E m \in messages :
        /\ m.mtype \in {"RequestVote", "RequestVoteResponse", "AppendEntries", "AppendEntriesResponse"}
        /\ m.mdest = s
        /\ m.mterm > currentTerm[s]
        /\ currentTerm'  = [currentTerm  EXCEPT ![s] = m.mterm]
        /\ state'        = [state        EXCEPT ![s] = Follower]
        /\ votedFor'     = [votedFor     EXCEPT ![s] = Nil]
        /\ votesGranted' = [votesGranted EXCEPT ![s] = {}]
        /\ UNCHANGED <<log, commitIndex, messages>>

\* Grant a vote iff term is current and candidate log is up-to-date; persist the vote
\* atomically in this step (models "persist votedFor before reply").
\* Higher-term requests are handled first by UpdateTerm (step-down); here m.mterm = currentTerm.
HandleRequestVote(s) ==
    \E m \in messages :
        /\ m.mtype = "RequestVote"
        /\ m.mdest = s
        /\ m.mterm = currentTerm[s]            \* not stale, not higher (UpdateTerm handles higher)
        /\ LET candUpToDate ==
                    \/ m.mlastLogTerm > LastTerm(log[s])
                    \/ /\ m.mlastLogTerm = LastTerm(log[s])
                       /\ m.mlastLogIndex >= LastIndex(log[s])
               grant ==
                    /\ votedFor[s] \in {Nil, m.msource}
                    /\ candUpToDate
           IN /\ votedFor' = [votedFor EXCEPT ![s] = IF grant THEN m.msource ELSE votedFor[s]]
              /\ Send([ mtype   |-> "RequestVoteResponse",
                        mterm   |-> currentTerm[s],
                        msource |-> s,
                        mdest   |-> m.msource,
                        mgranted |-> grant ])
              /\ UNCHANGED <<currentTerm, state, log, commitIndex, votesGranted>>

\* Candidate collects a granted vote response (for the current term).
HandleVoteResponse(s) ==
    \E m \in messages :
        /\ m.mtype = "RequestVoteResponse"
        /\ m.mdest = s
        /\ m.mterm = currentTerm[s]
        /\ state[s] = Candidate
        /\ m.mgranted
        /\ votesGranted' = [votesGranted EXCEPT ![s] = votesGranted[s] \cup {m.msource}]
        /\ UNCHANGED <<currentTerm, state, votedFor, log, commitIndex, messages>>

\* Become leader on a quorum of votes in the current term.
BecomeLeader(s) ==
    /\ state[s] = Candidate
    /\ votesGranted[s] \in Quorum
    /\ state' = [state EXCEPT ![s] = Leader]
    /\ UNCHANGED <<currentTerm, votedFor, log, commitIndex, votesGranted, messages>>

\* Leader accepts a client command.
ClientRequest(s, v) ==
    /\ state[s] = Leader
    /\ Len(log[s]) < MaxLogLen                  \* bound the search
    /\ log' = [log EXCEPT ![s] = Append(@, [term |-> currentTerm[s], value |-> v])]
    /\ UNCHANGED <<currentTerm, state, votedFor, commitIndex, votesGranted, messages>>

\* Replication: leader s sends entries (>=0) to follower d from some nextIndex.
\* We let prevLogIndex range over any valid index of the leader's log; entries are the
\* suffix after prevLogIndex.  This non-deterministically covers all nextIndex choices.
AppendEntries(s, d) ==
    /\ state[s] = Leader
    /\ s # d
    /\ \E prevLogIndex \in 0 .. Len(log[s]) :
         LET prevLogTerm == TermAt(log[s], prevLogIndex)
             entries     == SubSeq(log[s], prevLogIndex + 1, Len(log[s]))
         IN Send([ mtype         |-> "AppendEntries",
                   mterm         |-> currentTerm[s],
                   msource       |-> s,
                   mdest         |-> d,
                   mprevLogIndex |-> prevLogIndex,
                   mprevLogTerm  |-> prevLogTerm,
                   mentries      |-> entries,
                   mleaderCommit |-> commitIndex[s] ])
    /\ UNCHANGED <<currentTerm, state, votedFor, log, commitIndex, votesGranted>>

\* Follower handles AppendEntries: reject on prevLog mismatch; otherwise delete any
\* conflicting suffix, append new entries (Log Matching), and adopt leaderCommit.
\* Higher-term messages are handled first by UpdateTerm (step-down), so here m.mterm =
\* currentTerm[s]; thus the server is NOT the term's leader (ElectionSafety) and any log
\* truncation happens while it is a Follower/Candidate — preserving LeaderAppendOnly.
HandleAppendEntries(s) ==
    \E m \in messages :
        /\ m.mtype = "AppendEntries"
        /\ m.mdest = s
        /\ m.mterm = currentTerm[s]            \* not stale, not higher (UpdateTerm handles higher)
        /\ LET \* prevLog must match: index in range (or 0) and matching term.
               logOk ==
                    \/ m.mprevLogIndex = 0
                    \/ /\ m.mprevLogIndex <= Len(log[s])
                       /\ m.mprevLogTerm = TermAt(log[s], m.mprevLogIndex)
               success == logOk
               \* Raft conflict rule (Fig.2 §3/§4): only delete existing entries that CONFLICT
               \* with a new one (same index, different term); do NOT truncate matching entries.
               \* This makes redelivery of a stale/short AppendEntries idempotent (it must never
               \* erase entries the follower already agreed on, e.g. ones it has committed).
               \* First incoming entry index (1-based) whose term conflicts with our existing log,
               \* or "no conflict" when every overlapping entry already matches.
               conflict ==
                   { k \in 1 .. Len(m.mentries) :
                        /\ m.mprevLogIndex + k <= Len(log[s])
                        /\ log[s][m.mprevLogIndex + k].term # m.mentries[k].term }
               \* Keep our log up to (prevLogIndex + firstConflict - 1), then append remaining
               \* incoming entries.  If no conflict and incoming adds nothing new, log is unchanged.
               firstConflict == IF conflict = {} THEN 0 ELSE CHOOSE k \in conflict :
                                    \A j \in conflict : k <= j
               newLog  == IF ~success
                          THEN log[s]
                          ELSE IF firstConflict = 0
                               \* No conflict: keep our log, append only the genuinely-new tail.
                               THEN IF m.mprevLogIndex + Len(m.mentries) <= Len(log[s])
                                    THEN log[s]   \* incoming fully subsumed — idempotent no-op
                                    ELSE SubSeq(log[s], 1, m.mprevLogIndex)
                                           \o m.mentries
                               \* Conflict: truncate at first conflict, append incoming suffix.
                               ELSE SubSeq(log[s], 1, m.mprevLogIndex + firstConflict - 1)
                                      \o SubSeq(m.mentries, firstConflict, Len(m.mentries))
               lastNew == m.mprevLogIndex + Len(m.mentries)
           IN \* A current-term AppendEntries from the leader: recognize it (Candidate steps down).
              /\ state'       = [state EXCEPT ![s] = Follower]
              /\ log'         = [log EXCEPT ![s] = newLog]
              /\ commitIndex' = [commitIndex EXCEPT ![s] =
                                   IF success
                                   THEN Max(commitIndex[s], Min(m.mleaderCommit, lastNew))
                                   ELSE commitIndex[s]]
              /\ Send([ mtype       |-> "AppendEntriesResponse",
                        mterm       |-> currentTerm[s],
                        msource     |-> s,
                        mdest       |-> m.msource,
                        msuccess    |-> success,
                        mmatchIndex |-> IF success THEN lastNew ELSE 0 ])
              /\ UNCHANGED <<currentTerm, votedFor, votesGranted>>

\* Leader advances commitIndex only when a Quorum stores an entry AND that entry is of the
\* CURRENT term (the Raft commitment rule that makes StateMachineSafety hold).
\* "Stored on a quorum" is established directly from the leader's own log being a prefix of
\* the agreeing servers' logs; we read the agreement off the logs (matchIndex equivalent).
AdvanceCommitIndex(s) ==
    /\ state[s] = Leader
    /\ \E newCommit \in (commitIndex[s] + 1) .. Len(log[s]) :
         /\ log[s][newCommit].term = currentTerm[s]      \* current-term entry only
         /\ LET agree == { i \in Server :
                             /\ Len(log[i]) >= newCommit
                             /\ log[i][newCommit] = log[s][newCommit] }
            IN agree \in Quorum
         /\ commitIndex' = [commitIndex EXCEPT ![s] = newCommit]
    /\ UNCHANGED <<currentTerm, state, votedFor, log, votesGranted, messages>>

Next ==
    \/ \E s \in Server : Timeout(s)
    \/ \E s \in Server : UpdateTerm(s)
    \/ \E s \in Server : HandleRequestVote(s)
    \/ \E s \in Server : HandleVoteResponse(s)
    \/ \E s \in Server : BecomeLeader(s)
    \/ \E s \in Server, v \in Value : ClientRequest(s, v)
    \/ \E s, d \in Server : AppendEntries(s, d)
    \/ \E s \in Server : HandleAppendEntries(s)
    \/ \E s \in Server : AdvanceCommitIndex(s)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* Symmetry over interchangeable model values (server ids and client values).
Symmetry == Permutations(Server) \cup Permutations(Value)

----------------------------------------------------------------------------
\* State constraint to bound the model: caps terms, log lengths, and in-flight messages.
\* The MaxMsgs cap bounds the (otherwise monotonically growing) message SET so the reachable
\* state space is finite and fully explorable.  messages remains a set with idempotent
\* re-delivery — the cap only limits how many DISTINCT messages may be simultaneously in flight,
\* which still covers reorder/duplicate/partition scenarios within the bound.
StateConstraint ==
    /\ \A s \in Server : currentTerm[s] <= MaxTerm
    /\ \A s \in Server : Len(log[s]) <= MaxLogLen
    /\ Cardinality(messages) <= MaxMsgs

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

\* PROPERTY wrapper so the cfg can check the action invariant as a temporal property.
LeaderAppendOnlyProp == [][LeaderAppendOnly]_vars

THEOREM Spec => [](ElectionSafety /\ LogMatching /\ StateMachineSafety)
=============================================================================
