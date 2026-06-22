#pragma once

// SessionMonotonic.hpp — C-MONO (read-your-writes / session-monotonic) checker.
//
// SPEC: specs/checker-framework.md §4 C-MONO.
//   "within a client session, a read ⊥ observe an EARLIER value than that
//    session already observed/wrote for k."
//
// EXACT LEVEL (V-CHK4 — no stronger, no weaker):
//   C-MONO is a PER-SESSION, PER-KEY ordering check. It judges ONLY the values a
//   SINGLE client session sees relative to what THAT SAME session already
//   observed or wrote — it says nothing about cross-client order (C-LIN) or
//   value provenance (C-INT). It needs a reliable "earlier/later" relation on
//   values; the only one decidable from the client-op History without coupling
//   to storage internals is a session's OWN writes, which the workload tags with
//   a per-client, strictly-increasing op-index (value "c<client>_v<i>"). So
//   C-MONO asserts exactly two session-local guarantees, derived from the
//   session's own acknowledged writes:
//
//   (MONO-1) READ-YOUR-WRITES. After a session ACK's a write/cas of value v to
//            key k, every LATER read of k BY THE SAME SESSION (invoke strictly
//            after the write returned) must NOT return ∅ and must NOT return one
//            of that session's OWN STRICTLY-EARLIER writes to k. (It MAY return
//            v, a later own-write, or another client's value — C-MONO does not
//            constrain cross-client values, only that the session does not lose
//            sight of its own committed work or move backward over it.)
//
//   (MONO-2) MONOTONIC-READS over own writes. The sequence of the session's own
//            write-values that it READS BACK for k must be non-decreasing in that
//            session's write-index: once a session has read back its own write
//            index i for k, a later read by that session of k must not return its
//            OWN write of a strictly-smaller index i' < i.
//
//   Both are conservative on purpose: they fire ONLY on an own-write going
//   backward (an unambiguous read-your-writes violation), never on observing
//   another client's value (which is a linearization concern, left to C-LIN).
//
// WITNESS SHAPE: the prior own-write (op_id, key, value, index) the session is
//   entitled to + the offending later read (op_id, key, result) that went
//   backward (∅ or a smaller own-index). Replayable via the stamped seed.
//
// VALUE-INDEX DECODING: a value matches this session's own write iff it has the
//   form "c<client_id>_v<i>"; <i> is its monotonic write-index. Values not of
//   that shape (or another client's) are simply not "own writes" and never count
//   as going-backward — keeping C-MONO at exactly its level. The decode is a
//   pure string parse (no std::*_distribution, no locale, deterministic).
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

class SessionMonotonicChecker final : public Checker {
public:
    [[nodiscard]] Verdict final(const History& history) override {
        // Per (client,key): the highest own-write index the session has either
        // ACKNOWLEDGED writing or READ BACK so far. A later read of that key by
        // the session must not drop below it for the session's OWN values, and
        // must not return ∅ once the session has an acknowledged write.
        std::vector<SessionKey> state;  // sorted by (client,key)

        // History is (return_vt, seq)-ordered: a faithful real-time order. We
        // walk it once; "later" = appears later in this order with strictly
        // greater invoke_vt than the prior event's return_vt where it matters.
        for (const Op& op : history) {
            if (!op.ok) {
                continue;  // errored ops observe/commit nothing
            }
            SessionKey& sk = slot(state, op.client_id, op.key);

            if (op.kind == OpKind::Write || op.kind == OpKind::Cas) {
                if (op.result != "ack") {
                    continue;  // a non-committing cas (mismatch) writes nothing
                }
                long long idx = own_index(op.client_id, op.value);
                if (idx >= 0) {
                    sk.has_ack = true;
                    sk.ack_return_vt = op.return_vt;
                    if (idx > sk.high_idx) {
                        sk.high_idx = idx;
                        sk.high_value = op.value;
                        sk.high_op_id = op.op_id;
                    }
                }
                continue;
            }

            // op.kind == Read (ok).
            if (!sk.has_ack) {
                continue;  // nothing the session is entitled to yet
            }
            // Only constrain reads invoked strictly after the entitling write
            // returned (real-time read-your-writes: a read concurrent with the
            // write is not obligated to see it).
            if (op.invoke_vt <= sk.ack_return_vt) {
                continue;
            }

            // MONO-1: must not return ∅ after an acknowledged own write.
            if (op.result.empty()) {
                return backward_violation(sk, op, "returned ∅");
            }

            long long ridx = own_index(op.client_id, op.result);
            if (ridx >= 0) {
                // MONO-2: own value read back must not be a strictly-smaller
                // index than the highest own-index the session is entitled to.
                if (ridx < sk.high_idx) {
                    return backward_violation(
                        sk, op,
                        "returned own write index v" + std::to_string(ridx) +
                            " < entitled v" + std::to_string(sk.high_idx));
                }
                // Reading back an own value at/above high → advance the floor.
                if (ridx > sk.high_idx) {
                    sk.high_idx = ridx;
                    sk.high_value = op.result;
                }
            }
            // A read returning another client's value is allowed (cross-client
            // order is C-LIN's concern). It neither advances nor violates.
        }

        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override { return "C-MONO"; }

    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §4 C-MONO "
               "(read-your-writes / session-monotonic)";
    }

private:
    struct SessionKey {
        std::uint64_t client = 0;
        std::string key;
        bool has_ack = false;
        long long high_idx = -1;       // highest own write-index entitled
        std::string high_value;        // its value (for the witness)
        std::uint64_t high_op_id = 0;  // the entitling write op
        core::Tick ack_return_vt = 0;  // when the latest own write returned
    };

    static SessionKey& slot(std::vector<SessionKey>& v, std::uint64_t client,
                            const std::string& key) {
        std::size_t pos = 0;
        while (pos < v.size() &&
               (v[pos].client < client ||
                (v[pos].client == client && v[pos].key < key))) {
            ++pos;
        }
        if (pos < v.size() && v[pos].client == client && v[pos].key == key) {
            return v[pos];
        }
        SessionKey sk;
        sk.client = client;
        sk.key = key;
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), std::move(sk));
        return v[pos];
    }

    // Parse "c<client>_v<i>" → i if it is THIS client's own-write value, else -1.
    // Pure deterministic string parse; no locale, no std::*_distribution.
    static long long own_index(std::uint64_t client, const std::string& value) {
        const std::string prefix =
            "c" + std::to_string(client) + "_v";
        if (value.size() <= prefix.size() ||
            value.compare(0, prefix.size(), prefix) != 0) {
            return -1;
        }
        long long idx = 0;
        for (std::size_t i = prefix.size(); i < value.size(); ++i) {
            const char ch = value[i];
            if (ch < '0' || ch > '9') {
                return -1;  // malformed suffix → not a clean own-write tag
            }
            idx = idx * 10 + (ch - '0');
        }
        return idx;
    }

    static Verdict backward_violation(const SessionKey& sk, const Op& read_op,
                                      const std::string& how) {
        std::string witness =
            "SESSION READ WENT BACKWARD client=" + std::to_string(sk.client) +
            " key=" + sk.key + " entitled_own_write_op_id=" +
            std::to_string(sk.high_op_id) + " entitled_value=\"" +
            sk.high_value + "\" (idx v" + std::to_string(sk.high_idx) +
            "); offending read_op_id=" + std::to_string(read_op.op_id) +
            " result=\"" + read_op.result + "\" (" + how + ")";
        return verdict_violation(
            std::move(witness),
            "C-MONO: within a client session, a read observed an EARLIER value "
            "than the session already wrote/observed for the key "
            "(read-your-writes / monotonic-reads violated).");
    }
};

}  // namespace lockstep::harness::checkers
