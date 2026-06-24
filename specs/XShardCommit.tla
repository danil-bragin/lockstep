-------------------------- MODULE XShardCommit --------------------------
\* Lockstep — Phase 9 S9.3 CROSS-SHARD ATOMIC COMMIT spec (Calvin-style, NO 2PC).
\*
\* Purpose: the model-checked source of truth for how a transaction that touches
\* keys on MULTIPLE shards commits ATOMICALLY (all-or-nothing) and is placed at
\* ONE global position by the deterministic Sequencer merge — the property that
\* neither Sequencer.tla nor CommitOrdering.tla covers.
\*
\* WHY A NEW SPEC (the gap the two existing specs do NOT close):
\*   - Sequencer.tla models ShardOf : [Txn -> Shard]: each txn is owned by EXACTLY
\*     ONE shard, appears in exactly one shard's input log, and is emitted ONCE.
\*     A cross-shard txn touches >= 2 shards, so it lands in >= 2 shard logs and a
\*     naive merge would emit it MULTIPLE times at MULTIPLE global positions —
\*     which would make the executor apply its writes more than once and could
\*     leave a partial commit (some shards, not others). Sequencer.tla's
\*     ExactlyOnceGlobal is keyed on (shard, idx), so it does NOT forbid the SAME
\*     txn appearing once per shard.
\*   - CommitOrdering.tla executes a single global seqLog deterministically, but
\*     ASSUMES that seqLog already lists each txn at exactly one position; it says
\*     nothing about how a txn replicated across shards collapses to one position,
\*     nor about cross-shard atomicity.
\*
\* THE CALVIN INSIGHT MODELED HERE (deterministic ordering removes 2PC):
\*   A cross-shard txn is replicated into the committed Raft log of EVERY shard it
\*   touches (so every replica/shard agrees it happened). The deterministic merge
\*   then collapses the txn's per-shard appearances into ONE global position, and
\*   the executor applies ALL of its writes together at that position. Because the
\*   merge + execution are pure deterministic functions of the agreed per-shard
\*   logs, every replica computes the SAME single position and the SAME atomic
\*   apply — so the txn commits on ALL its shards or (if not yet replicated +
\*   sealed everywhere) on NONE. There is no 2PC, no prepare, no distributed lock:
\*   atomicity is a consequence of the deterministic global order.
\*
\* THE DEDUP + SEAL-GATE RULE (what makes it atomic and exactly-once):
\*   * A txn is GLOBALLY SEALABLE only once it is committed on ALL its shards into
\*     a sealed epoch. Its global EPOCH is the MAX epoch at which any of its shards
\*     committed it (a shard that lags pins the whole txn's epoch). It is placed at
\*     the global position of that epoch's batch, ordered within the batch by the
\*     rank of its LOWEST-ranked involved shard (a fixed total order over txns).
\*   * It is emitted EXACTLY ONCE (collapsing the per-shard appearances). All its
\*     writes — across every shard it touches — are applied together at that one
\*     position: ATOMIC. A txn NOT yet sealed on all its shards is absent from the
\*     global log entirely: NONE of its writes are visible. So: all-or-nothing.
\*
\* How to model-check (TLC):
\*   - Finite instance, e.g. Shard = {sh1, sh2}, Txn = {a, b, c} where some txns
\*     are single-shard and at least one is cross-shard, MaxEpoch = 2.
\*   - State constraint bounds epoch + per-shard log length.
\*   - Invariants: AtomicAllOrNone, ExactlyOnceGlobal, OneGlobalPosition,
\*     GlobalOrderDeterministic, PerShardSubmissionPreserved, EpochMonotone.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Shard,        \* set of shard ids (each runs its own Phase-4 consensus log)
          Txn,          \* set of transaction ids (single- AND cross-shard)
          ShardsOf,     \* [Txn -> SUBSET Shard]  the shards each txn TOUCHES (>=1)
          ShardRank,     \* [Shard -> Nat]  injective rank: a total order over shards
          MaxEpoch,     \* Nat  search bound: highest epoch that may be opened
          sh1, sh2,     \* model values for the shard ids
          a, b, c       \* model values for the txn ids

\* Concrete instance maps (function literals are not legal in a .cfg, so the .cfg
\* `<-`-maps the CONSTANTS onto these operators). a,b touch BOTH shards (cross-
\* shard); c touches only sh2 (single-shard). ShardRank is injective (sh1<sh2).
ShardsOfDef   == (a :> {sh1, sh2} @@ b :> {sh1, sh2} @@ c :> {sh2})
ShardRankDef  == (sh1 :> 0 @@ sh2 :> 1)

----------------------------------------------------------------------------
VARIABLES
    inputLog,   \* [Shard -> Seq([txn : Txn, epoch : Nat])]
                \*   each shard's committed Raft log: txn ids in consensus order,
                \*   epoch-tagged. A cross-shard txn appears in EVERY shard it
                \*   touches (replicated into each). Append-only per shard.
    epoch,      \* Nat  the current OPEN epoch shards may commit into ("now")
    sealed,     \* Nat  highest epoch the sequencer has SEALED into the global log
    globalLog,  \* Seq([txn : Txn, epoch : Nat, gshard : Shard])
                \*   the produced global total order: each txn ONCE, with its
                \*   global epoch + its ordering (lowest-rank) shard.
    appliedOn   \* [Txn -> SUBSET Shard]  the shards on which the executor has
                \*   APPLIED this txn's writes. The atomicity ground truth.

vars == <<inputLog, epoch, sealed, globalLog, appliedOn>>

----------------------------------------------------------------------------
\* Helpers

\* Has shard s committed txn t into its input log yet? (and at which epoch)
CommittedOn(s, t) == \E i \in 1 .. Len(inputLog[s]) : inputLog[s][i].txn = t

EpochOn(s, t) ==
    LET i == CHOOSE i \in 1 .. Len(inputLog[s]) : inputLog[s][i].txn = t
    IN inputLog[s][i].epoch

\* A txn is FULLY COMMITTED once every shard it touches has committed it.
FullyCommitted(t) == \A s \in ShardsOf[t] : CommittedOn(s, t)

\* The txn's GLOBAL epoch = the MAX epoch at which any of its shards committed it
\* (the laggard pins it). Defined only when FullyCommitted(t).
GlobalEpoch(t) ==
    LET es == { EpochOn(s, t) : s \in ShardsOf[t] }
    IN CHOOSE e \in es : \A f \in es : f <= e

\* The txn's ordering shard = the LOWEST-ranked shard it touches (a fixed total-
\* order key, since ShardRank is injective). Used as the within-batch sort key.
OrderShard(t) ==
    CHOOSE s \in ShardsOf[t] :
        \A r \in ShardsOf[t] \ {s} : ShardRank[s] < ShardRank[r]

\* A txn is GLOBALLY SEALABLE at boundary `sld` iff it is fully committed and its
\* global epoch is sealed. This is the SEAL GATE: a txn missing on ANY of its
\* shards (or whose laggard epoch is unsealed) is NOT yet in the global log.
Sealable(t, sld) == FullyCommitted(t) /\ GlobalEpoch(t) <= sld

\* The set of txns whose global epoch is exactly e and that are sealable at sld.
EpochTxns(e, sld) == { t \in Txn : Sealable(t, sld) /\ GlobalEpoch(t) = e }

\* The fixed total order over txns used to sort a batch deterministically.
\* TxnKey = (order-shard rank, then a fixed per-txn tiebreak), so two txns with the
\* SAME lowest-rank shard are broken by a fixed injective rank over txn ids — a
\* total order => SortTxns yields a UNIQUE sequence (no nondeterministic choice).
\* idRank is any fixed injection over Txn (CHOOSE one, fixed for the whole run).
idRank == CHOOSE f \in [Txn -> 0 .. (Cardinality(Txn) - 1)] :
              \A x, y \in Txn : x /= y => f[x] /= f[y]
TxnKey(t) == ShardRank[OrderShard(t)] * Cardinality(Txn) + idRank[t]

\* Sort a set of txns into a sequence by ascending TxnKey (a total order => unique).
RECURSIVE SortTxns(_)
SortTxns(S) ==
    IF S = {} THEN << >>
    ELSE LET m == CHOOSE x \in S : \A y \in S \ {x} : TxnKey(x) < TxnKey(y)
         IN <<[txn |-> m, epoch |-> GlobalEpoch(m), gshard |-> OrderShard(m)]>>
              \o SortTxns(S \ {m})

\* The PURE-FUNCTION oracle: the global log produced by sealing epochs 1..upto in
\* order, each epoch's batch = its sealable txns sorted by TxnKey. A function of
\* inputLog + upto only — no nondeterministic choice. GlobalOrderDeterministic
\* pins the live globalLog equal to this.
RECURSIVE GlobalLogUpto(_)
GlobalLogUpto(upto) ==
    IF upto = 0 THEN << >>
    ELSE GlobalLogUpto(upto - 1) \o SortTxns(EpochTxns(upto, upto))

----------------------------------------------------------------------------
Init ==
    /\ inputLog  = [s \in Shard |-> << >>]
    /\ epoch     = 1
    /\ sealed    = 0
    /\ globalLog = << >>
    /\ appliedOn = [t \in Txn |-> {}]

----------------------------------------------------------------------------
\* Actions

\* A shard s commits a txn t it TOUCHES into its input log at the open epoch. For a
\* cross-shard txn this fires once PER shard it touches (replicated into each shard's
\* Raft log). Guarded: s must touch t, and t not already on s (append-once per shard).
Commit(s, t) ==
    /\ s \in ShardsOf[t]
    /\ ~CommittedOn(s, t)
    /\ inputLog' = [inputLog EXCEPT ![s] = Append(@, [txn |-> t, epoch |-> epoch])]
    /\ UNCHANGED <<epoch, sealed, globalLog, appliedOn>>

\* Open the next epoch (bounded by MaxEpoch). Monotone.
AdvanceEpoch ==
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ UNCHANGED <<inputLog, sealed, globalLog, appliedOn>>

\* Seal the next epoch into the global log. Only a CLOSED epoch may be sealed
\* (guard sealed+1 < epoch — the open epoch has moved past it so no late commit can
\* land in it), and epochs seal strictly in order. The batch = every txn whose
\* GLOBAL epoch is sealed+1 and is fully committed, sorted deterministically, each
\* emitted EXACTLY ONCE. Recomputed from scratch (pure) to match the oracle.
SealEpoch ==
    /\ sealed + 1 < epoch
    /\ sealed' = sealed + 1
    /\ globalLog' = GlobalLogUpto(sealed + 1)
    /\ UNCHANGED <<inputLog, epoch, appliedOn>>

\* The executor applies a sealed txn's writes to ALL the shards it touches, ATOMIC:
\* one step sets appliedOn[t] to its full shard set. A txn becomes applicable only
\* once it is in the global log (sealed everywhere). This is the ALL step of all-or-
\* nothing: there is NO step that applies a strict subset of a txn's shards.
Apply(t) ==
    /\ \E p \in 1 .. Len(globalLog) : globalLog[p].txn = t
    /\ appliedOn[t] = {}
    /\ appliedOn' = [appliedOn EXCEPT ![t] = ShardsOf[t]]
    /\ UNCHANGED <<inputLog, epoch, sealed, globalLog>>

Next ==
    \/ \E s \in Shard, t \in Txn : Commit(s, t)
    \/ AdvanceEpoch
    \/ SealEpoch
    \/ \E t \in Txn : Apply(t)

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

----------------------------------------------------------------------------
\* State constraint to bound the model.
StateConstraint ==
    /\ epoch  <= MaxEpoch
    /\ sealed <= MaxEpoch
    /\ \A s \in Shard : Len(inputLog[s]) <= Cardinality(Txn)

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable. TLC must show these always hold.

\* (1) AtomicAllOrNone — THE cross-shard atomicity property. Every txn's applied
\* set is EITHER empty (committed on none of its shards yet) OR exactly the full
\* set of shards it touches (committed on ALL). NEVER a strict, non-empty subset —
\* a PARTIAL cross-shard commit (writes on some shards but not others) is forbidden.
AtomicAllOrNone ==
    \A t \in Txn : appliedOn[t] = {} \/ appliedOn[t] = ShardsOf[t]

\* (2) OneGlobalPosition — each txn appears AT MOST ONCE in the global log (the
\* per-shard appearances of a cross-shard txn collapse to one global position).
OneGlobalPosition ==
    \A p, q \in 1 .. Len(globalLog) :
        globalLog[p].txn = globalLog[q].txn => p = q

\* (3) ExactlyOnceGlobal — a txn is in the global log IFF it is sealable (fully
\* committed on all its shards into a sealed epoch). No sealable txn is lost; no
\* not-yet-fully-committed txn is fabricated into the global order.
ExactlyOnceGlobal ==
    \A t \in Txn :
        (\E p \in 1 .. Len(globalLog) : globalLog[p].txn = t)
            <=> Sealable(t, sealed)

\* (4) AppliedImpliesGlobal — the executor only applies txns that are in the global
\* log (you cannot atomically apply a txn that the sequencer has not placed). With
\* (1) this gives: applied => in global log => sealable everywhere => atomic.
AppliedImpliesGlobal ==
    \A t \in Txn :
        appliedOn[t] /= {} =>
            (\E p \in 1 .. Len(globalLog) : globalLog[p].txn = t)

\* (5) GlobalOrderDeterministic — the produced global log equals the pure from-
\* scratch recomputation (a deterministic function of inputLog + sealed alone).
\* Two replicas with the same per-shard logs + sealed boundary produce the
\* IDENTICAL global order => identical single positions => identical atomic apply.
GlobalOrderDeterministic ==
    globalLog = GlobalLogUpto(sealed)

\* (6) PerShardSubmissionPreserved — for two txns BOTH ordered by the same shard
\* (same OrderShard) in the same global epoch, their global order follows the fixed
\* TxnKey order (deterministic, stable). (The merge never makes an arbitrary
\* choice within a batch.)
PerShardSubmissionPreserved ==
    \A p, q \in 1 .. Len(globalLog) :
        (p < q /\ globalLog[p].epoch = globalLog[q].epoch
              /\ globalLog[p].gshard = globalLog[q].gshard)
            => TxnKey(globalLog[p].txn) < TxnKey(globalLog[q].txn)

\* (7) EpochMonotone — global entries appear in non-decreasing global-epoch order.
EpochMonotone ==
    /\ sealed <= epoch
    /\ \A p, q \in 1 .. Len(globalLog) :
         (p < q) => globalLog[p].epoch <= globalLog[q].epoch

----------------------------------------------------------------------------
THEOREM Spec =>
    [](AtomicAllOrNone /\ OneGlobalPosition /\ ExactlyOnceGlobal
       /\ AppliedImpliesGlobal /\ GlobalOrderDeterministic
       /\ PerShardSubmissionPreserved /\ EpochMonotone)
=============================================================================
