#pragma once

// RaftNodeA.hpp — Phase 4 Stage I, IMPLEMENTATION A. A complete Raft
// ConsensusNode (the seam in consensus/ConsensusNode.hpp), built STRICTLY to
// conform to the model-checked spec specs/Consensus.tla. This is one of TWO
// independent implementations (impl A here); the dual build is the master-plan
// §6.5 blind-spot defense — so this file is written ONLY from the spec + the
// seam + the providers, never from impl B.
//
// ============================================================================
// SPEC ACTION → CODE MAPPING (specs/Consensus.tla)
// ============================================================================
//   Timeout(s)            -> on_election_timeout(): become Candidate, bump term,
//                            self-vote, persist (term,vote), broadcast RequestVote.
//   UpdateTerm(s)         -> maybe_step_down(m.term): any RPC carrying a strictly
//                            higher term => adopt term, revert to Follower, clear
//                            vote, persist — BEFORE the message is handled. This is
//                            handled as its own step at the top of EVERY handler
//                            (so a stale Leader steps down to Follower first, then
//                            a later AppendEntries may truncate — keeping
//                            LeaderAppendOnly: a Leader never mutates its log).
//   HandleRequestVote(s)  -> handle_request_vote(): grant iff term==current AND
//                            (votedFor in {Nil, cand}) AND candidate up-to-date;
//                            PERSIST votedFor BEFORE sending the response.
//   HandleVoteResponse(s) -> handle_vote_response(): collect a granted vote for the
//                            current term while Candidate.
//   BecomeLeader(s)       -> become_leader(): on a quorum of votes, become Leader,
//                            init nextIndex/matchIndex, send initial heartbeats.
//   ClientRequest(s,v)    -> submit(): only a Leader appends [currentTerm, value];
//                            PERSIST the entry BEFORE it is replicable/commitable.
//   AppendEntries(s,d)    -> replicate_to(d): leader sends entries from nextIndex-1
//                            as prevLogIndex with the suffix; also the heartbeat.
//   HandleAppendEntries(s)-> handle_append_entries(): reject on prevLog mismatch;
//                            else delete only at the FIRST conflicting index, append
//                            the incoming suffix (idempotent on stale/short redeliv),
//                            adopt leaderCommit (Min(leaderCommit,lastNew)); PERSIST
//                            log mutations BEFORE replying success.
//   AdvanceCommitIndex(s) -> advance_commit_index(): commit index N only when a
//                            QUORUM stores it AND log[N].term == currentTerm.
//
// ============================================================================
// PERSIST-BEFORE-REPLY + CRASH/RESTART RECOVERY (IDisk)
// ============================================================================
// Durable image = an append-structured stream of CRC'd, length-framed records on
// the per-node SimDisk: META(term,vote), ENTRY(index,term,value), TRUNC(newLen).
// EVERY action that changes (currentTerm, votedFor, log) appends the matching
// record(s) and SYNCs BEFORE it acts on / replies about that change (spec's
// persist-before-reply). On crash() in-memory state is dropped; restart() reads
// back the DURABLE PREFIX only (SimDisk recover-to-prefix) and replays the records
// to rebuild (currentTerm, votedFor, log) — a torn / un-synced tail record fails
// its CRC/length frame and is dropped, so recovery honors exactly the durable
// prefix. Honoring the durable vote/term is what prevents two leaders in a term
// after a crash.
//
// ============================================================================
// MESSAGE ENCODING (INetwork; the impl owns the wire format)
// ============================================================================
// Each RPC is a flat little-endian byte record framed as [u32 crc32][u32 len]
// [payload]; the payload is [u8 type][fields...]. type ∈ {RequestVote,
// RequestVoteResp, AppendEntries, AppendEntriesResp}. The CRC guards against a
// corrupted delivery; a frame that fails CRC/len is dropped. All fields the spec
// names round-trip (term, source, dest, lastLogIndex/Term, prevLogIndex/Term,
// entries, leaderCommit, granted, success, matchIndex).
//
// ============================================================================
// DETERMINISM (binding; consensus/ is NOT lint-exempt)
// ============================================================================
// Pure function of (seed): ALL time via IClock, ALL randomness (election jitter)
// via IRandom, ALL peer IO via INetwork, ALL durability via IDisk. No wall-clock,
// no threads, no std::*_distribution, no unordered iteration affecting output. The
// log() span is mutation-lifetime only — never parked across a co_await (V-RKV1):
// internal coroutines copy out what they need before awaiting.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>

namespace lockstep::consensus::raft_a {

using core::Endpoint;
using core::Error;
using core::Future;
using core::IClock;
using core::IDisk;
using core::INetwork;
using core::IRandom;
using core::Message;
using core::Tick;

namespace wire {

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, reflected) — a small self-contained implementation so the
// node header carries no external dependency. Deterministic, table-driven.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* data,
                                         std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// ---- little-endian fixed-width put/get over a byte vector ------------------

inline void put_u8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }

inline void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}

inline void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

inline void put_str(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size()));
    for (char c : s) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
}

// Frame a built body into a durable record: [u32 crc32(body)][u32 len][body] as one
// vector<std::byte>, in a SINGLE allocation (reserve) with no intermediate `frame` vector
// and no per-byte copy loop. Byte-identical to the old body->frame->out path (the A/B
// durable cross-check verifies the bytes). Cuts ~1 heap alloc per durable record on the
// hot path (rec_entry runs once per Submit).
[[nodiscard]] inline std::vector<std::byte> frame_record(
    const std::vector<std::uint8_t>& body) {
    const std::uint32_t crc = crc32(body.data(), body.size());
    const std::uint32_t len = static_cast<std::uint32_t>(body.size());
    std::vector<std::byte> out;
    out.reserve(8 + body.size());
    for (std::uint32_t v : {crc, len}) {
        out.push_back(static_cast<std::byte>(v & 0xFFu));
        out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
        out.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
    }
    for (std::uint8_t b : body) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

// A bounds-checked cursor over a byte span. ok() turns false on any overrun, so
// a truncated / corrupt frame decodes to a rejected (empty) message.
struct Reader {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    std::size_t i = 0;
    bool good = true;

    [[nodiscard]] bool ok() const noexcept { return good; }

    std::uint8_t u8() noexcept {
        if (i + 1 > n) {
            good = false;
            return 0;
        }
        return p[i++];
    }
    std::uint32_t u32() noexcept {
        if (i + 4 > n) {
            good = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            v |= static_cast<std::uint32_t>(p[i++]) << (8 * k);
        }
        return v;
    }
    std::uint64_t u64() noexcept {
        if (i + 8 > n) {
            good = false;
            return 0;
        }
        std::uint64_t v = 0;
        for (int k = 0; k < 8; ++k) {
            v |= static_cast<std::uint64_t>(p[i++]) << (8 * k);
        }
        return v;
    }
    std::string str() {
        const std::uint32_t len = u32();
        if (!good || i + len > n) {
            good = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
};

}  // namespace wire

// ---------------------------------------------------------------------------
// Message model (mirrors specs/Consensus.tla's message schema).
// ---------------------------------------------------------------------------
enum class MsgType : std::uint8_t {
    RequestVote = 1,
    RequestVoteResp = 2,
    AppendEntries = 3,
    AppendEntriesResp = 4,
    // C4.3 InstallSnapshot: leader ships its snapshot to a follower whose
    // nextIndex has fallen at/below the leader's discarded prefix (the entries
    // it needs were compacted away, so AppendEntries cannot catch it up).
    InstallSnapshot = 5,
    InstallSnapshotResp = 6,
    // C4.2 MEMBERSHIP (Membership.tla). A dedicated RPC pair carries the config
    // chain rollout, kept OFF the value-log replication path so the value log
    // observable is untouched (the five base-Raft checkers stay byte-identical when
    // no change is proposed). ConfigChange ships ONE new chain config (its chain
    // index + the member set) from the leader; ConfigChangeResp acks adoption.
    ConfigChange = 7,
    ConfigChangeResp = 8,
};

struct RaftMsg {
    MsgType type = MsgType::RequestVote;
    Term term = 0;
    std::uint64_t source = 0;
    std::uint64_t dest = 0;
    // CLUSTER IDENTITY (restore-new-cluster split-brain guard, plan P2). A per-cluster
    // token stamped on every message; a node DROPS any message whose token differs from
    // its own, so a stale node from an old/decommissioned cluster (same ids + ports after
    // a snapshot-restore onto a fresh cluster) can never vote or replicate into it. 0 =
    // the unset/legacy token (a single-cluster deployment leaves it 0 — byte-identical).
    std::uint64_t cluster_token = 0;

    // RequestVote
    Index last_log_index = 0;
    Term last_log_term = 0;
    // C4.2 Membership: the candidate's config-chain index (cfgIdx[c]). A voter
    // refuses a candidate whose cfgIdx is BEHIND its own (the config-log up-to-date
    // rule, Membership.tla RequestVote). Always carried on RequestVote.
    std::uint64_t cfg_index = 0;

    // C4.2 ConfigChange / ConfigChangeResp: the chain index of the proposed config
    // and its member set (sorted ids). ConfigChangeResp echoes the index it adopted.
    std::uint64_t config_chain_index = 0;
    std::vector<std::uint64_t> config_members;

    // RequestVoteResp / AppendEntriesResp / InstallSnapshotResp
    bool granted = false;  // vote granted
    bool success = false;  // append accepted
    Index match_index = 0;

    // AppendEntries
    Index prev_log_index = 0;
    Term prev_log_term = 0;
    Index leader_commit = 0;
    std::vector<LogEntry> entries;

    // InstallSnapshot (Snapshot.tla InstallSnapshot(s,d)): lastIncludedIndex +
    // the snapshot.state (our state machine = the folded committed entries, so
    // the state is exactly the committed prefix entry list through the base).
    Index last_included_index = 0;
    std::vector<LogEntry> snap_state;
};

// ---------------------------------------------------------------------------
// Encode a RaftMsg to a CRC'd, length-framed byte buffer. Frame layout:
//   [u32 crc32(payload)][u32 payload_len][payload...]
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<std::byte> encode(const RaftMsg& m) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(m.type));
    wire::put_u64(body, m.term);
    wire::put_u64(body, m.source);
    wire::put_u64(body, m.dest);
    wire::put_u64(body, m.cluster_token);  // cluster-identity guard (P2)
    switch (m.type) {
        case MsgType::RequestVote:
            wire::put_u64(body, m.last_log_index);
            wire::put_u64(body, m.last_log_term);
            wire::put_u64(body, m.cfg_index);  // C4.2 config-log up-to-date check
            break;
        case MsgType::RequestVoteResp:
            wire::put_u8(body, m.granted ? 1u : 0u);
            break;
        case MsgType::AppendEntries:
            wire::put_u64(body, m.prev_log_index);
            wire::put_u64(body, m.prev_log_term);
            wire::put_u64(body, m.leader_commit);
            wire::put_u32(body, static_cast<std::uint32_t>(m.entries.size()));
            for (const LogEntry& e : m.entries) {
                wire::put_u64(body, e.term);
                wire::put_str(body, e.value);
            }
            break;
        case MsgType::AppendEntriesResp:
            wire::put_u8(body, m.success ? 1u : 0u);
            wire::put_u64(body, m.match_index);
            break;
        case MsgType::InstallSnapshot:
            wire::put_u64(body, m.last_included_index);
            wire::put_u32(body, static_cast<std::uint32_t>(m.snap_state.size()));
            for (const LogEntry& e : m.snap_state) {
                wire::put_u64(body, e.term);
                wire::put_str(body, e.value);
            }
            break;
        case MsgType::InstallSnapshotResp:
            wire::put_u64(body, m.match_index);  // follower's new logical length
            break;
        case MsgType::ConfigChange:
            wire::put_u64(body, m.config_chain_index);
            wire::put_u32(body, static_cast<std::uint32_t>(m.config_members.size()));
            for (std::uint64_t id : m.config_members) {
                wire::put_u64(body, id);
            }
            break;
        case MsgType::ConfigChangeResp:
            wire::put_u64(body, m.config_chain_index);  // chain index acked
            break;
    }

    const std::uint32_t crc =
        wire::crc32(body.data(), body.size());

    std::vector<std::uint8_t> frame;
    wire::put_u32(frame, crc);
    wire::put_u32(frame, static_cast<std::uint32_t>(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());

    std::vector<std::byte> out(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        out[i] = static_cast<std::byte>(frame[i]);
    }
    return out;
}

// Decode a frame. Returns false on any CRC / length / field overrun (a corrupt
// delivery is dropped — defense in depth over the bus's own faults).
[[nodiscard]] inline bool decode(std::span<const std::byte> bytes, RaftMsg& out) {
    if (bytes.size() < 8) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    wire::Reader head{raw, bytes.size(), 0, true};
    const std::uint32_t crc = head.u32();
    const std::uint32_t len = head.u32();
    if (!head.ok() || 8u + len > bytes.size()) {
        return false;
    }
    const std::uint8_t* body = raw + 8;
    if (wire::crc32(body, len) != crc) {
        return false;
    }
    wire::Reader r{body, len, 0, true};
    out.type = static_cast<MsgType>(r.u8());
    out.term = r.u64();
    out.source = r.u64();
    out.dest = r.u64();
    out.cluster_token = r.u64();  // cluster-identity guard (P2)
    switch (out.type) {
        case MsgType::RequestVote:
            out.last_log_index = r.u64();
            out.last_log_term = r.u64();
            out.cfg_index = r.u64();  // C4.2 config-log up-to-date check
            break;
        case MsgType::RequestVoteResp:
            out.granted = (r.u8() != 0);
            break;
        case MsgType::AppendEntries: {
            out.prev_log_index = r.u64();
            out.prev_log_term = r.u64();
            out.leader_commit = r.u64();
            const std::uint32_t count = r.u32();
            // Guard a wild count so a corrupt frame cannot drive a huge alloc.
            if (!r.ok() || count > len) {
                return false;
            }
            out.entries.clear();
            out.entries.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                out.entries.push_back(std::move(e));
            }
            break;
        }
        case MsgType::AppendEntriesResp:
            out.success = (r.u8() != 0);
            out.match_index = r.u64();
            break;
        case MsgType::InstallSnapshot: {
            out.last_included_index = r.u64();
            const std::uint32_t count = r.u32();
            if (!r.ok() || count > len) {
                return false;
            }
            out.snap_state.clear();
            out.snap_state.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                out.snap_state.push_back(std::move(e));
            }
            break;
        }
        case MsgType::InstallSnapshotResp:
            out.match_index = r.u64();
            break;
        case MsgType::ConfigChange: {
            out.config_chain_index = r.u64();
            const std::uint32_t count = r.u32();
            if (!r.ok() || count > len) {
                return false;
            }
            out.config_members.clear();
            out.config_members.reserve(count);
            for (std::uint32_t k = 0; k < count; ++k) {
                out.config_members.push_back(r.u64());
            }
            break;
        }
        case MsgType::ConfigChangeResp:
            out.config_chain_index = r.u64();
            break;
        default:
            return false;
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
// Durable record format (IDisk). Each record is [u32 crc32(rec)][u32 len][rec];
// rec = [u8 kind][fields]. Replayed in order on restart.
// ---------------------------------------------------------------------------
enum class RecKind : std::uint8_t {
    Meta = 1,   // currentTerm, votedFor (UINT64_MAX = Nil)
    Entry = 2,  // index, term, value (an appended log entry)
    Trunc = 3,  // new_len (truncate the log to this length)
    // C4.3 Snapshot.tla TakeSnapshot/InstallSnapshot: lastIncludedIndex + the
    // folded snapshot.state (our state machine = the committed prefix entry
    // list). On replay a SNAPSHOT record RESETS the reconstructed log to its
    // state and sets the base, so the durable prefix it covers is compacted
    // (recovery replays snapshot + retained suffix, never the discarded prefix).
    Snapshot = 4,
    // C4.2 MEMBERSHIP (Membership.tla): one config-chain entry. Configs live in the
    // log (Raft §4.1), so they are durable: a CONFIG record carries the chain index
    // + the member set. On replay, CONFIG records rebuild the config chain and the
    // node's adopted cfgIdx (= the highest chain index it durably stored), so a
    // restarted node honors the config it had adopted (a removed server stays
    // removed; an added one stays added) — never regressing membership across crash.
    Config = 5,
    // INCREMENTAL snapshot persist: instead of re-serializing the WHOLE folded state on
    // every compaction (a full Snapshot record, O(current size) ⇒ O(n^2) of disk I/O over
    // a long run), a SnapshotDelta carries ONLY the entries folded since the last persist
    // — [from_base+1 .. to_base]. On replay deltas ACCUMULATE (append to the reconstructed
    // state, advance the base), so the durable snapshot stream is append-only and each
    // entry is written exactly once. A full Snapshot record still RESETS (used by
    // InstallSnapshot, where a follower replaces its whole state with the leader's). The
    // committed log is unchanged (compaction stays transparent — Snapshot.tla).
    SnapshotDelta = 6,
};

[[nodiscard]] inline std::vector<std::byte> rec_meta(Term term,
                                                     std::uint64_t voted_for) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::Meta));
    wire::put_u64(body, term);
    wire::put_u64(body, voted_for);
    return wire::frame_record(body);
}

[[nodiscard]] inline std::vector<std::byte> rec_entry(Index index,
                                                      const LogEntry& e) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::Entry));
    wire::put_u64(body, index);
    wire::put_u64(body, e.term);
    wire::put_str(body, e.value);
    return wire::frame_record(body);
}

[[nodiscard]] inline std::vector<std::byte> rec_trunc(Index new_len) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::Trunc));
    wire::put_u64(body, new_len);
    return wire::frame_record(body);
}

// SNAPSHOT durable record: lastIncludedIndex + the folded state (the committed
// prefix entry list). On replay it resets the reconstructed (base, state) — the
// durable compaction point.
[[nodiscard]] inline std::vector<std::byte> rec_snapshot(
    Index last_included_index, const std::vector<LogEntry>& state) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::Snapshot));
    wire::put_u64(body, last_included_index);
    wire::put_u32(body, static_cast<std::uint32_t>(state.size()));
    for (const LogEntry& e : state) {
        wire::put_u64(body, e.term);
        wire::put_str(body, e.value);
    }
    return wire::frame_record(body);
}

// SNAPSHOT-DELTA durable record: [from_base, to_base] + ONLY the entries in
// (from_base, to_base] (the increment folded since the last persisted snapshot point).
// On replay it APPENDS to the reconstructed state and advances the base — append-only
// compaction so each committed entry is written to the snapshot stream exactly once.
[[nodiscard]] inline std::vector<std::byte> rec_snapshot_delta(
    Index from_base, Index to_base, const std::vector<LogEntry>& delta) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::SnapshotDelta));
    wire::put_u64(body, from_base);
    wire::put_u64(body, to_base);
    wire::put_u32(body, static_cast<std::uint32_t>(delta.size()));
    for (const LogEntry& e : delta) {
        wire::put_u64(body, e.term);
        wire::put_str(body, e.value);
    }
    return wire::frame_record(body);
}

// C4.2 CONFIG durable record: chain index + member set. Replayed to rebuild the
// config chain + the node's adopted cfgIdx.
[[nodiscard]] inline std::vector<std::byte> rec_config(
    std::uint64_t chain_index, const std::vector<std::uint64_t>& members) {
    std::vector<std::uint8_t> body;
    wire::put_u8(body, static_cast<std::uint8_t>(RecKind::Config));
    wire::put_u64(body, chain_index);
    wire::put_u32(body, static_cast<std::uint32_t>(members.size()));
    for (std::uint64_t id : members) {
        wire::put_u64(body, id);
    }
    return wire::frame_record(body);
}

// ---------------------------------------------------------------------------
// RaftNodeA — the implementation.
// ---------------------------------------------------------------------------
class RaftNodeA final : public ConsensusNode {
public:
    RaftNodeA(const NodeDeps& deps, const NodeConfig& cfg)
        : sched_(deps.sched),
          clock_(deps.clock),
          rng_(deps.rng),
          net_(deps.net),
          disk_(deps.disk),
          cfg_(cfg),
          self_(cfg.self_id),
          cluster_token_(cfg.cluster_token) {
        peers_.reserve(cfg.cluster.size());
        for (std::uint64_t id : cfg.cluster) {
            if (id != self_) {
                peers_.push_back(id);
            }
        }
        next_index_.assign(cfg.cluster.size(), 0);
        match_index_.assign(cfg.cluster.size(), 0);
        sent_index_.assign(cfg.cluster.size(), 0);
        // C4.2 MEMBERSHIP: seed the config chain with configs[1] = init_config
        // (Membership.tla InitConfig). EMPTY init_config ⇒ the full cluster (fixed
        // membership; byte-identical to the pre-membership build). The chain + the
        // adopted cfg index are reset on recovery from the durable CONFIG records.
        std::vector<std::uint64_t> init = cfg.init_config.empty() ? cfg.cluster
                                                                  : cfg.init_config;
        cfg_chain_.clear();
        cfg_chain_.push_back(init);
        cfg_idx_ = 0;
        cfg_committed_idx_ = 0;
        cfg_adopted_.assign(cfg.cluster.size(), 0);
    }

    RaftNodeA(const RaftNodeA&) = delete;
    RaftNodeA& operator=(const RaftNodeA&) = delete;
    RaftNodeA(RaftNodeA&&) = delete;
    RaftNodeA& operator=(RaftNodeA&&) = delete;
    ~RaftNodeA() override = default;

    // ---- client surface (spec ClientRequest) -----------------------------

    [[nodiscard]] SubmitResult submit(const std::string& value) override {
        SubmitResult r;
        if (!running_ || role_ != Role::Leader) {
            r.accepted = false;
            r.leader_hint = leader_hint_;
            return r;
        }
        // ClientRequest(s,v): append [currentTerm, value]. Persist the entry
        // BEFORE it is replicable/committable (persist-before-reply: a leader must
        // not count an entry toward commitment unless it is durable on itself).
        LogEntry e;
        e.term = current_term_;
        e.value = value;
        append_entry(e);
        const Index idx = llen();
        match_index_[self_idx()] = idx;  // leader trivially stores its own entry
        persist_entry(idx, e);
        r.accepted = true;
        r.term = current_term_;
        r.index = idx;
        // SINGLE-MEMBER CONFIG SELF-COMMIT. Commitment is normally driven from
        // handle_append_entries_resp (a peer ack). With NO peers in the current config
        // (quorum() == 1 ⇔ config size 1) no ack EVER arrives, so commit_index_ would
        // never advance past 0. For the lone leader the entry becomes committable the
        // instant it is DURABLE — but durability is fsync COMPLETION, not persist-ENQUEUE.
        // S9.2 BUG (async io_uring fdatasync): advancing here, right after persist_entry()
        // ENQUEUES the record, marked the entry committed while it was still page-cache-only;
        // an abrupt crash (SIGKILL / power loss) in the enqueue->fsync window lost a
        // COMMITTED entry ("commit implies durable" violated). Synchronous fsync masked the
        // sub-ms gap; async io_uring widened + exposed it. FIX: the N=1 self-commit now fires
        // at the persist worker's POST-SYNC point (advance_n1_self_commit_, called once
        // sync() completes for the just-persisted prefix), so commit_index_ only follows
        // the leader's own fsync. The N>=2 path is UNTOUCHED: it stays ack-driven (a
        // follower acks only after IT persists; a quorum of durable followers makes the
        // entry durable even if the leader's own fsync lags), so for >= 2 members nothing
        // in this submit() or the worker's post-sync hook changes — byte-identical.
        // S8.2b COALESCE the per-submit broadcast. The OLD code called
        // broadcast_append() on EVERY submit, so a depth-K pipelined burst did K
        // full re-scans of the unacked suffix x peers — O(backlog x inflight)
        // synchronous reactor work that blocked the one coroutine and starved the
        // heartbeat (S8.2a collapse). Now a submit only marks replication PENDING and
        // kicks ONE bounded pass per scheduler turn: repl_kicked_ de-dups repeated
        // submits arriving back-to-back within the same turn so they coalesce into a
        // single bounded broadcast instead of K. The flag is cleared at the top of
        // every timer-loop tick (a new turn) and a still-pending backlog is flushed
        // there, so liveness holds: pending entries beyond the first bounded batch
        // ship on the next ack (handle_append_entries_resp re-kick) or next tick.
        repl_pending_ = true;
        if (!repl_kicked_) {
            repl_kicked_ = true;
            broadcast_append(/*heartbeat=*/false);
            repl_pending_ = false;
        }
        return r;
    }

    // ---- observables (map onto specs/Consensus.tla state variables) -------

    [[nodiscard]] Role role() const noexcept override { return role_; }
    [[nodiscard]] Term current_term() const noexcept override {
        return current_term_;
    }
    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        // Observable = the FULL logical log (snapshot prefix + retained suffix),
        // so compaction is invisible to the conformance harness (Snapshot.tla
        // ReconstructUpTo). The view is rebuilt here and is valid until the next
        // mutation — the harness COPIES it per step (V-RKV1).
        rebuild_log_view();
        return {log_view_.data(), log_view_.size()};
    }
    [[nodiscard]] Index commit_index() const noexcept override {
        return commit_index_;
    }

    // CTRL (P4 part 2): the repair watermark (0 = healthy). Non-zero after recovery
    // detected a corrupt committed entry with a valid physical suffix — the node is in
    // repair mode (won't run/vote-short) until replication refills its log to this length.
    [[nodiscard]] Index repair_watermark() const noexcept { return repair_watermark_; }

    // ---- lifecycle the harness drives ------------------------------------

    void start() override {
        if (running_) {
            return;
        }
        running_ = true;
        recovered_ = false;
        // Recover any durable state (first start sees an empty disk → no-op). The
        // election deadline is armed at the END of recovery so we never start an
        // election on un-recovered (stale) term/vote/log state.
        recover_from_disk();
        // Spawn the receive loop + the timer loop. They run until the node is no
        // longer running (crash) — gated on a generation counter so a restart
        // mints fresh loops and stale ones become no-ops.
        sched_->spawn(recv_loop(this, gen_));
        sched_->spawn(timer_loop(this, gen_));
    }

    void crash() override {
        // Simulated power loss: drop ALL non-durable in-memory state. Bump the
        // generation so any in-flight loop coroutine observes the change and exits
        // (it serves nothing until restart()). The durable disk image survives
        // (the harness drives SimDisk::crash() alongside this).
        running_ = false;
        recovered_ = false;
        ++gen_;
        role_ = Role::Follower;
        current_term_ = 0;
        voted_for_ = kNil;
        log_.clear();
        snap_base_ = 0;
        snap_state_.clear();
        snap_persisted_base_ = 0;
        applied_index_ = 0;
        log_view_.clear();
        commit_index_ = 0;
        repair_watermark_ = 0;  // CTRL (P4): re-derived fresh on the next recovery
        votes_granted_.clear();
        leader_hint_ = kNoLeader;
        for (auto& v : next_index_) {
            v = 0;
        }
        for (auto& v : match_index_) {
            v = 0;
        }
        // C4.2: drop the in-memory config chain to the init config; recovery rebuilds
        // the adopted chain from the durable CONFIG records (recover-to-prefix).
        cfg_chain_ = init_config_chain();
        cfg_idx_ = 0;
        cfg_committed_idx_ = 0;
        for (auto& v : cfg_adopted_) {
            v = 0;
        }
        // Power loss drops the non-durable write pipeline: any record appended but
        // not yet synced is lost (matches SimDisk::crash() — recover-to-prefix).
        write_queue_.clear();
        retained_.clear();
        worker_running_ = false;
        want_sync_ = false;
        repl_pending_ = false;
        repl_kicked_ = false;
    }

    void restart() override {
        // Reopen the durable image and rebuild (currentTerm, votedFor, log) from
        // the surviving durable PREFIX, re-enter as Follower, resume.
        running_ = true;
        recovered_ = false;
        ++gen_;
        role_ = Role::Follower;
        commit_index_ = 0;
        snap_base_ = 0;
        snap_state_.clear();
        snap_persisted_base_ = 0;
        applied_index_ = 0;
        log_view_.clear();
        votes_granted_.clear();
        leader_hint_ = kNoLeader;
        recover_from_disk();  // arms the election deadline when recovery completes
        sched_->spawn(recv_loop(this, gen_));
        sched_->spawn(timer_loop(this, gen_));
    }

    [[nodiscard]] std::uint64_t id() const noexcept override { return self_; }

    // C4.3 snapshot introspection (measurement only; not safety-observable).
    [[nodiscard]] Index snapshot_index() const noexcept override {
        return snap_base_;
    }
    [[nodiscard]] std::size_t physical_log_size() const noexcept override {
        return log_.size();
    }
    [[nodiscard]] std::uint64_t snapshots_taken() const noexcept override {
        return snapshots_taken_;
    }
    [[nodiscard]] std::uint64_t snapshots_installed() const noexcept override {
        return snapshots_installed_;
    }

    // ---- C4.2 MEMBERSHIP CHANGE (Membership.tla) -------------------------

    // ProposeConfigChange(s): a CURRENT-term Leader, while its config is SETTLED
    // (previous change committed — commit-before-next), proposes a SINGLE-server
    // change. Returns true iff admissible + proposed.
    [[nodiscard]] bool propose_config_change(std::uint64_t server,
                                             bool add) override {
        if (!running_ || role_ != Role::Leader) {
            return false;
        }
        // commit-before-next: the chain must be SETTLED — the previous change fully
        // propagated (cfg_committed_idx_ == cfg_idx_ == head). Without this, two
        // single-server changes compose into a net 2-server jump whose old/new
        // majorities can be DISJOINT (Ongaro §4.2.3 / Membership.tla Settled).
        if (cfg_idx_ != cfg_chain_.size() - 1 || cfg_committed_idx_ != cfg_idx_) {
            return false;
        }
        const std::vector<std::uint64_t>& cur = cfg_chain_[cfg_idx_];
        const bool present = in_config(cur, server);
        if (add == present) {
            return false;  // add an existing / remove an absent member ⇒ no-op
        }
        if (add && !in_config(cfg_.cluster, server)) {
            return false;  // can only add a server from the universe (Server set)
        }
        if (!add && server == self_) {
            return false;  // leader stays a member (no self-removal; spec s \in newC)
        }
        // Build the new config (delta == 1 by construction: one add or one remove).
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
            return false;  // never empty the cluster
        }
        sort_ids(nc);
        // Append the new config to the chain; the leader adopts it IMMEDIATELY
        // (Raft: a leader uses C_new as soon as it is appended to its log). Other
        // servers catch up via the ConfigChange RPC — THIS is the straddle window.
        cfg_chain_.push_back(nc);
        cfg_idx_ = cfg_chain_.size() - 1;
        persist_config(cfg_idx_, nc);
        // Replicate to the UNION of old+new config members (a removed server is not
        // told; an added server learns it joined). The leader counts adoptions from
        // the NEW config to learn when the change is committed (commit-before-next).
        broadcast_config_change();
        return true;
    }

    // FORCE-NEW-CLUSTER (plan P5 — quorum-loss recovery). After PERMANENT majority loss
    // the cluster cannot commit (no quorum survives). This UNILATERALLY reconfigures the
    // survivor into a single-node cluster {self} under a FRESH cluster identity, KEEPING
    // its committed log, so it self-elects (its own config is quorum 1) and resumes
    // committing. DANGEROUS + operator-only: it bypasses the quorum-gated config protocol,
    // so it is sound ONLY when the old majority is truly gone (pick the most up-to-date
    // survivor — a behind survivor loses the entries it lacks). The fresh token makes any
    // surviving old member a FOREIGN cluster (dropped by the identity guard, P2) so it
    // cannot rejoin and fork. Members are re-added afterward via the normal add path (they
    // catch up from this survivor's log). NOT durable across restart by itself: relaunch
    // the survivor with --cluster-token = new_token to keep the new identity.
    void force_new_cluster(std::uint64_t new_token) override {
        if (!running_) {
            return;
        }
        cfg_chain_.assign(1, std::vector<std::uint64_t>{self_});
        cfg_idx_ = 0;
        cfg_committed_idx_ = 0;
        for (auto& v : cfg_adopted_) {
            v = 0;
        }
        cluster_token_ = new_token;
        persist_config(0, cfg_chain_[0]);  // durable single-node config (recovers on restart)
        // Step down to Follower + re-arm; on the next timeout the node self-elects (its
        // {self} config is its own quorum) and commits its preserved log forward.
        role_ = Role::Follower;
        leader_hint_ = kNoLeader;
        arm_election_deadline();
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

private:
    static constexpr std::uint64_t kNil = UINT64_MAX;       // votedFor = Nil
    static constexpr std::uint64_t kNoLeader = UINT64_MAX;  // unknown leader

    // ===================================================================
    // Background loops. Both are gated on the generation counter `my_gen`:
    // after crash()/restart() the counter changes, so a loop from a prior
    // generation becomes an immediate no-op (it stops awaiting + returns).
    // ===================================================================

    static core::Task recv_loop(RaftNodeA* self, std::uint64_t my_gen) {
        for (;;) {
            if (self->gen_ != my_gen || !self->running_) {
                co_return;
            }
            Message msg = co_await self->net_->recv();
            if (self->gen_ != my_gen || !self->running_) {
                co_return;  // crashed while parked on recv()
            }
            RaftMsg m;
            if (self->recovered_ && decode(msg.payload, m) &&
                m.dest == self->self_ && m.cluster_token == self->cluster_token_) {
                // Foreign-cluster messages (a stale node from a decommissioned cluster
                // with the same id/port after a restore) are DROPPED here (V-CLUSTER-ID).
                self->dispatch(m);
            }
        }
    }

    static core::Task timer_loop(RaftNodeA* self, std::uint64_t my_gen) {
        // A coarse virtual-time wheel: wake every `tick` ticks and check the
        // election / heartbeat deadlines. We cannot preempt a parked recv(), so
        // deadlines are enforced by this periodic wake (a pure function of the
        // virtual clock). The granularity is a fraction of the heartbeat interval
        // so timing stays responsive yet bounded.
        const Tick step = self->timer_step();
        for (;;) {
            if (self->gen_ != my_gen || !self->running_) {
                co_return;
            }
            co_await self->clock_->delay(step);
            if (self->gen_ != my_gen || !self->running_) {
                co_return;
            }
            if (!self->recovered_) {
                continue;  // do not act until durable state is recovered + armed
            }
            const Tick now = self->clock_->now();
            if (self->role_ == Role::Leader) {
                // S8.2b: a fresh scheduler turn. Re-open the per-turn coalesce gate
                // (the next submit-burst kicks at most ONE bounded broadcast), and if
                // a previous burst left replication PENDING (its kick was coalesced
                // away), flush it now with a single bounded pass so no backlog stalls.
                self->repl_kicked_ = false;
                if (self->repl_pending_) {
                    self->repl_pending_ = false;
                    self->broadcast_append(/*heartbeat=*/false);
                }
                if (now >= self->heartbeat_deadline_) {
                    self->broadcast_append(/*heartbeat=*/true);
                    // C4.2: keep pushing the config rollout until it commits (a
                    // dropped/partitioned ConfigChange is retried on each heartbeat,
                    // so the change makes progress under faults). No-op once
                    // committed (Settled) — only re-sent while the chain is in flight.
                    if (self->cfg_committed_idx_ < self->cfg_idx_) {
                        self->broadcast_config_change();
                    }
                    self->heartbeat_deadline_ = now + self->cfg_.heartbeat_interval;
                }
            } else {
                if (now >= self->election_deadline_) {
                    self->on_election_timeout();
                }
            }
        }
    }

    [[nodiscard]] Tick timer_step() const noexcept {
        Tick s = cfg_.heartbeat_interval / 2;
        if (s < 1) {
            s = 1;
        }
        return s;
    }

    // ===================================================================
    // Dispatch — UpdateTerm(s) FIRST (step down on a strictly higher term),
    // then route to the per-type handler at the (possibly adopted) term.
    // ===================================================================

    void dispatch(const RaftMsg& m) {
        // UpdateTerm(s): a strictly higher term on ANY message ⇒ adopt term,
        // revert to Follower, clear vote, persist — BEFORE handling the message.
        maybe_step_down(m.term);

        switch (m.type) {
            case MsgType::RequestVote:
                handle_request_vote(m);
                break;
            case MsgType::RequestVoteResp:
                handle_vote_response(m);
                break;
            case MsgType::AppendEntries:
                handle_append_entries(m);
                break;
            case MsgType::AppendEntriesResp:
                handle_append_entries_resp(m);
                break;
            case MsgType::InstallSnapshot:
                handle_install_snapshot(m);
                break;
            case MsgType::InstallSnapshotResp:
                handle_install_snapshot_resp(m);
                break;
            case MsgType::ConfigChange:
                handle_config_change(m);
                break;
            case MsgType::ConfigChangeResp:
                handle_config_change_resp(m);
                break;
        }
    }

    // UpdateTerm(s): step down on a strictly higher term. Persists (term, Nil).
    void maybe_step_down(Term term) {
        if (term > current_term_) {
            current_term_ = term;
            role_ = Role::Follower;
            voted_for_ = kNil;
            votes_granted_.clear();
            leader_hint_ = kNoLeader;
            persist_meta();
            arm_election_deadline();
        }
    }

    // ===================================================================
    // Timeout(s): become Candidate, bump term, self-vote, broadcast RequestVote.
    // ===================================================================

    void on_election_timeout() {
        if (!running_) {
            return;
        }
        // C4.2 Membership Timeout(s) guard: only a server that is a MEMBER of its own
        // current config stands for election. A server removed by a change (no longer
        // in the config it has adopted) does NOT start an election — it cannot
        // disrupt the cluster (Membership.tla Timeout: s \in Cfg(s)).
        if (!in_current_config(self_)) {
            arm_election_deadline();
            return;
        }
        // CTRL (P4 part 2): while a corrupt committed entry is unrepaired, do NOT stand
        // for election — our log is truncated below the length we HAD, so leading now
        // could drop a committed entry. Wait (as a Follower) to be repaired by a leader.
        if (repair_watermark_ > 0 && llen() < repair_watermark_) {
            arm_election_deadline();
            return;
        }
        // state[s] ∈ {Follower, Candidate} (a Leader does not time out into a new
        // election here — it heartbeats). Bump term, become Candidate, self-vote.
        ++current_term_;
        role_ = Role::Candidate;
        voted_for_ = self_;
        votes_granted_.clear();
        votes_granted_.push_back(self_);
        leader_hint_ = kNoLeader;
        // Persist (term, vote=self) BEFORE soliciting votes (persist-before-reply
        // — a self-vote at a new term must be durable, else a crash could let us
        // re-vote differently at the same term and break ElectionSafety).
        persist_meta();
        arm_election_deadline();

        // A single-node config is its own quorum (quorum over the CURRENT config).
        if (config_votes() >= quorum()) {
            become_leader();
            return;
        }

        RaftMsg base;
        base.type = MsgType::RequestVote;
        base.term = current_term_;
        base.source = self_;
        base.last_log_index = llen();
        base.last_log_term = last_log_term();
        base.cfg_index = cfg_idx_;  // C4.2 config-log up-to-date rule
        for (std::uint64_t d : peers_) {
            RaftMsg m = base;
            m.dest = d;
            send_to(d, m);
        }
    }

    // ===================================================================
    // HandleRequestVote(s): grant iff term current, not already voted (or for the
    // same candidate), and candidate up-to-date. Persist votedFor BEFORE reply.
    // ===================================================================

    void handle_request_vote(const RaftMsg& m) {
        // Stale-term request: reject at OUR current term (sender will step down).
        if (m.term < current_term_) {
            RaftMsg resp;
            resp.type = MsgType::RequestVoteResp;
            resp.term = current_term_;
            resp.source = self_;
            resp.dest = m.source;
            resp.granted = false;
            send_to(m.source, resp);
            return;
        }
        // m.term == current_term_ here (higher was handled by maybe_step_down).
        const bool cand_up_to_date =
            (m.last_log_term > last_log_term()) ||
            (m.last_log_term == last_log_term() &&
             m.last_log_index >= llen());
        // C4.2 Membership RequestVote: the CONFIG-LOG up-to-date rule. A voter refuses
        // a candidate whose config-chain index is BEHIND its own (cfgIdx[c] >=
        // cfgIdx[v]). This is ESSENTIAL: it stops a server stranded on a stale,
        // superseded config (e.g. a removed server that never learned it was removed)
        // from collecting a quorum under the old config and electing a SECOND leader.
        const bool cand_cfg_up_to_date = (m.cfg_index >= cfg_idx_);
        const bool can_vote =
            (voted_for_ == kNil || voted_for_ == m.source);
        // CTRL (P4 part 2): if we recovered past a corrupt committed entry, we KNOW we had
        // a log at least `repair_watermark_` long. Refuse a candidate shorter than that —
        // it is missing an entry WE had (possibly committed), so voting it in could lose
        // that entry (the corrupt-node-forms-a-short-majority disaster). This is stricter
        // than the standard up-to-date check, which compares only against our (truncated)
        // last entry.
        const bool not_behind_repair =
            (repair_watermark_ == 0 || m.last_log_index >= repair_watermark_);
        const bool grant =
            can_vote && cand_up_to_date && cand_cfg_up_to_date && not_behind_repair;

        if (grant && voted_for_ != m.source) {
            voted_for_ = m.source;
            persist_meta();  // PERSIST the vote BEFORE replying (persist-before-reply)
            arm_election_deadline();  // granting a vote resets our election timer
        }

        RaftMsg resp;
        resp.type = MsgType::RequestVoteResp;
        resp.term = current_term_;
        resp.source = self_;
        resp.dest = m.source;
        resp.granted = grant;
        send_to(m.source, resp);
    }

    // ===================================================================
    // HandleVoteResponse(s): collect a granted vote for the current term while a
    // Candidate. BecomeLeader(s) on a quorum.
    // ===================================================================

    void handle_vote_response(const RaftMsg& m) {
        if (role_ != Role::Candidate || m.term != current_term_ || !m.granted) {
            return;
        }
        // Add the granting peer to votesGranted (a set: no double-count).
        for (std::uint64_t v : votes_granted_) {
            if (v == m.source) {
                return;
            }
        }
        votes_granted_.push_back(m.source);
        // BecomeLeader(s): quorum over the CURRENT config (Membership.tla). A vote
        // from a non-member (e.g. a server only the OLD config knew) does not count.
        if (config_votes() >= quorum()) {
            become_leader();
        }
    }

    // BecomeLeader(s): on quorum, become Leader, init nextIndex/matchIndex, send
    // initial heartbeats.
    void become_leader() {
        role_ = Role::Leader;
        leader_hint_ = self_;
        const Index last = llen();
        for (std::size_t i = 0; i < next_index_.size(); ++i) {
            next_index_[i] = last + 1;  // optimistic: assume followers match
            match_index_[i] = 0;
            sent_index_[i] = 0;  // S8.2b: no batch in flight to a peer yet
        }
        match_index_[self_idx()] = last;
        // C4.2: reset per-peer config-adoption tracking; the leader knows only its
        // own adoption. A leader re-confirms the head config is committed by
        // re-collecting adoptions (it re-broadcasts ConfigChange below while the
        // chain is in flight). cfg_committed_idx_ is recomputed from acks; if the
        // head was already committed before this term it is re-proven harmlessly.
        cfg_adopted_.assign(cfg_.cluster.size(), 0);
        // A new leader must NOT assume its head config is cluster-committed: it could
        // have adopted the head locally while a quorum had not. Treat the head as
        // UNSETTLED (committed = the previous index) and re-prove it via adoptions
        // before allowing the next change — preserving commit-before-next across an
        // election that lands mid-rollout (Membership.tla Settled re-checked).
        cfg_committed_idx_ = (cfg_idx_ > 0) ? cfg_idx_ - 1 : 0;
        heartbeat_deadline_ = clock_->now();  // beat immediately
        broadcast_append(/*heartbeat=*/true);
        if (cfg_committed_idx_ < cfg_idx_) {
            broadcast_config_change();
        }
        maybe_commit_config();  // single-config chains re-settle immediately
    }

    // ===================================================================
    // AppendEntries(s,d) + heartbeat: leader replicates to each peer from its
    // believed nextIndex (prevLogIndex = nextIndex-1, entries = the suffix).
    // ===================================================================

    void broadcast_append(bool heartbeat) {
        if (role_ != Role::Leader) {
            return;
        }
        for (std::uint64_t d : peers_) {
            replicate_to(d, heartbeat);
        }
    }

    void replicate_to(std::uint64_t dest, bool heartbeat) {
        const std::size_t di = idx_of(dest);
        Index ni = next_index_[di];
        if (ni < 1) {
            ni = 1;
        }
        const Index prev_index = ni - 1;

        // C4.3 InstallSnapshot(s,d): the entries the follower needs (prevLogIndex
        // and below) were DISCARDED by our compaction (prev_index < snap_base_), so
        // AppendEntries cannot carry the prevLog term — ship the snapshot instead.
        // (Snapshot.tla InstallSnapshot guard: nextIndex at/below the leader's
        // lastIncludedIndex.) prev_index == snap_base_ is fine: term_at(snap_base_)
        // is still reconstructable from snap_state_, so a normal AppendEntries works.
        if (prev_index < snap_base_) {
            send_install_snapshot(dest);
            return;
        }

        const Term prev_term = term_at(prev_index);

        RaftMsg m;
        m.type = MsgType::AppendEntries;
        m.term = current_term_;
        m.source = self_;
        m.dest = dest;
        m.prev_log_index = prev_index;
        m.prev_log_term = prev_term;
        m.leader_commit = commit_index_;
        // S8.2b BOUNDED REPLICATION BATCHING. Entries = at most kMaxBatch entries
        // of the suffix after prevLogIndex (NOT the whole unacked suffix). Raft's
        // AppendEntries inherently carries a chosen [prevLogIndex+1 .. k] range, so
        // capping k is a pure refinement of Consensus.tla's AppendEntries (which
        // already permits sending a subset) — no invariant is weakened. This bounds
        // each send to O(kMaxBatch) reactor work, so a depth-K pipelined burst can
        // no longer build an O(backlog) suffix that, re-shipped x peers x inflight,
        // blocks the single reactor coroutine and starves the heartbeat timer (the
        // S8.2a-diagnosed 3-node collapse). LIVENESS: the next batch ships on the
        // follower's ack (handle_append_entries_resp re-kicks replicate_to while the
        // follower still lags) and on every heartbeat — the leader keeps draining.
        // A pure heartbeat still ships up to kMaxBatch (idempotent on the follower).
        (void)heartbeat;
        Index sent = 0;
        for (Index abs = prev_index + 1; abs <= llen() && sent < kMaxBatch;
             ++abs, ++sent) {
            m.entries.push_back(entry_at(abs));
        }
        // S8.2b: record the highest index in this batch so the ack handler can tell
        // whether the batch is fully ACKED (no in-flight overlap) before re-kicking.
        sent_index_[di] = prev_index + sent;
        send_to(dest, m);
    }

    // Ship our snapshot to a lagging follower (Snapshot.tla InstallSnapshot(s,d)).
    void send_install_snapshot(std::uint64_t dest) {
        RaftMsg m;
        m.type = MsgType::InstallSnapshot;
        m.term = current_term_;
        m.source = self_;
        m.dest = dest;
        m.last_included_index = snap_base_;
        m.snap_state = snap_state_;  // the folded committed prefix (= state machine)
        send_to(dest, m);
    }

    // ===================================================================
    // HandleAppendEntries(s): reject on prevLog mismatch; else delete only at the
    // first conflicting index, append the incoming suffix, adopt leaderCommit.
    // Persist log mutations BEFORE replying success.
    // ===================================================================

    void handle_append_entries(const RaftMsg& m) {
        // Stale-term AppendEntries: reject at our term; do NOT touch state.
        if (m.term < current_term_) {
            RaftMsg resp;
            resp.type = MsgType::AppendEntriesResp;
            resp.term = current_term_;
            resp.source = self_;
            resp.dest = m.source;
            resp.success = false;
            resp.match_index = 0;
            send_to(m.source, resp);
            return;
        }
        // m.term == current_term_ (higher handled by maybe_step_down). A
        // current-term AppendEntries means the sender is the term's leader, so we
        // recognize it: a Candidate steps down to Follower. (ElectionSafety: there
        // is exactly one leader at this term, so this is never us.)
        role_ = Role::Follower;
        leader_hint_ = m.source;
        arm_election_deadline();

        // logOk: prevLogIndex == 0, OR in-range with a matching term. Absolute-index
        // aware (Snapshot.tla: the retained log is a contiguous suffix above
        // logBase). prevLogIndex inside our snapshotted prefix (< snap_base_) is NOT
        // auto-accepted: term_at reconstructs the term from snap_state_ for
        // prevLogIndex >= 1, so the SAME term-match guard applies uniformly — a
        // leader whose prevLog conflicts with our (committed, snapshotted) prefix is
        // correctly rejected, never allowed to truncate a committed entry. We never
        // weaken the conflict guard at/below the snapshot point.
        const bool log_ok =
            (m.prev_log_index == 0) ||
            (m.prev_log_index <= llen() &&
             term_at(m.prev_log_index) == m.prev_log_term);

        if (!log_ok) {
            RaftMsg resp;
            resp.type = MsgType::AppendEntriesResp;
            resp.term = current_term_;
            resp.source = self_;
            resp.dest = m.source;
            resp.success = false;
            resp.match_index = 0;
            send_to(m.source, resp);
            return;
        }

        // Raft conflict rule: find the FIRST incoming entry (1-based k) whose term
        // conflicts with our existing log at ABSOLUTE index prevLogIndex+k. Entries
        // at/below snap_base_ are already snapshotted (committed) — never re-examine
        // or truncate them (the prefix is safe; truncation only ever happens above
        // the snapshot point, mirroring the spec).
        bool mutated = false;
        std::size_t first_conflict = 0;  // 1-based k, 0 = none
        for (std::size_t k = 1; k <= m.entries.size(); ++k) {
            const Index at = m.prev_log_index + k;
            if (at <= snap_base_) {
                continue;  // inside the snapshotted prefix — committed, no conflict
            }
            if (at <= llen()) {
                if (term_at(at) != m.entries[k - 1].term) {
                    first_conflict = k;
                    break;
                }
            } else {
                break;  // beyond our log — nothing to conflict with
            }
        }

        if (first_conflict != 0) {
            // Conflict: truncate at the first conflicting absolute index, then
            // append the incoming suffix from there. Truncation happens while we
            // are a Follower (we set role above) — LeaderAppendOnly preserved.
            const Index keep = m.prev_log_index + first_conflict - 1;
            if (llen() > keep) {
                truncate_to(keep);
                persist_trunc(keep);
                mutated = true;
            }
            for (std::size_t k = first_conflict; k <= m.entries.size(); ++k) {
                append_entry(m.entries[k - 1]);
                persist_entry(llen(), m.entries[k - 1]);
                mutated = true;
            }
        } else {
            // No conflict: append only the genuinely-new tail (idempotent if the
            // incoming entries are fully subsumed by what we already hold).
            const Index incoming_last = m.prev_log_index + m.entries.size();
            if (incoming_last > llen()) {
                const std::size_t start =
                    static_cast<std::size_t>(llen() - m.prev_log_index);
                for (std::size_t k = start; k < m.entries.size(); ++k) {
                    append_entry(m.entries[k]);
                    persist_entry(llen(), m.entries[k]);
                    mutated = true;
                }
            }
        }
        (void)mutated;

        // CTRL (P4 part 2): once replication has refilled our log up to the length we HAD
        // before a corrupt-entry recovery, the hole is repaired — resume full participation.
        if (repair_watermark_ > 0 && llen() >= repair_watermark_) {
            repair_watermark_ = 0;
        }

        // Adopt leaderCommit: commitIndex = Max(commitIndex, Min(leaderCommit,
        // lastNew)) where lastNew = prevLogIndex + len(entries).
        const Index last_new = m.prev_log_index + m.entries.size();
        const Index adopt =
            (m.leader_commit < last_new) ? m.leader_commit : last_new;
        if (adopt > commit_index_) {
            commit_index_ = adopt;
        }
        // Apply newly-committed entries to the state machine, then maybe compact
        // (Snapshot.tla Apply + TakeSnapshot).
        apply_and_maybe_snapshot();

        RaftMsg resp;
        resp.type = MsgType::AppendEntriesResp;
        resp.term = current_term_;
        resp.source = self_;
        resp.dest = m.source;
        resp.success = true;
        resp.match_index = last_new;
        send_to(m.source, resp);
    }

    // ===================================================================
    // C4.3 HandleInstallSnapshot (Snapshot.tla InstallSnapshot(s,d)): a lagging
    // follower whose needed entries were discarded by the leader ADOPTS the
    // leader's snapshot WHOLESALE — applied state := snapshot.state, applied/log
    // base := lastIncludedIndex — then keeps the retained suffix the leader ships
    // via subsequent AppendEntries. We never regress: an install that does not
    // advance our snapshot point is ignored (idempotent / stale redelivery).
    // ===================================================================
    void handle_install_snapshot(const RaftMsg& m) {
        // Stale-term: reject at our term (sender steps down).
        if (m.term < current_term_) {
            return;
        }
        role_ = Role::Follower;
        leader_hint_ = m.source;
        arm_election_deadline();

        // Only adopt a snapshot that is strictly ahead of what we already hold;
        // never discard an un-applied/uncommitted suffix we genuinely have that is
        // already past the snapshot point.
        if (m.last_included_index > snap_base_ &&
            m.last_included_index > commit_index_) {
            // Adopt the snapshot wholesale: state := snapshot.state, base := lii.
            snap_state_ = m.snap_state;          // the folded committed prefix
            snap_base_ = m.last_included_index;
            applied_index_ = m.last_included_index;
            if (commit_index_ < m.last_included_index) {
                commit_index_ = m.last_included_index;
            }
            // The retained suffix is reset; the leader fills it via AppendEntries.
            log_.clear();
            log_view_.clear();
            ++snapshots_installed_;
            // Durable: persist the FULL snapshot (RESET) — InstallSnapshot REPLACES the
            // whole state with the leader's, so prior accumulated deltas no longer apply.
            // Subsequent local compactions persist incrementally from this base.
            persist_snapshot_full();
        }
        // CTRL (P4 part 2): a snapshot covering our known-had length repairs the hole.
        if (repair_watermark_ > 0 && llen() >= repair_watermark_) {
            repair_watermark_ = 0;
        }

        RaftMsg resp;
        resp.type = MsgType::InstallSnapshotResp;
        resp.term = current_term_;
        resp.source = self_;
        resp.dest = m.source;
        // Report a match point AT LEAST m.last_included_index: the follower now holds
        // the committed prefix through the leader's snapshot point (it just adopted
        // it, or already had it committed). If we already held the prefix but our own
        // snap_base_ is lower (caught up via AppendEntries, never self-snapshotted),
        // reporting the stale low snap_base_ would make the leader re-ship the
        // snapshot forever (zero-virtual-time InstallSnapshot livelock). Snapshot.tla
        // InstallSnapshot(s,d): after install commitIndex[d] >= base.
        resp.match_index = snap_base_ > m.last_included_index ? snap_base_
                                                              : m.last_included_index;
        send_to(m.source, resp);
    }

    void handle_install_snapshot_resp(const RaftMsg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        const std::size_t di = idx_of(m.source);
        if (m.match_index > match_index_[di]) {
            match_index_[di] = m.match_index;
        }
        // After install, the follower holds [1..match_index]; continue replication
        // from the entry right after the snapshot point.
        next_index_[di] = m.match_index + 1;
        replicate_to(m.source, /*heartbeat=*/false);
    }

    // ===================================================================
    // C4.2 MEMBERSHIP CHANGE rollout (Membership.tla AdoptConfig + the commit-
    // before-next gate). A leader ships ConfigChange (chain index + members) to the
    // union of old+new config; a follower ADOPTS any chain config strictly ahead of
    // its own (cfgIdx only ever moves FORWARD — Membership.tla AdoptConfig) and acks
    // the index it now sits at. The leader marks the change COMMITTED once a quorum
    // of the NEW config has adopted it; only then may the chain accept the next
    // change (Settled). cfgIdx + the chain are DURABLE (configs live in the log).
    // ===================================================================

    // The leader re-broadcasts its head config to every other server in the universe
    // that might still be a member of old or new (a removed server is excluded so it
    // never learns it was removed — it ages out via the config-log up-to-date rule).
    void broadcast_config_change() {
        if (role_ != Role::Leader || cfg_chain_.empty()) {
            return;
        }
        const std::vector<std::uint64_t>& head = cfg_chain_[cfg_idx_];
        const std::vector<std::uint64_t>& prev =
            cfg_idx_ > 0 ? cfg_chain_[cfg_idx_ - 1] : head;
        for (std::uint64_t d : peers_) {
            // Tell members of the new config OR the immediately-previous one (so a
            // server being removed transitions, and a server being added joins).
            if (in_config(head, d) || in_config(prev, d)) {
                send_config_change_to(d);
            }
        }
    }

    void send_config_change_to(std::uint64_t dest) {
        RaftMsg m;
        m.type = MsgType::ConfigChange;
        m.term = current_term_;
        m.source = self_;
        m.dest = dest;
        m.config_chain_index = cfg_idx_;
        m.config_members = cfg_chain_[cfg_idx_];
        send_to(dest, m);
    }

    // Follower handles a ConfigChange: ADOPT the config if it is strictly ahead of
    // ours (forward-only). We extend our chain to match (filling the head). Then ack
    // the index we now hold. A stale-term sender is ignored (it is not the leader).
    void handle_config_change(const RaftMsg& m) {
        if (m.term < current_term_) {
            return;  // stale leader — ignore (it will step down)
        }
        // A current-term ConfigChange means the sender is the term's leader.
        role_ = Role::Follower;
        leader_hint_ = m.source;
        arm_election_deadline();

        const std::uint64_t idx = m.config_chain_index;
        if (idx > cfg_idx_) {
            // Forward-only adopt. Grow the chain to length idx+1 with this head
            // config at position idx (the focused chain is global + totally ordered,
            // so we only ever need the head we are catching up to).
            if (idx >= cfg_chain_.size()) {
                cfg_chain_.resize(idx + 1);
            }
            cfg_chain_[idx] = m.config_members;
            cfg_idx_ = idx;
            persist_config(cfg_idx_, cfg_chain_[cfg_idx_]);
        }

        RaftMsg resp;
        resp.type = MsgType::ConfigChangeResp;
        resp.term = current_term_;
        resp.source = self_;
        resp.dest = m.source;
        resp.config_chain_index = cfg_idx_;  // the index we now sit at
        send_to(m.source, resp);
    }

    // Leader handles a ConfigChangeResp: record the responder's adopted index; once
    // a QUORUM of the NEW (head) config has adopted the head, the change is
    // COMMITTED (Settled) — the chain may then accept the next single-server change.
    void handle_config_change_resp(const RaftMsg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        const std::size_t di = idx_of(m.source);
        if (m.config_chain_index > cfg_adopted_[di]) {
            cfg_adopted_[di] = m.config_chain_index;
        }
        maybe_commit_config();
    }

    // Commit-before-next: the head config is committed when a quorum of the head
    // config's members have adopted the head chain index (the leader counts itself).
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
                    ++adopted;  // the leader has adopted the head
                }
            } else if (cfg_adopted_[i] >= head) {
                ++adopted;
            }
        }
        if (adopted * 2 > hc.size()) {
            cfg_committed_idx_ = head;
        }
    }

    // Leader handles an AppendEntriesResp: advance/back off nextIndex + matchIndex.
    void handle_append_entries_resp(const RaftMsg& m) {
        if (role_ != Role::Leader || m.term != current_term_) {
            return;
        }
        const std::size_t di = idx_of(m.source);
        if (m.success) {
            // matchIndex is monotonic; nextIndex = matchIndex + 1.
            if (m.match_index > match_index_[di]) {
                match_index_[di] = m.match_index;
            }
            next_index_[di] = match_index_[di] + 1;
            advance_commit_index();
            // Apply newly-committed entries on the leader, then maybe compact.
            apply_and_maybe_snapshot();
            // S8.2b: with bounded batches an ack may have covered only the FIRST
            // kMaxBatch of a larger backlog, so we DO want to ship the next batch at
            // ack rate (tick-only drain would cap 3-node throughput at kMaxBatch/tick).
            // BUT we must NOT re-send while the previous batch is still IN FLIGHT: a
            // batch [a..b] acked at index a (a<b) would otherwise re-ship [a+1..],
            // overlapping the un-acked [a+1..b] — that DOUBLES the leader's send rate
            // per ack and tips even depth-1 into heartbeat starvation -> election
            // (measured: 3-node depth-1 regressed to fault=1). sent_index_[di] records
            // the last index we shipped this peer; only re-kick once the follower has
            // ACKED THROUGH it (match_index >= sent_index_) AND more remains — so each
            // peer has at most ONE bounded batch outstanding, no overlap, no cascade.
            if (match_index_[di] >= sent_index_[di] && next_index_[di] <= llen()) {
                replicate_to(m.source, /*heartbeat=*/false);
            }
        } else {
            // prevLog mismatch: back off nextIndex and retry (decrement, floor 1).
            // Floor the backoff at snap_base_+1 so we never chase a nextIndex into
            // our discarded prefix in a loop; replicate_to switches to
            // InstallSnapshot once nextIndex-1 falls below the follower's base.
            if (next_index_[di] > 1) {
                --next_index_[di];
            }
            replicate_to(m.source, /*heartbeat=*/false);
        }
    }

    // ===================================================================
    // AdvanceCommitIndex(s): commit index N only when a QUORUM stores it AND
    // log[N].term == currentTerm (the Raft current-term commitment rule).
    // ===================================================================

    void advance_commit_index() {
        if (role_ != Role::Leader) {
            return;
        }
        for (Index n = llen(); n > commit_index_; --n) {
            // Current-term entry only — never commit an older-term entry by
            // replication count alone (StateMachineSafety).
            if (term_at(n) != current_term_) {
                continue;
            }
            // C4.2 Membership: count agreement over the CURRENT config only (a
            // server outside the config does not contribute to its commit quorum).
            std::size_t agree = 0;
            for (std::size_t i = 0; i < match_index_.size(); ++i) {
                if (!in_current_config(cfg_.cluster[i])) {
                    continue;
                }
                if (i == self_idx()) {
                    ++agree;  // leader stores it (its own log)
                } else if (match_index_[i] >= n) {
                    ++agree;
                }
            }
            if (agree >= quorum()) {
                commit_index_ = n;
                break;  // n is the highest committable index
            }
        }
    }

    // ===================================================================
    // C4.3 Apply + TakeSnapshot (Snapshot.tla Apply(s) / TakeSnapshot(s)).
    // Apply(s): fold every committed-but-unapplied entry into the state machine
    // (our state machine = the committed entry LIST, so "fold e" = append e to
    // snap_state_'s logical continuation). We track applied_index_; the retained
    // suffix holds [snap_base_+1 .. llen()] so applied entries above the base are
    // still in log_. TakeSnapshot(s): once the retained suffix grows past the
    // threshold AND there is a fresh applied prefix to fold (applied_index_ >
    // snap_base_), capture snap_state_ := entries[1..applied_index_] (folding the
    // about-to-be-discarded prefix into the snapshot — the CRITICAL safety: never
    // discard an entry not yet folded) and DISCARD log_[1..applied_index_].
    // ===================================================================
    void apply_and_maybe_snapshot() {
        // Apply: advance applied_index_ up to commit_index_ (entries are already in
        // snap_state_/log_; "applying" here just records how far the state machine
        // has consumed — snap_state_ already equals the committed prefix by
        // construction, so the fold is implicit in the retained representation).
        if (commit_index_ > applied_index_) {
            applied_index_ = commit_index_;
        }
        // TakeSnapshot guard: only fold/discard an APPLIED prefix, and only once the
        // retained suffix is large enough to be worth compacting.
        if (applied_index_ > snap_base_ &&
            log_.size() > cfg_.snapshot_threshold) {
            take_snapshot();
        }
    }

    void take_snapshot() {
        const Index i = applied_index_;  // snapshot through the applied index
        if (i <= snap_base_) {
            return;  // nothing fresh to fold (defensive)
        }
        // snapshot.state := fold[1..i] = snap_state_ ++ retained[snap_base_+1..i].
        // (snap_state_ already holds [1..snap_base_]; append the applied middle.)
        const std::size_t take =
            static_cast<std::size_t>(i - snap_base_);
        snap_state_.insert(snap_state_.end(), log_.begin(),
                           log_.begin() + static_cast<std::ptrdiff_t>(take));
        // DISCARD the log prefix <= i (Snapshot.tla: SubSeq(log, i-base+1, Len)).
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(take));
        snap_base_ = i;
        log_view_.clear();
        ++snapshots_taken_;
        // Persist the snapshot so recovery starts from it (durable compaction).
        persist_snapshot();
    }

    // ===================================================================
    // Persistence (IDisk). THE DURABILITY-ORDERING INVARIANT: durable records
    // MUST hit IDisk in logical mutation order (on-disk order == the order in
    // which (term,vote,log) changed in memory), so recovery replays the exact
    // ordered prefix. We CANNOT spawn one writer coroutine per record: each would
    // await append()+sync() under random per-op disk latency and the spawned
    // writers would RACE, scrambling on-disk order vs logical order. Instead ALL
    // durable writes (meta/term/vote, log entries, truncations) flow through ONE
    // in-order FIFO queue drained by a SINGLE sequential persist worker. The
    // worker appends each queued record in arrival order, then sync()s — so the
    // byte stream is a pure function of (seed) and recovers exactly to its synced
    // prefix. Persist-before-reply is preserved: a step enqueues its record(s)
    // (logically ordered) and requests a sync barrier BEFORE it acts/replies; the
    // deterministic single-thread scheduler runs the worker drain ahead of any
    // observable downstream effect, and recovery only ever sees a durable prefix.
    // ===================================================================

    void persist_meta() {
        // CTRL DUAL-COPY METADATA (plan P4). (currentTerm, votedFor) cannot be
        // reconstructed from peers, so a single torn/flipped metadata write that is
        // lost on recovery would silently REVERT the vote/term — risking a double vote
        // in a term (two leaders → split-brain). Persist TWO consecutive CRC'd copies:
        // if the trailing copy is torn/un-synced on a crash, recovery still finds the
        // leading copy with the SAME latest values, so the vote/term survive. The two
        // copies carry identical values, so replaying both is idempotent and the
        // recovered (term,vote) is unchanged in the no-fault case (V-META-DURABLE).
        enqueue_durable(rec_meta(current_term_, voted_for_));
        enqueue_durable(rec_meta(current_term_, voted_for_));
    }
    void persist_entry(Index index, const LogEntry& e) {
        enqueue_durable(rec_entry(index, e));
    }
    void persist_trunc(Index new_len) {
        enqueue_durable(rec_trunc(new_len));
    }
    // INCREMENTAL local compaction persist: write ONLY the entries folded since the last
    // persisted base as a SnapshotDelta. snap_state_ holds [1..snap_base_]; the new slice
    // is (snap_persisted_base_ .. snap_base_]. O(delta), not O(snap_base_) — this is what
    // removes the O(n^2) snapshot I/O over a long run.
    void persist_snapshot() {
        if (snap_base_ <= snap_persisted_base_) {
            return;  // nothing new to persist
        }
        const std::size_t lo = static_cast<std::size_t>(snap_persisted_base_);
        std::vector<LogEntry> delta(snap_state_.begin() + static_cast<std::ptrdiff_t>(lo),
                                    snap_state_.end());
        enqueue_durable(rec_snapshot_delta(snap_persisted_base_, snap_base_, delta));
        snap_persisted_base_ = snap_base_;
    }
    // FULL snapshot persist (RESET on replay). Used when InstallSnapshot replaces the whole
    // state with the leader's — the prior accumulated deltas no longer apply, so a full
    // record is written and replay resets to it. Subsequent local compactions are deltas.
    void persist_snapshot_full() {
        enqueue_durable(rec_snapshot(snap_base_, snap_state_));
        snap_persisted_base_ = snap_base_;
    }
    // C4.2: persist one config-chain entry (chain index + members) so the adopted
    // config survives a crash (Membership.tla: configs live in the durable log).
    void persist_config(std::uint64_t chain_index,
                        const std::vector<std::uint64_t>& members) {
        enqueue_durable(rec_config(chain_index, members));
    }

    // Append a framed record to the in-order FIFO write queue and request a
    // durability barrier; kick the single persist worker if it is idle. The
    // record is enqueued at the point the in-memory mutation happened, so queue
    // order == logical mutation order (the worker preserves it on disk).
    void enqueue_durable(std::vector<std::byte> rec) {
        if (disk_ == nullptr) {
            return;
        }
        write_queue_.push_back(std::move(rec));
        want_sync_ = true;  // every durable mutation wants a sync barrier
        ensure_persist_worker();
    }

    // Spawn the single FIFO persist worker if one is not already in flight. Only
    // ONE worker drains the queue at a time, so disk appends stay serialized and
    // on-disk order matches the logical enqueue order.
    void ensure_persist_worker() {
        if (worker_running_ || disk_ == nullptr) {
            return;
        }
        worker_running_ = true;
        sched_->spawn(persist_worker(this, gen_));
    }

    // The single sequential persist worker: drains write_queue_ in FIFO order,
    // appending each record to IDisk; once the queue is empty it issues a sync()
    // barrier if one was requested. Records enqueued WHILE the worker is awaiting
    // are picked up before it exits — keeping the on-disk byte order a pure,
    // deterministic function of the logical mutation order. Gated on the
    // generation counter so a crash/restart retires a stale worker.
    static core::Task persist_worker(RaftNodeA* self, std::uint64_t my_gen) {
        for (;;) {
            if (self->gen_ != my_gen || self->disk_ == nullptr) {
                self->worker_running_ = false;
                co_return;
            }
            if (!self->write_queue_.empty()) {
                // Move the front record into a frame-owned buffer so its span
                // stays valid across the append await (no dangling span).
                self->retained_.push_back(std::move(self->write_queue_.front()));
                self->write_queue_.erase(self->write_queue_.begin());
                const std::vector<std::byte>& rec = self->retained_.back();
                core::Offset off = 0;
                Error e = co_await self->disk_->append({rec.data(), rec.size()}, off);
                (void)e;
                continue;  // re-check the queue (more may have been enqueued)
            }
            if (self->want_sync_) {
                self->want_sync_ = false;
                Error s = co_await self->disk_->sync();
                (void)s;
                // POST-SYNC N=1 SELF-COMMIT HOOK. The sync() above made the whole
                // enqueued prefix DURABLE on this node's own disk. For a lone leader
                // (quorum() == 1) that is exactly the condition under which its own
                // entry is committable, so advance commit_index here — AFTER fsync,
                // not at submit-enqueue (the S9.2 durability gap). advance_commit_index()
                // logic is UNCHANGED; only WHERE/WHEN the N=1 path calls it moved.
                // Same scheduler thread as submit() — no new concurrency. GATED on
                // quorum() == 1 so for ANY config with >= 2 members this is a strict
                // no-op (commitment stays ack-driven, byte-identical to before).
                self->advance_n1_self_commit();
                continue;  // a record/sync may have been re-requested while parked
            }
            self->worker_running_ = false;
            co_return;
        }
    }

    // N=1-only: once the persist worker's sync() has made the enqueued prefix
    // durable, the lone leader self-commits its own entries (no peer ack ever
    // arrives in a single-member config). A strict no-op for quorum() >= 2.
    void advance_n1_self_commit() {
        if (role_ != Role::Leader || quorum() != 1) {
            return;
        }
        advance_commit_index();
        apply_and_maybe_snapshot();
    }

    // ===================================================================
    // Recovery: replay the durable prefix to rebuild (term, vote, log). A torn /
    // un-synced tail record fails its CRC/len frame and stops replay (recover-to-
    // prefix). Synchronous against the durable image (SimDisk read sees durable).
    // ===================================================================

    void recover_from_disk() {
        if (disk_ == nullptr) {
            return;
        }
        sched_->spawn(recover_task(this, gen_));
    }

    static core::Task recover_task(RaftNodeA* self, std::uint64_t my_gen) {
        // Read the whole durable image in one pass, framing records. SimDisk read
        // past end-of-device reports NotFound, so we probe the length by reading
        // growing windows; simpler: read the durable snapshot length via repeated
        // header+body reads until a short/failed read.
        IDisk* disk = self->disk_;
        std::uint64_t off = 0;
        Term rec_term = self->current_term_;
        std::uint64_t rec_vote = self->voted_for_;
        std::vector<LogEntry> rec_log;     // retained suffix (abs idx rec_base+1 ..)
        Index rec_base = 0;                // snapshot.lastIncludedIndex
        std::vector<LogEntry> rec_state;   // snapshot.state (folded prefix)
        // C4.2 MEMBERSHIP: rebuild the config chain + adopted cfgIdx from CONFIG
        // records. The chain starts at the init config (configs[1]); each CONFIG
        // record extends/sets a chain index. The highest index seen is the adopted
        // cfgIdx (forward-only). cfg_committed is re-learned via replication, not
        // persisted (like commitIndex).
        std::vector<std::vector<std::uint64_t>> rec_chain = self->init_config_chain();
        std::uint64_t rec_cfg_idx = 0;
        bool rec_have_cfg = false;
        bool any = false;
        Index rec_repair_to = 0;  // CTRL (P4): highest ENTRY index found PAST a corrupt record

        for (;;) {
            if (self->gen_ != my_gen) {
                co_return;
            }
            // Read the 8-byte frame header [crc][len].
            std::vector<std::byte> head(8);
            Error he = co_await disk->read(off, {head.data(), head.size()});
            if (!he.ok()) {
                break;  // end-of-device / torn header → durable prefix ends here
            }
            const auto* hp = reinterpret_cast<const std::uint8_t*>(head.data());
            wire::Reader hr{hp, 8, 0, true};
            const std::uint32_t crc = hr.u32();
            const std::uint32_t len = hr.u32();
            if (!hr.ok() || len == 0 || len > (1u << 20)) {
                break;
            }
            std::vector<std::byte> body(len);
            Error be = co_await disk->read(off + 8, {body.data(), body.size()});
            if (!be.ok()) {
                break;  // torn body → drop this partial tail record
            }
            const auto* bp = reinterpret_cast<const std::uint8_t*>(body.data());
            if (wire::crc32(bp, len) != crc) {
                // CTRL (P4 part 2): a record whose FRAMING is intact (we read exactly `len`
                // bytes) but whose CRC fails — a corrupt record, not merely a torn tail.
                // Scan FORWARD past it (using each following record's own intact framing)
                // to learn whether the physical log EXTENDED beyond our clean prefix: any
                // CRC-valid ENTRY after the hole proves we HAD that entry. The highest such
                // index becomes the repair watermark. We still recover only the clean
                // PREFIX (break below) — the leader refills the hole via AppendEntries —
                // but the watermark stops us running/voting-short until it is repaired.
                std::uint64_t soff = off + 8 + static_cast<std::uint64_t>(len);
                for (;;) {
                    std::vector<std::byte> sh(8);
                    if (!(co_await disk->read(soff, {sh.data(), sh.size()})).ok()) break;
                    const auto* shp = reinterpret_cast<const std::uint8_t*>(sh.data());
                    wire::Reader shr{shp, 8, 0, true};
                    const std::uint32_t scrc = shr.u32();
                    const std::uint32_t slen = shr.u32();
                    if (!shr.ok() || slen == 0 || slen > (1u << 20)) break;
                    std::vector<std::byte> sb(slen);
                    if (!(co_await disk->read(soff + 8, {sb.data(), sb.size()})).ok()) break;
                    const auto* sbp = reinterpret_cast<const std::uint8_t*>(sb.data());
                    if (wire::crc32(sbp, slen) != scrc) break;
                    wire::Reader sr{sbp, slen, 0, true};
                    if (static_cast<RecKind>(sr.u8()) == RecKind::Entry) {
                        const Index sindex = sr.u64();
                        if (sr.ok() && sindex > rec_repair_to) rec_repair_to = sindex;
                    }
                    soff += 8u + slen;
                }
                break;  // recover only the clean prefix; the watermark drives the repair.
            }
            // Decode the record and apply it.
            wire::Reader r{bp, len, 0, true};
            const auto kind = static_cast<RecKind>(r.u8());
            if (kind == RecKind::Meta) {
                rec_term = r.u64();
                rec_vote = r.u64();
            } else if (kind == RecKind::Entry) {
                const Index index = r.u64();
                LogEntry e;
                e.term = r.u64();
                e.value = r.str();
                if (!r.ok()) {
                    break;
                }
                // REBUILD-BY-ORDERED-REPLAY, with a CONTIGUITY GUARD that makes the
                // replay a strict PREFIX. The durable stream is written in logical
                // mutation order by the single FIFO persist worker, so a correctly
                // synced ENTRY at position k carries index == k and extends the log
                // by exactly one at the tail. We APPEND in arrival order (we do NOT
                // trust `index` to place/pad/repair an out-of-order arrival — that
                // would silently reorder and cannot fill a gap). But we DO use
                // `index` to VERIFY contiguity: the next ENTRY must claim
                // index == rec_log.size()+1. If it does not — e.g. an append that
                // was issued but never truly synced got its bytes resurrected at a
                // stale offset after a crash, surfacing a later entry where an
                // earlier one belongs — that is a torn/non-prefix tail, so we STOP
                // and keep only the contiguous durable prefix (recover-to-prefix,
                // the Phase-2/3 lesson). This is what keeps two nodes from holding
                // a different value at the same (index,term) after crash/restart.
                // Contiguity is checked over the ABSOLUTE logical length (snapshot
                // base + retained suffix) so a post-snapshot ENTRY continues right
                // after the snapshot point.
                if (index != rec_base + static_cast<Index>(rec_log.size()) + 1) {
                    break;  // non-contiguous ENTRY: prefix ends before it
                }
                rec_log.push_back(e);
            } else if (kind == RecKind::Trunc) {
                const Index new_len = r.u64();
                // A TRUNC may only shorten the contiguous suffix we have so far, and
                // never below the snapshot base (a committed/snapshotted prefix is
                // never truncated). One naming a length we never reached / below the
                // base is a torn/non-prefix tail.
                if (new_len > rec_base + static_cast<Index>(rec_log.size()) ||
                    new_len < rec_base) {
                    break;
                }
                rec_log.resize(static_cast<std::size_t>(new_len - rec_base));
            } else if (kind == RecKind::Snapshot) {
                // SNAPSHOT record: set (base, state). Compacts the durable prefix.
                // CRITICAL: the stream order is "ENTRY... then SNAPSHOT", so by the
                // time we read SNAPSHOT(base=i) the replay may ALREADY hold suffix
                // entries at absolute indices > i (they were appended before the
                // snapshot was taken). Those MUST be retained (they are above the
                // snapshot point — Snapshot.tla keeps SubSeq(log, i-base+1, Len)).
                // Only DROP the suffix entries at/below the new base; keep the rest.
                // (Dropping the whole suffix here lost a committed entry — backprop.)
                const Index lii = r.u64();
                const std::uint32_t count = r.u32();
                if (!r.ok() || count > len) {
                    break;
                }
                std::vector<LogEntry> st;
                st.reserve(count);
                for (std::uint32_t k = 0; k < count; ++k) {
                    LogEntry e;
                    e.term = r.u64();
                    e.value = r.str();
                    st.push_back(std::move(e));
                }
                if (!r.ok() || lii < rec_base ||
                    static_cast<Index>(st.size()) != lii) {
                    break;  // malformed / regressing snapshot → stop at prefix
                }
                // Retain suffix entries above the new base (abs index > lii). The
                // current suffix covers abs [rec_base+1 .. rec_base+rec_log.size()].
                const Index cur_abs_end = rec_base + static_cast<Index>(rec_log.size());
                std::vector<LogEntry> kept;
                if (cur_abs_end > lii) {
                    const std::size_t drop = static_cast<std::size_t>(lii - rec_base);
                    kept.assign(rec_log.begin() + static_cast<std::ptrdiff_t>(drop),
                                rec_log.end());
                }
                rec_base = lii;
                rec_state = std::move(st);
                rec_log = std::move(kept);
            } else if (kind == RecKind::SnapshotDelta) {
                // INCREMENTAL snapshot: APPEND (from_base, to_base] to the reconstructed
                // state + advance the base. Contiguity: from_base must == the current base
                // (the delta picks up exactly where the prior persisted state ended).
                const Index from_base = r.u64();
                const Index to_base = r.u64();
                const std::uint32_t count = r.u32();
                if (!r.ok() || count > len || from_base != rec_base ||
                    to_base < from_base ||
                    static_cast<Index>(count) != to_base - from_base) {
                    break;  // malformed / non-contiguous delta → stop at the durable prefix
                }
                std::vector<LogEntry> add;
                add.reserve(count);
                for (std::uint32_t k = 0; k < count; ++k) {
                    LogEntry e;
                    e.term = r.u64();
                    e.value = r.str();
                    add.push_back(std::move(e));
                }
                if (!r.ok()) {
                    break;
                }
                // Append the delta to the folded state (state now covers [1..to_base]).
                rec_state.insert(rec_state.end(),
                                 std::make_move_iterator(add.begin()),
                                 std::make_move_iterator(add.end()));
                // Retain suffix entries above the new base (same rule as a full snapshot):
                // the current suffix covers abs [rec_base+1 .. rec_base+rec_log.size()].
                const Index cur_abs_end = rec_base + static_cast<Index>(rec_log.size());
                std::vector<LogEntry> kept;
                if (cur_abs_end > to_base) {
                    const std::size_t drop = static_cast<std::size_t>(to_base - rec_base);
                    kept.assign(rec_log.begin() + static_cast<std::ptrdiff_t>(drop),
                                rec_log.end());
                }
                rec_base = to_base;
                rec_log = std::move(kept);
            } else if (kind == RecKind::Config) {
                // C4.2: a CONFIG record sets a chain index to a member set. Forward-
                // only (the chain never regresses); the highest index is the adopted
                // cfgIdx. A regressing / wildly-large index is a torn tail → stop.
                const std::uint64_t cidx = r.u64();
                const std::uint32_t mcount = r.u32();
                if (!r.ok() || mcount > len) {
                    break;
                }
                std::vector<std::uint64_t> members;
                members.reserve(mcount);
                for (std::uint32_t k = 0; k < mcount; ++k) {
                    members.push_back(r.u64());
                }
                if (!r.ok() || cidx == 0 || cidx > rec_chain.size()) {
                    break;  // index 0 is the immutable init config; a gap = torn tail
                }
                if (cidx >= rec_chain.size()) {
                    rec_chain.resize(cidx + 1);
                }
                rec_chain[cidx] = std::move(members);
                if (cidx > rec_cfg_idx) {
                    rec_cfg_idx = cidx;
                }
                rec_have_cfg = true;
            } else {
                break;  // unknown record kind → stop
            }
            if (!r.ok()) {
                break;
            }
            any = true;
            off += 8 + len;
        }

        if (self->gen_ != my_gen) {
            co_return;  // crashed/restarted again while recovering
        }
        if (any) {
            self->current_term_ = rec_term;
            self->voted_for_ = rec_vote;
            self->snap_base_ = rec_base;
            self->snap_state_ = std::move(rec_state);
            // Everything reconstructed is by definition already durable, so the next local
            // compaction persists deltas from here (not re-writing the recovered state).
            self->snap_persisted_base_ = rec_base;
            self->log_ = std::move(rec_log);
            // The snapshotted prefix is applied by definition; recovery restores
            // appliedIndex to the snapshot point (commitIndex is re-learned via
            // replication / leaderCommit, never persisted).
            self->applied_index_ = rec_base;
            self->log_view_.clear();
        }
        // CTRL (P4 part 2): if a corrupt entry was detected AND the physical log proved it
        // extended beyond our recovered clean prefix, arm the repair watermark. Otherwise
        // (a clean recovery or a plain torn tail with nothing valid after) it stays 0 and
        // every existing path is byte-identical.
        self->repair_watermark_ =
            (rec_repair_to > self->snap_base_ + static_cast<Index>(self->log_.size()))
                ? rec_repair_to
                : 0;
        // C4.2: restore the adopted config chain + cfgIdx (forward-only). If no
        // CONFIG record survived, the node sits at the init config (index 0). The
        // committed index is conservatively re-learned (a leader re-confirms; a
        // follower trusts the leader's rollout), so reset it here.
        self->cfg_chain_ = std::move(rec_chain);
        self->cfg_idx_ = rec_have_cfg ? rec_cfg_idx : 0;
        self->cfg_committed_idx_ = self->cfg_idx_;  // adopted ⇒ locally settled
        self->cfg_adopted_.assign(self->cfg_.cluster.size(), 0);
        // Recovery done: arm the election deadline and open the gates so the timer
        // loop + dispatch may now act on the (recovered) durable state.
        self->arm_election_deadline();
        self->recovered_ = true;
        co_return;
    }

    // ===================================================================
    // Helpers
    // ===================================================================

    // ===================================================================
    // Compacted-log accessors. The consensus core reasons in ABSOLUTE 1-based
    // logical indices; these translate to the retained suffix / snapshot. An
    // absolute index <= snap_base_ has been compacted into snap_state_ (the spec
    // never re-reads a discarded entry by index in the hot path — prevLog checks
    // only ever land at or after the snapshot point because nextIndex below the
    // base triggers InstallSnapshot instead).
    // ===================================================================

    // Logical log length (Snapshot.tla logBase + Len(log)).
    [[nodiscard]] Index llen() const noexcept {
        return snap_base_ + static_cast<Index>(log_.size());
    }

    // Term at absolute 1-based index i (0 if out of range / index 0). Reads the
    // snapshot prefix for i <= snap_base_, else the retained suffix.
    [[nodiscard]] Term term_at(Index i) const noexcept {
        if (i == 0 || i > llen()) {
            return 0;
        }
        if (i <= snap_base_) {
            return snap_state_[static_cast<std::size_t>(i) - 1].term;
        }
        return log_[static_cast<std::size_t>(i - snap_base_) - 1].term;
    }

    // Entry at absolute 1-based index i (must be in range).
    [[nodiscard]] const LogEntry& entry_at(Index i) const noexcept {
        if (i <= snap_base_) {
            return snap_state_[static_cast<std::size_t>(i) - 1];
        }
        return log_[static_cast<std::size_t>(i - snap_base_) - 1];
    }

    // Append an entry at the tail (absolute index llen()+1).
    void append_entry(const LogEntry& e) { log_.push_back(e); }

    // Truncate the logical log to absolute length new_len (new_len >= snap_base_;
    // we never truncate into the snapshotted/applied prefix — Follower truncation
    // only ever happens at/after the snapshot point, like the spec).
    void truncate_to(Index new_len) {
        if (new_len < snap_base_) {
            new_len = snap_base_;  // never erase a committed/applied prefix
        }
        log_.resize(static_cast<std::size_t>(new_len - snap_base_));
    }

    // Rebuild the full logical-log view (snap_state_ ++ log_) for the observable
    // log(). Pure function of current state; invisible to compaction.
    void rebuild_log_view() const {
        log_view_.clear();
        log_view_.reserve(snap_state_.size() + log_.size());
        log_view_.insert(log_view_.end(), snap_state_.begin(), snap_state_.end());
        log_view_.insert(log_view_.end(), log_.begin(), log_.end());
    }

    [[nodiscard]] Term last_log_term() const noexcept {
        return llen() == 0 ? 0 : term_at(llen());
    }

    void arm_election_deadline() {
        // Randomized election timeout in [min,max] from IRandom (breaks symmetric
        // split votes). Re-armed on heartbeat / granted vote / step-down.
        const Tick jitter = static_cast<Tick>(rng_->uniform_range(
            cfg_.election_timeout_min, cfg_.election_timeout_max));
        election_deadline_ = clock_->now() + jitter;
    }

    void send_to(std::uint64_t dest, const RaftMsg& m) {
        RaftMsg out = m;
        out.cluster_token = cluster_token_;  // stamp our cluster identity on every send (P2)
        sched_->spawn(send_task(net_, dest, encode(out)));
    }

    static core::Task send_task(INetwork* net, std::uint64_t dest,
                                std::vector<std::byte> bytes) {
        Error e = co_await net->send(Endpoint{dest}, {bytes.data(), bytes.size()});
        (void)e;
        co_return;
    }

    [[nodiscard]] std::size_t self_idx() const noexcept { return idx_of(self_); }

    [[nodiscard]] std::size_t idx_of(std::uint64_t id) const noexcept {
        // cfg_.cluster is sorted (seam contract); find the dense index of id.
        for (std::size_t i = 0; i < cfg_.cluster.size(); ++i) {
            if (cfg_.cluster[i] == id) {
                return i;
            }
        }
        return 0;
    }

    // ---- C4.2 MEMBERSHIP helpers (Membership.tla) ------------------------

    static void sort_ids(std::vector<std::uint64_t>& v) {
        // Deterministic insertion sort (no std::sort: keep output a pure fn of input
        // with a stable, allocation-free order; the sets are tiny).
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

    // Membership.tla Cfg(s) membership: is `id` in the config this server uses now?
    [[nodiscard]] bool in_current_config(std::uint64_t id) const noexcept {
        return !cfg_chain_.empty() && in_config(cfg_chain_[cfg_idx_], id);
    }

    // Quorum size over the CURRENT config (Membership.tla Quorum(Cfg(s))).
    [[nodiscard]] std::size_t quorum() const noexcept {
        const std::size_t n = cfg_chain_.empty() ? cfg_.cluster.size()
                                                 : cfg_chain_[cfg_idx_].size();
        return n / 2 + 1;
    }

    // Count granted votes that come from CURRENT-config members (a vote from a
    // non-member does not count toward the config quorum).
    [[nodiscard]] std::size_t config_votes() const noexcept {
        std::size_t c = 0;
        for (std::uint64_t v : votes_granted_) {
            if (in_current_config(v)) {
                ++c;
            }
        }
        return c;
    }

    // The chain seeded with configs[1] = init_config (used to reset on recovery).
    [[nodiscard]] std::vector<std::vector<std::uint64_t>> init_config_chain() const {
        std::vector<std::uint64_t> init =
            cfg_.init_config.empty() ? cfg_.cluster : cfg_.init_config;
        return {init};
    }

    // ---- injected boundary handles + config ------------------------------
    core::IScheduler* sched_;
    IClock* clock_;
    IRandom* rng_;
    INetwork* net_;
    IDisk* disk_;
    NodeConfig cfg_;
    std::uint64_t self_;
    std::uint64_t cluster_token_;  // cluster-identity guard (P2 restore-new-cluster)
    std::vector<std::uint64_t> peers_;

    // ---- raft state (in-memory; the spec's per-server variables) ----------
    Role role_ = Role::Follower;
    Term current_term_ = 0;
    std::uint64_t voted_for_ = kNil;   // votedFor (kNil = Nil)
    // log_ is the PHYSICALLY-RETAINED SUFFIX above the snapshot point (Snapshot.tla
    // `log[s]`): log_[k] is absolute logical index snap_base_+k+1. The discarded
    // prefix [1..snap_base_] lives folded into snap_state_ (= our state machine's
    // applied state, which for a replicated log IS the committed-prefix entry list).
    std::vector<LogEntry> log_;        // retained suffix (abs idx snap_base_+1 ..)
    Index snap_base_ = 0;              // snapshot.lastIncludedIndex (= logBase[s])
    std::vector<LogEntry> snap_state_; // snapshot.state: folded entries [1..base]
    Index snap_persisted_base_ = 0;    // base up to which snap_state_ is DURABLY persisted
                                       // (as deltas); persist_snapshot writes (this..base]
    Index applied_index_ = 0;          // Snapshot.tla appliedIndex[s]
    // log_view_ is the FULL logical log reconstructed from (snap_state_ ++ log_),
    // returned by log() so the observable surface is UNCHANGED by compaction
    // (Snapshot.tla ReconstructUpTo: snapshot.state then the retained suffix).
    mutable std::vector<LogEntry> log_view_;
    Index commit_index_ = 0;           // commitIndex[s]
    // CTRL (P4 part 2) repair watermark: 0 = healthy. Set by recovery when a corrupt
    // committed entry is detected AND the physical log proved it extended past our clean
    // prefix (valid ENTRY records after the hole) — the length we HAD. While
    // repair_watermark_ > llen() the node neither runs for election nor grants a vote to a
    // candidate whose log is shorter than it, so a corrupt node + lagging peers cannot
    // elect a leader that DROPPED a committed entry (CTRL's silent-loss disaster). It is
    // cleared once replication refills the log up to this length (the hole is repaired).
    Index repair_watermark_ = 0;
    std::vector<std::uint64_t> votes_granted_;  // votesGranted[s] (a set)

    // C4.3 compaction trigger: snapshot once the retained suffix exceeds this many
    // entries AND there is a fresh applied prefix to fold + discard. Kept small so
    // the in-gate workload actually triggers compaction on some nodes.
    static constexpr std::size_t kSnapshotThreshold = 8;
    // S8.2b BOUNDED REPLICATION BATCHING: cap of entries per AppendEntries. A leader
    // ships at most this many log entries per send; the next batch rides the next ack
    // / heartbeat. Bounds each replicate_to to O(kMaxBatch) reactor work so a
    // pipelined burst no longer blocks the single reactor coroutine + starves the
    // heartbeat timer (S8.2a-diagnosed 3-node collapse). 64 = a sane Raft default
    // (amortizes per-RPC framing while keeping a single send small).
    static constexpr Index kMaxBatch = 64;
    // Measurement counters (introspection only; NOT persisted, reset on crash).
    std::uint64_t snapshots_taken_ = 0;
    std::uint64_t snapshots_installed_ = 0;

    // ---- leader replication bookkeeping (dense by cluster index) ----------
    std::vector<Index> next_index_;
    std::vector<Index> match_index_;
    // S8.2b: highest log index shipped to each peer in its last AppendEntries batch.
    // The ack handler re-kicks the NEXT bounded batch only once match_index_ >=
    // sent_index_ (the previous batch fully landed) so at most one batch is in flight
    // per peer — no ack-driven re-send cascade (which regressed depth-1 to fault=1).
    std::vector<Index> sent_index_;

    // ---- C4.2 MEMBERSHIP CHANGE state (Membership.tla) --------------------
    // cfg_chain_ is the GLOBAL config chain (configs[1], configs[2], ...): a
    // totally-ordered sequence where adjacent configs differ by <= 1 server.
    // cfg_chain_[0] = the init config; cfg_idx_ = the latest index this node has
    // ADOPTED (Membership.tla cfgIdx[s], forward-only). cfg_committed_idx_ = the
    // highest index known committed cluster-wide (commit-before-next: a leader may
    // only propose the next change once cfg_committed_idx_ == cfg_idx_ == head).
    // cfg_adopted_ (leader-only, by cluster index) tracks each peer's adopted index
    // so the leader learns when the head config has reached a quorum of itself.
    std::vector<std::vector<std::uint64_t>> cfg_chain_;
    std::uint64_t cfg_idx_ = 0;
    std::uint64_t cfg_committed_idx_ = 0;
    std::vector<std::uint64_t> cfg_adopted_;

    // ---- timing + lifecycle ----------------------------------------------
    Tick election_deadline_ = 0;
    Tick heartbeat_deadline_ = 0;
    std::uint64_t leader_hint_ = kNoLeader;
    bool running_ = false;
    bool recovered_ = false;  // durable state replayed + election deadline armed
    std::uint64_t gen_ = 0;   // bumped on crash/restart to retire stale loops

    // ---- S8.2b replication coalescing (de-dup per-submit broadcast bursts) -----
    // repl_pending_: a submit appended an entry whose replication kick was coalesced
    // away (some earlier submit this turn already kicked) — the timer loop flushes it.
    // repl_kicked_: a bounded broadcast already fired this scheduler turn, so further
    // submits in the same turn only mark pending (one kick per turn, not K). Both are
    // pure in-memory scheduling hints, NOT durable / NOT safety state; reset on crash.
    bool repl_pending_ = false;
    bool repl_kicked_ = false;

    // ---- single FIFO durable-write pipeline ------------------------------
    // write_queue_ holds framed records not yet handed to IDisk, in logical
    // mutation order. ONE persist worker (worker_running_) drains them in order,
    // then sync()s if want_sync_. retained_ keeps each record's bytes alive for
    // the lifetime of its (non-owning span) append. This serialization is what
    // makes on-disk order == logical order (the Phase-4 durability bug fix).
    std::vector<std::vector<std::byte>> write_queue_;
    std::vector<std::vector<std::byte>> retained_;
    bool worker_running_ = false;
    bool want_sync_ = false;
};

// ---------------------------------------------------------------------------
// Factory: returns ONE RaftNodeA behind the seam. The ClusterDriver swaps whole
// implementations by swapping this factory (the §6.5 cross-check seam).
// ---------------------------------------------------------------------------
[[nodiscard]] inline ConsensusNodeFactory make_raft_a_factory() {
    return [](const NodeDeps& deps, const NodeConfig& cfg)
               -> std::unique_ptr<ConsensusNode> {
        return std::make_unique<RaftNodeA>(deps, cfg);
    };
}

}  // namespace lockstep::consensus::raft_a
