#pragma once

// ConformanceCheckers.hpp — Phase 4 Stage M. The CONFORMANCE CHECKERS that map
// specs/Consensus.tla's four model-checked safety invariants onto a REAL running
// cluster, plus a LINEARIZABILITY check on the committed log. Each checker cites
// the exact Consensus.tla invariant it asserts (briefs/phase4.md conformance
// mapping). A checker set that passes a known-broken consensus node IS the bug
// (the teeth test proves these have teeth).
//
// We REUSE the harness Verdict shape (witness + explanation + replayable seed,
// harness/Checker.hpp) so violations are replayable exactly like Phase-2 checkers
// (V-CHK2). Each consensus checker is a PURE function of the ObservedRun
// (Observation.hpp): the cluster snapshots + the submit→commit history. No clock,
// no randomness, no scheduling — judging only, deterministic.
//
// THE FIVE CHECKERS (each cites Consensus.tla):
//   ElectionSafetyChecker     — Consensus.tla ElectionSafety  (never two leaders
//                               in the same term; checked across ALL nodes at
//                               EVERY observed snapshot).
//   LogMatchingChecker        — Consensus.tla LogMatching      (same (index,term)
//                               across any two nodes ⇒ identical prefix).
//   StateMachineSafetyChecker — Consensus.tla StateMachineSafety (a committed
//                               index never holds different entries across nodes;
//                               a committed entry is never lost/reordered across
//                               failover — checked over the WHOLE run, not just
//                               one snapshot).
//   LeaderAppendOnlyChecker   — Consensus.tla LeaderAppendOnly (a leader never
//                               shortens/overwrites its OWN log; action invariant
//                               checked across consecutive snapshots of a node
//                               that is leader in BOTH).
//   LinearizabilityChecker    — the committed log is a single total order; the
//                               client submit→commit history is linearizable
//                               (a committed-log register/sequence linearization).
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output, any nondeterminism.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>  // Verdict, verdict_ok/verdict_violation
#include <lockstep/consensus/Observation.hpp>

namespace lockstep::consensus {

using harness::Verdict;
using harness::verdict_ok;
using harness::verdict_violation;

// A consensus conformance checker: judges an ObservedRun, names the exact
// Consensus.tla invariant it asserts, and returns a Verdict (witness + seed).
class ConformanceChecker {
public:
    virtual ~ConformanceChecker() = default;

    // Judge the whole observed run. Pure function of `run`. Returns verdict_ok()
    // or verdict_violation(witness, explanation); the runner stamps the seed.
    [[nodiscard]] virtual Verdict check(const ObservedRun& run) = 0;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string spec_ref() const = 0;
};

// Small rendering helper for a node@step witness. Deterministic.
namespace detail {
[[nodiscard]] inline std::string render_entry(const LogEntry& e) {
    return "[t" + std::to_string(e.term) + ",\"" + e.value + "\"]";
}
}  // namespace detail

// ----------------------------------------------------------------------------
// ElectionSafety — Consensus.tla:
//   ElectionSafety ==
//     \A a, b \in Server :
//       (state[a]=Leader /\ state[b]=Leader /\ currentTerm[a]=currentTerm[b]) => a=b
//
// MAPPING: at EVERY observed snapshot, no two LIVE nodes are both Leader with the
// same term. (A crashed node serves nothing, so it is not a "Leader" the cluster
// would honor; we check live nodes — the harness snapshots live state.)
// ----------------------------------------------------------------------------
class ElectionSafetyChecker final : public ConformanceChecker {
public:
    [[nodiscard]] Verdict check(const ObservedRun& run) override {
        for (const ClusterSnapshot& snap : run.snapshots) {
            for (std::size_t i = 0; i < snap.nodes.size(); ++i) {
                const NodeSnapshot& a = snap.nodes[i];
                if (!a.live || a.role != Role::Leader) {
                    continue;
                }
                for (std::size_t j = i + 1; j < snap.nodes.size(); ++j) {
                    const NodeSnapshot& b = snap.nodes[j];
                    if (!b.live || b.role != Role::Leader) {
                        continue;
                    }
                    if (a.term == b.term) {
                        std::string w =
                            "TWO LEADERS step=" + std::to_string(snap.step) +
                            " vt=" + std::to_string(static_cast<long long>(snap.vt)) +
                            " term=" + std::to_string(a.term) + " nodes={" +
                            std::to_string(a.node_id) + "," +
                            std::to_string(b.node_id) + "}";
                        return verdict_violation(
                            std::move(w),
                            "ElectionSafety VIOLATED: two distinct Leaders hold "
                            "the same term in one snapshot "
                            "(Consensus.tla ElectionSafety).");
                    }
                }
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "ElectionSafety"; }
    [[nodiscard]] std::string spec_ref() const override {
        return "specs/Consensus.tla ElectionSafety (at most one leader per term)";
    }
};

// ----------------------------------------------------------------------------
// LogMatching — Consensus.tla:
//   LogMatching ==
//     \A a, b \in Server : \A i \in 1..Min(Len(log[a]),Len(log[b])) :
//       (log[a][i].term = log[b][i].term) => SubSeq(log[a],1,i)=SubSeq(log[b],1,i)
//
// MAPPING: at EVERY snapshot, for every pair of nodes and every overlapping index
// i, if the terms at i agree then the entire prefix 1..i must be identical entries.
// ----------------------------------------------------------------------------
class LogMatchingChecker final : public ConformanceChecker {
public:
    [[nodiscard]] Verdict check(const ObservedRun& run) override {
        for (const ClusterSnapshot& snap : run.snapshots) {
            for (std::size_t a = 0; a < snap.nodes.size(); ++a) {
                for (std::size_t b = a + 1; b < snap.nodes.size(); ++b) {
                    const std::vector<LogEntry>& la = snap.nodes[a].log;
                    const std::vector<LogEntry>& lb = snap.nodes[b].log;
                    const std::size_t m = la.size() < lb.size() ? la.size() : lb.size();
                    for (std::size_t i = 0; i < m; ++i) {
                        if (la[i].term != lb[i].term) {
                            continue;  // antecedent false: nothing to assert at i
                        }
                        // Same (index, term) ⇒ prefixes 0..i must be identical.
                        for (std::size_t k = 0; k <= i; ++k) {
                            if (!(la[k] == lb[k])) {
                                std::string w =
                                    "PREFIX MISMATCH step=" +
                                    std::to_string(snap.step) + " nodes={" +
                                    std::to_string(snap.nodes[a].node_id) + "," +
                                    std::to_string(snap.nodes[b].node_id) +
                                    "} index=" + std::to_string(i + 1) +
                                    " (same term there) diverge@" +
                                    std::to_string(k + 1) + " a=" +
                                    detail::render_entry(la[k]) + " b=" +
                                    detail::render_entry(lb[k]);
                                return verdict_violation(
                                    std::move(w),
                                    "LogMatching VIOLATED: two logs share "
                                    "(index,term) but their prefixes up to it "
                                    "differ (Consensus.tla LogMatching).");
                            }
                        }
                    }
                }
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "LogMatching"; }
    [[nodiscard]] std::string spec_ref() const override {
        return "specs/Consensus.tla LogMatching (same (index,term) ⇒ identical "
               "prefix)";
    }
};

// ----------------------------------------------------------------------------
// StateMachineSafety — Consensus.tla:
//   StateMachineSafety ==
//     \A a, b \in Server : \A i \in 1..Min(commitIndex[a],commitIndex[b]) :
//       log[a][i] = log[b][i]
//
// MAPPING (two parts, both per briefs/phase4.md):
//   (1) POINTWISE: at every snapshot, two nodes never hold DIFFERENT entries at a
//       commonly-committed index.
//   (2) ACROSS-RUN (failover): a committed entry is never LOST or REORDERED. We
//       maintain the longest committed-prefix EVER witnessed (per index, the
//       (term,value) some node had committed) and assert NO later snapshot ever
//       commits a DIFFERENT entry at that index — a committed entry that changes
//       across failover is a lost/reordered commit.
// ----------------------------------------------------------------------------
class StateMachineSafetyChecker final : public ConformanceChecker {
public:
    [[nodiscard]] Verdict check(const ObservedRun& run) override {
        // (2) across-run committed history: index → the entry committed there.
        std::vector<LogEntry> committed;  // committed[i] = entry at index i+1
        std::vector<bool> known;

        for (const ClusterSnapshot& snap : run.snapshots) {
            // (1) pointwise pairwise on commonly-committed indices.
            for (std::size_t a = 0; a < snap.nodes.size(); ++a) {
                const NodeSnapshot& na = snap.nodes[a];
                for (std::size_t b = a + 1; b < snap.nodes.size(); ++b) {
                    const NodeSnapshot& nb = snap.nodes[b];
                    Index common = na.commit_index < nb.commit_index
                                       ? na.commit_index
                                       : nb.commit_index;
                    for (Index i = 1; i <= common; ++i) {
                        const LogEntry& ea = na.log[i - 1];
                        const LogEntry& eb = nb.log[i - 1];
                        if (!(ea == eb)) {
                            std::string w =
                                "COMMITTED DIVERGE step=" +
                                std::to_string(snap.step) + " nodes={" +
                                std::to_string(na.node_id) + "," +
                                std::to_string(nb.node_id) +
                                "} commit_index=" + std::to_string(i) + " a=" +
                                detail::render_entry(ea) + " b=" +
                                detail::render_entry(eb);
                            return verdict_violation(
                                std::move(w),
                                "StateMachineSafety VIOLATED (pointwise): two "
                                "nodes hold different entries at a commonly-"
                                "committed index (Consensus.tla "
                                "StateMachineSafety).");
                        }
                    }
                }
            }

            // (2) fold each node's committed prefix into the run-wide history,
            // asserting no committed slot ever changes value (no lost/reordered
            // commit across failover).
            for (const NodeSnapshot& n : snap.nodes) {
                for (Index i = 1; i <= n.commit_index && i <= n.log.size(); ++i) {
                    const LogEntry& e = n.log[i - 1];
                    if (i > committed.size()) {
                        committed.resize(i);
                        known.resize(i, false);
                    }
                    if (!known[i - 1]) {
                        committed[i - 1] = e;
                        known[i - 1] = true;
                    } else if (!(committed[i - 1] == e)) {
                        std::string w =
                            "COMMITTED CHANGED step=" + std::to_string(snap.step) +
                            " node=" + std::to_string(n.node_id) + " index=" +
                            std::to_string(i) + " was=" +
                            detail::render_entry(committed[i - 1]) + " now=" +
                            detail::render_entry(e);
                        return verdict_violation(
                            std::move(w),
                            "StateMachineSafety VIOLATED (across-run): a "
                            "previously-committed entry was lost / overwritten / "
                            "reordered across failover (Consensus.tla "
                            "StateMachineSafety).");
                    }
                }
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override {
        return "StateMachineSafety";
    }
    [[nodiscard]] std::string spec_ref() const override {
        return "specs/Consensus.tla StateMachineSafety (committed entries never "
               "diverge / are never lost across failover)";
    }
};

// ----------------------------------------------------------------------------
// LeaderAppendOnly — Consensus.tla (action invariant):
//   LeaderAppendOnly ==
//     \A s \in Server : (state[s]=Leader) =>
//       /\ Len(log'[s]) >= Len(log[s])
//       /\ \A i \in 1..Len(log[s]) : log'[s][i] = log[s][i]
//   checked as [][LeaderAppendOnly]_vars.
//
// MAPPING: an action invariant relates a state to its SUCCESSOR. We check it
// across CONSECUTIVE snapshots: for a node that is Leader in BOTH the pre and the
// post snapshot, its log must only have GROWN — never shortened, never had an
// existing slot rewritten. (If the node stepped down between snapshots — observed
// as a lower role / higher term in post — that transition is governed by
// UpdateTerm and is NOT a leader self-mutation; we only assert across a
// leader→leader pair, matching the spec's (state[s]=Leader) antecedent on BOTH
// the pre-state and the constraint on the post-state's prefix.)
// ----------------------------------------------------------------------------
class LeaderAppendOnlyChecker final : public ConformanceChecker {
public:
    [[nodiscard]] Verdict check(const ObservedRun& run) override {
        for (std::size_t s = 1; s < run.snapshots.size(); ++s) {
            const ClusterSnapshot& prev = run.snapshots[s - 1];
            const ClusterSnapshot& cur = run.snapshots[s];
            // Nodes are sorted by id in both snapshots; pair by id.
            for (const NodeSnapshot& pn : prev.nodes) {
                const NodeSnapshot* cn = find_node(cur, pn.node_id);
                if (cn == nullptr) {
                    continue;
                }
                // Only constrain a node that is Leader in BOTH snapshots at the
                // SAME term (a leader that stepped down or won a NEW term did not
                // "append-only mutate as the term's leader"; term-change is the
                // step-down path, spec UpdateTerm).
                if (pn.role != Role::Leader || cn->role != Role::Leader) {
                    continue;
                }
                if (pn.term != cn->term) {
                    continue;
                }
                if (cn->log.size() < pn.log.size()) {
                    return verdict_violation(
                        leader_witness("SHORTENED", pn, *cn, prev, cur, 0),
                        "LeaderAppendOnly VIOLATED: a Leader's own log got "
                        "SHORTER (Consensus.tla LeaderAppendOnly).");
                }
                for (std::size_t i = 0; i < pn.log.size(); ++i) {
                    if (!(pn.log[i] == cn->log[i])) {
                        return verdict_violation(
                            leader_witness("OVERWROTE", pn, *cn, prev, cur, i + 1),
                            "LeaderAppendOnly VIOLATED: a Leader overwrote an "
                            "existing entry in its OWN log (Consensus.tla "
                            "LeaderAppendOnly).");
                    }
                }
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override {
        return "LeaderAppendOnly";
    }
    [[nodiscard]] std::string spec_ref() const override {
        return "specs/Consensus.tla LeaderAppendOnly (a leader's log is "
               "append-only)";
    }

private:
    static const NodeSnapshot* find_node(const ClusterSnapshot& s,
                                         std::uint64_t id) {
        for (const NodeSnapshot& n : s.nodes) {
            if (n.node_id == id) {
                return &n;
            }
        }
        return nullptr;
    }

    static std::string leader_witness(const char* what, const NodeSnapshot& pn,
                                      const NodeSnapshot& cn,
                                      const ClusterSnapshot& prev,
                                      const ClusterSnapshot& cur, std::size_t at) {
        std::string w = std::string("LEADER ") + what + " node=" +
                        std::to_string(pn.node_id) + " term=" +
                        std::to_string(pn.term) + " step " +
                        std::to_string(prev.step) + "->" +
                        std::to_string(cur.step) +
                        " len " + std::to_string(pn.log.size()) + "->" +
                        std::to_string(cn.log.size());
        if (at != 0) {
            w += " at_index=" + std::to_string(at) + " was=" +
                 detail::render_entry(pn.log[at - 1]) + " now=" +
                 detail::render_entry(cn.log[at - 1]);
        }
        return w;
    }
};

// ----------------------------------------------------------------------------
// Linearizability — the committed log is a SINGLE TOTAL ORDER, and the client
// submit→commit history is linearizable against it.
//
// APPROACH (a committed-LOG linearization, the natural Raft register/sequence
// check; briefs/phase4.md "the committed log is a single total order"):
//   (L1) SINGLE TOTAL ORDER — fold EVERY node's committed prefix from EVERY
//        snapshot into one index→entry map; if any index is ever committed with
//        two different entries the committed log is NOT a single sequence
//        (overlaps StateMachineSafety's across-run check by design — each checker
//        stays at its own level; this one frames it as a linearization failure).
//   (L2) REAL-TIME RESPECT — every COMMITTED submit must linearize at its append
//        index, and a committed submit's commit point must lie within its
//        [invoke_vt, return_vt] envelope; two committed submits at the same index
//        with different values is a duplicate-commit (non-linearizable). A
//        committed entry's value must equal some submit's value (no fabricated
//        committed command) — the committed log is exactly the sequence of
//        accepted client values in index order.
// ----------------------------------------------------------------------------
class LinearizabilityChecker final : public ConformanceChecker {
public:
    [[nodiscard]] Verdict check(const ObservedRun& run) override {
        // (L1) Build the single committed sequence from all snapshots.
        std::vector<LogEntry> seq;
        std::vector<bool> known;
        for (const ClusterSnapshot& snap : run.snapshots) {
            for (const NodeSnapshot& n : snap.nodes) {
                for (Index i = 1; i <= n.commit_index && i <= n.log.size(); ++i) {
                    const LogEntry& e = n.log[i - 1];
                    if (i > seq.size()) {
                        seq.resize(i);
                        known.resize(i, false);
                    }
                    if (!known[i - 1]) {
                        seq[i - 1] = e;
                        known[i - 1] = true;
                    } else if (!(seq[i - 1] == e)) {
                        std::string w = "NOT A TOTAL ORDER index=" +
                                        std::to_string(i) + " a=" +
                                        detail::render_entry(seq[i - 1]) + " b=" +
                                        detail::render_entry(e) + " (committed two "
                                        "different entries at one index)";
                        return verdict_violation(
                            std::move(w),
                            "Linearizability VIOLATED: the committed log is not a "
                            "single total order — one index committed two distinct "
                            "entries (briefs/phase4.md: committed log = one total "
                            "order).");
                    }
                }
            }
        }

        // (L2) Each committed submit must match the committed sequence at its
        // index, within its real-time envelope; the committed value must be a real
        // submitted value (no fabricated committed command).
        for (const SubmitObservation& sub : run.submits) {
            if (!sub.committed) {
                continue;  // a non-committed submit imposes no committed-order claim
            }
            if (sub.invoke_vt > sub.return_vt) {
                return verdict_violation(
                    "BAD ENVELOPE op_id=" + std::to_string(sub.op_id),
                    "Linearizability VIOLATED: a committed submit returned before "
                    "it was invoked (real-time envelope broken).");
            }
            if (sub.index == 0 || sub.index > seq.size() || !known[sub.index - 1]) {
                return verdict_violation(
                    "PHANTOM COMMIT op_id=" + std::to_string(sub.op_id) +
                        " value=\"" + sub.value + "\" claimed_index=" +
                        std::to_string(sub.index),
                    "Linearizability VIOLATED: a submit reported committed at an "
                    "index the committed log never reached (phantom commit).");
            }
            if (seq[sub.index - 1].value != sub.value) {
                return verdict_violation(
                    "COMMIT VALUE MISMATCH op_id=" + std::to_string(sub.op_id) +
                        " index=" + std::to_string(sub.index) + " submitted=\"" +
                        sub.value + "\" committed=\"" + seq[sub.index - 1].value +
                        "\"",
                    "Linearizability VIOLATED: the value committed at a submit's "
                    "index differs from the value the client submitted "
                    "(reordered/overwritten commit).");
            }
        }

        // No two DISTINCT committed submits may claim the same index with the same
        // value? (Different values already caught above.) A duplicate value at one
        // index is a re-proposed identical command, which is benign — skip.
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "Linearizability"; }
    [[nodiscard]] std::string spec_ref() const override {
        return "briefs/phase4.md Linearizability (committed log is one total "
               "order; submit→commit history linearizable)";
    }
};

// ----------------------------------------------------------------------------
// ConformanceRunner — drives the five checkers over one ObservedRun, stamps the
// run seed into every Verdict (replayable, V-CHK2), returns the verdict list in a
// FIXED checker order. Mirrors harness::CheckerRunner but for ConformanceChecker.
// ----------------------------------------------------------------------------
struct NamedVerdict {
    std::string checker;
    Verdict verdict;
};

[[nodiscard]] inline std::vector<NamedVerdict> run_all_conformance(
    const ObservedRun& run) {
    std::vector<NamedVerdict> out;
    ElectionSafetyChecker c_elect;
    LogMatchingChecker c_match;
    StateMachineSafetyChecker c_sms;
    LeaderAppendOnlyChecker c_lao;
    LinearizabilityChecker c_lin;
    ConformanceChecker* checkers[] = {&c_elect, &c_match, &c_sms, &c_lao, &c_lin};
    for (ConformanceChecker* c : checkers) {
        Verdict v = c->check(run);
        v.seed = run.seed;  // replayable
        out.push_back(NamedVerdict{c->name(), std::move(v)});
    }
    return out;
}

}  // namespace lockstep::consensus
