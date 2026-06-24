#pragma once

// RaftNodeB.hpp — Phase 4 Stage I, IMPLEMENTATION B. An independent, complete
// Raft ConsensusNode built STRICTLY from specs/Consensus.tla + the seam
// (ConsensusNode.hpp). This is one of TWO blind, independently-authored impls
// (master-plan §6.5): it shares NO code, message encoding, or loop structure
// with impl A on purpose, so the dual-implementation cross-check finds blind
// spots one impl alone would miss.
//
// ============================================================================
// SPEC CONFORMANCE MAP — every Consensus.tla action ↦ a method here.
// ============================================================================
//   Timeout(s)            ↦ begin_election()  : state∈{F,C}, ++term, vote self,
//                           votesGranted={self}, broadcast RequestVote. PERSIST
//                           (term,vote) + sync BEFORE the broadcast leaves.
//   UpdateTerm(s)         ↦ maybe_step_down(m.mterm) : on ANY message carrying a
//                           strictly-higher term, adopt it, revert to Follower,
//                           clear votedFor, PERSIST+sync, THEN process the msg.
//                           Modelled as its own pre-step so log truncation only
//                           ever happens as a Follower (LeaderAppendOnly).
//   HandleRequestVote(s)  ↦ on_request_vote() : grant iff votedFor∈{Nil,cand}
//                           AND candidate up-to-date; PERSIST votedFor + sync
//                           BEFORE the reply is sent (persist-before-reply).
//   HandleVoteResponse(s) ↦ on_vote_response() : collect a granted vote at the
//                           current term while Candidate.
//   BecomeLeader(s)       ↦ become_leader() : on a quorum of votes, become
//                           Leader, seed nextIndex/matchIndex, send heartbeats.
//   ClientRequest(s,v)    ↦ submit() : Leader-only append of [currentTerm,v];
//                           PERSIST log + sync synchronously (durable on accept).
//   AppendEntries(s,d)    ↦ send_append_entries() : Leader replicates the suffix
//                           after prevLogIndex (= nextIndex[d]-1) to follower d;
//                           heartbeat is the empty-suffix case.
//   HandleAppendEntries(s)↦ on_append_entries() : reject on prevLog mismatch;
//                           else apply the Raft conflict rule (truncate ONLY at
//                           the first conflicting index, append the genuinely-new
//                           tail — redelivery idempotent), adopt leaderCommit;
//                           PERSIST log + sync BEFORE the reply.
//   AdvanceCommitIndex(s) ↦ advance_commit_index() : Leader commits index N only
//                           when a quorum stores it AND log[N].term==currentTerm.
//
// SAFETY (model-checked): ElectionSafety / LogMatching / StateMachineSafety /
// LeaderAppendOnly hold because (a) a vote/term is durable before it is acted on
// (no two leaders in a term after a crash), (b) step-down precedes any truncation,
// (c) the conflict rule never erases an agreed prefix, (d) only current-term
// quorum entries commit.
//
// ============================================================================
// INTERNAL STRUCTURE (impl-B's own choice; distinct from impl A):
//   * ONE recv-loop coroutine pulls messages one at a time and dispatches.
//   * ONE ticker coroutine drives election timeouts (deadline+epoch) and, while
//     Leader, periodic heartbeats.
//   * RPC = a hand-rolled little-endian binary frame with a trailing CRC32 over
//     the body (the Phase-2/3 per-record-CRC lesson): a corrupt/truncated frame
//     is dropped, never mis-decoded.
//   * Durable image = an append-only stream of CRC'd records on IDisk
//     (TERM / VOTE / TRUNCATE / ENTRY). On restart we replay the longest
//     CRC-valid prefix and rebuild (currentTerm, votedFor, log) — exactly the
//     SimDisk durable-prefix recovery (Phase 2/3 recover-to-prefix lesson).
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output, any nondeterminism.
// All time = core::IClock; all randomness = core::IRandom; all RPC = core::INetwork;
// all durability = core::IDisk. Pure function of (seed).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/IScheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>

namespace lockstep::consensus::raft_b {

// ----------------------------------------------------------------------------
// Wire codec — impl-B's own binary RPC framing. Little-endian fixed widths +
// length-prefixed strings, a 1-byte message tag, and a trailing CRC32 over the
// whole body. The decoder validates the CRC and the length envelope; any frame
// that fails is DROPPED (returns false) — a corrupt/duplicated/torn frame can
// never be mis-interpreted as a different message (Phase-2/3 CRC lesson).
// ----------------------------------------------------------------------------
namespace wire {

enum class Kind : std::uint8_t {
    RequestVote = 1,
    RequestVoteResp = 2,
    AppendEntries = 3,
    AppendEntriesResp = 4,
    // C4.3 InstallSnapshot: leader ships its snapshot to a follower lagging below
    // the leader's discarded prefix (Snapshot.tla InstallSnapshot(s,d)).
    InstallSnapshot = 5,
    InstallSnapshotResp = 6,
    // C4.2 MEMBERSHIP (Membership.tla). A dedicated RPC pair carries the config
    // chain rollout, OFF the value-log replication path (so the value-log observable
    // and the five base-Raft checkers are byte-identical when no change is proposed).
    ConfigChange = 7,
    ConfigChangeResp = 8,
};

// CRC32 (IEEE 802.3, reflected) — a small, fully-specified, table-free loop so
// the byte stream is identical on every platform. Used both on the wire and in
// the durable record stream.
[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) {
            std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(c & 1U));
            c = (c >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~c;
}

// A tiny append/extract buffer in fixed little-endian layout. No streams, no
// std::format — deterministic byte-for-byte.
struct Writer {
    std::vector<std::uint8_t> buf;

    void u8(std::uint8_t v) { buf.push_back(v); }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
        }
    }
    void str(const std::string& s) {
        u64(static_cast<std::uint64_t>(s.size()));
        for (char ch : s) {
            buf.push_back(static_cast<std::uint8_t>(ch));
        }
    }
    // Finalize: append CRC32 over the body so far, return the framed bytes.
    [[nodiscard]] std::vector<std::byte> finish() const {
        std::uint32_t c = crc32(buf.data(), buf.size());
        std::vector<std::byte> out;
        out.reserve(buf.size() + 4);
        for (std::uint8_t b : buf) {
            out.push_back(static_cast<std::byte>(b));
        }
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((c >> (8 * i)) & 0xFFU));
        }
        return out;
    }
};

struct Reader {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    std::size_t at = 0;
    bool bad = false;

    [[nodiscard]] std::uint8_t u8() {
        if (at + 1 > n) {
            bad = true;
            return 0;
        }
        return p[at++];
    }
    [[nodiscard]] std::uint64_t u64() {
        if (at + 8 > n) {
            bad = true;
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(p[at++]) << (8 * i);
        }
        return v;
    }
    [[nodiscard]] std::string str() {
        std::uint64_t len = u64();
        if (bad || at + len > n) {
            bad = true;
            return {};
        }
        std::string s;
        s.reserve(static_cast<std::size_t>(len));
        for (std::uint64_t i = 0; i < len; ++i) {
            s.push_back(static_cast<char>(p[at++]));
        }
        return s;
    }
};

// The decoded message (a tagged union flattened into one struct — small, POD-ish).
struct Msg {
    Kind kind{};
    std::uint64_t term = 0;
    std::uint64_t from = 0;
    // RequestVote
    std::uint64_t last_log_index = 0;
    std::uint64_t last_log_term = 0;
    // C4.2 Membership: the candidate's config-chain index (cfgIdx[c]) — the config-
    // log up-to-date rule (a voter refuses a candidate whose cfgIdx is behind its own).
    std::uint64_t cfg_index = 0;
    // C4.2 ConfigChange / ConfigChangeResp: proposed chain index + member set.
    std::uint64_t config_chain_index = 0;
    std::vector<std::uint64_t> config_members;
    // RequestVoteResp / AppendEntriesResp
    bool granted = false;
    bool success = false;
    std::uint64_t match_index = 0;
    // AppendEntries
    std::uint64_t prev_log_index = 0;
    std::uint64_t prev_log_term = 0;
    std::uint64_t leader_commit = 0;
    std::vector<LogEntry> entries;
    // InstallSnapshot: lastIncludedIndex + folded snapshot.state (= the committed
    // prefix entry list, our state machine's applied state).
    std::uint64_t last_included_index = 0;
    std::vector<LogEntry> snap_state;
};

// Validate the trailing CRC32 and decode. Returns false (dropped) on any
// framing/CRC failure — a torn or corrupt frame is never mis-decoded.
[[nodiscard]] inline bool decode(std::span<const std::byte> payload, Msg& out) {
    if (payload.size() < 5) {
        return false;  // need at least 1 tag byte + 4 CRC bytes
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    std::size_t body_len = payload.size() - 4;
    std::uint32_t want = 0;
    for (int i = 0; i < 4; ++i) {
        want |= static_cast<std::uint32_t>(bytes[body_len + i]) << (8 * i);
    }
    if (crc32(bytes, body_len) != want) {
        return false;  // corrupt frame: drop it
    }
    Reader r{bytes, body_len, 0, false};
    std::uint8_t tag = r.u8();
    out = Msg{};
    out.kind = static_cast<Kind>(tag);
    out.term = r.u64();
    out.from = r.u64();
    switch (out.kind) {
        case Kind::RequestVote:
            out.last_log_index = r.u64();
            out.last_log_term = r.u64();
            out.cfg_index = r.u64();  // C4.2 config-log up-to-date check
            break;
        case Kind::RequestVoteResp:
            out.granted = (r.u8() != 0);
            break;
        case Kind::AppendEntriesResp:
            out.success = (r.u8() != 0);
            out.match_index = r.u64();
            break;
        case Kind::AppendEntries: {
            out.prev_log_index = r.u64();
            out.prev_log_term = r.u64();
            out.leader_commit = r.u64();
            std::uint64_t count = r.u64();
            // Guard against an absurd count (a corrupt-but-CRC-coincident frame
            // cannot occur — CRC validated — but bound anyway for robustness).
            for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                out.entries.push_back(std::move(e));
            }
            break;
        }
        case Kind::InstallSnapshot: {
            out.last_included_index = r.u64();
            std::uint64_t count = r.u64();
            for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                out.snap_state.push_back(std::move(e));
            }
            break;
        }
        case Kind::InstallSnapshotResp:
            out.match_index = r.u64();
            break;
        case Kind::ConfigChange: {
            out.config_chain_index = r.u64();
            std::uint64_t count = r.u64();
            for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
                out.config_members.push_back(r.u64());
            }
            break;
        }
        case Kind::ConfigChangeResp:
            out.config_chain_index = r.u64();
            break;
        default:
            return false;  // unknown tag
    }
    return !r.bad;
}

}  // namespace wire

// ----------------------------------------------------------------------------
// Durable record stream — impl-B's own on-disk format. We APPEND CRC'd records
// to IDisk and sync(); on restart we read back the whole device and replay the
// longest CRC-valid prefix. A torn/lying tail record fails its CRC and is
// dropped, recovering exactly the durable prefix (SimDisk semantics; Phase-2/3
// recover-to-prefix). Records:
//   TERM     (term, votedFor_present, votedFor)   — full persisted vote+term
//   TRUNCATE (new_len)                            — log shortened to new_len
//   ENTRY    (index, term, value)                 — one appended log entry, carrying
//                                                   its 1-based log POSITION
// The latest TERM record wins; ENTRY/TRUNCATE rebuild the log in order.
//
// THE RECOVER-TO-PREFIX-WITH-CONTIGUITY INVARIANT (Phase-4 durability fix):
// the single FIFO persist worker writes records in LOGICAL mutation order, so a
// correctly-synced ENTRY at stream position k carries index == k and extends the
// log by exactly one at the tail. SimDisk::crash() can RE-STAGE the bytes of an
// in-flight (un-synced) append at a stale offset — surfacing a LATER entry where
// an earlier one belongs. Replaying by raw arrival order would silently accept
// that permuted tail, so two replicas could end up holding a DIFFERENT value at
// the same (index,term). To defend, ENTRY carries its index and replay VERIFIES
// contiguity: the next ENTRY must claim index == log.size()+1, and a TRUNCATE may
// only shorten WITHIN the contiguous prefix recovered so far. The first record
// that violates this is a torn / out-of-order tail: replay STOPS and keeps only
// the contiguous durable prefix. The index is used to DETECT a gap, never to
// place/pad/repair an out-of-order arrival.
// ----------------------------------------------------------------------------
namespace durable {

enum class RecKind : std::uint8_t {
    Term = 1,
    Truncate = 2,
    Entry = 3,
    Snapshot = 4,
    // C4.2 MEMBERSHIP (Membership.tla): one config-chain entry (chain index +
    // members). Configs live in the durable log; replay rebuilds the adopted chain
    // + cfgIdx so a restarted node honors the membership it had adopted.
    Config = 5,
};

// Build one framed record: [u32 body_len][body...][u32 crc-of-body].
[[nodiscard]] inline std::vector<std::byte> frame(const std::vector<std::uint8_t>& body) {
    std::uint32_t c = wire::crc32(body.data(), body.size());
    std::vector<std::byte> out;
    out.reserve(body.size() + 8);
    auto put_u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
        }
    };
    put_u32(static_cast<std::uint32_t>(body.size()));
    for (std::uint8_t b : body) {
        out.push_back(static_cast<std::byte>(b));
    }
    put_u32(c);
    return out;
}

inline void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
    }
}
inline void put_str(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u64(b, static_cast<std::uint64_t>(s.size()));
    for (char ch : s) {
        b.push_back(static_cast<std::uint8_t>(ch));
    }
}

[[nodiscard]] inline std::vector<std::byte> term_record(std::uint64_t term, bool has_vote,
                                                        std::uint64_t vote) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecKind::Term));
    put_u64(body, term);
    body.push_back(has_vote ? 1 : 0);
    put_u64(body, vote);
    return frame(body);
}
[[nodiscard]] inline std::vector<std::byte> truncate_record(std::uint64_t new_len) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecKind::Truncate));
    put_u64(body, new_len);
    return frame(body);
}
[[nodiscard]] inline std::vector<std::byte> entry_record(std::uint64_t index,
                                                         const LogEntry& e) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecKind::Entry));
    put_u64(body, index);  // 1-based log position — the contiguity guard checks it
    put_u64(body, e.term);
    put_str(body, e.value);
    return frame(body);
}
// C4.3 SNAPSHOT record: lastIncludedIndex + folded snapshot.state. On replay it
// RESETS the reconstructed (base, state) — the durable compaction point.
[[nodiscard]] inline std::vector<std::byte> snapshot_record(
    std::uint64_t last_included_index, const std::vector<LogEntry>& state) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecKind::Snapshot));
    put_u64(body, last_included_index);
    put_u64(body, static_cast<std::uint64_t>(state.size()));
    for (const LogEntry& e : state) {
        put_u64(body, e.term);
        put_str(body, e.value);
    }
    return frame(body);
}

// C4.2 CONFIG record: chain index + member set.
[[nodiscard]] inline std::vector<std::byte> config_record(
    std::uint64_t chain_index, const std::vector<std::uint64_t>& members) {
    std::vector<std::uint8_t> body;
    body.push_back(static_cast<std::uint8_t>(RecKind::Config));
    put_u64(body, chain_index);
    put_u64(body, static_cast<std::uint64_t>(members.size()));
    for (std::uint64_t id : members) {
        put_u64(body, id);
    }
    return frame(body);
}

// Recovered durable state.
struct Recovered {
    std::uint64_t term = 0;
    bool has_vote = false;
    std::uint64_t vote = 0;
    std::uint64_t snap_base = 0;       // snapshot.lastIncludedIndex
    std::vector<LogEntry> snap_state;  // snapshot.state (folded prefix)
    std::vector<LogEntry> log;         // retained suffix (abs idx snap_base+1 ..)
    // C4.2 MEMBERSHIP: replayed config-chain records, in stream order (chain index +
    // members). recover_task folds these onto the init config to rebuild the adopted
    // chain + cfgIdx. (replay() does not know the init config, so it just collects.)
    std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> cfg_records;
};

// Replay the longest CRC-valid record prefix from a raw durable byte image.
[[nodiscard]] inline Recovered replay(const std::vector<std::byte>& img) {
    Recovered rec;
    const auto* p = reinterpret_cast<const std::uint8_t*>(img.data());
    std::size_t n = img.size();
    std::size_t at = 0;
    auto get_u32 = [&](std::size_t off, std::uint32_t& v) -> bool {
        if (off + 4 > n) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(p[off + i]) << (8 * i);
        }
        return true;
    };
    while (at < n) {
        std::uint32_t blen = 0;
        if (!get_u32(at, blen)) {
            break;  // torn length header at the tail
        }
        std::size_t body_off = at + 4;
        std::size_t crc_off = body_off + blen;
        std::uint32_t want = 0;
        if (crc_off + 4 > n) {
            break;  // torn record at the tail — stop at the durable prefix
        }
        (void)get_u32(crc_off, want);
        if (blen == 0 || wire::crc32(p + body_off, blen) != want) {
            break;  // failed CRC — durable prefix ends here
        }
        // Decode the body.
        wire::Reader r{p + body_off, blen, 0, false};
        std::uint8_t k = r.u8();
        bool stop = false;  // a torn / non-prefix / unknown record ends the prefix
        switch (static_cast<RecKind>(k)) {
            case RecKind::Term: {
                std::uint64_t t = r.u64();
                std::uint8_t hv = r.u8();
                std::uint64_t v = r.u64();
                if (r.bad) {
                    stop = true;
                    break;
                }
                rec.term = t;
                rec.has_vote = (hv != 0);
                rec.vote = v;
                break;
            }
            case RecKind::Truncate: {
                std::uint64_t nl = r.u64();
                if (r.bad) {
                    stop = true;
                    break;
                }
                // A TRUNCATE may only shorten the contiguous SUFFIX recovered so
                // far, never below the snapshot base (a snapshotted/committed prefix
                // is never truncated). One naming a length we never reached / below
                // the base is a torn / re-staged tail — stop at the prefix before it.
                if (nl > rec.snap_base + rec.log.size() || nl < rec.snap_base) {
                    stop = true;
                    break;
                }
                rec.log.resize(static_cast<std::size_t>(nl - rec.snap_base));
                break;
            }
            case RecKind::Entry: {
                std::uint64_t idx = r.u64();
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                if (r.bad) {
                    stop = true;
                    break;
                }
                // CONTIGUITY GUARD over the ABSOLUTE logical length (snap_base +
                // retained suffix): a correctly-synced ENTRY must claim
                // index == snap_base + log.size() + 1 (it extends the suffix by one
                // at the tail). If not, a re-staged / out-of-order tail — STOP.
                if (idx != rec.snap_base + rec.log.size() + 1) {
                    stop = true;
                    break;
                }
                rec.log.push_back(std::move(e));
                break;
            }
            case RecKind::Snapshot: {
                std::uint64_t lii = r.u64();
                std::uint64_t count = r.u64();
                std::vector<LogEntry> st;
                for (std::uint64_t i = 0; i < count && !r.bad; ++i) {
                    LogEntry e;
                    e.term = r.u64();
                    e.value = r.str();
                    st.push_back(std::move(e));
                }
                // A SNAPSHOT only ever advances the base, and its state length must
                // equal lastIncludedIndex (the folded committed prefix [1..lii]).
                if (r.bad || lii < rec.snap_base ||
                    static_cast<std::uint64_t>(st.size()) != lii) {
                    stop = true;
                    break;
                }
                // CRITICAL: the stream is "ENTRY... then SNAPSHOT", so the suffix may
                // already hold entries at abs index > lii (appended before the
                // snapshot). Retain those (they are above the snapshot point); only
                // DROP the suffix at/below lii. Clearing the whole suffix here lost a
                // committed entry — backprop.
                std::uint64_t cur_abs_end = rec.snap_base + rec.log.size();
                std::vector<LogEntry> kept;
                if (cur_abs_end > lii) {
                    std::size_t drop = static_cast<std::size_t>(lii - rec.snap_base);
                    kept.assign(rec.log.begin() + static_cast<std::ptrdiff_t>(drop),
                                rec.log.end());
                }
                rec.snap_base = lii;
                rec.snap_state = std::move(st);
                rec.log = std::move(kept);
                break;
            }
            case RecKind::Config: {
                // C4.2: a config-chain entry (index + members). Collected in stream
                // order; recover_task folds them onto the init config (forward-only).
                std::uint64_t cidx = r.u64();
                std::uint64_t mcount = r.u64();
                std::vector<std::uint64_t> members;
                for (std::uint64_t i = 0; i < mcount && !r.bad; ++i) {
                    members.push_back(r.u64());
                }
                if (r.bad) {
                    stop = true;
                    break;
                }
                rec.cfg_records.emplace_back(cidx, std::move(members));
                break;
            }
            default:
                stop = true;  // unknown record kind: stop at the valid prefix
                break;
        }
        if (stop) {
            break;  // durable prefix ends before this record
        }
        at = crc_off + 4;
    }
    return rec;
}

}  // namespace durable

// ----------------------------------------------------------------------------
// RaftNodeB — the replica.
// ----------------------------------------------------------------------------
class RaftNodeB final : public ConsensusNode {
public:
    RaftNodeB(const NodeDeps& deps, const NodeConfig& cfg)
        : sched_(deps.sched),
          clock_(deps.clock),
          rng_(deps.rng),
          net_(deps.net),
          disk_(deps.disk),
          cfg_(cfg),
          self_(cfg.self_id) {
        // C4.2 MEMBERSHIP: seed the config chain with configs[1] = init_config
        // (Membership.tla InitConfig). EMPTY ⇒ the full cluster (fixed membership,
        // byte-identical to the pre-membership build).
        cfg_chain_ = init_config_chain();
        cfg_idx_ = 0;
        cfg_committed_idx_ = 0;
        cfg_adopted_.assign(cfg_.cluster.size(), 0);
    }

    // ---- ConsensusNode: client surface --------------------------------

    [[nodiscard]] SubmitResult submit(const std::string& value) override {
        SubmitResult r;
        if (!running_ || role_ != Role::Leader) {
            r.accepted = false;
            r.leader_hint = leader_hint_;
            return r;
        }
        // ClientRequest(s,v): append [currentTerm, value]; persist + sync so the
        // entry is durable on accept (the seam: acceptance recorded durably).
        LogEntry e{current_term_, value};
        append_entry(e);
        const Index idx = llen();
        match_index_[index_of(self_)] = idx;
        persist_entry(idx, e);  // 1-based ABSOLUTE position
        sync_now();
        r.accepted = true;
        r.term = current_term_;
        r.index = idx;
        // SINGLE-MEMBER CONFIG SELF-COMMIT. Commitment is normally driven from
        // handle_append_entries_resp (a peer ack). With NO peers in the current config
        // (quorum() == 1 ⇔ config size 1) no ack EVER arrives, so commit_index_ would
        // never advance past 0. For the lone leader the entry becomes committable the
        // instant it is DURABLE — but durability is fsync COMPLETION, not persist-ENQUEUE.
        // S9.2 BUG (async io_uring fdatasync): advancing here, right after persist_entry()
        // + sync_now() only ENQUEUE the record + a sync barrier, marked the entry committed
        // while it was still page-cache-only; an abrupt crash (SIGKILL / power loss) in the
        // enqueue->fsync window lost a COMMITTED entry ("commit implies durable" violated).
        // Synchronous fsync masked the sub-ms gap; async io_uring widened + exposed it. FIX:
        // the N=1 self-commit now fires at the persist worker's POST-SYNC point
        // (advance_n1_self_commit(), called once sync() completes for the just-persisted
        // prefix), so commit_index_ only follows the leader's own fsync. The N>=2 path is
        // UNTOUCHED: it stays ack-driven (a follower acks only after IT persists; a quorum
        // of durable followers makes the entry durable even if the leader's own fsync lags),
        // so for >= 2 members nothing in submit() or the worker's hook changes — byte-identical.
        // S8.2b COALESCE the per-submit broadcast. The OLD code broadcast on EVERY
        // submit, so a depth-K pipelined burst did K full re-scans of the unacked
        // suffix x peers — O(backlog x inflight) synchronous reactor work that blocked
        // the one coroutine + starved the heartbeat (S8.2a collapse). Now a submit
        // marks replication PENDING and kicks at most ONE bounded pass per scheduler
        // turn: repl_kicked_ de-dups submits arriving back-to-back in the same turn so
        // they coalesce into a single bounded broadcast. The flag clears at the top of
        // each ticker turn; a still-pending backlog is flushed there. Liveness holds:
        // entries beyond the first bounded batch ship on the next ack (on_append_response
        // re-kick) or the next tick.
        repl_pending_ = true;
        if (!repl_kicked_) {
            repl_kicked_ = true;
            broadcast_append_entries();
            repl_pending_ = false;
        }
        return r;
    }

    // ---- ConsensusNode: observables -----------------------------------

    [[nodiscard]] Role role() const noexcept override { return role_; }
    [[nodiscard]] Term current_term() const noexcept override { return current_term_; }
    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        // Observable = the FULL logical log (snapshot prefix + retained suffix), so
        // compaction is invisible to the harness (Snapshot.tla ReconstructUpTo).
        // Valid until the next mutation; the harness COPIES it per step (V-RKV1).
        rebuild_log_view();
        return std::span<const LogEntry>(log_view_.data(), log_view_.size());
    }
    [[nodiscard]] Index commit_index() const noexcept override { return commit_index_; }

    // ---- ConsensusNode: lifecycle -------------------------------------

    void start() override {
        if (running_) {
            return;
        }
        running_ = true;
        recovered_ = false;
        ++loop_epoch_;
        // Recovery is async (IDisk reads are awaited). Until it completes the node
        // is an idle Follower (safe). The recv loop + ticker gate on recovered_.
        sched_->spawn(recover_task());
        spawn_recv_loop();
        spawn_ticker();
    }

    void crash() override {
        // Simulated power loss: drop ALL non-durable in-memory state. Running
        // coroutines observe running_==false (+ a bumped epoch) and exit. The
        // backing SimDisk::crash() is driven by the harness alongside this.
        running_ = false;
        ++loop_epoch_;
        role_ = Role::Follower;
        current_term_ = 0;
        voted_for_.reset();
        log_.clear();
        snap_base_ = 0;
        snap_state_.clear();
        applied_index_ = 0;
        log_view_.clear();
        commit_index_ = 0;
        votes_granted_.clear();
        next_index_.clear();
        match_index_.clear();
        sent_index_.clear();
        leader_hint_ = UINT64_MAX;
        recovered_ = false;
        repl_pending_ = false;
        repl_kicked_ = false;
        // C4.2: drop the in-memory config chain to init; recovery rebuilds it from
        // the durable CONFIG records (recover-to-prefix).
        cfg_chain_ = init_config_chain();
        cfg_idx_ = 0;
        cfg_committed_idx_ = 0;
        cfg_adopted_.assign(cfg_.cluster.size(), 0);
        // Drop the non-durable write pipeline (un-appended/un-synced records are
        // lost on power loss, matching SimDisk::crash()). retained_ holds bytes for
        // in-flight appends only; a fresh generation starts with an empty pipeline.
        write_queue_.clear();
        retained_.clear();
        worker_running_ = false;
        want_sync_ = false;
    }

    void restart() override {
        // Reopen the durable image, rebuild (term, vote, log) from the durable
        // prefix, re-enter as Follower, resume. Honors the durable vote/term so
        // failover never elects two leaders in a term. Recovery is async; the node
        // stays an idle Follower until it completes.
        role_ = Role::Follower;
        commit_index_ = 0;
        snap_base_ = 0;
        snap_state_.clear();
        applied_index_ = 0;
        log_view_.clear();
        votes_granted_.clear();
        leader_hint_ = UINT64_MAX;
        running_ = true;
        recovered_ = false;
        ++loop_epoch_;
        sched_->spawn(recover_task());
        spawn_recv_loop();
        spawn_ticker();
    }

    [[nodiscard]] std::uint64_t id() const noexcept override { return self_; }

    // C4.3 snapshot introspection (measurement only; not safety-observable).
    [[nodiscard]] Index snapshot_index() const noexcept override { return snap_base_; }
    [[nodiscard]] std::size_t physical_log_size() const noexcept override {
        return log_.size();
    }
    [[nodiscard]] std::uint64_t snapshots_taken() const noexcept override {
        return snapshots_taken_;
    }
    [[nodiscard]] std::uint64_t snapshots_installed() const noexcept override {
        return snapshots_installed_;
    }

    // ---- C4.2 MEMBERSHIP CHANGE (Membership.tla) ----------------------

    [[nodiscard]] bool propose_config_change(std::uint64_t server,
                                             bool add) override {
        if (!running_ || role_ != Role::Leader) {
            return false;
        }
        // commit-before-next: chain SETTLED (cfg_committed_idx_ == cfg_idx_ == head).
        if (cfg_idx_ != cfg_chain_.size() - 1 || cfg_committed_idx_ != cfg_idx_) {
            return false;
        }
        const std::vector<std::uint64_t>& cur = cfg_chain_[cfg_idx_];
        const bool present = in_config(cur, server);
        if (add == present) {
            return false;  // add existing / remove absent ⇒ no-op
        }
        if (add && !in_config(cfg_.cluster, server)) {
            return false;  // can only add from the universe (Server)
        }
        if (!add && server == self_) {
            return false;  // leader stays a member (s \in newC)
        }
        std::vector<std::uint64_t> nc;
        if (add) {
            nc = cur;
            nc.push_back(server);
        } else {
            for (std::uint64_t id : cur) {
                if (id != server) {
                    nc.push_back(id);
                }
            }
        }
        if (nc.empty()) {
            return false;
        }
        sort_ids(nc);
        // Append + adopt immediately (the leader uses C_new at once); others catch up
        // via ConfigChange — the straddle window.
        cfg_chain_.push_back(nc);
        cfg_idx_ = cfg_chain_.size() - 1;
        persist_config(cfg_idx_, nc);
        sync_now();
        broadcast_config_change();
        return true;
    }

    [[nodiscard]] std::vector<std::uint64_t> current_config() const override {
        return cfg_chain_.empty() ? std::vector<std::uint64_t>{}
                                  : cfg_chain_[cfg_idx_];
    }
    [[nodiscard]] std::uint64_t config_index() const noexcept override {
        return cfg_idx_;
    }
    [[nodiscard]] std::uint64_t config_committed_index() const noexcept override {
        return cfg_committed_idx_;
    }

    // ---- factory ------------------------------------------------------

    [[nodiscard]] static ConsensusNodeFactory factory() {
        return [](const NodeDeps& d, const NodeConfig& c)
                   -> std::unique_ptr<ConsensusNode> {
            return std::make_unique<RaftNodeB>(d, c);
        };
    }

private:
    // ===== cluster helpers =============================================

    [[nodiscard]] std::size_t cluster_size() const noexcept { return cfg_.cluster.size(); }
    // C4.2 Membership: quorum is over the CURRENT config (Membership.tla
    // Quorum(Cfg(s))), NOT the fixed cluster. With no change ever proposed the
    // current config == cluster, so this is byte-identical to the fixed quorum.
    [[nodiscard]] std::size_t quorum() const noexcept {
        const std::size_t n = cfg_chain_.empty() ? cluster_size()
                                                 : cfg_chain_[cfg_idx_].size();
        return n / 2 + 1;
    }

    [[nodiscard]] std::size_t index_of(std::uint64_t node) const noexcept {
        for (std::size_t i = 0; i < cfg_.cluster.size(); ++i) {
            if (cfg_.cluster[i] == node) {
                return i;
            }
        }
        return 0;
    }

    // ---- C4.2 MEMBERSHIP helpers (Membership.tla) --------------------
    static void sort_ids(std::vector<std::uint64_t>& v) {
        for (std::size_t i = 1; i < v.size(); ++i) {
            std::uint64_t x = v[i];
            std::size_t j = i;
            while (j > 0 && v[j - 1] > x) {
                v[j] = v[j - 1];
                --j;
            }
            v[j] = x;
        }
    }
    [[nodiscard]] static bool in_config(const std::vector<std::uint64_t>& c,
                                        std::uint64_t id) noexcept {
        for (std::uint64_t m : c) {
            if (m == id) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool in_current_config(std::uint64_t id) const noexcept {
        return !cfg_chain_.empty() && in_config(cfg_chain_[cfg_idx_], id);
    }
    [[nodiscard]] std::vector<std::vector<std::uint64_t>> init_config_chain() const {
        std::vector<std::uint64_t> init =
            cfg_.init_config.empty() ? cfg_.cluster : cfg_.init_config;
        return {init};
    }

    // Logical log length (Snapshot.tla logBase + Len(log)).
    [[nodiscard]] Index llen() const noexcept {
        return snap_base_ + static_cast<Index>(log_.size());
    }
    [[nodiscard]] Index last_log_index() const noexcept { return llen(); }
    [[nodiscard]] Term last_log_term() const noexcept {
        return llen() == 0 ? 0 : term_at(llen());
    }
    // Term at ABSOLUTE 1-based index i (0 if out of range). Reads the snapshot
    // prefix for i <= snap_base_, else the retained suffix.
    [[nodiscard]] Term term_at(Index i) const noexcept {
        if (i == 0 || i > llen()) {
            return 0;
        }
        if (i <= snap_base_) {
            return snap_state_[static_cast<std::size_t>(i - 1)].term;
        }
        return log_[static_cast<std::size_t>(i - snap_base_ - 1)].term;
    }
    // Entry at ABSOLUTE 1-based index i (must be in range).
    [[nodiscard]] const LogEntry& entry_at(Index i) const noexcept {
        if (i <= snap_base_) {
            return snap_state_[static_cast<std::size_t>(i - 1)];
        }
        return log_[static_cast<std::size_t>(i - snap_base_ - 1)];
    }
    void append_entry(const LogEntry& e) { log_.push_back(e); }
    // Truncate the logical log to absolute length new_len (never below snap_base_).
    void truncate_to(Index new_len) {
        if (new_len < snap_base_) {
            new_len = snap_base_;
        }
        log_.resize(static_cast<std::size_t>(new_len - snap_base_));
    }
    void rebuild_log_view() const {
        log_view_.clear();
        log_view_.reserve(snap_state_.size() + log_.size());
        log_view_.insert(log_view_.end(), snap_state_.begin(), snap_state_.end());
        log_view_.insert(log_view_.end(), log_.begin(), log_.end());
    }

    // ===== persistence (persist-before-reply) ==========================
    //
    // Every disk op is fire-and-forget onto the scheduler; we then sync_now().
    // Because these run on the deterministic scheduler, the observable behaviour
    // is a pure function of (seed). We persist BEFORE sending the reply/broadcast
    // that depends on the persisted fact.

    // Each persist_* enqueues a record onto the durable write queue; a single
    // persistence worker coroutine drains it (append, in order) onto IDisk. A
    // trailing sync_now() marks the queue for a durability barrier. Because the
    // worker is sequential and FIFO-scheduled, the on-disk byte stream is a pure
    // function of (seed) and recovers exactly to its synced prefix.
    void persist_term_vote() {
        bool hv = voted_for_.has_value();
        enqueue_durable(durable::term_record(current_term_, hv, hv ? *voted_for_ : 0));
    }
    void persist_entry(std::uint64_t index, const LogEntry& e) {
        enqueue_durable(durable::entry_record(index, e));
    }
    void persist_truncate(std::uint64_t new_len) {
        enqueue_durable(durable::truncate_record(new_len));
    }
    void persist_snapshot() {
        enqueue_durable(durable::snapshot_record(snap_base_, snap_state_));
    }
    // C4.2: persist one config-chain entry (configs live in the durable log).
    void persist_config(std::uint64_t chain_index,
                        const std::vector<std::uint64_t>& members) {
        enqueue_durable(durable::config_record(chain_index, members));
    }

    void enqueue_durable(std::vector<std::byte> rec) {
        write_queue_.push_back(std::move(rec));
        ensure_persist_worker();
    }

    // Request a durability barrier: the worker will sync() after draining the
    // records queued so far (persist-before-reply: the act that follows is only
    // observable after these records are durable in the deterministic order).
    void sync_now() {
        want_sync_ = true;
        ensure_persist_worker();
    }

    void ensure_persist_worker();

    // ===== election timing =============================================

    void arm_election_deadline() {
        core::Tick jitter =
            rng_->uniform_range(cfg_.election_timeout_min, cfg_.election_timeout_max);
        election_deadline_ = clock_->now() + jitter;
    }

    // ===== role transitions ============================================

    // UpdateTerm(s): adopt a strictly-higher term, revert to Follower, clear the
    // vote, persist+sync. Returns true if it stepped down.
    bool maybe_step_down(Term msg_term) {
        if (msg_term <= current_term_) {
            return false;
        }
        current_term_ = msg_term;
        role_ = Role::Follower;
        voted_for_.reset();
        votes_granted_.clear();
        persist_term_vote();
        sync_now();
        return true;
    }

    void become_leader() {
        role_ = Role::Leader;
        leader_hint_ = self_;
        next_index_.assign(cluster_size(), llen() + 1);
        match_index_.assign(cluster_size(), 0);
        sent_index_.assign(cluster_size(), 0);  // S8.2b: no batch in flight yet
        match_index_[index_of(self_)] = llen();
        // C4.2: a new leader must NOT assume its head config is cluster-committed; it
        // re-proves the head via adoption acks (commit-before-next across an election
        // that lands mid-rollout). Reset per-peer adoption + treat the head as
        // unsettled (committed = previous index), then re-broadcast + re-check.
        cfg_adopted_.assign(cluster_size(), 0);
        cfg_committed_idx_ = (cfg_idx_ > 0) ? cfg_idx_ - 1 : 0;
        broadcast_append_entries();  // immediate heartbeat asserts leadership
        if (cfg_committed_idx_ < cfg_idx_) {
            broadcast_config_change();
        }
        maybe_commit_config();  // single-config chains re-settle immediately
    }

    // Timeout(s): become candidate, ++term, self-vote, broadcast RequestVote.
    void begin_election() {
        // C4.2 Timeout guard: only a MEMBER of its own current config stands. A
        // removed server (not in the config it has adopted) does not start an
        // election — it cannot disrupt the cluster (Membership.tla Timeout: s \in Cfg).
        if (!in_current_config(self_)) {
            arm_election_deadline();
            return;
        }
        ++current_term_;
        role_ = Role::Candidate;
        voted_for_ = self_;
        votes_granted_.assign(cluster_size(), false);
        votes_granted_[index_of(self_)] = true;
        // Persist (term,vote) and sync BEFORE the request leaves (persist-before-
        // reply: a self-vote at this term is durable before we solicit others).
        persist_term_vote();
        sync_now();
        // A single-member config is its own quorum: with the self-vote already
        // counted, votes_count() == 1 >= quorum() == 1, so the lone candidate becomes
        // leader immediately — no RequestVote response will ever arrive to elect it.
        // GATED on quorum() == 1 so for any config with >= 2 members this branch is
        // provably UNREACHABLE: a strict no-op, election stays response-driven and the
        // broadcast→arm ordering below is byte-identical to before.
        if (quorum() == 1) {
            arm_election_deadline();
            become_leader();
            return;
        }
        broadcast_request_vote();
        arm_election_deadline();
    }

    [[nodiscard]] std::size_t votes_count() const noexcept {
        // C4.2: count granted votes from CURRENT-config members only (a vote from a
        // server outside the config does not count toward its quorum).
        std::size_t c = 0;
        for (std::size_t i = 0; i < votes_granted_.size(); ++i) {
            if (votes_granted_[i] && i < cfg_.cluster.size() &&
                in_current_config(cfg_.cluster[i])) {
                ++c;
            }
        }
        return c;
    }

    // ===== sending RPCs ================================================

    void broadcast_request_vote() {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::RequestVote));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(last_log_index());
        w.u64(last_log_term());
        w.u64(cfg_idx_);  // C4.2 config-log up-to-date rule
        std::vector<std::byte> frame = w.finish();
        for (std::uint64_t peer : cfg_.cluster) {
            if (peer != self_) {
                send_to(peer, frame);
            }
        }
    }

    void send_append_entries(std::uint64_t peer) {
        std::size_t pi = index_of(peer);
        Index next = next_index_.empty() ? (llen() + 1) : next_index_[pi];
        if (next < 1) {
            next = 1;
        }
        Index prev_index = next - 1;
        // C4.3 InstallSnapshot: the entries the follower needs were DISCARDED by our
        // compaction (prev_index < snap_base_), so AppendEntries cannot carry the
        // prevLog term — ship the snapshot instead (Snapshot.tla InstallSnapshot(s,d)).
        // prev_index == snap_base_ is fine (term_at(snap_base_) is reconstructable).
        if (prev_index < snap_base_) {
            send_install_snapshot(peer);
            return;
        }
        Term prev_term = term_at(prev_index);
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::AppendEntries));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(prev_index);
        w.u64(prev_term);
        w.u64(commit_index_);
        // S8.2b BOUNDED REPLICATION BATCHING: ship at most kMaxBatch entries per
        // AppendEntries (NOT the whole unacked suffix). Capping the chosen
        // [prevLogIndex+1 .. k] range is a pure refinement of Consensus.tla
        // AppendEntries (which already permits a subset) — no invariant weakened. It
        // bounds each send to O(kMaxBatch) reactor work so a depth-K pipelined burst
        // can't build an O(backlog) suffix that, re-shipped x peers x inflight, blocks
        // the single reactor coroutine + starves the heartbeat (the S8.2a 3-node
        // collapse). LIVENESS: the next batch rides the follower's ack (on_append_resp
        // re-kicks send_append_entries while the follower lags) and every heartbeat.
        std::uint64_t avail = llen() - prev_index;
        std::uint64_t count = avail < kMaxBatch ? avail : kMaxBatch;
        w.u64(count);
        Index last = prev_index + static_cast<Index>(count);
        for (Index abs = prev_index + 1; abs <= last; ++abs) {
            const LogEntry& e = entry_at(abs);
            w.u64(e.term);
            w.str(e.value);
        }
        // S8.2b: record the highest index in this batch so on_append_response only
        // re-kicks once the batch is fully ACKED (no in-flight overlap / cascade).
        if (pi < sent_index_.size()) {
            sent_index_[pi] = last;
        }
        send_to(peer, w.finish());
    }

    // Ship our snapshot to a lagging follower (Snapshot.tla InstallSnapshot(s,d)).
    void send_install_snapshot(std::uint64_t peer) {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::InstallSnapshot));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(snap_base_);
        w.u64(static_cast<std::uint64_t>(snap_state_.size()));
        for (const LogEntry& e : snap_state_) {
            w.u64(e.term);
            w.str(e.value);
        }
        send_to(peer, w.finish());
    }

    void broadcast_append_entries() {
        if (role_ != Role::Leader) {
            return;
        }
        for (std::uint64_t peer : cfg_.cluster) {
            if (peer != self_) {
                send_append_entries(peer);
            }
        }
    }

    void send_to(std::uint64_t peer, const std::vector<std::byte>& frame);

    // ===== handling RPCs ===============================================

    void on_request_vote(const wire::Msg& m) {
        // Stale-term request: reply not-granted at our (higher) term.
        if (m.term < current_term_) {
            reply_vote(m.from, false);
            return;
        }
        // HandleRequestVote: m.term == current_term_ here (higher handled by
        // maybe_step_down before dispatch). Grant iff not-yet-voted-for-another
        // AND candidate up-to-date; persist the vote BEFORE replying.
        bool cand_up_to_date =
            m.last_log_term > last_log_term() ||
            (m.last_log_term == last_log_term() && m.last_log_index >= last_log_index());
        // C4.2 config-log up-to-date rule: refuse a candidate whose config-chain
        // index is BEHIND ours (Membership.tla cfgIdx[c] >= cfgIdx[v]). Stops a
        // server stranded on a stale/superseded config from electing a 2nd leader.
        bool cand_cfg_up_to_date = (m.cfg_index >= cfg_idx_);
        bool can_vote = !voted_for_.has_value() || *voted_for_ == m.from;
        bool grant = can_vote && cand_up_to_date && cand_cfg_up_to_date;
        if (grant) {
            voted_for_ = m.from;
            persist_term_vote();
            sync_now();  // persist-before-reply
            // Receiving a valid current-term candidate resets our election timer.
            arm_election_deadline();
        }
        reply_vote(m.from, grant);
    }

    void reply_vote(std::uint64_t to, bool granted) {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::RequestVoteResp));
        w.u64(current_term_);
        w.u64(self_);
        w.u8(granted ? 1 : 0);
        send_to(to, w.finish());
    }

    void on_vote_response(const wire::Msg& m) {
        if (role_ != Role::Candidate || m.term != current_term_ || !m.granted) {
            return;
        }
        std::size_t pi = index_of(m.from);
        if (pi < votes_granted_.size() && !votes_granted_[pi]) {
            votes_granted_[pi] = true;
        }
        if (votes_count() >= quorum()) {
            become_leader();
        }
    }

    void on_append_entries(const wire::Msg& m) {
        // Stale-term AppendEntries: reject at our (higher) term.
        if (m.term < current_term_) {
            reply_append(m.from, false, 0);
            return;
        }
        // m.term == current_term_ (higher handled by maybe_step_down). A current-
        // term AppendEntries from the leader: recognize it (a Candidate steps
        // down to Follower) and reset the election timer.
        role_ = Role::Follower;
        leader_hint_ = m.from;
        arm_election_deadline();

        // logOk: prevLog index 0, OR in range with a matching term. Absolute-index
        // aware (the retained log is a contiguous suffix above the snapshot base).
        // prevLogIndex inside our snapshotted prefix is NOT auto-accepted: term_at
        // reconstructs the term from snap_state_, so the SAME term-match guard
        // applies uniformly — a leader conflicting with our committed/snapshotted
        // prefix is rejected, never allowed to truncate a committed entry.
        bool log_ok = (m.prev_log_index == 0) ||
                      (m.prev_log_index <= llen() &&
                       m.prev_log_term == term_at(m.prev_log_index));
        if (!log_ok) {
            reply_append(m.from, false, 0);
            return;
        }

        // Raft conflict rule over ABSOLUTE indices; entries at/below snap_base_ are
        // already snapshotted (committed) — never re-examine or truncate them.
        std::size_t first_conflict = 0;  // 1-based position within m.entries; 0 = none
        for (std::size_t k = 1; k <= m.entries.size(); ++k) {
            std::uint64_t idx = m.prev_log_index + k;
            if (idx <= snap_base_) {
                continue;  // inside the snapshotted prefix — committed, no conflict
            }
            if (idx <= llen() && term_at(idx) != m.entries[k - 1].term) {
                first_conflict = k;
                break;
            }
        }
        bool mutated = false;
        if (first_conflict == 0) {
            // No conflict among overlapping entries: append only the new tail.
            std::uint64_t already = llen();
            std::uint64_t incoming_end = m.prev_log_index + m.entries.size();
            if (incoming_end > already) {
                std::size_t start = static_cast<std::size_t>(already - m.prev_log_index);
                for (std::size_t k = start; k < m.entries.size(); ++k) {
                    append_entry(m.entries[k]);
                    persist_entry(llen(), m.entries[k]);
                    mutated = true;
                }
            }
            // else: incoming fully subsumed — idempotent no-op.
        } else {
            // Conflict: truncate to ABSOLUTE prevLogIndex + firstConflict - 1, append
            // the incoming suffix from firstConflict on.
            std::uint64_t keep = m.prev_log_index + first_conflict - 1;
            if (keep < llen()) {
                truncate_to(keep);
                persist_truncate(keep);
                mutated = true;
            }
            for (std::size_t k = first_conflict - 1; k < m.entries.size(); ++k) {
                append_entry(m.entries[k]);
                persist_entry(llen(), m.entries[k]);
                mutated = true;
            }
        }
        if (mutated) {
            sync_now();  // persist-before-reply: durable before we ack the match
        }

        // Adopt leaderCommit (bounded by the last index this AE covers).
        std::uint64_t last_new = m.prev_log_index + m.entries.size();
        if (m.leader_commit > commit_index_) {
            std::uint64_t nc = m.leader_commit < last_new ? m.leader_commit : last_new;
            if (nc > commit_index_) {
                commit_index_ = nc;
            }
        }
        // Apply newly-committed entries, then maybe compact (Snapshot.tla Apply +
        // TakeSnapshot).
        apply_and_maybe_snapshot();
        reply_append(m.from, true, last_new);
    }

    // ===================================================================
    // C4.3 HandleInstallSnapshot (Snapshot.tla InstallSnapshot(s,d)): a lagging
    // follower adopts the leader's snapshot WHOLESALE — applied state :=
    // snapshot.state, applied/log base := lastIncludedIndex — then keeps the suffix
    // the leader ships via subsequent AppendEntries. Never regresses.
    // ===================================================================
    void on_install_snapshot(const wire::Msg& m) {
        if (m.term < current_term_) {
            return;  // stale leader; sender steps down
        }
        role_ = Role::Follower;
        leader_hint_ = m.from;
        arm_election_deadline();

        if (m.last_included_index > snap_base_ &&
            m.last_included_index > commit_index_) {
            snap_state_ = m.snap_state;          // the folded committed prefix
            snap_base_ = m.last_included_index;
            applied_index_ = m.last_included_index;
            if (commit_index_ < m.last_included_index) {
                commit_index_ = m.last_included_index;
            }
            log_.clear();          // suffix reset; leader fills it via AppendEntries
            log_view_.clear();
            ++snapshots_installed_;
            persist_snapshot();    // durable: recovery starts from the snapshot
            sync_now();
        }
        // The follower now holds the committed prefix through the leader's snapshot
        // point (it either just adopted it, or already had it committed). Report a
        // match point AT LEAST m.last_included_index so the leader advances
        // next_index past its own snapshot base — otherwise, when we already hold the
        // prefix but our own snap_base_ is lower (caught up via AppendEntries, never
        // self-snapshotted), reporting the stale low snap_base_ makes the leader
        // re-ship the snapshot forever (zero-virtual-time InstallSnapshot livelock).
        // Snapshot.tla InstallSnapshot(s,d): after install commitIndex[d] >= base.
        const Index match = snap_base_ > m.last_included_index ? snap_base_
                                                              : m.last_included_index;
        reply_install_snapshot(m.from, match);
    }

    void reply_install_snapshot(std::uint64_t to, std::uint64_t match_index) {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::InstallSnapshotResp));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(match_index);
        send_to(to, w.finish());
    }

    void on_install_snapshot_response(const wire::Msg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        std::size_t pi = index_of(m.from);
        if (pi >= next_index_.size()) {
            return;
        }
        if (m.match_index > match_index_[pi]) {
            match_index_[pi] = m.match_index;
        }
        next_index_[pi] = m.match_index + 1;
        send_append_entries(m.from);  // continue replication above the snapshot
    }

    // ===== C4.2 MEMBERSHIP rollout (Membership.tla AdoptConfig + commit gate) =====

    void broadcast_config_change() {
        if (role_ != Role::Leader || cfg_chain_.empty()) {
            return;
        }
        const std::vector<std::uint64_t>& head = cfg_chain_[cfg_idx_];
        const std::vector<std::uint64_t>& prev =
            cfg_idx_ > 0 ? cfg_chain_[cfg_idx_ - 1] : head;
        for (std::uint64_t peer : cfg_.cluster) {
            if (peer == self_) {
                continue;
            }
            if (in_config(head, peer) || in_config(prev, peer)) {
                send_config_change_to(peer);
            }
        }
    }

    void send_config_change_to(std::uint64_t peer) {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::ConfigChange));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(cfg_idx_);
        w.u64(static_cast<std::uint64_t>(cfg_chain_[cfg_idx_].size()));
        for (std::uint64_t id : cfg_chain_[cfg_idx_]) {
            w.u64(id);
        }
        send_to(peer, w.finish());
    }

    void on_config_change(const wire::Msg& m) {
        if (m.term < current_term_) {
            return;  // stale leader — ignore
        }
        role_ = Role::Follower;
        leader_hint_ = m.from;
        arm_election_deadline();
        const std::uint64_t idx = m.config_chain_index;
        if (idx > cfg_idx_) {
            if (idx >= cfg_chain_.size()) {
                cfg_chain_.resize(idx + 1);
            }
            cfg_chain_[idx] = m.config_members;
            cfg_idx_ = idx;
            persist_config(cfg_idx_, cfg_chain_[cfg_idx_]);
            sync_now();
        }
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::ConfigChangeResp));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(cfg_idx_);  // the index we now sit at
        send_to(m.from, w.finish());
    }

    void on_config_change_response(const wire::Msg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        std::size_t pi = index_of(m.from);
        if (pi < cfg_adopted_.size() && m.config_chain_index > cfg_adopted_[pi]) {
            cfg_adopted_[pi] = m.config_chain_index;
        }
        maybe_commit_config();
    }

    // Head config committed once a quorum of the head config's members adopted it.
    void maybe_commit_config() {
        if (role_ != Role::Leader || cfg_chain_.empty()) {
            return;
        }
        const std::uint64_t head = static_cast<std::uint64_t>(cfg_chain_.size() - 1);
        if (cfg_committed_idx_ >= head) {
            return;
        }
        const std::vector<std::uint64_t>& hc = cfg_chain_[head];
        std::size_t adopted = 0;
        for (std::size_t i = 0; i < cfg_.cluster.size(); ++i) {
            if (!in_config(hc, cfg_.cluster[i])) {
                continue;
            }
            if (cfg_.cluster[i] == self_) {
                if (cfg_idx_ >= head) {
                    ++adopted;
                }
            } else if (i < cfg_adopted_.size() && cfg_adopted_[i] >= head) {
                ++adopted;
            }
        }
        if (adopted * 2 > hc.size()) {
            cfg_committed_idx_ = head;
        }
    }

    void reply_append(std::uint64_t to, bool success, std::uint64_t match_index) {
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::AppendEntriesResp));
        w.u64(current_term_);
        w.u64(self_);
        w.u8(success ? 1 : 0);
        w.u64(match_index);
        send_to(to, w.finish());
    }

    void on_append_response(const wire::Msg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        std::size_t pi = index_of(m.from);
        if (pi >= next_index_.size()) {
            return;
        }
        if (m.success) {
            match_index_[pi] = m.match_index;
            next_index_[pi] = m.match_index + 1;
            advance_commit_index();
            apply_and_maybe_snapshot();  // apply + maybe compact on the leader
            // S8.2b: ship the next bounded batch at ack rate (tick-only drain would cap
            // 3-node throughput at kMaxBatch/tick), but ONLY once the previous batch is
            // fully ACKED (match_index >= sent_index_) so at most ONE batch is in flight
            // per peer. A naive per-ack re-kick re-ships the still-in-flight tail every
            // ack -> doubles the leader's send rate -> heartbeat starvation -> election
            // (measured: 3-node depth-1 regressed to fault=1). The sent_index_ gate
            // removes the overlap/cascade; no re-kick once caught up (no busy loop).
            if (match_index_[pi] >= sent_index_[pi] && next_index_[pi] <= llen()) {
                send_append_entries(m.from);
            }
        } else {
            // Decrement nextIndex and retry (log backtracking). replicate switches
            // to InstallSnapshot once nextIndex-1 falls below the snapshot base.
            if (next_index_[pi] > 1) {
                --next_index_[pi];
            }
            send_append_entries(m.from);
        }
    }

    // C4.3 Apply + TakeSnapshot (Snapshot.tla Apply(s) / TakeSnapshot(s)). Our
    // state machine is the committed entry LIST, so "fold e" = the entry already
    // sits in snap_state_/log_; applying just advances applied_index_. TakeSnapshot
    // folds the applied prefix [snap_base_+1..applied_index_] into snap_state_ and
    // DISCARDS it from log_ — never discarding an un-applied/uncommitted entry.
    void apply_and_maybe_snapshot() {
        if (commit_index_ > applied_index_) {
            applied_index_ = commit_index_;
        }
        if (applied_index_ > snap_base_ && log_.size() > kSnapshotThreshold) {
            take_snapshot();
        }
    }

    void take_snapshot() {
        const Index i = applied_index_;
        if (i <= snap_base_) {
            return;
        }
        const std::size_t take = static_cast<std::size_t>(i - snap_base_);
        // snapshot.state := snap_state_ ++ retained[snap_base_+1..i] = fold[1..i].
        snap_state_.insert(snap_state_.end(), log_.begin(),
                           log_.begin() + static_cast<std::ptrdiff_t>(take));
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(take));
        snap_base_ = i;
        log_view_.clear();
        ++snapshots_taken_;
        persist_snapshot();  // durable compaction point
        sync_now();
    }

    // AdvanceCommitIndex(s): commit N only when a quorum stores it AND
    // log[N].term == currentTerm (the current-term commitment rule).
    void advance_commit_index() {
        for (Index n = llen(); n > commit_index_; --n) {
            if (term_at(n) != current_term_) {
                continue;  // only current-term entries commit by replication count
            }
            // C4.2: count agreement over the CURRENT config only.
            std::size_t agree = 0;
            for (std::size_t i = 0; i < match_index_.size(); ++i) {
                if (!in_current_config(cfg_.cluster[i])) {
                    continue;
                }
                if (cfg_.cluster[i] == self_) {
                    if (llen() >= n) {
                        ++agree;
                    }
                } else if (match_index_[i] >= n) {
                    ++agree;
                }
            }
            if (agree >= quorum()) {
                commit_index_ = n;
                break;  // highest committable index found (scanning downward)
            }
        }
    }

    // N=1-only: once the persist worker's sync() has made the enqueued prefix
    // durable, the lone leader self-commits its own entries (no peer ack ever
    // arrives in a single-member config). Leader-gated to match the original
    // submit()-path guard. A strict no-op for quorum() >= 2.
    void advance_n1_self_commit() {
        if (role_ != Role::Leader || quorum() != 1) {
            return;
        }
        advance_commit_index();
        apply_and_maybe_snapshot();
    }

    // ===== coroutine drivers (defined out-of-line) =====================

    void spawn_recv_loop();
    void spawn_ticker();

    core::Task recv_loop(std::uint64_t epoch);
    core::Task ticker(std::uint64_t epoch);
    core::Task persist_worker();
    core::Task send_task(std::uint64_t peer, std::vector<std::byte> frame);
    core::Task recover_task();

    // ===== state =======================================================

    core::IScheduler* sched_;
    core::IClock* clock_;
    core::IRandom* rng_;
    core::INetwork* net_;
    core::IDisk* disk_;
    NodeConfig cfg_;
    std::uint64_t self_;

    Role role_ = Role::Follower;
    Term current_term_ = 0;
    std::optional<std::uint64_t> voted_for_{};
    // log_ is the PHYSICALLY-RETAINED SUFFIX above the snapshot point (Snapshot.tla
    // `log[s]`): log_[k] is absolute logical index snap_base_+k+1. The discarded
    // prefix [1..snap_base_] is folded into snap_state_ (our state machine = the
    // committed-prefix entry list).
    std::vector<LogEntry> log_;        // retained suffix (abs idx snap_base_+1 ..)
    Index snap_base_ = 0;              // snapshot.lastIncludedIndex (= logBase[s])
    std::vector<LogEntry> snap_state_; // snapshot.state: folded entries [1..base]
    Index applied_index_ = 0;          // Snapshot.tla appliedIndex[s]
    // Full logical-log view (snap_state_ ++ log_) for the observable log(), so
    // compaction is invisible to the conformance harness (ReconstructUpTo).
    mutable std::vector<LogEntry> log_view_;
    Index commit_index_ = 0;

    // C4.3 compaction trigger: snapshot once the retained suffix exceeds this many
    // entries AND there is a fresh applied prefix to fold + discard.
    static constexpr std::size_t kSnapshotThreshold = 8;
    // S8.2b BOUNDED REPLICATION BATCHING: max entries per AppendEntries (mirror of
    // RaftNodeA::kMaxBatch). Bounds each send to O(kMaxBatch) reactor work so a
    // pipelined burst no longer blocks the single reactor coroutine + starves the
    // heartbeat timer (S8.2a-diagnosed 3-node collapse). 64 = sane Raft default.
    static constexpr std::uint64_t kMaxBatch = 64;
    // Measurement counters (introspection only; NOT persisted, reset on crash).
    std::uint64_t snapshots_taken_ = 0;
    std::uint64_t snapshots_installed_ = 0;

    std::vector<bool> votes_granted_;          // by cluster index (Candidate)
    std::vector<Index> next_index_;            // by cluster index (Leader)
    // S8.2b: highest index shipped to each peer in its last AppendEntries batch; the
    // ack handler re-kicks only once match_index_ >= sent_index_ (previous batch fully
    // landed) so at most one bounded batch is in flight per peer (no re-send cascade).
    std::vector<Index> sent_index_;            // by cluster index (Leader)
    std::vector<Index> match_index_;           // by cluster index (Leader)

    // ---- C4.2 MEMBERSHIP CHANGE state (Membership.tla) ----------------
    // cfg_chain_ = the global config chain (configs[1..], adjacent configs differ by
    // <= 1 server); cfg_idx_ = the latest index this node has ADOPTED (cfgIdx[s],
    // forward-only); cfg_committed_idx_ = highest index known committed cluster-wide
    // (commit-before-next gate); cfg_adopted_ (leader-only, by cluster index) tracks
    // each peer's adopted index so the leader learns when the head reaches a quorum.
    std::vector<std::vector<std::uint64_t>> cfg_chain_;
    std::uint64_t cfg_idx_ = 0;
    std::uint64_t cfg_committed_idx_ = 0;
    std::vector<std::uint64_t> cfg_adopted_;

    std::uint64_t leader_hint_ = UINT64_MAX;

    bool running_ = false;
    bool recovered_ = false;                   // async disk recovery done?
    std::uint64_t loop_epoch_ = 0;             // bumped on crash/restart to retire old coroutines
    core::Tick election_deadline_ = 0;

    // ---- S8.2b replication coalescing (de-dup per-submit broadcast bursts) -----
    // repl_pending_: a submit appended an entry whose replication kick was coalesced
    // away this turn (the ticker re-broadcasts unconditionally next turn). repl_kicked_:
    // a bounded broadcast already fired this scheduler turn, so further submits this
    // turn only mark pending (one kick per turn, not K). Pure in-memory scheduling
    // hints, NOT durable / NOT safety state; reset on crash.
    bool repl_pending_ = false;
    bool repl_kicked_ = false;

    // Durable write pipeline. write_queue_ holds framed records not yet handed to
    // IDisk; the worker drains them in order, then sync()s if want_sync_. A single
    // worker is in flight at a time (worker_running_) so disk ops stay serialized
    // and deterministic. retained_ keeps the record bytes alive for the duration
    // of the (non-owning span) append.
    std::vector<std::vector<std::byte>> write_queue_;
    std::vector<std::vector<std::byte>> retained_;
    bool worker_running_ = false;
    bool want_sync_ = false;
};

// ----------------------------------------------------------------------------
// Out-of-line coroutine + helper definitions.
// ----------------------------------------------------------------------------

inline void RaftNodeB::send_to(std::uint64_t peer, const std::vector<std::byte>& frame) {
    if (!running_) {
        return;
    }
    sched_->spawn(send_task(peer, frame));  // copies the frame into the task
}

inline core::Task RaftNodeB::send_task(std::uint64_t peer, std::vector<std::byte> frame) {
    // INetwork consumes the bytes during the call; the task owns `frame` so the
    // span stays valid. send() completes once accepted for delivery (not received).
    core::Error e = co_await net_->send(core::Endpoint{peer}, std::span<const std::byte>(
                                                                  frame.data(), frame.size()));
    (void)e;  // a partitioned/dropped link is fine — Raft retries via heartbeats.
    co_return;
}

inline void RaftNodeB::ensure_persist_worker() {
    if (worker_running_) {
        return;  // a worker is already draining; it will pick up new records
    }
    worker_running_ = true;
    sched_->spawn(persist_worker());
}

inline core::Task RaftNodeB::persist_worker() {
    // Drain the write queue in order, appending each record to IDisk, then sync()
    // if a barrier was requested. New records enqueued during the drain are picked
    // up before the worker exits, keeping the on-disk order deterministic.
    for (;;) {
        if (!running_) {
            worker_running_ = false;
            co_return;
        }
        if (!write_queue_.empty()) {
            retained_.push_back(std::move(write_queue_.front()));
            write_queue_.erase(write_queue_.begin());
            const std::vector<std::byte>& rec = retained_.back();
            core::Offset off = 0;
            core::Error e = co_await disk_->append(
                std::span<const std::byte>(rec.data(), rec.size()), off);
            (void)e;
            continue;
        }
        if (want_sync_) {
            want_sync_ = false;
            core::Error e = co_await disk_->sync();
            (void)e;
            // POST-SYNC N=1 SELF-COMMIT HOOK. The sync() above made the whole enqueued
            // prefix DURABLE on this node's own disk. For a lone leader (quorum() == 1)
            // that is exactly the condition under which its own entry is committable, so
            // advance commit_index here — AFTER fsync, not at submit-enqueue (the S9.2
            // durability gap). advance_commit_index() logic is UNCHANGED; only WHERE/WHEN
            // the N=1 path calls it moved. Same scheduler thread as submit() — no new
            // concurrency. GATED on quorum() == 1 so for ANY config with >= 2 members this
            // is a strict no-op (commitment stays ack-driven, byte-identical to before).
            advance_n1_self_commit();
            // A sync may have been re-requested or more records enqueued while we
            // awaited; loop to handle them.
            continue;
        }
        worker_running_ = false;
        co_return;
    }
}

inline core::Task RaftNodeB::recover_task() {
    const std::uint64_t epoch = loop_epoch_;
    // Read the whole durable image byte-by-byte until end-of-device (NotFound).
    // Disk faults are off under the cluster driver, so reads are clean; a short
    // read simply bounds the image. Small images (tens of records) ⇒ cheap.
    std::vector<std::byte> img;
    for (;;) {
        if (!running_ || epoch != loop_epoch_) {
            co_return;  // crashed/restarted under us — abandon this recovery
        }
        std::byte one[1];
        core::Error e = co_await disk_->read(static_cast<core::Offset>(img.size()),
                                             std::span<std::byte>(one, 1));
        if (epoch != loop_epoch_ || !running_) {
            co_return;
        }
        if (!e.ok()) {
            break;  // NotFound at end-of-device (or a fault) — image complete
        }
        img.push_back(one[0]);
    }
    durable::Recovered rec = durable::replay(img);
    current_term_ = rec.term;
    voted_for_ = rec.has_vote ? std::optional<std::uint64_t>(rec.vote)
                              : std::optional<std::uint64_t>{};
    snap_base_ = rec.snap_base;
    snap_state_ = std::move(rec.snap_state);
    log_ = std::move(rec.log);
    // The snapshotted prefix is applied by definition; commitIndex is re-learned
    // via replication (never persisted).
    applied_index_ = rec.snap_base;
    log_view_.clear();
    role_ = Role::Follower;
    // C4.2: fold the durable CONFIG records onto the init config (forward-only) to
    // rebuild the adopted chain + cfgIdx. A regressing / gap index is a torn tail —
    // we stop folding there (the records arrived in stream order from replay()).
    cfg_chain_ = init_config_chain();
    cfg_idx_ = 0;
    for (const auto& cr : rec.cfg_records) {
        const std::uint64_t cidx = cr.first;
        if (cidx == 0 || cidx > cfg_chain_.size()) {
            break;  // index 0 is the immutable init config; a gap = torn tail
        }
        if (cidx >= cfg_chain_.size()) {
            cfg_chain_.resize(cidx + 1);
        }
        cfg_chain_[cidx] = cr.second;
        if (cidx > cfg_idx_) {
            cfg_idx_ = cidx;
        }
    }
    cfg_committed_idx_ = cfg_idx_;  // adopted ⇒ locally settled (re-proven if leader)
    cfg_adopted_.assign(cfg_.cluster.size(), 0);
    arm_election_deadline();
    recovered_ = true;
    co_return;
}

inline void RaftNodeB::spawn_recv_loop() { sched_->spawn(recv_loop(loop_epoch_)); }
inline void RaftNodeB::spawn_ticker() { sched_->spawn(ticker(loop_epoch_)); }

inline core::Task RaftNodeB::recv_loop(std::uint64_t epoch) {
    for (;;) {
        core::Message msg = co_await net_->recv();
        if (!running_ || epoch != loop_epoch_) {
            co_return;  // retired by crash/restart
        }
        if (!recovered_) {
            continue;  // not yet recovered: ignore RPC (we are an idle Follower)
        }
        wire::Msg m;
        if (!wire::decode(msg.payload, m)) {
            continue;  // corrupt/torn frame — drop it (CRC guarded)
        }
        // UpdateTerm: a strictly-higher term steps us down BEFORE we process.
        maybe_step_down(m.term);
        switch (m.kind) {
            case wire::Kind::RequestVote:
                on_request_vote(m);
                break;
            case wire::Kind::RequestVoteResp:
                on_vote_response(m);
                break;
            case wire::Kind::AppendEntries:
                on_append_entries(m);
                break;
            case wire::Kind::AppendEntriesResp:
                on_append_response(m);
                break;
            case wire::Kind::InstallSnapshot:
                on_install_snapshot(m);
                break;
            case wire::Kind::InstallSnapshotResp:
                on_install_snapshot_response(m);
                break;
            case wire::Kind::ConfigChange:
                on_config_change(m);
                break;
            case wire::Kind::ConfigChangeResp:
                on_config_change_response(m);
                break;
        }
    }
}

inline core::Task RaftNodeB::ticker(std::uint64_t epoch) {
    // Wake every heartbeat_interval ticks. While Leader, send heartbeats. While
    // Follower/Candidate, fire an election if the (jittered) deadline has passed.
    const core::Tick tick = cfg_.heartbeat_interval > 0 ? cfg_.heartbeat_interval : 1;
    for (;;) {
        co_await clock_->delay(tick);
        if (!running_ || epoch != loop_epoch_) {
            co_return;  // retired by crash/restart
        }
        if (!recovered_) {
            continue;
        }
        if (role_ == Role::Leader) {
            // S8.2b: fresh scheduler turn — re-open the per-turn coalesce gate (clear
            // here, BEFORE the broadcast below, so the next submit-burst re-arms a
            // single bounded kick). repl_pending_ is subsumed by the unconditional
            // heartbeat broadcast that follows, so no separate flush is needed.
            repl_kicked_ = false;
            repl_pending_ = false;
            broadcast_append_entries();
            // C4.2: keep pushing the config rollout until it commits (a dropped
            // ConfigChange is retried each heartbeat); no-op once Settled.
            if (cfg_committed_idx_ < cfg_idx_) {
                broadcast_config_change();
            }
            continue;
        }
        if (clock_->now() >= election_deadline_) {
            begin_election();
        }
    }
}

}  // namespace lockstep::consensus::raft_b
