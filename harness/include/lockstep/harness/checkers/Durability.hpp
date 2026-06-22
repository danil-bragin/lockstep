#pragma once

// Durability.hpp — C-DUR (durability across crash+recover) checker.
//
// SPEC: specs/checker-framework.md §4 C-DUR.
//   "after node crash+recover, every write ack'd-before-crash AND
//    truly-durable → still present; lying-fsync'd → MAY be absent
//    (envelope-permitted), but ⊥ a NON-ack'd write appear,
//    ⊥ a committed value silently change."
//
// EXACT LEVEL (V-CHK4 — no stronger, no weaker):
//   The History is CLIENT-OP-LEVEL (spec §2 DECISION-B): it records no explicit
//   crash markers. C-DUR therefore asserts the crash-durability guarantees that
//   ARE decidable from the client's observations, and NOTHING the envelope
//   permits the system to lose:
//
//   (DUR-1) NO NON-COMMITTED WRITE APPEARS. The system's own report is the
//           ground truth for "non-ack'd": a Cas the system answered with
//           error=cas_mismatch DID NOT commit — it carried a new-value w that the
//           system explicitly refused. If a later read returns w and NO other
//           op (a Write, or a committing Cas) ever carried w for that key, then a
//           write the system said it rejected has nonetheless surfaced — a
//           phantom / non-ack'd write appearing post-fault. (We require NO other
//           legitimate source so we never false-alarm when two ops coincidentally
//           share a value — keeping C-DUR at exactly its level. The toy workload
//           tags values uniquely per (client,op), so this is precise there.)
//
//           IMPORTANT scoping vs C-INT: C-INT/INT-1 permits a read to observe a
//           write whose ACK was merely lost in transit (it truly committed at the
//           leader; envelope-permitted). C-DUR does NOT contradict that — a
//           lost-ack Write still carried its value as a real mutation, so it is a
//           legitimate source. C-DUR fires ONLY on a value whose sole origin is a
//           cas the system REPORTED as not-committed. That is the durability-
//           specific teeth: a rejected mutation must never become durable.
//
//   (DUR-2) A COMMITTED VALUE DOES NOT SILENTLY CHANGE INTO A FABRICATION. Every
//           non-∅ value a read observes for k must be traceable to a real
//           mutation that OFFERED that value to k (a Write/Cas new-value). A read
//           returning a value no mutation ever offered means a committed register
//           silently mutated into a value nothing wrote — a torn/bit-rot/phantom
//           durability failure. (This shares the provenance floor with C-INT by
//           design; C-DUR states it as the crash-durability invariant "a durable
//           value cannot spontaneously become something never written". Each
//           checker asserts its own level; overlap is allowed, gaps are not.)
//
// WITNESS SHAPE:
//   DUR-1: the rejected cas op (op_id, key, refused new-value) + the later read
//          (op_id, result) that surfaced it, with a note that no committing op
//          ever carried that value. Replayable via the stamped seed.
//   DUR-2: the offending read (op_id, key, result) + note "never written".
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting verdict. PURE function of
// the History.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/History.hpp>

namespace lockstep::harness::checkers {

class DurabilityChecker final : public Checker {
public:
    [[nodiscard]] Verdict final(const History& history) override {
        // Per key: values offered by a COMMITTING mutation (Write ok, or Cas-ack,
        // or a lost-ack Write whose value is still a real mutation), and values
        // offered ONLY by a REJECTED cas (error=cas_mismatch). Both sets are
        // sorted-unique; no hashing, deterministic order.
        std::vector<KeyVals> committed;  // legitimate sources
        std::vector<KeyVals> rejected;   // cas-mismatch new-values

        for (const Op& op : history) {
            if (op.kind == OpKind::Write) {
                // A Write (ack'd OR lost-ack) is a real mutation source: a lost
                // ack still committed at the leader (envelope-permitted to be
                // observed). It is a LEGITIMATE provenance source for DUR-1.
                add(committed, op.key, op.value);
            } else if (op.kind == OpKind::Cas) {
                if (op.ok && op.result == "ack") {
                    add(committed, op.key, op.value);
                } else if (!op.ok && op.error == "cas_mismatch") {
                    add(rejected, op.key, op.value);
                } else {
                    // lost-ack / timed-out cas: ambiguous (may have committed).
                    // Treat as a legitimate source so C-DUR does not false-alarm
                    // on an envelope-permitted lost ack.
                    add(committed, op.key, op.value);
                }
            }
        }

        for (const Op& op : history) {
            if (op.kind != OpKind::Read || !op.ok || op.result.empty()) {
                continue;
            }
            const bool legit = contains(committed, op.key, op.result);
            if (legit) {
                continue;  // observed value has a real committing source → fine
            }
            // Not from any committing source. Is it from a REJECTED cas only?
            if (contains(rejected, op.key, op.result)) {
                std::string witness =
                    "REJECTED WRITE SURFACED key=" + op.key + " value=\"" +
                    op.result + "\" read_op_id=" + std::to_string(op.op_id) +
                    " client=" + std::to_string(op.client_id) +
                    " (the system answered a cas carrying this value with "
                    "cas_mismatch — it must never become durable)";
                return verdict_violation(
                    std::move(witness),
                    "C-DUR/DUR-1: a write the system REPORTED as not-committed "
                    "(cas_mismatch) nevertheless appeared in a later read — a "
                    "non-ack'd write surfaced across the fault.");
            }
            // From nothing at all → fabricated durable value.
            std::string witness =
                "FABRICATED DURABLE VALUE key=" + op.key + " value=\"" +
                op.result + "\" read_op_id=" + std::to_string(op.op_id) +
                " client=" + std::to_string(op.client_id) +
                " (no mutation ever offered this value to this key)";
            return verdict_violation(
                std::move(witness),
                "C-DUR/DUR-2: a read observed a value no mutation ever wrote — a "
                "committed register silently changed into a fabricated value "
                "(torn/bit-rot/phantom durability failure).");
        }

        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "C-DUR"; }

    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §4 C-DUR (durability: no non-ack'd "
               "write appears; a committed value does not silently change)";
    }

private:
    struct KeyVals {
        std::string key;
        std::vector<std::string> values;  // sorted unique
    };

    static void add(std::vector<KeyVals>& w, const std::string& key,
                    const std::string& value) {
        std::size_t pos = 0;
        while (pos < w.size() && w[pos].key < key) {
            ++pos;
        }
        if (pos == w.size() || w[pos].key != key) {
            KeyVals kv;
            kv.key = key;
            w.insert(w.begin() + static_cast<std::ptrdiff_t>(pos), std::move(kv));
        }
        std::vector<std::string>& vals = w[pos].values;
        std::size_t vp = 0;
        while (vp < vals.size() && vals[vp] < value) {
            ++vp;
        }
        if (vp == vals.size() || vals[vp] != value) {
            vals.insert(vals.begin() + static_cast<std::ptrdiff_t>(vp), value);
        }
    }

    static bool contains(const std::vector<KeyVals>& w, const std::string& key,
                         const std::string& value) {
        for (const KeyVals& kv : w) {
            if (kv.key == key) {
                for (const std::string& v : kv.values) {
                    if (v == value) {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }
};

}  // namespace lockstep::harness::checkers
