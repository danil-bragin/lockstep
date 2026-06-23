#pragma once

// ProdConsensusNode.hpp — Phase 7 S5b-1. THE PROD CONSENSUS ASSEMBLY: a REAL Raft
// ConsensusNode (impl A) running on the PROD providers as a 1-NODE cluster, with a
// minimal client/admin protocol and PROVEN durable crash/restart recovery from the
// real ProdDisk. This is the consensus analogue of ProdServerNode (S5a) — but where
// S5a ran the in-memory MVCC DB stack, THIS finally exercises real DURABILITY: the
// consensus LOG lives on a real ProdDisk file (the node's IDisk), and a process
// restart rebuilds term/vote/log from those bytes (ConsensusNode::restart()).
//
// ============================================================================
// WHAT THIS ASSEMBLES — the ClusterDriver's per-node wiring, but on PROD providers
//   ClusterDriver (sim)                    ProdConsensusNode (prod)
//   ------------------------------------   ------------------------------------
//   core::Scheduler           (sched)      prod::ProdReactor          (sched)
//   core::SimClock(sched)     (clock)      reactor.clock()  [ONE shared ProdClock]
//   sim::SeededRandom(seed)   (rng)        prod::ProdRandom(seed)     (rng)
//   sim::SimNetwork (bus)     (net)        prod::ProdNetwork (bus)    (net) [real TCP]
//   sim::SimDisk              (disk)       prod::ProdDisk(data_dir)   (disk) [real fd]
//   make_raft_a_factory()(deps,cfg)        make_raft_a_factory()(deps,cfg) [UNCHANGED]
//
// The ConsensusNode (RaftNodeA) is taken UNCHANGED — it is provider-agnostic (it
// takes NodeDeps of core::IScheduler*/IClock*/IRandom*/INetwork*/IDisk*, and the
// prod providers ARE those interfaces). ZERO change to consensus/ — the whole point
// of the boundary architecture: the sim-proven Raft replica runs on real epoll +
// real sockets + a real disk with no core rewrite.
//
// ----------------------------------------------------------------------------
// CLUSTER ADDRESSING (the S5b-2 multi-node seam, built NOW):
// ProdNetworkBus already maps each node Endpoint id -> a stable loopback port
// (record_port/port_of), and a node's first send(Endpoint{peer_id}, ...) dials that
// port (ProdNetwork::ensure_connection). For a 1-node cluster there are NO peers, so
// no peer RPC is ever issued — but the addressing structure is the SAME one S5b-2
// will populate: add_node(id) for EVERY cluster member, and NodeConfig.cluster lists
// all ids. net->local() == Endpoint{self_id} is consistent with NodeConfig.self_id
// (asserted by valid()). S5b-2 just calls add_node for each peer + lists them in
// cluster; this assembly is unchanged.
//
// ----------------------------------------------------------------------------
// MINIMAL CLIENT/ADMIN PROTOCOL (line/length-framed over a real socket):
// A thin admin layer lets a client SUBMIT a value + read STATUS across the socket
// boundary. It runs over a SEPARATE ProdNetwork endpoint (the "admin" node id),
// NOT the consensus node's net — the consensus node's recv() stream is reserved for
// Raft peer RPC, so admin traffic must not be consumed by RaftNodeA::recv_loop. The
// admin serve-loop decodes one request frame, routes it to the node, and replies:
//   SUBMIT <value>  -> the LEADER appends + returns the log index (ADMIN_OK idx);
//                      a non-leader returns ADMIN_NOTLEADER (leader_hint).
//   STATUS          -> role, term, commit_index, and a digest of the committed log.
// The frame body is a tiny self-describing byte record (kind u8 + fields); the
// transport is ProdNetwork's existing 4-byte-LE length framing (no new wire code).
//
// ----------------------------------------------------------------------------
// N=1 SELF-COMMIT (FIXED). Previously RaftNodeA advanced commit_index ONLY in
// handle_append_entries_resp (a peer ack drives AdvanceCommitIndex), so an N=1
// (peerless) cluster — no peers to ack — never advanced commit_index past 0, even
// though the spec's AdvanceCommitIndex permits a leader to self-commit (a quorum of 1
// = the leader itself, with log[N].term == currentTerm). The sim ClusterDriver only
// ever ran N=3/N=5, so this degenerate path was never exercised — until this prod
// 1-node deployment surfaced it. THE FIX (consensus/, both impls A and B): the lone
// leader re-evaluates advance_commit_index() right after its own local append, and a
// lone candidate self-elects on its own vote — BOTH gated on quorum()==1, so they are a
// strict no-op for N>=2 (verified byte-identical at N=3 + TLC Consensus1.cfg confirms
// the spec invariants hold at N=1). commit_index now advances for a 1-node cluster; the
// durable LOG (persisted to consensus.wal + recovered on restart) remains the separate
// durability payload. S5b-2 (>=2 nodes) commits via real peer acks, unchanged.
//
// ----------------------------------------------------------------------------
// LINUX-ONLY (epoll + sockets): compiled only under __linux__ (ProdReactor /
// ProdNetwork are __linux__-guarded). The CMake target + its test are added only on
// Linux, so the macOS host build stays green (this assembly absent).
//
// providers/prod/ is the lint-exempt boundary zone. ProdConsensusNode itself touches
// NO raw syscall — it only assembles the provider surfaces + the ConsensusNode + the
// admin serve coroutine. RAII everywhere (the reactor/network/disk close their fds).
// BOUNDED run loops (run_until(pred, absolute-deadline)) so a 1-node election timer
// can NEVER spin the host. No real threads (one reactor = one thread).
//
// ASAN NOTE: the admin serve coroutine is a FREE FUNCTION over stable pointers (the
// node + the bus + the admin net are heap/stack-stable for the run) — NEVER an inline
// [&] lambda Task (a prior stage hit a stack-use-after-scope from that pattern).

#ifdef __linux__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdRandom.hpp>
#include <lockstep/prod/ProdReactor.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

namespace lockstep::prod {

namespace consensus = ::lockstep::consensus;

// ---------------------------------------------------------------------------
// ADMIN PROTOCOL — a tiny self-describing byte record carried over ProdNetwork's
// length framing. Request kinds the client sends; reply kinds the server returns.
// Encoding is hand-rolled LE (no dependency on the wire lib) so the admin surface
// is self-contained + auditable. All multi-byte fields are 8-byte LE; strings are
// a 4-byte LE length + bytes.
// ---------------------------------------------------------------------------
enum class AdminKind : std::uint8_t {
    Submit = 1,      // req: [Submit][str value]
    Status = 2,      // req: [Status]
    SubmitOk = 3,    // rep: [SubmitOk][u64 term][u64 index]
    NotLeader = 4,   // rep: [NotLeader][u64 leader_hint]
    StatusRep = 5,   // rep: [StatusRep][u8 role][u64 term][u64 commit][u32 n][entries]
};

// A decoded STATUS reply (the client-observable cluster state across the socket).
struct AdminStatus {
    std::uint8_t role = 0;            // consensus::Role as a byte
    std::uint64_t term = 0;
    std::uint64_t commit_index = 0;
    std::vector<std::string> committed;  // committed-log values [1..commit_index]
};

namespace admin_detail {

inline void put_u8(std::vector<std::byte>& b, std::uint8_t v) {
    b.push_back(static_cast<std::byte>(v));
}
inline void put_u32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}
inline void put_u64(std::vector<std::byte>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}
inline void put_str(std::vector<std::byte>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size()));
    for (char c : s) {
        b.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

// A bounds-checked little-endian reader over a payload span. ok() goes false on any
// over-read so a truncated/garbage frame is rejected, never read out of bounds.
struct Reader {
    const std::byte* p = nullptr;
    std::size_t n = 0;
    std::size_t off = 0;
    bool good = true;

    [[nodiscard]] bool ok() const noexcept { return good; }

    std::uint8_t u8() {
        if (off + 1 > n) {
            good = false;
            return 0;
        }
        return static_cast<std::uint8_t>(p[off++]);
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (off + 1 > n) {
                good = false;
                return 0;
            }
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[off++])) << (8 * i);
        }
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            if (off + 1 > n) {
                good = false;
                return 0;
            }
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[off++])) << (8 * i);
        }
        return v;
    }
    std::string str() {
        const std::uint32_t len = u32();
        if (!good || off + len > n) {
            good = false;
            return {};
        }
        std::string s;
        s.reserve(len);
        for (std::uint32_t i = 0; i < len; ++i) {
            s.push_back(static_cast<char>(static_cast<unsigned char>(p[off++])));
        }
        return s;
    }
};

}  // namespace admin_detail

// Encode the client-side requests (so a client TU builds frames without touching the
// encoder internals). The transport then length-frames + sends these bytes.
[[nodiscard]] inline std::vector<std::byte> encode_submit(const std::string& value) {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::Submit));
    admin_detail::put_str(b, value);
    return b;
}
[[nodiscard]] inline std::vector<std::byte> encode_status() {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::Status));
    return b;
}

// Decode a STATUS reply payload (client side). Returns false on a malformed frame.
[[nodiscard]] inline bool decode_status(std::span<const std::byte> payload,
                                        AdminStatus& out) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    const auto kind = static_cast<AdminKind>(r.u8());
    if (kind != AdminKind::StatusRep) {
        return false;
    }
    out.role = r.u8();
    out.term = r.u64();
    out.commit_index = r.u64();
    const std::uint32_t cnt = r.u32();
    out.committed.clear();
    out.committed.reserve(cnt);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        out.committed.push_back(r.str());
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
// ProdConsensusNode — assembles a real Raft replica on the prod providers + owns the
// ProdDisk (consensus log) over the data dir + serves the admin protocol. ONE
// in-process reactor drives the node, its timers, and the admin serve-loop. The node
// is constructed via make_raft_a_factory() (impl A) — UNCHANGED consensus surface.
// ---------------------------------------------------------------------------
class ProdConsensusNode {
public:
    // Assemble on an EXISTING reactor + bus. The bus must already add_node(self_id)
    // (the consensus net) AND add_node(admin_id) (the admin listen endpoint). The
    // ProdDisk is opened over data_dir/consensus.wal — the DURABLE consensus log.
    // `cluster` lists ALL consensus member ids (1-node: just {self_id}); S5b-2 lists
    // every peer and the bus add_node's each. `seed` feeds ProdRandom (election
    // jitter / backoff). The node is built but NOT started — call start().
    // Real-time Raft timing knobs (ns ticks). The defaults are the 1-node (S5b-1)
    // window — SHORT, so a lone node wins its own election promptly. S5b-2 (>=2 real
    // processes over real TCP) passes a WIDER window: the election timeout must exceed
    // a few heartbeat intervals + real cross-process connect/RTT latency, or followers
    // time out before the leader's heartbeat lands and the cluster never stabilizes on
    // ONE leader. A randomized [min,max] spread breaks symmetric split votes.
    struct Timing {
        core::Tick election_min = kElectionMinNs;
        core::Tick election_max = kElectionMaxNs;
        core::Tick heartbeat = kHeartbeatNs;
        core::Tick request_deadline = kRequestDeadlineNs;
    };

    // 1-node convenience ctor (S5b-1 call sites): default timing.
    ProdConsensusNode(ProdReactor& reactor, ProdNetworkBus& bus, std::uint64_t self_id,
                      std::uint64_t admin_id, const std::string& data_dir,
                      std::uint64_t seed, std::vector<std::uint64_t> cluster)
        : ProdConsensusNode(reactor, bus, self_id, admin_id, data_dir, seed,
                            std::move(cluster), Timing{}) {}

    // Full ctor with explicit timing (S5b-2 multi-process call site).
    ProdConsensusNode(ProdReactor& reactor, ProdNetworkBus& bus, std::uint64_t self_id,
                      std::uint64_t admin_id, const std::string& data_dir,
                      std::uint64_t seed, std::vector<std::uint64_t> cluster,
                      Timing timing)
        : reactor_(&reactor),
          self_id_(self_id),
          admin_id_(admin_id),
          net_(bus.node(self_id)),
          admin_net_(bus.node(admin_id)),
          rng_(seed),
          disk_(disk_sched_, data_dir.empty()
                                 ? std::string("/dev/null")
                                 : (data_dir + "/consensus.wal")) {
        consensus::NodeDeps deps;
        deps.sched = &reactor;
        deps.clock = &reactor.clock();
        deps.rng = &rng_;
        deps.net = net_;
        deps.disk = &disk_;

        consensus::NodeConfig nc;
        nc.self_id = self_id;
        nc.cluster = std::move(cluster);
        nc.election_timeout_min = timing.election_min;
        nc.election_timeout_max = timing.election_max;
        nc.heartbeat_interval = timing.heartbeat;
        nc.request_deadline = timing.request_deadline;

        const consensus::ConsensusNodeFactory factory =
            consensus::raft_a::make_raft_a_factory();
        node_ = factory(deps, nc);
    }

    ProdConsensusNode(const ProdConsensusNode&) = delete;
    ProdConsensusNode& operator=(const ProdConsensusNode&) = delete;
    ProdConsensusNode(ProdConsensusNode&&) = delete;
    ProdConsensusNode& operator=(ProdConsensusNode&&) = delete;

    // True if the consensus net + admin net + disk + node all assembled, AND the net
    // endpoint id agrees with the configured self_id (addressing consistency).
    [[nodiscard]] bool valid() const noexcept {
        return reactor_ != nullptr && net_ != nullptr && admin_net_ != nullptr &&
               node_ != nullptr && disk_.valid() && net_->local().id == self_id_;
    }

    [[nodiscard]] bool disk_valid() const noexcept { return disk_.valid(); }
    [[nodiscard]] core::Endpoint endpoint() const noexcept {
        return net_ != nullptr ? net_->local() : core::Endpoint{self_id_};
    }
    [[nodiscard]] core::Endpoint admin_endpoint() const noexcept {
        return admin_net_ != nullptr ? admin_net_->local() : core::Endpoint{admin_id_};
    }

    // Begin participating: start() the Raft node (arms its election timer, spawns its
    // recv/timer loops on the reactor) AND spawn the admin serve-loop. `admin_budget`
    // bounds the admin recv loop (NEVER unbounded). Idempotent on the node side.
    void start(int admin_budget) {
        if (node_ == nullptr || admin_net_ == nullptr) {
            return;
        }
        node_->start();
        reactor_->spawn(admin_serve(this, admin_budget));
    }

    // ---- direct (in-process) node surface — the admin protocol wraps these ----
    [[nodiscard]] consensus::SubmitResult submit(const std::string& value) {
        return node_->submit(value);
    }
    [[nodiscard]] consensus::Role role() const noexcept { return node_->role(); }
    [[nodiscard]] consensus::Term term() const noexcept {
        return node_->current_term();
    }
    [[nodiscard]] consensus::Index commit_index() const noexcept {
        return node_->commit_index();
    }
    // COPY the committed-log values out (V-RKV1: the span is valid only until the
    // next mutation — we deep-copy here so the caller holds owned strings).
    [[nodiscard]] std::vector<std::string> committed_values() const {
        std::vector<std::string> out;
        const std::span<const consensus::LogEntry> lg = node_->log();
        const consensus::Index ci = node_->commit_index();
        for (consensus::Index i = 1; i <= ci && i <= lg.size(); ++i) {
            out.push_back(lg[i - 1].value);
        }
        return out;
    }
    [[nodiscard]] std::vector<consensus::LogEntry> committed_entries() const {
        std::vector<consensus::LogEntry> out;
        const std::span<const consensus::LogEntry> lg = node_->log();
        const consensus::Index ci = node_->commit_index();
        for (consensus::Index i = 1; i <= ci && i <= lg.size(); ++i) {
            out.push_back(lg[i - 1]);
        }
        return out;
    }
    // The FULL durable log (every appended entry, persisted to ProdDisk + recovered
    // from it on restart) — the durability surface. COPY out (V-RKV1: the span is
    // valid only until the next mutation). For the durable-recovery proof: this is
    // what consensus.wal carries across a process restart, independent of commit.
    [[nodiscard]] std::vector<consensus::LogEntry> durable_entries() const {
        const std::span<const consensus::LogEntry> lg = node_->log();
        return {lg.begin(), lg.end()};
    }

    // ---- bounded run loops (the daemon + tests pump these) ----
    bool run_until(const std::function<bool()>& pred, core::Tick deadline_ns) {
        return reactor_->run_until(pred, deadline_ns);
    }
    void run_with_deadline(core::Tick deadline_ns) {
        reactor_->run_with_deadline(deadline_ns);
    }

    [[nodiscard]] ProdReactor& reactor() noexcept { return *reactor_; }
    [[nodiscard]] core::INetwork* admin_network() noexcept { return admin_net_; }

    // 1-node real-time timing knobs (ns). Public so a daemon/test can size its wall
    // guard ABOVE the election window (the node must reach Leader before the bound).
    static constexpr core::Tick kElectionMinNs = 8'000'000;    // 8 ms
    static constexpr core::Tick kElectionMaxNs = 20'000'000;   // 20 ms
    static constexpr core::Tick kHeartbeatNs = 4'000'000;      // 4 ms
    static constexpr core::Tick kRequestDeadlineNs = 200'000'000;  // 200 ms

private:
    // The admin serve-loop: a FREE FUNCTION (stable `self` pointer, no [&] capture)
    // that recv()s admin request frames off the admin net, decodes one, routes it to
    // the node, and sends a reply frame back to the requester. BOUNDED by `budget`
    // (NEVER unbounded). Each iteration co_awaits recv() then send(): a single
    // round-trip. On a malformed frame it simply ignores it (no reply) and continues.
    static core::Task admin_serve(ProdConsensusNode* self, int budget) {
        core::INetwork* net = self->admin_net_;
        for (int i = 0; i < budget; ++i) {
            core::Message msg = co_await net->recv();
            std::vector<std::byte> reply = self->handle_admin(msg.payload);
            if (!reply.empty()) {
                co_await net->send(msg.from, {reply.data(), reply.size()});
            }
        }
        co_return;
    }

    // Decode one admin request payload + produce the reply payload bytes (or empty on
    // a malformed frame). Pure routing over the node's surface — no IO here.
    [[nodiscard]] std::vector<std::byte> handle_admin(std::span<const std::byte> req) {
        admin_detail::Reader r{req.data(), req.size(), 0, true};
        const auto kind = static_cast<AdminKind>(r.u8());
        std::vector<std::byte> rep;
        if (kind == AdminKind::Submit) {
            const std::string value = r.str();
            if (!r.ok()) {
                return rep;  // malformed: ignore
            }
            const consensus::SubmitResult sr = node_->submit(value);
            if (sr.accepted) {
                admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::SubmitOk));
                admin_detail::put_u64(rep, sr.term);
                admin_detail::put_u64(rep, sr.index);
            } else {
                admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::NotLeader));
                admin_detail::put_u64(rep, sr.leader_hint);
            }
        } else if (kind == AdminKind::Status) {
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::StatusRep));
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(node_->role()));
            admin_detail::put_u64(rep, node_->current_term());
            admin_detail::put_u64(rep, node_->commit_index());
            // Digest = the FULL durable LOG values (every appended + persisted entry),
            // so the durable-recovery proof can read the recovered log back over the
            // socket from a fresh incarnation (COPY out; V-RKV1). commit_index is
            // reported separately above (the committed prefix is its [1..commit_index]
            // slice). For an N=1 cluster the impl does not self-advance commit_index
            // (see ProdConsensusNode's N=1 commit FLAG), so the durable LOG — not the
            // committed prefix — is the recovery payload the test asserts on.
            const std::span<const consensus::LogEntry> lg = node_->log();
            const std::uint32_t n = static_cast<std::uint32_t>(lg.size());
            admin_detail::put_u32(rep, n);
            for (std::uint32_t i = 0; i < n; ++i) {
                admin_detail::put_str(rep, lg[i].value);
            }
        }
        return rep;
    }

    ProdReactor* reactor_;
    std::uint64_t self_id_;
    std::uint64_t admin_id_;
    core::INetwork* net_;        // consensus peer-RPC handle (owned by the bus)
    core::INetwork* admin_net_;  // admin/client handle (owned by the bus)
    ProdRandom rng_;             // election jitter / backoff (seeded)
    core::Scheduler disk_sched_; // mints ProdDisk's inline-ready Futures (harness only)
    ProdDisk disk_;              // the DURABLE consensus log over data_dir
    std::unique_ptr<consensus::ConsensusNode> node_;  // impl A, UNCHANGED
};

}  // namespace lockstep::prod

#endif  // __linux__
