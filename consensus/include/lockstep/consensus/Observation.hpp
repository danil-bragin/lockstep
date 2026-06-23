#pragma once

// Observation.hpp — Phase 4 Stage M. The OBSERVED-RUN data the conformance
// checkers judge. The ClusterDriver snapshots all N nodes' observables
// (ConsensusNode role/current_term/log/commit_index) at every observed step and
// records the submit→commit client history. The checkers (ConformanceCheckers.hpp)
// map specs/Consensus.tla's four safety invariants onto this trace.
//
// Everything here is a PLAIN VALUE copied out of the live nodes at snapshot time
// (V-RKV1: never a span/pointer into a node's growable log held across a step).
// Pure data; no clock, no randomness, no scheduling.
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output, any nondeterminism.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/core/IClock.hpp>  // core::Tick (virtual time)
#include <lockstep/consensus/ConsensusNode.hpp>

namespace lockstep::consensus {

// A by-value snapshot of one node's observables at one instant. A deep COPY of
// the node's log (not a span) so the checkers can hold it for the whole run.
struct NodeSnapshot {
    std::uint64_t node_id = 0;
    bool live = true;  // false ⇒ node was crashed/killed at this step (serves none)
    Role role = Role::Follower;
    Term term = 0;
    Index commit_index = 0;
    std::vector<LogEntry> log;  // deep copy: log[i] is spec index (i+1)

    // C4.3 snapshot MEASUREMENT (introspection only; NOT a safety observable —
    // the five conformance checkers never read these). Captures how far this node
    // has compacted and how bounded its physical (in-memory) log is, so the
    // snapshot conformance test can prove compaction + InstallSnapshot fired and
    // that the retained log stays bounded while the logical log keeps growing.
    Index snapshot_index = 0;         // absolute index discarded up to (0 = none)
    std::size_t physical_log_size = 0;  // entries physically retained in memory
    std::uint64_t snapshots_taken = 0;
    std::uint64_t snapshots_installed = 0;
};

// All N nodes snapshotted at one observed step (one virtual-time instant).
struct ClusterSnapshot {
    core::Tick vt = 0;          // virtual time of the snapshot
    std::uint64_t step = 0;     // monotonic snapshot sequence (deterministic order)
    std::vector<NodeSnapshot> nodes;  // sorted by node_id (deterministic iteration)
};

// One client submit→commit observation (for the linearizability checker). A
// value is submitted to the believed leader; if accepted at (term,index) we then
// watch the cluster until that index is committed holding this value (commit) or
// the entry there is overwritten / the deadline passes (no-commit).
struct SubmitObservation {
    std::uint64_t op_id = 0;        // monotonic, unique
    std::uint64_t client_id = 0;
    std::string value;              // the submitted command (unique per submit)
    core::Tick invoke_vt = 0;       // when submit was issued
    core::Tick return_vt = 0;       // when committed (or gave up)
    bool accepted = false;          // a leader accepted the append
    bool committed = false;         // the entry committed (durably ordered)
    Term term = 0;                  // term it was appended at (when accepted)
    Index index = 0;                // 1-based commit index (when committed)
};

// The full observed run the checkers consume. Snapshots are in step order;
// commits are in op_id order. Pure data, deterministic.
struct ObservedRun {
    std::uint64_t seed = 0;
    std::uint64_t n_nodes = 0;
    std::vector<ClusterSnapshot> snapshots;       // every observed step
    std::vector<SubmitObservation> submits;       // client submit→commit history

    // The FINAL committed log, as the harness reconstructs it: the longest commit
    // prefix any node ever observed, with each committed slot's (term,value). The
    // cross-check (CrossCheck.hpp) compares this between two implementations.
    // Filled by the driver after the run.
    std::vector<LogEntry> committed_log;

    // DEFENSE-IN-DEPTH (root-cause-independent). True iff the driver's bounded
    // step backstop tripped during run_until: forward progress STALLED at one
    // virtual time (a zero-virtual-time message storm — e.g. a snapshot-install
    // ping-pong that never advances the clock). A correct run NEVER trips this;
    // it being true means the run was cut short before its virtual-time deadline
    // and a test MUST fail loudly (not hang the host). Deterministic: the cap is a
    // pure fn of cfg, so a storm trips identically across replays.
    bool progress_stalled = false;
};

}  // namespace lockstep::consensus
