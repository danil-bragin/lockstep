-------------------------- MODULE CommitOrdering --------------------------
\* Lockstep — Phase 5 deterministic commit-ordering spec (SKELETON).
\*
\* Purpose: the model-checked source of truth for how one-shot transactions commit.
\* Model: transactions are sequenced into a single total order (from consensus, Phase 4) and
\* executed deterministically and SEQUENTIALLY in that order — no 2PC, no concurrency at apply.
\* The sequencer order IS the serialization order; that is what makes the default strict-serializable.
\* The implementation (Phase 5, core-no-freelance) MUST conform to this spec.
\*
\* This skeleton fully specifies STATE and the SAFETY INVARIANTS (the deliverable).
\* Command semantics and the OLLP footprint check are abstract operators with TODO markers.
\*
\* How to model-check (TLC):
\*   - Finite instance, e.g. Txn = {t1,t2,t3}, Key = {x,y}, Empty a model value.
\*   - Provide RSet, WSet as concrete functions over Txn.
\*   - Store values range over Txn \cup {Empty} (each committed write stamps its txn id as a marker).
\*   - State constraint: bound Len(seqLog) <= Cardinality(Txn). Symmetry over independent txns where valid.
\*   - Invariants: SerializedBySeqLog, ReadsMatchSerialPrefix, StoreReflectsHistory, OLLPSound, ExactlyOnce.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Txn,      \* set of transaction ids
          Key,      \* set of keys
          Empty,    \* initial / no-value marker
          RSet,     \* [Txn -> SUBSET Key]  declared (OLLP-predicted) read set
          WSet      \* [Txn -> SUBSET Key]  declared write set

VARIABLES
    seqLog,    \* Seq(Txn)            the agreed total order from consensus
    applied,   \* Nat                 entries of seqLog already executed
    store,     \* [Key -> Txn \cup {Empty}]
    status,    \* [Txn -> {"pending","sequenced","committed","aborted"}]
    history    \* Seq([txn |-> Txn, reads |-> [Key -> Txn \cup {Empty}]])  serialization history

vars == <<seqLog, applied, store, status, history>>

----------------------------------------------------------------------------
\* Abstract semantics (fill in for the concrete model)

\* Read snapshot a txn observes at execution time (deterministic: current store, sequential apply).
Snapshot(t) == [k \in RSet[t] |-> store[k]]

\* Deterministic command. Marker model: each written key gets stamped with the txn id.
\* TODO: replace with the real deterministic function of Snapshot(t).
WriteValue(t) == [k \in WSet[t] |-> t]

\* OLLP recon check: the declared footprint still matches what the txn would actually touch
\* given the current store. Mismatch => abort + (real system) re-submit with a fresh recon read.
\* TODO: model the actual-vs-predicted footprint comparison.
FootprintValid(t) == TRUE

----------------------------------------------------------------------------
\* Helper: value of key k after applying the first j committed entries of `history`.
ValueAfterPrefix(k, j) ==
    IF \E i \in 1..j : k \in WSet[history[i].txn]
    THEN LET m == CHOOSE i \in 1..j :
                     /\ k \in WSet[history[i].txn]
                     /\ \A n \in (i+1)..j : k \notin WSet[history[n].txn]
         IN history[m].txn
    ELSE Empty

----------------------------------------------------------------------------
Init ==
    /\ seqLog  = << >>
    /\ applied = 0
    /\ store   = [k \in Key |-> Empty]
    /\ status  = [t \in Txn |-> "pending"]
    /\ history = << >>

\* Consensus commits a pending txn into the global total order (Phase 4 output).
\* Real-time ordering enters here: a txn takes its place in seqLog when sequenced.
Sequence(t) ==
    /\ status[t] = "pending"
    /\ seqLog' = Append(seqLog, t)
    /\ status' = [status EXCEPT ![t] = "sequenced"]
    /\ UNCHANGED <<applied, store, history>>

\* Deterministic, sequential execution of the next sequenced txn. No concurrency, no 2PC.
Execute ==
    /\ applied < Len(seqLog)
    /\ LET t == seqLog[applied + 1] IN
        /\ status[t] = "sequenced"
        /\ applied' = applied + 1
        /\ IF FootprintValid(t)
           THEN /\ store'   = [k \in Key |-> IF k \in WSet[t] THEN WriteValue(t)[k] ELSE store[k]]
                /\ history'  = Append(history, [txn |-> t, reads |-> Snapshot(t)])
                /\ status'   = [status EXCEPT ![t] = "committed"]
           ELSE /\ status'   = [status EXCEPT ![t] = "aborted"]
                /\ UNCHANGED <<store, history>>
        \* TODO: model abort -> re-sequence with a fresh recon read, instead of terminal abort.
    /\ UNCHANGED <<seqLog>>

Next ==
    \/ \E t \in Txn : Sequence(t)
    \/ Execute

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable. TLC must show these always hold.

\* Committed transactions appear in `history` in exactly their seqLog order
\* (the sequencer order is the serialization order).
SerializedBySeqLog ==
    \A i \in 1 .. Len(history) :
        \E j \in 1 .. Len(seqLog) :
            /\ seqLog[j] = history[i].txn
            /\ \A i2 \in 1 .. (i - 1) :
                 \E j2 \in 1 .. (j - 1) : seqLog[j2] = history[i2].txn

\* Strict serializability of reads: every committed txn read exactly the state produced by the
\* committed prefix before it — never a stale or future value.
ReadsMatchSerialPrefix ==
    \A i \in 1 .. Len(history) :
        \A k \in RSet[history[i].txn] :
            history[i].reads[k] = ValueAfterPrefix(k, i - 1)

\* The live store is exactly the serial result of the committed history.
StoreReflectsHistory ==
    \A k \in Key : store[k] = ValueAfterPrefix(k, Len(history))

\* OLLP soundness: nothing commits with an invalid footprint.
OLLPSound ==
    \A i \in 1 .. Len(history) : FootprintValid(history[i].txn)

\* Each txn reaches exactly one terminal state; committed txns appear once in history.
ExactlyOnce ==
    /\ \A t \in Txn : status[t] \in {"pending","sequenced","committed","aborted"}
    /\ \A i, j \in 1 .. Len(history) : history[i].txn = history[j].txn => i = j
    /\ \A t \in Txn : (status[t] = "committed") <=> (\E i \in 1..Len(history) : history[i].txn = t)

----------------------------------------------------------------------------
\* D5 read-level contracts — implement one checker per level (invariant TODOs):
\*   SnapshotRead(k, p)      : returns ValueAfterPrefix(k, p) for some committed prefix p; internally consistent.
\*   BoundedStaleness(k, K)  : returns ValueAfterPrefix(k, p) with Len(history) - p <= K.
\*   ReadYourWrites(sess, k) : the returned prefix includes every committed write of `sess`.
\* The default path (strict-serializable) is pinned by the four invariants above.

THEOREM Spec =>
    [](SerializedBySeqLog /\ ReadsMatchSerialPrefix /\ StoreReflectsHistory /\ OLLPSound /\ ExactlyOnce)
=============================================================================
