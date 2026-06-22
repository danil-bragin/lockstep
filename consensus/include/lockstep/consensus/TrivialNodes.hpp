#pragma once

// TrivialNodes.hpp — Phase 4 Stage M. Baseline + teeth STUBS that plug into the
// SAME seam (ConsensusNode.hpp) + driver (ClusterDriver.hpp) as the (future) real
// impls. NO real consensus algorithm lives here — only the minimal scripted
// observable behaviour the harness needs to (a) verify the no-op baseline is
// vacuously safe and (b) prove the conformance checkers have TEETH.
//
//   NoOpFollowerNode   — the "always-follower / no-op" baseline. NEVER becomes a
//                        leader, NEVER appends, NEVER commits. The four safety
//                        invariants hold VACUOUSLY (no leader ⇒ ElectionSafety
//                        trivially true; empty logs ⇒ LogMatching/StateMachine/
//                        LeaderAppendOnly trivially true). Linearizability holds
//                        vacuously too — BUT it makes NO PROGRESS, which the teeth
//                        test asserts explicitly (no submit ever commits), so a
//                        vacuously-safe-but-dead system is NOT mistaken for a
//                        correct one.
//
//   ScriptedNode       — a single scripted replica whose observables the teeth
//                        test forces into a SPECIFIC spec violation. Each "fault"
//                        variant targets exactly one Consensus.tla invariant so the
//                        matching conformance checker MUST flag it. A scripted node
//                        is a deliberately-WRONG ConsensusNode: a harness that does
//                        not flag it IS the bug (briefs/phase4.md Stage M teeth).
//
// These stubs do NOT use INetwork/IDisk for real replication — they are not an
// implementation; they exist to exercise the JUDGING machinery. They still take
// the full NodeDeps/NodeConfig via the factory (identical wiring), proving the
// seam admits any plug. All behaviour is a pure function of the driver's seeded
// stepping (no node-local randomness/clock reads beyond what the script needs).
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, any nondeterminism.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/consensus/ConsensusNode.hpp>

namespace lockstep::consensus {

// ----------------------------------------------------------------------------
// NoOpFollowerNode — the vacuously-safe baseline. Always Follower, term 0, empty
// log, commit 0. Accepts nothing (submit always rejects: not a leader).
// ----------------------------------------------------------------------------
class NoOpFollowerNode final : public ConsensusNode {
public:
    explicit NoOpFollowerNode(std::uint64_t id) noexcept : id_(id) {}

    [[nodiscard]] SubmitResult submit(const std::string&) override {
        SubmitResult r;
        r.accepted = false;  // never a leader ⇒ always rejects (spec ClientRequest)
        r.leader_hint = UINT64_MAX;
        return r;
    }

    [[nodiscard]] Role role() const noexcept override { return Role::Follower; }
    [[nodiscard]] Term current_term() const noexcept override { return 0; }
    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        return {};
    }
    [[nodiscard]] Index commit_index() const noexcept override { return 0; }

    void start() override {}
    void crash() override {}
    void restart() override {}
    [[nodiscard]] std::uint64_t id() const noexcept override { return id_; }

    // Factory: every node is a no-op follower. The whole cluster makes no
    // progress (no leader) — vacuously safe; the teeth test asserts no-progress.
    [[nodiscard]] static ConsensusNodeFactory factory() {
        return [](const NodeDeps&, const NodeConfig& nc)
                   -> std::unique_ptr<ConsensusNode> {
            return std::make_unique<NoOpFollowerNode>(nc.self_id);
        };
    }

private:
    std::uint64_t id_;
};

// ----------------------------------------------------------------------------
// ScriptedNode — a wrong replica whose observables the harness reads. The variant
// determines WHICH Consensus.tla invariant it breaks. The node mutates its own
// scripted state in response to driver stepping (it watches its OWN submit calls
// and the passage of role/term it presents) so the snapshots the driver takes
// capture the violation.
// ----------------------------------------------------------------------------
enum class TeethFault : std::uint8_t {
    // Every node claims Leader in the SAME term ⇒ two leaders in one term.
    // Breaks ElectionSafety.
    TwoLeadersSameTerm,
    // The (single) leader appends client entries, then SHORTENS its own log on a
    // later step (drops the last entry it was leader for). Breaks LeaderAppendOnly.
    LeaderTruncatesOwnLog,
    // The leader appends + "commits" entries, but on a later step OVERWRITES an
    // already-committed slot with a different value at the same index. Breaks
    // StateMachineSafety (committed entry lost/changed) AND Linearizability.
    DropCommittedEntry,
    // Two nodes present logs that share an (index,term) but disagree on the prefix
    // up to it. Breaks LogMatching.
    LogMatchingViolation,
    // NOT a violation on its own: a single leader honestly appends + commits the
    // real client values (no clobber, no truncate). Used by the cross-check as the
    // "other" implementation so a DropCommittedEntry node DIVERGES from it at the
    // clobbered index — proving the cross-check itself has teeth. On its own it
    // passes every conformance checker (a degenerate-but-correct single-leader run).
    HonestSingleLeader,
    // Like HonestSingleLeader but its leader commits a DIFFERENTLY-TAGGED value at
    // every index ("ALT:"+value). On its OWN it passes every conformance checker
    // (still a single total order). But its committed log DIFFERS from
    // HonestSingleLeader's at index 1 ⇒ the dual-impl cross-check MUST flag the
    // pair (two impls committing different entries at a shared index for the same
    // seed = at least one is wrong). This is the cross-check's teeth.
    HonestSingleLeaderAlt,
};

class ScriptedNode final : public ConsensusNode {
public:
    ScriptedNode(std::uint64_t id, TeethFault fault)
        : id_(id), fault_(fault) {}

    // The leader-electing nodes accept submits; the script appends + (later)
    // misbehaves. For TwoLeadersSameTerm + LogMatchingViolation every node is a
    // "leader" so any node accepts.
    [[nodiscard]] SubmitResult submit(const std::string& value) override {
        if (role() != Role::Leader) {
            SubmitResult r;
            r.accepted = false;
            return r;
        }
        // Append the entry at term 1 (the scripted term). The Alt variant tags
        // its committed values so its committed log differs from the plain honest
        // one (the cross-check teeth).
        const std::string stored =
            fault_ == TeethFault::HonestSingleLeaderAlt ? ("ALT:" + value) : value;
        log_.push_back(LogEntry{/*term=*/1, stored});
        // Script the per-fault misbehaviour driven by submit count.
        apply_script();
        SubmitResult r;
        r.accepted = true;
        r.term = 1;
        r.index = static_cast<Index>(log_.size());
        return r;
    }

    [[nodiscard]] Role role() const noexcept override {
        switch (fault_) {
            case TeethFault::TwoLeadersSameTerm:
                // EVERY node is Leader (so >= 2 leaders in the same term).
                return Role::Leader;
            case TeethFault::LogMatchingViolation:
                // Both nodes present as Leader is unnecessary here; node 0 leads.
                return id_ == 0 ? Role::Leader : Role::Follower;
            case TeethFault::LeaderTruncatesOwnLog:
            case TeethFault::DropCommittedEntry:
            case TeethFault::HonestSingleLeader:
            case TeethFault::HonestSingleLeaderAlt:
                // Lowest id is the (single) leader; others are followers.
                return id_ == 0 ? Role::Leader : Role::Follower;
        }
        return Role::Follower;
    }

    [[nodiscard]] Term current_term() const noexcept override {
        // Scripted term is 1 for every fault (so "same term" is shared).
        return 1;
    }

    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        if (fault_ == TeethFault::LogMatchingViolation) {
            return std::span<const LogEntry>(scripted_log_.data(),
                                             scripted_log_.size());
        }
        return std::span<const LogEntry>(log_.data(), log_.size());
    }

    [[nodiscard]] Index commit_index() const noexcept override {
        return commit_;
    }

    void start() override {
        if (fault_ == TeethFault::LogMatchingViolation) {
            // Two nodes share (index 2, term 1) but disagree on index 1: same
            // term at index 2, different entry at index 1 ⇒ LogMatching breaks.
            if (id_ == 0) {
                scripted_log_ = {LogEntry{1, "A1"}, LogEntry{1, "X2"}};
            } else if (id_ == 1) {
                scripted_log_ = {LogEntry{1, "B1"}, LogEntry{1, "X2"}};
            }
        }
    }
    void crash() override {}
    void restart() override {}
    [[nodiscard]] std::uint64_t id() const noexcept override { return id_; }

    [[nodiscard]] static ConsensusNodeFactory factory(TeethFault fault) {
        return [fault](const NodeDeps&, const NodeConfig& nc)
                   -> std::unique_ptr<ConsensusNode> {
            return std::make_unique<ScriptedNode>(nc.self_id, fault);
        };
    }

private:
    // Misbehave based on the submit count, so the violation lands in the trace.
    void apply_script() {
        switch (fault_) {
            case TeethFault::LeaderTruncatesOwnLog:
                // Grow to a high-water mark of >= 3 entries (visible across
                // snapshots while the driver waits for commit), THEN on the next
                // submit truncate back to length 1 — a forbidden non-append-only
                // mutation. The grow-then-shrink across consecutive leader@same-
                // term snapshots is exactly what LeaderAppendOnly forbids.
                if (log_.size() >= 4 && !truncated_) {
                    log_.resize(1);
                    truncated_ = true;
                }
                break;
            case TeethFault::DropCommittedEntry:
                // Mark everything committed (so index 1's ORIGINAL value is
                // committed + snapshotted across the first few submits), THEN once
                // the log has grown to >= 4 entries OVERWRITE the already-committed
                // slot 1 with a bogus value — losing a committed entry. The
                // high-water threshold guarantees the original committed value was
                // observed before the clobber, so the across-run check catches the
                // change (deterministically, every seed).
                commit_ = static_cast<Index>(log_.size());
                if (log_.size() >= 4 && !committed_clobbered_) {
                    log_[0] = LogEntry{1, "CLOBBERED"};
                    committed_clobbered_ = true;
                }
                break;
            case TeethFault::HonestSingleLeader:
            case TeethFault::HonestSingleLeaderAlt:
                // Honest: append + commit the (possibly tagged) values, never
                // mutate the past. Self-consistent ⇒ passes every checker alone.
                commit_ = static_cast<Index>(log_.size());
                break;
            case TeethFault::TwoLeadersSameTerm:
            case TeethFault::LogMatchingViolation:
                // The violation is purely in role/log presentation; no per-submit
                // mutation needed.
                break;
        }
    }

    std::uint64_t id_;
    TeethFault fault_;
    std::vector<LogEntry> log_;
    std::vector<LogEntry> scripted_log_;  // for LogMatchingViolation
    Index commit_ = 0;
    bool committed_clobbered_ = false;
    bool truncated_ = false;
};

}  // namespace lockstep::consensus
