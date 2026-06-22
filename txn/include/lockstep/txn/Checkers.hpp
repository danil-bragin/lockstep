#pragma once

// Checkers.hpp — Phase 5 Stage M. THE PER-D5-LEVEL + OLLP + SERIALIZABILITY
// CHECKERS. Each asserts EXACTLY its stated contract — no stronger, no weaker
// (master-plan §6.3) — and cites the CommitOrdering.tla invariant / D5 rule it
// maps onto. A violation carries a WITNESS + the SEED (V-CHK2) so it is replayable.
//
// Source of truth: specs/CommitOrdering.tla. The harness produces a RunResult
// (the per-txn observable commit info IN seqLog order). These checkers judge it.
//
// THE CHECKER SET:
//   serialized_by_seqlog   <- CommitOrdering.tla SerializedBySeqLog
//   exactly_once           <- CommitOrdering.tla ExactlyOnce
//   ollp_sound             <- CommitOrdering.tla OLLPSound (+ retry-bound C5.6)
//   strict_serializable    <- ReadsMatchSerialPrefix + StoreReflectsHistory,
//                             checked as an Elle-style serial-order + real-time
//                             linearizability test (the DEFAULT path).
//   snapshot_level         <- D5Snapshot     (each read internally consistent
//                                             as-of ONE version; no torn read;
//                                             NO real-time guarantee).
//   bounded_staleness_level<- D5BoundedStale (served prefix within stated lag).
//   ready_your_writes_level<- D5ReadYourWrites (a session sees its own writes).
//
// Each checker is a PURE deterministic function of (RunResult, the submitted
// ordered txns, ExecConfig). No clock, no randomness, ordered maps only. txn/ is
// NOT lint-exempt.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// A checker verdict. Mirrors harness::Verdict (ok + witness + explanation + seed)
// but kept local so txn/ does not depend on the KV-register History shape.
struct Verdict {
    bool ok = true;
    std::string checker;      // which checker produced this
    std::string spec_ref;     // the CommitOrdering.tla invariant / D5 rule cited
    std::string witness;      // minimal evidence on violation (empty when ok)
    std::string explanation;  // why it violates
    std::uint64_t seed = 0;   // stamped by the harness for replay (V-CHK2)
};

[[nodiscard]] inline Verdict ok_verdict(std::string checker, std::string spec_ref) {
    Verdict v;
    v.ok = true;
    v.checker = std::move(checker);
    v.spec_ref = std::move(spec_ref);
    return v;
}

[[nodiscard]] inline Verdict bad_verdict(std::string checker, std::string spec_ref,
                                         std::string witness, std::string explanation) {
    Verdict v;
    v.ok = false;
    v.checker = std::move(checker);
    v.spec_ref = std::move(spec_ref);
    v.witness = std::move(witness);
    v.explanation = std::move(explanation);
    return v;
}

// ---------------------------------------------------------------------------
// Helpers shared by the checkers.
// ---------------------------------------------------------------------------

// The committed txns in their reported serialization order. Aborts are dropped
// (they have no effect on the serial history).
[[nodiscard]] inline std::vector<const CommitInfo*> committed_in_order(
    const RunResult& run) {
    std::vector<const CommitInfo*> v;
    for (const CommitInfo& c : run.commits) {
        if (c.status == Status::Committed) {
            v.push_back(&c);
        }
    }
    return v;
}

// Value of key k after applying the first j committed txns of `seq` (the serial
// prefix). == CommitOrdering.tla ValueAfterPrefix(k, j). ∅ if never written.
[[nodiscard]] inline ReadResult value_after_prefix(
    const std::vector<const CommitInfo*>& seq, const Key& k, std::size_t j) {
    ReadResult v = std::nullopt;
    for (std::size_t i = 0; i < j && i < seq.size(); ++i) {
        const auto it = seq[i]->writes_committed.find(k);
        if (it != seq[i]->writes_committed.end()) {
            v = it->second;  // newest write at or before prefix j
        }
    }
    return v;
}

[[nodiscard]] inline std::string render_val(const ReadResult& v) {
    return v.has_value() ? ("\"" + *v + "\"") : "∅";
}

// ---------------------------------------------------------------------------
// SerializedBySeqLog (CommitOrdering.tla SerializedBySeqLog).
// Committed txns appear in history in exactly their seqLog order. Two parts:
//   (1) the reported seq_index is contiguous 1,2,3,... over committed txns (the
//       sequencer order IS the serialization order; commit versions monotone);
//   (2) the COMMIT ORDER is a SUBSEQUENCE of the agreed seqLog (submitted) order
//       — an executor that applies txns out of the agreed total order (e.g.
//       reverses the batch) is flagged here. The submitted order is the agreed
//       global order from consensus (Phase 4); committing in a different relative
//       order breaks "the sequencer order is the serialization order".
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_serialized_by_seqlog(
    const RunResult& run, const std::vector<Txn>& submitted) {
    const char* nm = "serialized_by_seqlog";
    const char* sr = "CommitOrdering.tla SerializedBySeqLog";
    const auto seq = committed_in_order(run);

    // (1) contiguous seq_index + non-zero monotone commit version.
    Seq expect = 0;
    for (const CommitInfo* c : seq) {
        ++expect;
        if (c->seq_index != expect) {
            return bad_verdict(
                nm, sr,
                "txn_id=" + std::to_string(c->txn_id) + " seq_index=" +
                    std::to_string(c->seq_index) + " expected=" + std::to_string(expect),
                "committed txns must appear in contiguous seqLog order; this "
                "commit's serialization index is out of order.");
        }
        if (c->commit_version == kNoSeq) {
            return bad_verdict(
                nm, sr, "txn_id=" + std::to_string(c->txn_id) + " commit_version=0",
                "a committed txn must carry a non-zero monotonic commit version.");
        }
    }

    // (2) the commit order is a subsequence of the submitted seqLog order.
    std::map<std::uint64_t, std::size_t> submit_pos;
    for (std::size_t i = 0; i < submitted.size(); ++i) {
        submit_pos[submitted[i].id] = i;
    }
    std::size_t last = 0;
    bool have_last = false;
    for (const CommitInfo* c : seq) {
        const auto it = submit_pos.find(c->txn_id);
        if (it == submit_pos.end()) {
            continue;
        }
        if (have_last && it->second < last) {
            return bad_verdict(
                nm, sr,
                "txn_id=" + std::to_string(c->txn_id) +
                    " committed AFTER a txn that was LATER in the submitted seqLog "
                    "(submit_pos=" + std::to_string(it->second) + " < prior " +
                    std::to_string(last) + ")",
                "the commit order must be a subsequence of the agreed seqLog "
                "(consensus) order; this executor applied txns OUT OF the agreed "
                "total order.");
        }
        last = it->second;
        have_last = true;
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// ExactlyOnce (CommitOrdering.tla ExactlyOnce).
// Each txn reaches exactly one terminal state; a committed txn appears once.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_exactly_once(const RunResult& run,
                                                const std::vector<Txn>& submitted) {
    const char* nm = "exactly_once";
    const char* sr = "CommitOrdering.tla ExactlyOnce";
    std::map<std::uint64_t, int> terminal_count;
    for (const CommitInfo& c : run.commits) {
        if (c.status == Status::Pending) {
            return bad_verdict(nm, sr, "txn_id=" + std::to_string(c.txn_id),
                               "a final result must be terminal (committed or "
                               "aborted), never left pending.");
        }
        terminal_count[c.txn_id] += 1;
    }
    for (const auto& [id, n] : terminal_count) {
        if (n != 1) {
            return bad_verdict(nm, sr,
                               "txn_id=" + std::to_string(id) + " terminal_count=" +
                                   std::to_string(n),
                               "each submitted txn must reach EXACTLY one terminal "
                               "outcome (no duplicated/replayed effects).");
        }
    }
    for (const Txn& t : submitted) {
        if (terminal_count.find(t.id) == terminal_count.end()) {
            return bad_verdict(nm, sr, "txn_id=" + std::to_string(t.id),
                               "every submitted txn must reach a terminal outcome; "
                               "this one was dropped (no commit, no abort).");
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// OLLPSound (CommitOrdering.tla OLLPSound) + the C5.6 retry bound.
// Nothing commits with an invalid footprint; the executor reports footprint
// validity AT EACH TXN'S OWN serialization point (snapshot-stable). Also: no
// committed/aborted txn exceeded the MaxRetry re-sequence bound.
//
// We ALSO independently RE-DERIVE the footprint validity from the recorded reads
// (we do not just trust the executor's self-report): a committed txn whose body,
// run on its recorded snapshot, would have asked for a key outside its declared
// reads is a footprint violation regardless of what footprint_valid claims.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_ollp_sound(const RunResult& run,
                                              const std::vector<Txn>& submitted,
                                              const ExecConfig& cfg) {
    const char* nm = "ollp_sound";
    const char* sr = "CommitOrdering.tla OLLPSound (+ C5.6 retry bound)";

    std::map<std::uint64_t, const Txn*> by_id;
    for (const Txn& t : submitted) {
        by_id[t.id] = &t;
    }

    for (const CommitInfo& c : run.commits) {
        if (c.retries > cfg.max_retry) {
            return bad_verdict(
                nm, sr,
                "txn_id=" + std::to_string(c.txn_id) + " retries=" +
                    std::to_string(c.retries) + " max_retry=" + std::to_string(cfg.max_retry),
                "OLLP re-sequencing must be bounded by MaxRetry (C5.6 starvation "
                "avoidance); this txn exceeded the bound.");
        }
        if (c.status != Status::Committed) {
            continue;
        }
        if (!c.footprint_valid) {
            return bad_verdict(
                nm, sr, "txn_id=" + std::to_string(c.txn_id) + " footprint_valid=false",
                "a committed txn's OLLP recon must have been VALID at its own "
                "serialization point; this one committed on a stale/invalid "
                "footprint (skipped re-sequence).");
        }
        // Independent re-derivation: replay the body on the RECORDED reads and
        // check it asks for no key outside the declared footprint.
        const auto bit = by_id.find(c.txn_id);
        if (bit == by_id.end() || !bit->second->body) {
            continue;
        }
        const Txn& t = *bit->second;
        const Txn::Outcome oc = t.body(c.reads_observed);
        for (const Key& ek : oc.extra_reads) {
            bool declared = false;
            for (const Read& r : t.declared_reads) {
                if (r.key == ek) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c.txn_id) + " extra_read=\"" + ek +
                        "\" not in declared footprint (recorded reads: serialized at " +
                        "version " + std::to_string(c.commit_version - 1) + ")",
                    "re-derived from the committed txn's OWN recorded snapshot: its "
                    "body needed a key OUTSIDE the declared (OLLP-predicted) read "
                    "footprint, so its recon was NOT valid at its serialization "
                    "point — it must have re-sequenced, not committed.");
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// STRICT-SERIALIZABLE (the DEFAULT path) — Elle-style serial-order check.
// CommitOrdering.tla ReadsMatchSerialPrefix + StoreReflectsHistory.
//
// The reported serialization order IS the single total order. We assert it is
// REGISTER-LEGAL: every StrictSerializable read in a committed txn observed
// exactly ValueAfterPrefix(k, i-1) — the value the committed prefix strictly
// before it produced (no stale, no future value). Combined with
// serialized_by_seqlog's commit-order-is-a-subsequence-of-seqLog (the real-time
// part), this is full strict serializability: ONE explicit serial order that is
// register-legal AND respects the agreed real-time (consensus) order.
//
// Only StrictSerializable reads are held to the exact-prefix rule; relaxed-level
// reads are judged by their own D5 checker (no stronger, no weaker).
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_strict_serializable(const RunResult& run) {
    const char* nm = "strict_serializable";
    const char* sr =
        "CommitOrdering.tla ReadsMatchSerialPrefix + StoreReflectsHistory "
        "(Elle-style serial-order linearizability of strict reads)";
    const auto seq = committed_in_order(run);

    for (std::size_t i = 0; i < seq.size(); ++i) {
        const CommitInfo& c = *seq[i];
        for (const CommitInfo::ServedRead& s : c.served_reads) {
            if (s.level != Level::StrictSerializable) {
                continue;  // relaxed reads judged by their own D5 checker
            }
            const ReadResult expect = value_after_prefix(seq, s.key, i);
            if (s.value != expect) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c.txn_id) + " (serial pos " +
                        std::to_string(i + 1) + ") read key=\"" + s.key + "\" got " +
                        render_val(s.value) + " expected " + render_val(expect) +
                        " (= value after committed prefix " + std::to_string(i) + ")",
                    "a StrictSerializable read must observe EXACTLY the committed "
                    "prefix strictly before this txn's serialization point — never "
                    "a stale or a snapshot value. This read is not linearizable.");
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// SNAPSHOT (CommitOrdering.tla D5Snapshot).
// EXACTLY: every Snapshot read in a committed txn is internally consistent as-of
// ONE committed version — each key read at the SAME prefix p, and the served
// value equals ValueAfterPrefix(k, p). NO real-time guarantee is required (p may
// be older than the txn's own serialization point — that is permitted and is the
// whole point of Snapshot). The check is therefore: (1) all Snapshot reads in a
// txn share ONE served_version, and (2) each value matches the serial value at
// that version. It does NOT assert linearizability (no stronger).
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_snapshot_level(const RunResult& run) {
    const char* nm = "snapshot_level";
    const char* sr = "CommitOrdering.tla D5Snapshot (consistent as-of one version, "
                     "no torn read; no real-time guarantee)";
    const auto seq = committed_in_order(run);

    for (const CommitInfo* c : seq) {
        // (1) all Snapshot reads in this txn must share ONE prefix p (no torn read
        // across versions).
        bool have_p = false;
        Seq p = kNoSeq;
        for (const CommitInfo::ServedRead& s : c->served_reads) {
            if (s.level != Level::Snapshot) {
                continue;
            }
            if (!have_p) {
                p = s.served_version;
                have_p = true;
            } else if (s.served_version != p) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c->txn_id) + " key=\"" + s.key +
                        "\" served at version " + std::to_string(s.served_version) +
                        " but a sibling Snapshot read used version " + std::to_string(p),
                    "a Snapshot read set must be internally consistent as-of ONE "
                    "version — this txn read different keys at different versions "
                    "(a TORN read across versions).");
            }
        }
        if (!have_p) {
            continue;
        }
        // (2) each Snapshot read equals the serial value at version p. We map the
        // served version (a commit_version) to a prefix length: the number of
        // committed txns whose commit_version <= p.
        std::size_t prefix_len = 0;
        for (const CommitInfo* d : seq) {
            if (d->commit_version <= p) {
                ++prefix_len;
            }
        }
        for (const CommitInfo::ServedRead& s : c->served_reads) {
            if (s.level != Level::Snapshot) {
                continue;
            }
            const ReadResult expect = value_after_prefix(seq, s.key, prefix_len);
            if (s.value != expect) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c->txn_id) + " key=\"" + s.key +
                        "\" at version " + std::to_string(p) + " got " +
                        render_val(s.value) + " expected " + render_val(expect),
                    "a Snapshot read must equal the serial value AT its chosen "
                    "version; this value is not the consistent as-of-version value "
                    "(a torn / fabricated snapshot).");
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// BOUNDED STALENESS (CommitOrdering.tla D5BoundedStale).
// EXACTLY: a BoundedStaleness read served from a local replica is at most
// max_lag committed entries behind the live committed tip AT the txn's
// serialization point. We assert (tip_at_txn - served_version) <= max_lag AND the
// served value is a valid serial value at the served prefix (it is a real past
// state, not fabricated). tip_at_txn for a committed txn = commit_version - 1 (the
// committed prefix strictly before it). NO real-time-tighter guarantee is
// required (that is the relaxation).
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_bounded_staleness_level(const RunResult& run) {
    const char* nm = "bounded_staleness_level";
    const char* sr = "CommitOrdering.tla D5BoundedStale (served prefix within the "
                     "stated log-lag of the tip)";
    const auto seq = committed_in_order(run);

    for (const CommitInfo* c : seq) {
        const Seq tip_at_txn = (c->commit_version > 0) ? (c->commit_version - 1) : 0;
        for (const CommitInfo::ServedRead& s : c->served_reads) {
            if (s.level != Level::BoundedStaleness) {
                continue;
            }
            if (s.served_version > tip_at_txn) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c->txn_id) + " key=\"" + s.key +
                        "\" served_version=" + std::to_string(s.served_version) +
                        " > tip=" + std::to_string(tip_at_txn),
                    "a read cannot be served from a FUTURE version (beyond the "
                    "committed tip at its serialization point).");
            }
            const Seq lag = tip_at_txn - s.served_version;
            if (lag > s.max_lag) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c->txn_id) + " key=\"" + s.key +
                        "\" lag=" + std::to_string(lag) + " > max_lag=" +
                        std::to_string(s.max_lag) + " (served_version=" +
                        std::to_string(s.served_version) + ", tip=" +
                        std::to_string(tip_at_txn) + ")",
                    "a BoundedStaleness read must be within the stated log-lag of "
                    "the committed tip; this local-replica read fell further behind "
                    "than the contract allows.");
            }
            // The served value must be a real serial value at the served prefix.
            std::size_t prefix_len = 0;
            for (const CommitInfo* d : seq) {
                if (d->commit_version <= s.served_version) {
                    ++prefix_len;
                }
            }
            const ReadResult expect = value_after_prefix(seq, s.key, prefix_len);
            if (s.value != expect) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c->txn_id) + " key=\"" + s.key +
                        "\" served_version=" + std::to_string(s.served_version) +
                        " got " + render_val(s.value) + " expected " + render_val(expect),
                    "a BoundedStaleness read must return a REAL past serial value "
                    "at its served prefix, not a fabricated one.");
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// READ-YOUR-WRITES (CommitOrdering.tla D5ReadYourWrites).
// EXACTLY: within a session, a read observes that session's OWN prior committed
// writes. We track, per session, the highest commit_version at which that session
// committed a write to each key; a ReadYourWrites read of that key in the same
// session must be served from a prefix >= that version, so it sees that write (or
// a newer one). The witness names the lost write.
//
// A session is identified by Read::session AND by the txns that carry the same
// session on their reads/writes. Since the seam models a txn's session via its
// reads' session field, we treat a txn as "in session sid" if any of its reads
// carries sid; its writes are then that session's writes.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_read_your_writes_level(const RunResult& run) {
    const char* nm = "read_your_writes_level";
    const char* sr = "CommitOrdering.tla D5ReadYourWrites (a session observes its "
                     "own prior writes)";
    const auto seq = committed_in_order(run);

    // (session, key) -> the value+version the session last wrote.
    struct LastWrite {
        Seq version = kNoSeq;
        Value value;
    };
    std::map<std::pair<SessionId, Key>, LastWrite> session_writes;

    for (const CommitInfo* c : seq) {
        // Which session(s) does this txn belong to? (the sessions named on its
        // RYW reads). A txn may legitimately carry one session.
        std::vector<SessionId> sessions;
        for (const CommitInfo::ServedRead& s : c->served_reads) {
            if (s.level == Level::ReadYourWrites && s.session != 0) {
                bool seen = false;
                for (SessionId x : sessions) {
                    if (x == s.session) {
                        seen = true;
                    }
                }
                if (!seen) {
                    sessions.push_back(s.session);
                }
            }
        }

        // First, CHECK this txn's RYW reads against the session's prior writes.
        for (const CommitInfo::ServedRead& s : c->served_reads) {
            if (s.level != Level::ReadYourWrites || s.session == 0) {
                continue;
            }
            const auto it = session_writes.find({s.session, s.key});
            if (it == session_writes.end()) {
                continue;  // session never wrote this key: nothing to observe
            }
            // The read must reflect the session's own prior write: either the read
            // returns that exact written value, OR it was served from a prefix at
            // or after the write (so it sees that write or a newer committed one).
            const bool sees_version = s.served_version >= it->second.version;
            const bool sees_value = s.value.has_value() && *s.value == it->second.value;
            if (!sees_version && !sees_value) {
                return bad_verdict(
                    nm, sr,
                    "session=" + std::to_string(s.session) + " key=\"" + s.key +
                        "\" own_write_version=" + std::to_string(it->second.version) +
                        " value=" + render_val(it->second.value) +
                        " but read served_version=" + std::to_string(s.served_version) +
                        " got " + render_val(s.value),
                    "a ReadYourWrites read must observe the session's OWN prior "
                    "committed write (served from a prefix at/after that write, or "
                    "returning at least that value); this session LOST its own "
                    "write.");
            }
        }

        // Then, RECORD this txn's writes under its session(s).
        for (SessionId sid : sessions) {
            for (const auto& [k, val] : c->writes_committed) {
                session_writes[{sid, k}] = LastWrite{c->commit_version, val};
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// DIFFERENTIAL — the system-under-test's observable results match the oracle's
// (a serial execution in seqLog order). This is the strict-serializable spec
// realized as "agrees with the ground truth". Compares committed txns by id:
// same status, same reads_observed, same writes_committed, same result.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Verdict check_differential(const RunResult& sut,
                                                const RunResult& oracle) {
    const char* nm = "differential_vs_oracle";
    const char* sr = "briefs/phase5.md Stage M DIFFERENTIAL (SUT == serial oracle)";

    std::map<std::uint64_t, const CommitInfo*> oref;
    for (const CommitInfo& c : oracle.commits) {
        oref[c.txn_id] = &c;
    }
    for (const CommitInfo& c : sut.commits) {
        const auto it = oref.find(c.txn_id);
        if (it == oref.end()) {
            return bad_verdict(nm, sr, "txn_id=" + std::to_string(c.txn_id),
                               "the SUT reported a txn the oracle did not.");
        }
        const CommitInfo& o = *it->second;
        if (c.status != o.status) {
            return bad_verdict(
                nm, sr,
                "txn_id=" + std::to_string(c.txn_id) + " sut_status=" +
                    status_name(c.status) + " oracle_status=" + status_name(o.status),
                "the SUT's terminal status disagrees with the serial oracle.");
        }
        if (c.status != Status::Committed) {
            continue;
        }
        if (c.writes_committed != o.writes_committed) {
            return bad_verdict(
                nm, sr, "txn_id=" + std::to_string(c.txn_id) + " writes differ",
                "the SUT committed different writes than the serial oracle (a "
                "non-serializable result).");
        }
        if (c.result != o.result) {
            return bad_verdict(
                nm, sr,
                "txn_id=" + std::to_string(c.txn_id) + " sut_result=\"" + c.result +
                    "\" oracle_result=\"" + o.result + "\"",
                "the SUT's observable result disagrees with the serial oracle.");
        }
        // The STRICT reads must match the oracle's serial-prefix reads exactly.
        // (Relaxed-level reads may legitimately differ — they are judged by their
        // own D5 checker, not the differential.)
        std::map<Key, ReadResult> oracle_reads = o.reads_observed;
        for (const CommitInfo::ServedRead& s : c.served_reads) {
            if (s.level != Level::StrictSerializable) {
                continue;
            }
            const auto oit = oracle_reads.find(s.key);
            const ReadResult oval =
                (oit != oracle_reads.end()) ? oit->second : std::nullopt;
            if (s.value != oval) {
                return bad_verdict(
                    nm, sr,
                    "txn_id=" + std::to_string(c.txn_id) + " key=\"" + s.key +
                        "\" sut=" + render_val(s.value) + " oracle=" + render_val(oval),
                    "a StrictSerializable read in the SUT observed a different "
                    "value than the serial oracle (non-serializable read).");
            }
        }
    }
    return ok_verdict(nm, sr);
}

// ---------------------------------------------------------------------------
// Run the FULL checker battery for a SUT run against the oracle run + the
// submitted txns + config. Returns every verdict (ok or not) so a dashboard can
// print the whole matrix; the seed is stamped into each.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<Verdict> run_all_checkers(
    const RunResult& sut, const RunResult& oracle,
    const std::vector<Txn>& submitted, const ExecConfig& cfg,
    std::uint64_t seed) {
    std::vector<Verdict> out;
    out.push_back(check_serialized_by_seqlog(sut, submitted));
    out.push_back(check_exactly_once(sut, submitted));
    out.push_back(check_ollp_sound(sut, submitted, cfg));
    out.push_back(check_strict_serializable(sut));
    out.push_back(check_snapshot_level(sut));
    out.push_back(check_bounded_staleness_level(sut));
    out.push_back(check_read_your_writes_level(sut));
    out.push_back(check_differential(sut, oracle));
    for (Verdict& v : out) {
        v.seed = seed;
    }
    return out;
}

}  // namespace lockstep::txn
