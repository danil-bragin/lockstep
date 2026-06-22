#pragma once

// Integrity.hpp — C-INT (integrity) checker.
//
// SPEC: specs/checker-framework.md §4 C-INT.
//   "∀ read → returns a value that was actually written (or the initial ∅).
//    ⊥ fabricated/torn value. ∀ ack'd write → eventually observable;
//    ⊥ lost ack'd write, ⊥ phantom write."
//
// EXACT LEVEL (V-CHK4 — no stronger, no weaker):
//   C-INT is a VALUE-PROVENANCE check. It judges ONLY whether every observed
//   value is traceable to a real operation; it does NOT judge ordering
//   (that is C-LIN), session monotonicity (C-MONO), or durability across crash
//   (C-DUR). It asserts the two halves of integrity that are decidable from the
//   client-op History alone:
//
//   (INT-1) NO FABRICATED / TORN / PHANTOM READ. Every successful read of key k
//           returns either ∅ (empty result) OR a value that some mutation
//           (Write or Cas) on k carried as its new-value somewhere in the run.
//           A read returning a value no client ever tried to write for k is
//           fabricated (or a torn/bit-rot value the storage manufactured).
//           NOTE: the value need only have been WRITTEN by *some* op (ack'd or
//           not) — a read may legitimately observe a write whose ack was lost
//           (envelope-permitted). What it may NOT observe is a value that was
//           never offered to k at all. (The stricter "only ack'd writes may
//           become visible" claim is C-DUR's job, not C-INT's — keeping C-INT
//           at exactly its level.)
//
//   (INT-2) NO LOST ACK'D WRITE. Every write/cas that was ACKNOWLEDGED (ok) and
//           whose value was the LAST acknowledged mutation of k (by real time)
//           and is never subsequently overwritten by a later ack'd mutation,
//           must be OBSERVABLE: some read of k that is invoked strictly after
//           that write returned must return that value (or a value from an
//           equally-late-or-later ack'd write). If, after an ack'd write of v to
//           k with no later ack'd mutation of k, EVERY later read of k returns ∅
//           or a strictly-earlier value, the ack'd write was silently lost.
//           This half is intentionally CONSERVATIVE (it only fires when there is
//           a clean, unambiguous "later reads all contradict the ack" witness)
//           so C-INT never false-alarms on a merely-reordered-but-present write
//           (ordering subtleties belong to C-LIN).
//
// WITNESS SHAPE:
//   INT-1: the offending read op (op_id, client, key, result) + a note that the
//          value was never written to that key. Replayable via the stamped seed.
//   INT-2: the ack'd write op (op_id, key, value) that was lost + the set of
//          later reads that all failed to observe it.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting verdict. This checker is a
// PURE, DETERMINISTIC function of the History (ordered iteration only).

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/History.hpp>

namespace lockstep::harness::checkers {

class IntegrityChecker final : public Checker {
public:
    [[nodiscard]] Verdict final(const History& history) override {
        // --- INT-1: every read value is traceable to a write on that key. -----
        // Collect, per key, the set of values ANY mutation offered (deterministic
        // ordered insertion; we keep a sorted unique vector, no hashing).
        std::vector<KeyVals> written;  // sorted by key
        for (const Op& op : history) {
            if (op.kind == OpKind::Write || op.kind == OpKind::Cas) {
                // The mutation's new-value is op.value. An empty value is a
                // legitimate write of ∅, recorded so a later ∅-read traces.
                add_written(written, op.key, op.value);
            }
        }

        for (const Op& op : history) {
            if (op.kind != OpKind::Read || !op.ok) {
                continue;  // only successful reads carry an observed value
            }
            if (op.result.empty()) {
                continue;  // ∅ is always legitimate (initial / deleted register)
            }
            if (!value_written(written, op.key, op.result)) {
                std::string witness =
                    "FABRICATED READ op_id=" + std::to_string(op.op_id) +
                    " client=" + std::to_string(op.client_id) +
                    " key=" + op.key + " result=\"" + op.result +
                    "\" (no Write/Cas ever offered this value to this key)";
                return verdict_violation(
                    std::move(witness),
                    "C-INT/INT-1: a read returned a value that was never "
                    "written to that key — a fabricated/torn/phantom value.");
            }
        }

        // --- INT-2: no silently-lost ack'd write. -----------------------------
        // For each key, find the LAST (by return_vt, seq) acknowledged mutation.
        // If it is never overwritten by a later ack'd mutation, then every read
        // of k invoked strictly after it returned must be able to observe it (or
        // a later ack'd value). If ALL such later reads contradict it (∅ or a
        // value from a strictly-earlier ack'd mutation), the ack was lost.
        Verdict dur = check_no_lost_ack(history, written);
        if (!dur.ok) {
            return dur;
        }

        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "C-INT"; }

    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §4 C-INT (integrity: no "
               "fabricated/torn/phantom value; no lost ack'd write)";
    }

private:
    struct KeyVals {
        std::string key;
        std::vector<std::string> values;  // sorted unique
    };

    // Insert (key,value) into the sorted-by-key, sorted-unique-values structure.
    static void add_written(std::vector<KeyVals>& w, const std::string& key,
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

    static bool value_written(const std::vector<KeyVals>& w,
                              const std::string& key, const std::string& value) {
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

    // INT-2 helper. Conservative lost-ack detection (see header note).
    static Verdict check_no_lost_ack(const History& history,
                                     const std::vector<KeyVals>& /*written*/) {
        // Distinct keys, in deterministic (history) order of first appearance.
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
            // The last acknowledged mutation of key (history is (return_vt,seq)
            // ordered, so the last such op in iteration order is the latest).
            const Op* last_ack = nullptr;
            for (const Op& op : history) {
                if (op.key != key || !op.ok) {
                    continue;
                }
                const bool committed =
                    op.kind == OpKind::Write ||
                    (op.kind == OpKind::Cas && op.result == "ack");
                if (committed) {
                    last_ack = &op;
                }
            }
            if (last_ack == nullptr) {
                continue;  // no ack'd mutation on this key → nothing to lose
            }

            // Gather every read of key invoked strictly after last_ack returned.
            // If at least one such read exists and EVERY one of them returns a
            // value that is NOT last_ack->value AND is empty-or-not-the-ack'd
            // value, the ack'd write is unobservable → lost. (Conservative: we
            // require there to be a later read at all, and require ALL of them to
            // contradict, so a present-but-reordered value never trips this.)
            bool any_later_read = false;
            bool any_observed = false;
            std::string contra;
            int contra_count = 0;
            for (const Op& op : history) {
                if (op.kind != OpKind::Read || !op.ok || op.key != key) {
                    continue;
                }
                if (op.invoke_vt <= last_ack->return_vt) {
                    continue;  // not strictly after the ack returned
                }
                any_later_read = true;
                if (op.result == last_ack->value) {
                    any_observed = true;
                    break;
                }
                if (contra_count < 4) {
                    contra += " read_op_id=" + std::to_string(op.op_id) +
                              " got=\"" + op.result + "\"";
                    ++contra_count;
                }
            }

            if (any_later_read && !any_observed) {
                std::string witness =
                    "LOST ACK'D WRITE key=" + key +
                    " ack_op_id=" + std::to_string(last_ack->op_id) +
                    " value=\"" + last_ack->value + "\"; later reads:" + contra;
                return verdict_violation(
                    std::move(witness),
                    "C-INT/INT-2: an acknowledged write of the last value to a "
                    "key with no later ack'd mutation was never observable by "
                    "any subsequent read — the ack'd write was lost.");
            }
        }
        return verdict_ok();
    }
};

}  // namespace lockstep::harness::checkers
