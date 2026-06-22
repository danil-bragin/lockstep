#pragma once

// Linearizable.hpp — C-LIN (linearizability) checker for KV REGISTERS.
//
// SPEC: specs/checker-framework.md §4 C-LIN.
//   "∃ a single total order of ALL ops consistent with per-op real-time
//    (invoke_vt<return_vt across clients) + register semantics (read returns
//    last write; cas atomic)."
//
// EXACT LEVEL (V-CHK4 — no stronger, no weaker):
//   C-LIN asserts FULL single-register linearizability, PER KEY (independent
//   registers linearize independently — a system is linearizable iff every key's
//   sub-history is). For each key it searches for ONE total order of that key's
//   ops that (a) respects real time — if op A returned before op B was invoked
//   then A precedes B — and (b) obeys register semantics: a read returns the
//   value of the most-recent preceding committed write/cas (∅ if none); a cas
//   commits iff the current value equals cas_old. If such an order exists the key
//   is linearizable; if the bounded search proves none exists, C-LIN reports a
//   VIOLATION; if the search exceeds its documented bound, C-LIN reports
//   UNDECIDED-WITHIN-BOUND — LOUDLY, never a silent pass (V-CHK4: no missed bug).
//
//   It is NOT weaker than linearizability (it does the real linearization
//   search, not a heuristic) and NOT stronger (it permits ANY real-time-and-
//   register-legal order; it does not impose session order beyond real time —
//   that overlap with C-MONO is intentional, each checker stays at its level).
//
// SEARCH APPROACH — Wing & Gong backtracking with pruning (per key):
//   * Operations considered: every op on the key. Successful writes/cas-acks are
//     "effective" mutations; reads observe; a cas-mismatch (ok=false,
//     error=cas_mismatch) is a READ-LIKE observation that the current value did
//     NOT equal cas_old (it commits nothing but constrains the order). Timed-out
//     / unavailable ops (ok=false, other errors) are OMITTED: a lost-ack op may
//     or may not have taken effect, so it imposes no constraint we can rely on
//     (omitting them keeps C-LIN sound — it never FAILS a history that a more
//     careful model would pass; the "may-have-happened" ambiguity is precisely
//     why we drop them rather than force them in).
//   * Wing-Gong frontier: an op is a legal NEXT choice iff its invoke_vt is
//     <= the minimum return_vt over all not-yet-linearized ops (so nothing that
//     must-precede-it is still pending). We try each frontier op: apply it to the
//     running register value, check it is register-legal at this point
//     (read/cas-mismatch must match the current value; a committing write/cas
//     sets it), recurse, and BACKTRACK on failure.
//   * PRUNING: a visited-set of (linearized-bitmask) states already proven
//     dead-ends, so we never re-explore an equivalent prefix. With the frontier
//     restriction this keeps the search tractable on the small per-key histories
//     the toy system produces.
//
// BOUND (documented, surfaced loudly):
//   * kMaxOpsPerKey = 64: a key with more linearizable-relevant ops than this is
//     not searched exhaustively → UNDECIDED-WITHIN-BOUND (loud).
//   * kMaxSteps     = 200000: total recursive expansions across the key's search.
//     Hitting it → UNDECIDED-WITHIN-BOUND (loud). These bounds are generous for
//     the toy workload (a few clients × tens of ops over a few keys) yet hard-cap
//     pathological blowups so the checker itself always terminates (no livelock).
//
// WITNESS SHAPE:
//   * Violation: the key + the minimal set of ops on it that cannot be ordered
//     (the key's op list rendered), with the explanation that no real-time-and-
//     register-legal order exists.
//   * Undecided: the key + which bound was hit (ops-count or step-budget), so the
//     run is flagged for human attention — NEVER counted as a pass.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting verdict. PURE deterministic
// function of the History.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/History.hpp>

namespace lockstep::harness::checkers {

class LinearizableChecker final : public Checker {
public:
    // Documented bounds (see header). Public so tests can cite them.
    static constexpr std::size_t kMaxOpsPerKey = 64;
    static constexpr std::uint64_t kMaxSteps = 200000;

    [[nodiscard]] Verdict final(const History& history) override {
        // Partition ops by key (deterministic first-appearance order).
        std::vector<std::string> keys;
        for (const Op& op : history) {
            bool seen = false;
            for (const std::string& k : keys) {
                if (k == op.key) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                keys.push_back(op.key);
            }
        }

        for (const std::string& key : keys) {
            Verdict v = check_key(history, key);
            if (!v.ok) {
                return v;  // first offending key surfaces (with its witness)
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "C-LIN"; }

    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §4 C-LIN (linearizability: one "
               "real-time + register-legal total order)";
    }

private:
    // A proven dead-end search state (linearized bitmask + register value).
    struct Visited {
        std::uint64_t mask = 0;
        bool present = false;
        std::string cur;
    };
    static constexpr std::size_t kMaxVisited = 300000;

    // A linearization-relevant op on one key.
    struct LinOp {
        std::uint64_t op_id = 0;
        std::uint64_t client = 0;
        core::Tick invoke = 0;
        core::Tick ret = 0;
        // Effect/observation classification:
        enum class Eff { Read, WriteCommit, CasCommit, CasMismatch } eff{};
        std::string value;    // read result OR committed new-value
        std::string cas_old;  // for CasCommit / CasMismatch
        // True for an op whose ACK was LOST (may or may not have committed). The
        // search may SKIP it (model "did not take effect") or PLACE it (model
        // "took effect") — trying both keeps C-LIN SOUND: it never forces a
        // may-have-happened op in, nor forbids a needed one out.
        bool optional = false;
    };

    Verdict check_key(const History& history, const std::string& key) {
        std::vector<LinOp> ops;
        for (const Op& op : history) {
            if (op.key != key) {
                continue;
            }
            LinOp lo;
            lo.op_id = op.op_id;
            lo.client = op.client_id;
            lo.invoke = op.invoke_vt;
            lo.ret = op.return_vt;
            lo.cas_old = op.cas_old;
            if (op.kind == OpKind::Read) {
                if (!op.ok) {
                    continue;  // failed read observes nothing reliable → omit
                }
                lo.eff = LinOp::Eff::Read;
                lo.value = op.result;
            } else if (op.kind == OpKind::Write) {
                // A Write that ACK'd is a mandatory committing write. A Write
                // whose ACK was LOST is OPTIONAL: it MAY have committed
                // (envelope-permitted), so the search tries both placing it and
                // skipping it — staying SOUND (never rejects a linearizable
                // history because of a may-have-happened write).
                lo.eff = LinOp::Eff::WriteCommit;
                lo.value = op.value;
                lo.optional = !op.ok;
            } else {  // Cas
                if (op.ok && op.result == "ack") {
                    lo.eff = LinOp::Eff::CasCommit;
                    lo.value = op.value;
                } else if (!op.ok && op.error == "cas_mismatch") {
                    lo.eff = LinOp::Eff::CasMismatch;  // observed cur != cas_old
                } else {
                    // Lost-ack / timed-out cas: ambiguous (may have committed).
                    // Model as an OPTIONAL committing write of its new-value: if
                    // it committed it set value; if it would have mismatched it
                    // committed nothing → skipped. (Skipping also covers the
                    // mismatch case, so this is sound — it never forces a cas in.)
                    lo.eff = LinOp::Eff::WriteCommit;
                    lo.value = op.value;
                    lo.optional = true;
                }
            }
            ops.push_back(std::move(lo));
        }

        const std::size_t n = ops.size();
        if (n == 0) {
            return verdict_ok();
        }
        if (n > kMaxOpsPerKey) {
            return undecided(key, "ops-on-key " + std::to_string(n) + " > bound " +
                                      std::to_string(kMaxOpsPerKey));
        }

        // Backtracking state: linearized bitmask, current register value+present,
        // a visited set of dead-end masks, and a step budget.
        steps_ = 0;
        budget_hit_ = false;
        visited_.clear();
        const bool ok = search(ops, /*mask=*/0, /*present=*/false,
                               /*cur=*/std::string());
        if (budget_hit_) {
            return undecided(key, "step budget " + std::to_string(kMaxSteps) +
                                      " exhausted");
        }
        if (ok) {
            return verdict_ok();
        }
        return not_linearizable(key, ops);
    }

    // Returns true if the remaining (un-set in mask) ops can be linearized from
    // register state (present, cur). Wing-Gong frontier + visited pruning.
    bool search(const std::vector<LinOp>& ops, std::uint64_t mask, bool present,
                const std::string& cur) {
        const std::size_t n = ops.size();
        if (mask == (n == 64 ? ~0ULL : ((1ULL << n) - 1))) {
            return true;  // all ops placed → a full legal order exists
        }
        if (steps_ >= kMaxSteps) {
            budget_hit_ = true;
            return false;
        }
        ++steps_;

        // Pruning: if this exact (mask, present, cur) state was already proven a
        // dead end, skip. visited_ is kept SORTED so the membership test is a
        // deterministic binary search (no hashing, no unordered container) and
        // the whole search stays tractable even near the step bound.
        const Visited probe{mask, present, cur};
        if (visited_contains(probe)) {
            return false;  // proven dead end already
        }

        // Frontier: minimum return_vt among not-yet-linearized ops.
        core::Tick min_ret = 0;
        bool first = true;
        for (std::size_t i = 0; i < n; ++i) {
            if ((mask & (1ULL << i)) != 0) {
                continue;
            }
            if (first || ops[i].ret < min_ret) {
                min_ret = ops[i].ret;
                first = false;
            }
        }

        for (std::size_t i = 0; i < n; ++i) {
            if ((mask & (1ULL << i)) != 0) {
                continue;
            }
            const LinOp& op = ops[i];
            // An OPTIONAL op (lost-ack write/cas) may be SKIPPED at any time
            // (model "it never took effect"): consume it without changing the
            // register state. Sound because a skipped op imposes no constraint.
            if (op.optional) {
                if (search(ops, mask | (1ULL << i), present, cur)) {
                    return true;
                }
                if (budget_hit_) {
                    return false;
                }
            }
            // Wing-Gong real-time legality: op may TAKE EFFECT next only if
            // nothing that must precede it is still pending — i.e. its invoke is
            // not strictly after some pending op's return (invoke <= min_ret).
            if (op.invoke > min_ret) {
                continue;
            }
            // Register legality at this point.
            bool next_present = present;
            std::string next_cur = cur;
            if (!apply(op, present, cur, next_present, next_cur)) {
                continue;  // not register-legal here → cannot be next
            }
            if (search(ops, mask | (1ULL << i), next_present, next_cur)) {
                return true;
            }
            if (budget_hit_) {
                return false;
            }
        }

        // No frontier choice worked → record this dead end (sorted insert) and
        // backtrack.
        if (visited_.size() < kMaxVisited) {
            visited_insert(Visited{mask, present, cur});
        }
        return false;
    }

    // Strict-weak ordering on Visited: (mask, present, cur). Deterministic.
    static bool visited_less(const Visited& a, const Visited& b) {
        if (a.mask != b.mask) {
            return a.mask < b.mask;
        }
        if (a.present != b.present) {
            return static_cast<int>(a.present) < static_cast<int>(b.present);
        }
        return a.cur < b.cur;
    }

    [[nodiscard]] bool visited_contains(const Visited& probe) const {
        std::size_t lo = 0;
        std::size_t hi = visited_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (visited_less(visited_[mid], probe)) {
                lo = mid + 1;
            } else if (visited_less(probe, visited_[mid])) {
                hi = mid;
            } else {
                return true;
            }
        }
        return false;
    }

    void visited_insert(const Visited& v) {
        std::size_t lo = 0;
        std::size_t hi = visited_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (visited_less(visited_[mid], v)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        visited_.insert(visited_.begin() + static_cast<std::ptrdiff_t>(lo), v);
    }

    // Apply one op to (present,cur). Returns false if it is register-illegal at
    // this point (a read/cas-mismatch contradicting the current value). On a
    // committing op, writes next_* and returns true.
    static bool apply(const LinOp& op, bool present, const std::string& cur,
                      bool& next_present, std::string& next_cur) {
        const std::string current = present ? cur : std::string();
        switch (op.eff) {
            case LinOp::Eff::Read:
                // Read must observe the current value (∅ ⇒ empty).
                if (op.value != current) {
                    return false;
                }
                next_present = present;
                next_cur = cur;
                return true;
            case LinOp::Eff::CasMismatch:
                // A mismatch proves current != cas_old at this point.
                if (current == op.cas_old) {
                    return false;
                }
                next_present = present;
                next_cur = cur;
                return true;
            case LinOp::Eff::CasCommit:
                // A committing cas requires current == cas_old, then sets value.
                if (current != op.cas_old) {
                    return false;
                }
                next_present = true;
                next_cur = op.value;
                return true;
            case LinOp::Eff::WriteCommit:
                next_present = true;
                next_cur = op.value;
                return true;
        }
        return false;
    }

    Verdict not_linearizable(const std::string& key,
                             const std::vector<LinOp>& ops) {
        std::string witness = "NOT LINEARIZABLE key=" + key + " ops=[";
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const LinOp& op = ops[i];
            witness += (i == 0 ? "" : ", ");
            witness += "{op_id=" + std::to_string(op.op_id) + " c" +
                       std::to_string(op.client) + " " + eff_name(op.eff) +
                       " val=\"" + op.value + "\"";
            if (op.eff == LinOp::Eff::CasCommit ||
                op.eff == LinOp::Eff::CasMismatch) {
                witness += " old=\"" + op.cas_old + "\"";
            }
            witness += " [" + std::to_string(static_cast<long long>(op.invoke)) +
                       "," + std::to_string(static_cast<long long>(op.ret)) + "]}";
        }
        witness += "]";
        return verdict_violation(
            std::move(witness),
            "C-LIN: no total order of this key's ops respects both real time "
            "(invoke<return across clients) AND register semantics — the "
            "sub-history is not linearizable.");
    }

    static Verdict undecided(const std::string& key, const std::string& why) {
        std::string witness =
            "UNDECIDED-WITHIN-BOUND key=" + key + " (" + why + ")";
        return verdict_violation(
            std::move(witness),
            "C-LIN: the bounded linearization search did NOT finish for this "
            "key — the result is UNDECIDED within the documented bound. This is "
            "reported LOUDLY as a non-pass (NEVER silently treated as "
            "linearizable). Raise the bound or shrink the history and re-run.");
    }

    static const char* eff_name(LinOp::Eff e) {
        switch (e) {
            case LinOp::Eff::Read:
                return "read";
            case LinOp::Eff::WriteCommit:
                return "write";
            case LinOp::Eff::CasCommit:
                return "cas-commit";
            case LinOp::Eff::CasMismatch:
                return "cas-mismatch";
        }
        return "?";
    }

    std::uint64_t steps_ = 0;
    bool budget_hit_ = false;
    std::vector<Visited> visited_;
};

}  // namespace lockstep::harness::checkers
