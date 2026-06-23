---------------------------- MODULE Membership ----------------------------
\* Lockstep — Phase 4 spec C4.2: DYNAMIC MEMBERSHIP CHANGE (single-server change).
\*
\* Purpose: the model-checked source of truth for adding/removing servers from the consensus
\* group WITHOUT violating election safety. The danger this spec exists to rule out: during a
\* config change, two DISJOINT majorities — one in the old config C_old, one in the new config
\* C_new — each elect a leader in the SAME term -> split brain / two leaders in one term.
\*
\* The fix modeled here is the SINGLE-SERVER CHANGE rule (Ongaro, "Consensus: Bridging Theory
\* and Practice", §4.1): a config change adds OR removes exactly ONE server at a time. With a
\* single-server delta, ANY majority of C_old and ANY majority of C_new intersect
\* (QuorumOverlap below) — so the two configs can never independently elect two leaders in one
\* term. ElectionSafety is preserved across the change.
\*
\* MODELING DISCIPLINE (why this shape is checkable AND sound).
\* This is a FOCUSED model: configs + elections, NOT full log replication (that is verified
\* separately in Consensus.tla). The configuration history is a single GLOBAL, totally-ordered
\* CHAIN `configs[0], configs[1], ...`, each consecutive pair differing by <= 1 server (the
\* single-server rule). Each server tracks `cfgIdx[s]` — the index of the LATEST config it has
\* adopted (configs live in the log; a server uses the newest config it has seen, Raft §4.1).
\* This chain abstraction is faithful to Raft (config entries are appended to ONE log and
\* propagate forward) and it forbids the physically-impossible divergence a free per-server
\* "config is any subset" model would admit. A server only ever moves FORWARD along the chain.
\*
\* The transition window is the joint-consensus straddle of TWO ADJACENT chain configs: a
\* candidate that straddles indices i and i+1 must win a majority of BOTH (joint quorum). Two
\* adjacent configs differ by one server, so their majorities always overlap — the property
\* that MAKES single-server change safe and that the checker verifies has teeth.
\*
\* How to model-check (TLC):  scripts/tlc.sh -config specs/Membership.cfg specs/Membership.tla

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS Server,            \* set of ALL server ids that may ever participate
          Nil,               \* placeholder for "no vote"
          Follower, Candidate, Leader

\* Search bounds (supplied as model values in the .cfg).
CONSTANTS MaxTerm,           \* cap on election terms
          MaxConfigs         \* cap on how many config entries the chain may grow to

\* The single-server delta. 1 == single-server change (SAFE, the real rule). A second cfg sets
\* this to 2 to demonstrate the checker has TEETH (a 2-at-once jump breaks overlap).
CONSTANTS MaxChangeDelta

\* The initial cluster configuration, configs[1] of the chain (a subset of Server).
CONSTANTS InitConfig

----------------------------------------------------------------------------
VARIABLES
    currentTerm,    \* [Server -> Nat]
    state,          \* [Server -> {Follower, Candidate, Leader}]
    votedFor,       \* [Server -> Server \cup {Nil}]
    votesGranted,   \* [Server -> SUBSET Server]
    configs,        \* Seq(SUBSET Server) — the GLOBAL config chain (configs[1] = InitConfig)
    cfgIdx          \* [Server -> 1..Len(configs)] — latest config index each server has adopted

vars == <<currentTerm, state, votedFor, votesGranted, configs, cfgIdx>>

----------------------------------------------------------------------------
\* Helpers

\* Majority subsets of a given config set c (a Quorum is relative to a CONFIG, not Server).
Quorum(c) == {Q \in SUBSET c : Cardinality(Q) * 2 > Cardinality(c)}

\* Symmetric-difference cardinality (the change delta between two configs).
DeltaSize(a, b) == Cardinality((a \ b) \cup (b \ a))

\* The config a server currently uses == the latest one it has adopted.
Cfg(s) == configs[cfgIdx[s]]

\* The chain is "settled" (no transition in flight) when every member of the newest config has
\* adopted that newest config — i.e. all live servers sit at the chain's head.
ChainHead == Len(configs)
Settled ==
    \A s \in configs[ChainHead] : cfgIdx[s] = ChainHead

----------------------------------------------------------------------------
Init ==
    /\ currentTerm  = [s \in Server |-> 0]
    /\ state        = [s \in Server |-> Follower]
    /\ votedFor     = [s \in Server |-> Nil]
    /\ votesGranted = [s \in Server |-> {}]
    /\ configs      = << InitConfig >>
    /\ cfgIdx       = [s \in Server |-> 1]

----------------------------------------------------------------------------
\* Actions

\* Election timeout: a server that is a member of ITS OWN current config becomes candidate,
\* bumps term, self-votes. (A removed server — not in its latest config — does not stand.)
Timeout(s) ==
    /\ s \in Cfg(s)
    /\ state[s] \in {Follower, Candidate}
    /\ currentTerm[s] < MaxTerm                  \* bound the search
    /\ currentTerm'  = [currentTerm  EXCEPT ![s] = currentTerm[s] + 1]
    /\ state'        = [state        EXCEPT ![s] = Candidate]
    /\ votedFor'     = [votedFor     EXCEPT ![s] = s]
    /\ votesGranted' = [votesGranted EXCEPT ![s] = {s}]
    /\ UNCHANGED <<configs, cfgIdx>>

\* Grant a vote (folds "adopt higher term, then vote" into one atomic step, as Consensus.tla's
\* UpdateTerm + HandleRequestVote). A member v grants to candidate c iff:
\*   - c's term is >= v's and v has not already voted for someone else in that term (one vote
\*     per term, tracked by votedFor + term); AND
\*   - c is at least as CONFIG-UP-TO-DATE as v: cfgIdx[c] >= cfgIdx[v]. This is the config-log
\*     recency rule (the analogue of Raft's log up-to-date check, restricted to config entries
\*     — the only log content this focused model keeps). It is ESSENTIAL for safety: it stops a
\*     server stranded on a stale, superseded config (e.g. a removed server that never learned
\*     it was removed) from collecting a quorum under that old config and electing itself a
\*     second leader. A lagging candidate is refused by every server that has moved ahead, so it
\*     can never out-vote the up-to-date cluster.
RequestVote(v, c) ==
    /\ v # c
    /\ state[c] = Candidate
    /\ v \in Cfg(v)                               \* only an active member votes
    /\ cfgIdx[c] >= cfgIdx[v]                      \* candidate's config log is up-to-date
    /\ \/ currentTerm[c] > currentTerm[v]         \* higher term: adopt + grant fresh
       \/ /\ currentTerm[c] = currentTerm[v]      \* same term: grant iff not yet voted away
          /\ votedFor[v] \in {Nil, c}
    /\ currentTerm'  = [currentTerm  EXCEPT ![v] = currentTerm[c]]
    /\ state'        = [state        EXCEPT ![v] =
                          IF currentTerm[c] > currentTerm[v] THEN Follower ELSE state[v]]
    /\ votedFor'     = [votedFor     EXCEPT ![v] = c]
    /\ votesGranted' = [votesGranted EXCEPT ![c] = votesGranted[c] \cup {v}]
    /\ UNCHANGED <<configs, cfgIdx>>

\* Become leader on a quorum of votes. The candidate must win a majority of EVERY config that is
\* live for it: its own latest config, AND — if it straddles a transition — the immediately
\* PRECEDING chain config too (the joint-consensus rule for the single-server window). Because
\* consecutive chain configs differ by <= 1 server, those configs overlap, so a rival cannot
\* assemble the complementary majority of both -> at most one leader per term.
\* `LiveConfigs(s)` = the set of chain indices a candidate at s must satisfy: its own index, and
\* the previous index while the transition into Cfg(s) has not yet fully settled behind it.
LiveConfigs(s) ==
    IF cfgIdx[s] > 1 /\ (\E t \in Server : cfgIdx[t] = cfgIdx[s] - 1 /\ t \in configs[cfgIdx[s]-1])
    THEN {cfgIdx[s] - 1, cfgIdx[s]}
    ELSE {cfgIdx[s]}

BecomeLeader(s) ==
    /\ state[s] = Candidate
    /\ \A i \in LiveConfigs(s) : votesGranted[s] \cap configs[i] \in Quorum(configs[i])
    /\ state' = [state EXCEPT ![s] = Leader]
    /\ UNCHANGED <<currentTerm, votedFor, votesGranted, configs, cfgIdx>>

\* A leader's leadership is "live" only while its vote set is still a majority of its current
\* config. A leader whose config changed underneath it (it was removed, or a swap superseded the
\* config it won under) has lost the cluster and must not drive further changes.
LeaderQuorumValid(s) == votesGranted[s] \cap Cfg(s) \in Quorum(Cfg(s))

\* A leader PROPOSES a single-server membership change: it appends a new config to the chain.
\* Admissible only when:
\*   - the leader is genuinely the CURRENT leader: it holds a live quorum of its config AND the
\*     maximum term among that config's members (no stale lower-term leader mutating membership);
\*   - the chain is SETTLED (the previous change fully propagated) — commit-before-next, the
\*     precondition that stops two single-server changes composing into an unsafe net 2-jump
\*     (Ongaro §4.2.3); together with "<= MaxChangeDelta per step" this keeps any two
\*     simultaneously-live chain configs ADJACENT, hence overlapping;
\*   - the new config differs from the head by <= MaxChangeDelta, is non-empty, and keeps the
\*     leader a member;
\*   - the chain has room (bound the search).
\* The leader adopts the new config immediately (Raft: a leader uses C_new as soon as it is
\* appended to its log); other servers catch up via AdoptConfig — THIS is the straddle window.
ProposeConfigChange(s) ==
    /\ state[s] = Leader
    /\ cfgIdx[s] = ChainHead
    /\ LeaderQuorumValid(s)
    /\ \A m \in Cfg(s) : currentTerm[s] >= currentTerm[m]
    /\ Settled
    /\ Len(configs) < MaxConfigs
    /\ \E newC \in SUBSET Server :
         /\ newC # configs[ChainHead]
         /\ newC # {}                             \* never empty the cluster
         /\ s \in newC                            \* leader stays a member (no self-removal)
         /\ DeltaSize(configs[ChainHead], newC) <= MaxChangeDelta
         /\ configs' = Append(configs, newC)
         /\ cfgIdx'  = [cfgIdx EXCEPT ![s] = Len(configs) + 1]
    /\ UNCHANGED <<currentTerm, state, votedFor, votesGranted>>

\* A server catches up to the head of the chain by adopting the next config (moves one step
\* forward). Any server may do this in any order — modeling the asynchronous rollout that
\* produces the old/new straddle. A server only ever advances (never moves backward).
AdoptConfig(s) ==
    /\ cfgIdx[s] < ChainHead
    /\ cfgIdx' = [cfgIdx EXCEPT ![s] = cfgIdx[s] + 1]
    /\ UNCHANGED <<currentTerm, state, votedFor, votesGranted, configs>>

\* Step down on a higher term (mirrors Consensus.tla UpdateTerm): a Candidate/Leader that sees a
\* strictly higher term anywhere adopts it and reverts to Follower, clearing its stale vote set.
\* Ensures only the highest-term leader persists.
StepDown(s) ==
    /\ state[s] \in {Candidate, Leader}
    /\ \E t \in Server : currentTerm[t] > currentTerm[s]
    /\ \E t \in Server :
         /\ currentTerm[t] > currentTerm[s]
         /\ currentTerm'  = [currentTerm  EXCEPT ![s] = currentTerm[t]]
    /\ state'        = [state        EXCEPT ![s] = Follower]
    /\ votedFor'     = [votedFor     EXCEPT ![s] = Nil]
    /\ votesGranted' = [votesGranted EXCEPT ![s] = {}]
    /\ UNCHANGED <<configs, cfgIdx>>

Next ==
    \/ \E s \in Server : Timeout(s)
    \/ \E v, c \in Server : RequestVote(v, c)
    \/ \E s \in Server : BecomeLeader(s)
    \/ \E s \in Server : StepDown(s)
    \/ \E s \in Server : ProposeConfigChange(s)
    \/ \E s \in Server : AdoptConfig(s)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* No SYMMETRY: InitConfig singles out the add/remove candidate server, so server ids are NOT
\* fully interchangeable; Permutations(Server) would be unsound. The instance is small enough to
\* explore exhaustively without symmetry reduction.

----------------------------------------------------------------------------
\* State constraint to bound the model: caps terms and chain length.
StateConstraint ==
    /\ \A s \in Server : currentTerm[s] <= MaxTerm
    /\ Len(configs) <= MaxConfigs

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable. TLC must show these always hold.

\* (1) ElectionSafety: at most one leader per term (preserved across the change).
ElectionSafety ==
    \A a, b \in Server :
        (state[a] = Leader /\ state[b] = Leader /\ currentTerm[a] = currentTerm[b]) => a = b

\* (2) QuorumOverlap: the property that MAKES single-server change safe. ANY two CONSECUTIVE
\* chain configs (the only pair that can be simultaneously live during a transition) have the
\* property that any majority of one and any majority of the other intersect. With a
\* single-server delta this always holds; a 2-server jump can break it (Membership2.cfg — the
\* teeth). Checked over every adjacent pair in the chain.
QuorumOverlap ==
    \A i \in 1 .. (Len(configs) - 1) :
        \A Qa \in Quorum(configs[i]), Qb \in Quorum(configs[i+1]) :
            Qa \cap Qb # {}

\* (3) ConfigChangeSafety: across ANY reachable state during/after a membership change, no two
\* servers are leaders in the same term — i.e. no two disjoint majorities (old + new) both
\* elect. This is the headline C4.2 deliverable: ElectionSafety re-asserted under membership
\* dynamics. (Same predicate as ElectionSafety; named separately to document intent.)
ConfigChangeSafety ==
    \A a, b \in Server :
        (state[a] = Leader /\ state[b] = Leader /\ currentTerm[a] = currentTerm[b]) => a = b

\* Type sanity, including the single-server CHAIN invariant: every consecutive pair of configs
\* in the chain differs by at most MaxChangeDelta. This makes the "configs are a single-server
\* chain" modeling assumption an explicitly-checked fact, not just an action side effect.
TypeOK ==
    /\ currentTerm  \in [Server -> Nat]
    /\ state        \in [Server -> {Follower, Candidate, Leader}]
    /\ votedFor     \in [Server -> Server \cup {Nil}]
    /\ cfgIdx       \in [Server -> 1 .. Len(configs)]
    /\ \A i \in 1 .. Len(configs) : configs[i] \subseteq Server
    /\ \A i \in 1 .. (Len(configs) - 1) : DeltaSize(configs[i], configs[i+1]) <= MaxChangeDelta

THEOREM Spec => [](ElectionSafety /\ QuorumOverlap /\ ConfigChangeSafety)
=============================================================================
