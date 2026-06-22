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
    // RequestVoteResp / AppendEntriesResp
    bool granted = false;
    bool success = false;
    std::uint64_t match_index = 0;
    // AppendEntries
    std::uint64_t prev_log_index = 0;
    std::uint64_t prev_log_term = 0;
    std::uint64_t leader_commit = 0;
    std::vector<LogEntry> entries;
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

enum class RecKind : std::uint8_t { Term = 1, Truncate = 2, Entry = 3 };

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

// Recovered durable state.
struct Recovered {
    std::uint64_t term = 0;
    bool has_vote = false;
    std::uint64_t vote = 0;
    std::vector<LogEntry> log;
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
                // A TRUNCATE may only shorten the contiguous prefix recovered so
                // far. One naming a length we never reached is a torn / re-staged
                // tail — stop and keep the durable prefix before it.
                if (nl > rec.log.size()) {
                    stop = true;
                    break;
                }
                rec.log.resize(static_cast<std::size_t>(nl));
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
                // CONTIGUITY GUARD: a correctly-synced ENTRY at this stream
                // position must claim index == log.size()+1 (it extends the log by
                // exactly one at the tail). If it does not — an in-flight append
                // re-staged after crash() surfaced an entry out of position — this
                // is a non-prefix tail: STOP, keep the contiguous durable prefix.
                // (Use the index to DETECT the gap, never to place/pad/repair.)
                if (idx != rec.log.size() + 1) {
                    stop = true;
                    break;
                }
                rec.log.push_back(std::move(e));
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
          self_(cfg.self_id) {}

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
        log_.push_back(e);
        match_index_[index_of(self_)] = log_.size();
        persist_entry(log_.size(), e);  // 1-based position == new log length
        sync_now();
        r.accepted = true;
        r.term = current_term_;
        r.index = log_.size();
        // Push the new entry out promptly (replication also runs on heartbeats).
        broadcast_append_entries();
        return r;
    }

    // ---- ConsensusNode: observables -----------------------------------

    [[nodiscard]] Role role() const noexcept override { return role_; }
    [[nodiscard]] Term current_term() const noexcept override { return current_term_; }
    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        return std::span<const LogEntry>(log_.data(), log_.size());
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
        commit_index_ = 0;
        votes_granted_.clear();
        next_index_.clear();
        match_index_.clear();
        leader_hint_ = UINT64_MAX;
        recovered_ = false;
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
    [[nodiscard]] std::size_t quorum() const noexcept { return cluster_size() / 2 + 1; }

    [[nodiscard]] std::size_t index_of(std::uint64_t node) const noexcept {
        for (std::size_t i = 0; i < cfg_.cluster.size(); ++i) {
            if (cfg_.cluster[i] == node) {
                return i;
            }
        }
        return 0;
    }

    [[nodiscard]] Term last_log_term() const noexcept {
        return log_.empty() ? 0 : log_.back().term;
    }
    [[nodiscard]] Index last_log_index() const noexcept {
        return static_cast<Index>(log_.size());
    }
    // Term at 1-based index i (0 if out of range / index 0). Matches TermAt.
    [[nodiscard]] Term term_at(Index i) const noexcept {
        if (i == 0 || i > log_.size()) {
            return 0;
        }
        return log_[static_cast<std::size_t>(i - 1)].term;
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
        next_index_.assign(cluster_size(), log_.size() + 1);
        match_index_.assign(cluster_size(), 0);
        match_index_[index_of(self_)] = log_.size();
        broadcast_append_entries();  // immediate heartbeat asserts leadership
    }

    // Timeout(s): become candidate, ++term, self-vote, broadcast RequestVote.
    void begin_election() {
        ++current_term_;
        role_ = Role::Candidate;
        voted_for_ = self_;
        votes_granted_.assign(cluster_size(), false);
        votes_granted_[index_of(self_)] = true;
        // Persist (term,vote) and sync BEFORE the request leaves (persist-before-
        // reply: a self-vote at this term is durable before we solicit others).
        persist_term_vote();
        sync_now();
        broadcast_request_vote();
        arm_election_deadline();
    }

    [[nodiscard]] std::size_t votes_count() const noexcept {
        std::size_t c = 0;
        for (bool g : votes_granted_) {
            if (g) {
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
        std::vector<std::byte> frame = w.finish();
        for (std::uint64_t peer : cfg_.cluster) {
            if (peer != self_) {
                send_to(peer, frame);
            }
        }
    }

    void send_append_entries(std::uint64_t peer) {
        std::size_t pi = index_of(peer);
        Index next = next_index_.empty() ? (log_.size() + 1) : next_index_[pi];
        if (next < 1) {
            next = 1;
        }
        Index prev_index = next - 1;
        Term prev_term = term_at(prev_index);
        wire::Writer w;
        w.u8(static_cast<std::uint8_t>(wire::Kind::AppendEntries));
        w.u64(current_term_);
        w.u64(self_);
        w.u64(prev_index);
        w.u64(prev_term);
        w.u64(commit_index_);
        std::uint64_t count = log_.size() - static_cast<std::size_t>(prev_index);
        w.u64(count);
        for (std::size_t i = static_cast<std::size_t>(prev_index); i < log_.size(); ++i) {
            w.u64(log_[i].term);
            w.str(log_[i].value);
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
        bool can_vote = !voted_for_.has_value() || *voted_for_ == m.from;
        bool grant = can_vote && cand_up_to_date;
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

        // logOk: prevLog must be index 0, or in range with a matching term.
        bool log_ok = (m.prev_log_index == 0) ||
                      (m.prev_log_index <= log_.size() &&
                       m.prev_log_term == term_at(m.prev_log_index));
        if (!log_ok) {
            reply_append(m.from, false, 0);
            return;
        }

        // Raft conflict rule: scan incoming entries; find the FIRST index whose
        // term conflicts with our existing log. Truncate ONLY there, append the
        // genuinely-new tail. A subsumed/short re-delivery is an idempotent no-op
        // (never erases an agreed/committed prefix).
        std::size_t first_conflict = 0;  // 1-based position within m.entries; 0 = none
        for (std::size_t k = 1; k <= m.entries.size(); ++k) {
            std::uint64_t idx = m.prev_log_index + k;
            if (idx <= log_.size() &&
                log_[static_cast<std::size_t>(idx - 1)].term != m.entries[k - 1].term) {
                first_conflict = k;
                break;
            }
        }
        bool mutated = false;
        if (first_conflict == 0) {
            // No conflict among overlapping entries: append only the new tail.
            std::uint64_t already = log_.size();
            std::uint64_t incoming_end = m.prev_log_index + m.entries.size();
            if (incoming_end > already) {
                std::size_t start = static_cast<std::size_t>(already - m.prev_log_index);
                for (std::size_t k = start; k < m.entries.size(); ++k) {
                    log_.push_back(m.entries[k]);
                    persist_entry(log_.size(), m.entries[k]);
                    mutated = true;
                }
            }
            // else: incoming fully subsumed — idempotent no-op.
        } else {
            // Conflict: truncate to prevLogIndex + firstConflict - 1, append the
            // incoming suffix from firstConflict on.
            std::size_t keep = static_cast<std::size_t>(m.prev_log_index + first_conflict - 1);
            if (keep < log_.size()) {
                log_.resize(keep);
                persist_truncate(keep);
                mutated = true;
            }
            for (std::size_t k = first_conflict - 1; k < m.entries.size(); ++k) {
                log_.push_back(m.entries[k]);
                persist_entry(log_.size(), m.entries[k]);
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
        reply_append(m.from, true, last_new);
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
        } else {
            // Decrement nextIndex and retry (log backtracking).
            if (next_index_[pi] > 1) {
                --next_index_[pi];
            }
            send_append_entries(m.from);
        }
    }

    // AdvanceCommitIndex(s): commit N only when a quorum stores it AND
    // log[N].term == currentTerm (the current-term commitment rule).
    void advance_commit_index() {
        for (Index n = static_cast<Index>(log_.size()); n > commit_index_; --n) {
            if (term_at(n) != current_term_) {
                continue;  // only current-term entries commit by replication count
            }
            std::size_t agree = 0;
            for (std::size_t i = 0; i < match_index_.size(); ++i) {
                if (cfg_.cluster[i] == self_) {
                    if (log_.size() >= n) {
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
    std::vector<LogEntry> log_;
    Index commit_index_ = 0;

    std::vector<bool> votes_granted_;          // by cluster index (Candidate)
    std::vector<Index> next_index_;            // by cluster index (Leader)
    std::vector<Index> match_index_;           // by cluster index (Leader)

    std::uint64_t leader_hint_ = UINT64_MAX;

    bool running_ = false;
    bool recovered_ = false;                   // async disk recovery done?
    std::uint64_t loop_epoch_ = 0;             // bumped on crash/restart to retire old coroutines
    core::Tick election_deadline_ = 0;

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
    log_ = std::move(rec.log);
    role_ = Role::Follower;
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
            broadcast_append_entries();
            continue;
        }
        if (clock_->now() >= election_deadline_) {
            begin_election();
        }
    }
}

}  // namespace lockstep::consensus::raft_b
