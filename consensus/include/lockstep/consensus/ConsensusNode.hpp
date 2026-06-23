#pragma once

// ConsensusNode.hpp — Phase 4 Stage M. THE CONSENSUS SEAM.
//
// This is the interface a SINGLE Raft replica implements. It is authored
// VERIFICATION-FIRST: it exists BEFORE either real implementation (Stage I:
// impl A + impl B by two independent agents from specs/Consensus.tla), so the
// conformance / linearizability / cross-check harness (ClusterDriver.hpp,
// ConformanceCheckers.hpp, CrossCheck.hpp) can judge BOTH impls — and a teeth
// stub — through one identical surface (briefs/phase4.md Stage M).
//
// ============================================================================
// DESIGN PRINCIPLE — the seam is SPEC-ANCHORED to specs/Consensus.tla.
// ============================================================================
// Every observable below maps DIRECTLY onto a TLA+ state variable so the
// conformance checkers can assert the four model-checked safety invariants on a
// REAL running cluster (the conformance mapping in briefs/phase4.md):
//
//   TLA variable                 →  ConsensusNode observable
//   ----------------------------    -----------------------------------------
//   state[s]   ∈ {Follower,         role()      → Role { Follower, Candidate,
//                Candidate, Leader}                       Leader }
//   currentTerm[s] : Nat          →  current_term() : Term (= std::uint64_t)
//   log[s] : Seq([term,value])    →  log() : std::span<const LogEntry>
//                                       LogEntry { term, value }
//   commitIndex[s] : Nat          →  commit_index() : Index (1-based, 0 = none)
//
// votedFor[s] and votesGranted[s] are NODE-INTERNAL Raft state, not part of the
// harness-observable surface (the four safety invariants are stated over
// state/currentTerm/log/commitIndex only — ElectionSafety reads state+term,
// LogMatching/StateMachineSafety/LeaderAppendOnly read log+commitIndex). An impl
// MAY expose more for its own tests; the harness reads only what is below.
//
// ============================================================================
// HOW THE HARNESS DRIVES A NODE — lifecycle + observables.
// ============================================================================
//   * submit(value) — CLIENT request. Only a LEADER accepts (spec ClientRequest:
//     state[s] = Leader). A non-leader MUST reject (NotLeader). The returned
//     SubmitResult reports accept/reject and, on accept, the log index the leader
//     placed the entry at (so the harness can match submit→commit for the
//     linearizability check). Acceptance is NOT commitment: the entry is committed
//     only once commit_index() reaches that index AND the entry there is still
//     this value (a stale leader may have its entry overwritten — spec's conflict
//     rule). The harness derives commit from the observables, never from submit.
//
//   * OBSERVABLES (role/current_term/log/commit_index) — pure, const, side-effect
//     free reads of the node's CURRENT in-memory state. The harness snapshots all
//     N nodes at every observed step to check the safety invariants pointwise.
//
//   * LIFECYCLE the harness drives:
//       start()   — begin participating (arm election timer, serve RPC). Idempotent
//                   while running.
//       crash()   — simulated power loss. The node loses ALL non-durable in-memory
//                   state; on restart it MUST recover term/vote/log from IDisk only
//                   (spec's persist-before-reply: a vote/term/log acted upon was
//                   durable, so recovery respects it — never two leaders after a
//                   crash). After crash() the node serves nothing until restart().
//       restart() — reopen the durable IDisk image, rebuild in-memory state from
//                   the durable prefix (currentTerm, votedFor, log), re-enter as a
//                   Follower, and resume. Mirrors SimDisk crash()/recover():
//                   staged + lying bytes are gone; the durable prefix survives.
//
// The node uses INetwork for ALL peer RPC (RequestVote / AppendEntries and their
// responses — schema is the impl's choice, but it must round-trip the spec's
// message fields) and IDisk to persist currentTerm / votedFor / log BEFORE acting
// on them (spec persist-before-reply). It reads time ONLY via IClock (election /
// heartbeat timers) and randomness ONLY via IRandom (election-timeout jitter to
// break symmetric split votes). NO wall-clock, NO std::random, NO threads.
//
// ============================================================================
// HOW AN IMPL IS CONSTRUCTED — the factory (so two impls + a teeth-stub plug in
// identically). THIS SHAPE IS BINDING: Stage I impls code against it.
// ============================================================================
//   NodeConfig         — per-node identity + cluster view + timing knobs.
//   ConsensusNodeFactory = std::function<
//       std::unique_ptr<ConsensusNode>(const NodeDeps&, const NodeConfig&)>;
//   NodeDeps           — the injected boundary handles (Scheduler&, IClock&,
//                        IRandom&, INetwork endpoint, IDisk&). All non-owning
//                        references EXCEPT the per-node INetwork handle (a value),
//                        which the cluster mints from the shared SimNetworkBus.
//
// A factory returns ONE replica. The ClusterDriver calls the factory N times
// (one per node id) with that node's deps + config, then start()s each. Swapping
// impl A ↔ impl B ↔ teeth-stub is JUST swapping the factory — the driver,
// workload, fault schedule, and checkers are untouched (the dual-implementation
// cross-check, master-plan §6.5).
//
// ============================================================================
// INVARIANTS (binding; consensus/ is NOT lint-exempt):
//   * Pure function of (seed): the node draws ALL nondeterminism from the
//     injected IRandom + IClock. Same seed ⇒ byte-identical run.
//   * NO pointer into a growable container held across a co_await (V-RKV1):
//     log() returns a span valid only until the next mutation; the harness COPIES
//     what it needs each step (it never parks a span across a node step).
//   * The observables are READS: calling them must not advance the node or the
//     clock (the harness snapshots between scheduler quiescence points).
// ============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/IScheduler.hpp>

namespace lockstep::consensus {

// A node's role. Mirrors specs/Consensus.tla state[s] ∈ {Follower, Candidate,
// Leader}. ElectionSafety is stated over (role == Leader, currentTerm).
enum class Role : std::uint8_t {
    Follower = 0,
    Candidate = 1,
    Leader = 2,
};

[[nodiscard]] inline const char* role_name(Role r) noexcept {
    switch (r) {
        case Role::Follower:
            return "Follower";
        case Role::Candidate:
            return "Candidate";
        case Role::Leader:
            return "Leader";
    }
    return "?";
}

// A Raft term. specs/Consensus.tla currentTerm[s] : Nat (monotonic per node).
using Term = std::uint64_t;

// A 1-based log index. 0 means "no entry" (matches the spec's Len/commitIndex
// convention: commitIndex = 0 ⇒ nothing committed; index i refers to log[i]).
using Index = std::uint64_t;

// One replicated-log entry. Mirrors the spec entry == [term : Nat, value : Value].
// `value` is the opaque client command (we use a string so the harness can mint
// unique values and match submit→commit for linearizability). Two entries are
// "the same entry" iff BOTH term and value match (the spec compares records).
struct LogEntry {
    Term term = 0;
    std::string value;

    friend bool operator==(const LogEntry& a, const LogEntry& b) {
        return a.term == b.term && a.value == b.value;
    }
};

// The outcome of a client submit(). Only a leader accepts (spec ClientRequest
// guard state[s] = Leader); a non-leader rejects so the client can retry/redirect.
struct SubmitResult {
    bool accepted = false;  // true ⇒ this node was leader and appended the entry
    Term term = 0;          // the term the entry was appended at (when accepted)
    Index index = 0;        // 1-based log index the entry landed at (when accepted)
    // On reject, a hint to the believed leader's node id (UINT64_MAX = unknown).
    std::uint64_t leader_hint = 0;
};

// ----------------------------------------------------------------------------
// ConsensusNode — the seam. ONE replica. Authored by the impl (Stage I) /
// the teeth stub. The harness owns N of these and drives them.
// ----------------------------------------------------------------------------
class ConsensusNode {
public:
    virtual ~ConsensusNode() = default;

    // ---- client surface (spec ClientRequest) -----------------------------

    // Submit a client value. ONLY a leader accepts (returns accepted=true with the
    // term+index it appended at, per spec ClientRequest: append [term, value]). A
    // Follower/Candidate rejects (accepted=false, optional leader_hint). This is a
    // SYNCHRONOUS decision against the node's CURRENT role — it does NOT block on
    // replication/commit. The harness records the invoke, then watches commit_index
    // + log to learn if/when the entry committed (it derives commit from
    // observables, exactly as the spec's AdvanceCommitIndex governs commitIndex).
    [[nodiscard]] virtual SubmitResult submit(const std::string& value) = 0;

    // ---- observables (map onto specs/Consensus.tla state variables) -------

    // state[s]: current role. ElectionSafety reads (role==Leader, current_term).
    [[nodiscard]] virtual Role role() const noexcept = 0;

    // currentTerm[s]: this node's current term (monotonic non-decreasing).
    [[nodiscard]] virtual Term current_term() const noexcept = 0;

    // log[s]: the replicated log, index 1..N as entries 0..N-1 of the span (the
    // span is 0-based; spec index i ↦ span[i-1]). The returned view is valid only
    // until the node next mutates its log (V-RKV1): the harness COPIES it per step.
    [[nodiscard]] virtual std::span<const LogEntry> log() const noexcept = 0;

    // commitIndex[s]: highest 1-based index known committed on this node (0 = none).
    // The spec only advances this via AdvanceCommitIndex / leaderCommit adoption;
    // StateMachineSafety is stated over committed indices.
    [[nodiscard]] virtual Index commit_index() const noexcept = 0;

    // ---- lifecycle the harness drives ------------------------------------

    // Begin participating: arm the election timer, start serving RPC. Idempotent.
    virtual void start() = 0;

    // Simulated power loss: drop ALL non-durable in-memory state. The node serves
    // nothing until restart(). The backing IDisk's crash() is driven by the
    // harness alongside this (durable prefix survives; staged + lying bytes lost).
    virtual void crash() = 0;

    // Reopen the durable IDisk image and rebuild (currentTerm, votedFor, log) from
    // the surviving durable prefix, re-enter as Follower, resume. After a crash a
    // node MUST honor its durable vote/term (persist-before-reply) so failover
    // never elects two leaders in a term.
    virtual void restart() = 0;

    // This node's id (== its INetwork Endpoint id). Stable for the node's life.
    [[nodiscard]] virtual std::uint64_t id() const noexcept = 0;

    // ---- C4.2 MEMBERSHIP CHANGE (single-server) — additive, default no-op ----
    // Mirrors specs/Membership.tla. A node implementing dynamic membership tracks a
    // CONFIG CHAIN (the global, totally-ordered sequence of configs, each adjacent
    // pair differing by <= 1 server) and the config index it has adopted (cfgIdx).
    // Quorum is computed over the CURRENT config (the config this server believes it
    // is in), NOT a fixed cluster. These default to the FIXED-membership behavior so
    // a node that does not implement membership change (teeth stubs / the baseline)
    // is unaffected, and so the five base-Raft conformance checkers are byte-
    // identical when no change is ever proposed (membership is purely additive).

    // ProposeConfigChange(s) (Membership.tla): the CURRENT-term LEADER, while its
    // config is STABLE (the previous change committed — commit-before-next) and the
    // delta is a SINGLE server (add==true ⇒ add `server`; add==false ⇒ remove it),
    // appends ONE new config to the chain and begins replicating it. Returns true
    // iff the change was ADMISSIBLE and proposed (leader, settled, delta<=1, the new
    // config is non-empty and keeps the leader a member). A non-leader / unsettled /
    // non-single-server request returns false (REFUSED — the single-server rule).
    [[nodiscard]] virtual bool propose_config_change(std::uint64_t server, bool add) {
        (void)server;
        (void)add;
        return false;
    }

    // The config this server currently believes it is in (Membership.tla Cfg(s) =
    // configs[cfgIdx[s]]) — sorted node ids. Default: empty (fixed-membership node).
    [[nodiscard]] virtual std::vector<std::uint64_t> current_config() const {
        return {};
    }

    // cfgIdx[s] (Membership.tla): the chain index of the latest config this server
    // has adopted (0 = the initial config configs[1]). Monotonic non-decreasing.
    [[nodiscard]] virtual std::uint64_t config_index() const noexcept { return 0; }

    // The chain index known COMMITTED (the previous change fully propagated). When
    // this equals config_index() the chain is SETTLED and a new change may start
    // (commit-before-next). Default 0.
    [[nodiscard]] virtual std::uint64_t config_committed_index() const noexcept {
        return 0;
    }

    // ---- C4.3 snapshot introspection (OPTIONAL; default no-op) -------------
    // These are NOT part of the safety-observable surface (the five conformance
    // checkers read only role/term/log/commit_index above). They exist so the
    // snapshot conformance test can MEASURE compaction (the log prefix actually
    // discarded) and detect that a lagging follower was caught up by
    // InstallSnapshot, WITHOUT changing observable behavior. A node that does not
    // implement snapshotting (teeth stubs, the always-follower baseline) keeps the
    // defaults — zero, i.e. "never compacted", which is honest for it.

    // snapshot.lastIncludedIndex: the absolute index through which this node has
    // snapshotted + DISCARDED its in-memory log prefix (0 = no snapshot yet).
    [[nodiscard]] virtual Index snapshot_index() const noexcept { return 0; }

    // The number of entries PHYSICALLY retained in memory (the compacted suffix).
    // With snapshotting this is bounded; without it, it equals the full log length.
    [[nodiscard]] virtual std::size_t physical_log_size() const noexcept { return 0; }

    // Monotone counters: how many times this node took a snapshot / adopted a
    // leader's snapshot via InstallSnapshot. Prove compaction + catch-up actually
    // fired (the test asserts these are non-zero across the sweep — non-vacuous).
    [[nodiscard]] virtual std::uint64_t snapshots_taken() const noexcept { return 0; }
    [[nodiscard]] virtual std::uint64_t snapshots_installed() const noexcept {
        return 0;
    }
};

// ----------------------------------------------------------------------------
// CONSTRUCTION CONTRACT (binding) — how an impl is wired. Stage I impls + the
// teeth stub all take EXACTLY these deps + config via the factory below.
// ----------------------------------------------------------------------------

// The injected boundary handles for one node. All nondeterminism flows through
// these (cardinal rule 1). `net` is a per-node INetwork handle the cluster minted
// from the shared SimNetworkBus (a value type — see SimNetwork.hpp); `disk` is
// this node's durable store (one SimDisk per node). References are non-owning and
// must outlive the node (the ClusterDriver guarantees this).
struct NodeDeps {
    core::IScheduler* sched = nullptr;  // spawn server/timer coroutines onto this
    core::IClock* clock = nullptr;      // ALL time (election/heartbeat timers)
    core::IRandom* rng = nullptr;       // ALL randomness (election-timeout jitter)
    core::INetwork* net = nullptr;      // peer RPC (this node's bus endpoint)
    core::IDisk* disk = nullptr;        // durable term/vote/log (persist-before-reply)
};

// Per-node configuration: identity, the full cluster membership (so the node knows
// the quorum size + whom to RPC), and timing knobs. The cluster view is the set of
// ALL node ids (including self); quorum = floor(N/2)+1 (spec Quorum). Election /
// heartbeat windows are in virtual ticks; the impl draws a per-election jitter in
// [election_timeout_min, election_timeout_max] from IRandom to break split votes.
struct NodeConfig {
    std::uint64_t self_id = 0;
    std::vector<std::uint64_t> cluster;  // ALL node ids (sorted; includes self_id)

    // C4.2 MEMBERSHIP: the INITIAL config configs[1] of the Membership.tla chain —
    // the set of node ids the cluster STARTS with (a subset of `cluster`, the
    // universe of all servers that may EVER participate, == Server in the spec). A
    // single-server change later adds/removes one of `cluster`'s ids to/from this.
    // EMPTY ⇒ init_config = cluster (fixed membership; backward-compatible: every
    // existing test leaves this empty, so quorum is over the full cluster exactly as
    // before and behavior is byte-identical).
    std::vector<std::uint64_t> init_config;

    core::Tick election_timeout_min = 15;  // randomized election timeout window
    core::Tick election_timeout_max = 30;
    core::Tick heartbeat_interval = 5;     // leader AppendEntries heartbeat period

    // Bounded client-side patience: how long submit-driven traffic waits before the
    // harness considers an op timed out (kept here so the impl + harness agree).
    core::Tick request_deadline = 200;
};

// THE FACTORY. Returns ONE replica wired to its deps + config. The ClusterDriver
// calls it once per node id. Swapping the factory swaps the whole implementation
// (impl A ↔ impl B ↔ teeth-stub) behind the identical driver + checkers — this is
// the dual-implementation cross-check seam (master-plan §6.5).
using ConsensusNodeFactory =
    std::function<std::unique_ptr<ConsensusNode>(const NodeDeps&, const NodeConfig&)>;

}  // namespace lockstep::consensus
