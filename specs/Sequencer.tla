--------------------------- MODULE Sequencer ---------------------------
\* Lockstep — Phase 4 C4.4 SEQUENCER spec (the multi-shard global total order).
\*
\* Purpose: the model-checked source of truth for how the per-shard committed logs
\* (Phase 4, Raft, one consensus order PER shard) combine into ONE global deterministic
\* total order. This global order is the linearization point the distributed-txn layer
\* (Phase 5, CommitOrdering.tla) consumes: CommitOrdering already ASSUMES a single global
\* seqLog; the sequencer is what PRODUCES that seqLog from N shards.
\*
\* Model (Calvin-style epoch batching, D2 pure log-order):
\*   - A few shards, each with a committed input log: a sequence of txn ids in that shard's
\*     fixed consensus order. Per-shard order is immutable once committed (Phase 4 gives it).
\*   - Each committed entry is tagged with the EPOCH in which it was committed.
\*   - The sequencer advances an epoch boundary one epoch at a time, IN ORDER. When it seals
\*     epoch e it appends to the global log the batch for epoch e: every shard's epoch-e
\*     entries, merged by a FIXED deterministic rule — sort the batch by (shardId, perShardIdx).
\*     That rule is a total order on (shard, index) so the merge is a pure function with no
\*     nondeterministic choice. Per-shard relative order is preserved because within one shard
\*     the index is strictly increasing.
\*
\* The deterministic merge being a PURE FUNCTION of the shard logs + epoch boundary is the whole
\* point: GlobalLogUpto(sealed) below recomputes the global log from scratch, and the invariant
\* GlobalOrderDeterministic asserts the live produced log always equals that pure recomputation.
\*
\* How to model-check (TLC):
\*   - Finite instance, e.g. Shard = {sh1, sh2}, Txn = {a, b, c, d}, MaxEpoch = 2.
\*   - State constraint bounds epoch + per-shard log length so the space is finite.
\*   - Invariants: GlobalOrderDeterministic, PerShardOrderPreserved, EpochMonotone,
\*     ExactlyOnceGlobal, NoLossSealed.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Shard,      \* set of shard ids (each runs its own Phase-4 consensus log)
          Txn,        \* set of transaction ids (the things being ordered)
          ShardOf,    \* [Txn -> Shard]  which shard owns (commits) each txn
          MaxEpoch,   \* Nat  search bound: highest epoch that may be opened/sealed
          sh1, sh2,   \* model values for the shard ids (so the instance operators can name them)
          a, b, c, d  \* model values for the txn ids

\* A total order on shard ids, used as the tie-break key in the deterministic merge.
\* Defined in the .tla (a function literal is not legal in a .cfg) and `<-`-mapped in the .cfg.
\* Any total order works; the only requirement for determinism is that it is total + fixed.
CONSTANT ShardRank    \* [Shard -> Nat]  injective rank giving a total order over shards

\* Concrete instance functions. The .cfg `<-`-maps the CONSTANTS ShardOf / ShardRank onto these
\* operators (function literals like (a :> sh1) are not legal in a .cfg, so they live here).
\* sh1/sh2 and a/b/c/d are model values declared in the .cfg.
\* a,b are owned (committed) by sh1; c,d by sh2. ShardRank is injective (sh1 -> 0, sh2 -> 1):
\* the fixed total order over shards used as the deterministic merge tie-break.
ShardOfDef   == (a :> sh1 @@ b :> sh1 @@ c :> sh2 @@ d :> sh2)
ShardRankDef == (sh1 :> 0 @@ sh2 :> 1)

----------------------------------------------------------------------------
VARIABLES
    inputLog,   \* [Shard -> Seq([txn : Txn, epoch : Nat])]
                \*   each shard's committed input log: txn ids in consensus order, epoch-tagged.
                \*   Append-only; per-shard order is fixed by Phase-4 consensus.
    epoch,      \* Nat  the current OPEN epoch shards may commit into (the "now" epoch)
    sealed,     \* Nat  highest epoch the sequencer has SEALED into the global log
                \*   (epochs 1..sealed are done; sealed < epoch means batches still pending)
    globalLog   \* Seq([txn : Txn, shard : Shard, idx : Nat, epoch : Nat])
                \*   the produced global total order. idx = the entry's per-shard 1-based index.

vars == <<inputLog, epoch, sealed, globalLog>>

----------------------------------------------------------------------------
\* Helpers

\* The set of per-shard entries (as records with their position) that belong to epoch e.
\* An entry is identified by (shard s, index i); it carries the txn and its epoch tag.
EpochEntries(e) ==
    UNION { { [shard |-> s, idx |-> i, txn |-> inputLog[s][i].txn, epoch |-> e]
                : i \in { j \in 1 .. Len(inputLog[s]) : inputLog[s][j].epoch = e } }
            : s \in Shard }

\* The FIXED deterministic total order on (shard, idx) used to merge a batch.
\* Sort key: (ShardRank[shard], idx). ShardRank is injective so ties only occur within a shard,
\* where idx (strictly increasing along a shard's log) breaks them — a total, deterministic order.
LessEntry(e1, e2) ==
    \/ ShardRank[e1.shard] < ShardRank[e2.shard]
    \/ /\ ShardRank[e1.shard] = ShardRank[e2.shard]
       /\ e1.idx < e2.idx

\* Deterministically sort a finite set of entries into a sequence by LessEntry.
\* (Pure function: with a total order LessEntry there is exactly ONE sorted sequence.)
SortEntries(S) ==
    LET RECURSIVE Build(_)
        Build(remaining) ==
            IF remaining = {} THEN << >>
            ELSE LET m == CHOOSE x \in remaining :
                             \A y \in remaining \ {x} : LessEntry(x, y)
                 IN <<m>> \o Build(remaining \ {m})
    IN Build(S)

\* The deterministic global log PRODUCED by sealing epochs 1..upto, in epoch order, each epoch's
\* batch merged by SortEntries. This is the PURE-FUNCTION oracle: a function of inputLog + upto
\* only, with no reference to the live globalLog. GlobalOrderDeterministic pins globalLog to it.
RECURSIVE GlobalLogUpto(_)
GlobalLogUpto(upto) ==
    IF upto = 0 THEN << >>
    ELSE GlobalLogUpto(upto - 1) \o SortEntries(EpochEntries(upto))

----------------------------------------------------------------------------
Init ==
    /\ inputLog  = [s \in Shard |-> << >>]
    /\ epoch     = 1
    /\ sealed    = 0
    /\ globalLog = << >>

----------------------------------------------------------------------------
\* Actions

\* A shard commits (via its own consensus) a txn into its input log, tagged with the OPEN epoch.
\* Per-shard order is whatever order shards commit in — fixed once appended (append-only).
\* Guard: a txn appears in at most one shard's log (its owner) and at most once overall.
Commit(s, t) ==
    /\ ShardOf[t] = s
    /\ \A sh \in Shard : \A i \in 1 .. Len(inputLog[sh]) : inputLog[sh][i].txn /= t  \* not yet committed
    /\ inputLog' = [inputLog EXCEPT ![s] = Append(@, [txn |-> t, epoch |-> epoch])]
    /\ UNCHANGED <<epoch, sealed, globalLog>>

\* Open the next epoch: stop accepting commits into `epoch`, move "now" forward. Bounded by MaxEpoch.
\* (Epochs are global wall-time-free batching boundaries; advancing is deterministic and monotone.)
AdvanceEpoch ==
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ UNCHANGED <<inputLog, sealed, globalLog>>

\* Seal the next epoch into the global log: only a CLOSED epoch may be sealed, and epochs are
\* sealed strictly in order (sealed -> sealed+1). Appending the deterministic batch for sealed+1.
\* "Closed" = sealed+1 < epoch, i.e. the open epoch has moved past it so no more entries can land
\* in it (this is what makes EpochEntries(sealed+1) final at seal time — no future appends).
SealEpoch ==
    /\ sealed + 1 < epoch                       \* epoch sealed+1 is closed (open epoch moved past it)
    /\ sealed' = sealed + 1
    /\ globalLog' = globalLog \o SortEntries(EpochEntries(sealed + 1))
    /\ UNCHANGED <<inputLog, epoch>>

Next ==
    \/ \E s \in Shard, t \in Txn : Commit(s, t)
    \/ AdvanceEpoch
    \/ SealEpoch

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

----------------------------------------------------------------------------
\* State constraint to bound the model. Per-shard log length and epoch are capped; sealed follows.
StateConstraint ==
    /\ epoch  <= MaxEpoch
    /\ sealed <= MaxEpoch
    /\ \A s \in Shard : Len(inputLog[s]) <= Cardinality(Txn)

----------------------------------------------------------------------------
\* SAFETY INVARIANTS — the deliverable. TLC must show these always hold.

\* (1) GlobalOrderDeterministic. The produced global log is a DETERMINISTIC function of the
\* per-shard logs + the sealed boundary: it equals the pure recomputation GlobalLogUpto(sealed),
\* which references only inputLog + sealed (never any nondeterministic choice). Two runs with the
\* same shard logs and the same sealed boundary therefore produce the IDENTICAL global order.
GlobalOrderDeterministic ==
    globalLog = GlobalLogUpto(sealed)

\* (2) PerShardOrderPreserved. For any two global entries from the SAME shard, their relative
\* order in the global log equals their per-shard consensus order (the index order). The merge
\* never reorders within a shard.
PerShardOrderPreserved ==
    \A p, q \in 1 .. Len(globalLog) :
        (globalLog[p].shard = globalLog[q].shard /\ p < q)
            => globalLog[p].idx < globalLog[q].idx

\* (3) EpochMonotone. Entries appear in the global log in non-decreasing epoch order (epoch e's
\* whole batch precedes epoch e+1's), and epochs are sealed in order (sealed within bounds).
EpochMonotone ==
    /\ sealed <= epoch
    /\ \A p, q \in 1 .. Len(globalLog) :
         (p < q) => globalLog[p].epoch <= globalLog[q].epoch

\* (4) ExactlyOnceGlobal. Every sealed input entry appears in the global log EXACTLY ONCE
\* (no loss, no duplication): no two distinct global positions share the same (shard, idx), and
\* every committed (shard, idx) entry whose epoch is sealed is present exactly once.
ExactlyOnceGlobal ==
    /\ \A p, q \in 1 .. Len(globalLog) :
         (globalLog[p].shard = globalLog[q].shard /\ globalLog[p].idx = globalLog[q].idx)
             => p = q
    /\ \A s \in Shard : \A i \in 1 .. Len(inputLog[s]) :
         (inputLog[s][i].epoch <= sealed)
             => (\E p \in 1 .. Len(globalLog) :
                    globalLog[p].shard = s /\ globalLog[p].idx = i)

\* (5) NoLossSealed. The converse direction: every entry IN the global log corresponds to a real
\* committed input entry with a sealed epoch (nothing fabricated). Together with ExactlyOnceGlobal
\* this is "the sealed prefix of every shard's input log appears in the global log exactly once".
NoLossSealed ==
    \A p \in 1 .. Len(globalLog) :
        LET e == globalLog[p] IN
            /\ e.idx \in 1 .. Len(inputLog[e.shard])
            /\ inputLog[e.shard][e.idx].txn = e.txn
            /\ inputLog[e.shard][e.idx].epoch = e.epoch
            /\ e.epoch <= sealed

----------------------------------------------------------------------------
THEOREM Spec =>
    [](GlobalOrderDeterministic /\ PerShardOrderPreserved /\ EpochMonotone
       /\ ExactlyOnceGlobal /\ NoLossSealed)
=============================================================================
