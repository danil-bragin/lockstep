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

#include <lockstep/prod/ProdAuth.hpp>    // AUTH/RBAC — principal->role->permission policy
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdMetrics.hpp>  // Phase 10 OBSERVABILITY — metrics registry
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
    // S8.3: a CHEAP O(1) commit-index query. Returns ONLY {role, term, commit}, WITHOUT
    // serializing the durable log (which the full Status path does, costing O(log size)
    // per call). The honest-commit-throughput measurement polls THIS per submit so the
    // hot path no longer pays the O(log) re-serialization cost as the log grows. The
    // full-log Status path (above) is UNCHANGED — the jepsen + cluster_smoke safety
    // checkers depend on its committed-log digest, so it stays intact.
    StatCommit = 6,     // req: [StatCommit]
    StatCommitRep = 7,  // rep: [StatCommitRep][u8 role][u64 term][u64 commit]
    // Phase 10 OBSERVABILITY: scrape this node's metrics in Prometheus text-exposition
    // format. Cheap (O(#metrics), NOT O(log)): the handler refreshes the gauges from the
    // existing consensus observables + the disk counters and serializes the fixed metric
    // set as a UTF-8 text blob. The full-log Status path is untouched.
    Metrics = 8,     // req: [Metrics]
    MetricsRep = 9,  // rep: [MetricsRep][str prometheus_text]
    // AUTH/RBAC: the server returns this when the connection's PRINCIPAL lacks permission
    // for the requested operation class (fail-closed). The op is NOT executed. The reply
    // carries a short reason string so the client/test can confirm it was an auth denial
    // (not a dead node / not-leader). This is the AUTHZ gate AFTER the mTLS AUTHN gate.
    AuthDenied = 10,    // rep: [AuthDenied][str reason]
    // P5 QUORUM-LOSS RECOVERY: force this survivor into a single-node cluster {self}
    // under a fresh identity token, so it self-elects + commits after a permanent
    // majority loss. DANGEROUS + mutating (Write op class, auth-gated). req carries the
    // new token; the reply just acks it was applied.
    ForceNewCluster = 11,     // req: [ForceNewCluster][u64 new_token]
    ForceNewClusterRep = 12,  // rep: [ForceNewClusterRep][u64 new_token]
    SqlQuery = 13,     // req: [SqlQuery][str sql]  (SQL-over-Raft local read)
    SqlQueryRep = 14,  // rep: [SqlQueryRep][str rendered_result]
};

// A decoded STATUS reply (the client-observable cluster state across the socket).
struct AdminStatus {
    std::uint8_t role = 0;            // consensus::Role as a byte
    std::uint64_t term = 0;
    std::uint64_t commit_index = 0;
    std::vector<std::string> committed;  // committed-log values [1..commit_index]
};

// S8.3: a decoded CHEAP commit-index reply — just {role, term, commit_index}, NO log.
struct AdminCommit {
    std::uint8_t role = 0;            // consensus::Role as a byte
    std::uint64_t term = 0;
    std::uint64_t commit_index = 0;
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
// S8.3: the CHEAP commit-index query request (no log payload in the reply).
[[nodiscard]] inline std::vector<std::byte> encode_stat_commit() {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::StatCommit));
    return b;
}
// Phase 10 OBSERVABILITY: the METRICS scrape request (reply carries the Prometheus text).
[[nodiscard]] inline std::vector<std::byte> encode_metrics() {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::Metrics));
    return b;
}
// P5 QUORUM-LOSS RECOVERY: force this node into a single-node cluster {self} under a
// fresh identity `new_token`. DANGEROUS (mutating); the reply echoes the token on apply.
[[nodiscard]] inline std::vector<std::byte> encode_force_new_cluster(std::uint64_t new_token) {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::ForceNewCluster));
    admin_detail::put_u64(b, new_token);
    return b;
}
// Decode a ForceNewCluster reply (client side): true + the echoed token on success.
[[nodiscard]] inline bool decode_force_new_cluster(std::span<const std::byte> payload,
                                                   std::uint64_t& out_token) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    if (static_cast<AdminKind>(r.u8()) != AdminKind::ForceNewClusterRep) {
        return false;
    }
    out_token = r.u64();
    return r.ok();
}
// SQL-over-Raft: encode a read-only SQL query request; decode its rendered-result reply.
[[nodiscard]] inline std::vector<std::byte> encode_sql_query(const std::string& sql) {
    std::vector<std::byte> b;
    admin_detail::put_u8(b, static_cast<std::uint8_t>(AdminKind::SqlQuery));
    admin_detail::put_str(b, sql);
    return b;
}
[[nodiscard]] inline bool decode_sql_query(std::span<const std::byte> payload, std::string& out) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    if (static_cast<AdminKind>(r.u8()) != AdminKind::SqlQueryRep) {
        return false;
    }
    out = r.str();
    return r.ok();
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

// S8.3: decode the CHEAP commit reply (client side). Returns false on a malformed frame.
[[nodiscard]] inline bool decode_stat_commit(std::span<const std::byte> payload,
                                             AdminCommit& out) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    const auto kind = static_cast<AdminKind>(r.u8());
    if (kind != AdminKind::StatCommitRep) {
        return false;
    }
    out.role = r.u8();
    out.term = r.u64();
    out.commit_index = r.u64();
    return r.ok();
}

// AUTH/RBAC: is this reply an AUTH-DENIED frame? (the principal lacked permission; the op
// did NOT execute). Returns true + fills `reason` on an AuthDenied reply, false otherwise.
[[nodiscard]] inline bool decode_auth_denied(std::span<const std::byte> payload,
                                             std::string& reason) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    const auto kind = static_cast<AdminKind>(r.u8());
    if (kind != AdminKind::AuthDenied) {
        return false;
    }
    reason = r.str();
    return r.ok();
}

// Phase 10 OBSERVABILITY: decode the METRICS reply — the Prometheus text blob (or empty
// on a malformed frame / a node that does not support the verb).
[[nodiscard]] inline bool decode_metrics(std::span<const std::byte> payload,
                                         std::string& out) {
    admin_detail::Reader r{payload.data(), payload.size(), 0, true};
    const auto kind = static_cast<AdminKind>(r.u8());
    if (kind != AdminKind::MetricsRep) {
        return false;
    }
    out = r.str();
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
        // P2 restore-new-cluster: the cluster-identity token. 0 = unset (a normal single
        // cluster). A restored cluster is booted with a FRESH token so a stale node from
        // the old cluster (same ids/ports) is dropped by the guard and cannot rejoin.
        std::uint64_t cluster_token = 0;
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
          // S9.2 — bind the consensus WAL disk to the REACTOR (not a throwaway sim
          // Scheduler): Futures are minted from the reactor's SchedulerSink and sync()
          // submits an ASYNC fdatasync through the reactor's io_uring ring, whose CQE is
          // the durability barrier. Falls back to synchronous fdatasync if the ring is
          // unavailable (seccomp-blocked) or the fd does not support it.
          disk_(reactor, data_dir.empty()
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
        nc.cluster_token = timing.cluster_token;  // P2 cluster-identity guard
        // PROD compaction cadence: snapshot every ~4096 entries instead of every 8. Each
        // snapshot RE-SERIALIZES the whole accumulated state (O(n)); at the gate's default
        // of 8 a long single-daemon run pays O(n^2) of redundant snapshot I/O. 4096 keeps
        // the retained log bounded (~tens of KB for small values) while cutting snapshot
        // frequency ~512x. Compaction is transparent to the committed log (Snapshot.tla),
        // so correctness + recovery are unchanged. The sim/cross-check keep the default 8.
        nc.snapshot_threshold = 4096;

        const consensus::ConsensusNodeFactory factory =
            consensus::raft_a::make_raft_a_factory();
        node_ = factory(deps, nc);

        // Phase 10 OBSERVABILITY: label this node's metrics stream. shard defaults to 0
        // (the single-shard daemon); the multi-shard runner overrides it via
        // set_metric_shard() before start(). node label = the Raft node id.
        metrics_.node = self_id;
    }

    ProdConsensusNode(const ProdConsensusNode&) = delete;
    ProdConsensusNode& operator=(const ProdConsensusNode&) = delete;
    ProdConsensusNode(ProdConsensusNode&&) = delete;
    ProdConsensusNode& operator=(ProdConsensusNode&&) = delete;

    // S9.2 — GRACEFUL-SHUTDOWN DURABILITY FLUSH. The dtor BODY runs BEFORE member
    // destructors (disk_ closes its fd in its own dtor), so flushing the reactor's
    // in-flight async fdatasyncs HERE guarantees every durably-INTENDED entry reaches
    // the platter while the WAL fd is still open — restoring the exact clean-exit
    // durability synchronous fdatasync gave for free. An ABRUPT crash (SIGKILL) skips
    // this dtor entirely (its un-completed fsyncs are honestly lost — correct crash
    // semantics). No-op when the ring is unavailable.
    ~ProdConsensusNode() {
        if (reactor_ != nullptr) {
            reactor_->flush_uring();
        }
    }

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
        // S9.2 — register the reactor's io_uring ring fd on its epoll set so async
        // fdatasync completions (CQEs) wake the loop and resolve the durability barrier.
        // No-op if the ring is unavailable (then the disk stays on synchronous fdatasync).
        reactor_->arm_uring();
        node_->start();
        reactor_->spawn(admin_serve(this, admin_budget));
        if (apply_fn_) {
            reactor_->spawn(apply_pump(this));  // SQL-over-Raft: apply committed entries in order
        }
    }

    // SQL-OVER-RAFT: install a callback invoked, in commit order, with each committed
    // entry's value as the LOCAL commit index advances — the node applies the replicated
    // log into its state machine (e.g. exec a committed SQL statement into a local
    // SqlEngine). Set BEFORE start(). Unset = no apply pump (unchanged behavior).
    void set_apply_fn(std::function<void(consensus::Index, const std::string&)> fn) {
        apply_fn_ = std::move(fn);
    }
    // Highest committed index applied into the state machine so far (0 = none). A PG-over-
    // Raft write waits on this reaching its submit index before replying (the value is durable
    // + applied on this node once applied_index() >= index).
    [[nodiscard]] consensus::Index applied_index() const noexcept { return applied_; }

    // SQL-OVER-RAFT read seam: a callback that runs a read-only SQL query against the local
    // applied state machine and returns a rendered result — the SqlQuery admin verb. Set
    // before start(). Unset = the verb returns an error.
    void set_query_fn(std::function<std::string(const std::string&)> fn) { query_fn_ = std::move(fn); }

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

    // ---- Phase 10 OBSERVABILITY — metrics surface ----------------------------
    // The metrics registry for THIS node/shard. Mutated ONLY on this reactor's thread
    // (single-writer); a scrape reads it on the same thread. set_metric_shard() stamps
    // the shard label (the multi-shard runner calls it before start()).
    void set_metric_shard(std::uint64_t shard_idx) noexcept { metrics_.shard = shard_idx; }

    // ---- AUTH/RBAC — install the principal->role->permission policy ------------
    // The daemon loads a policy (from --auth-policy / inline grants) and sets it here
    // BEFORE start(). When the policy is not enabled() (none configured), enforcement is
    // OPEN (every op allowed) so the existing non-auth deployments are byte-identical.
    void set_auth_policy(AuthPolicy policy) { auth_ = std::move(policy); }
    [[nodiscard]] const AuthPolicy& auth_policy() const noexcept { return auth_; }

    [[nodiscard]] ProdMetrics& metrics() noexcept { return metrics_; }
    [[nodiscard]] const ProdMetrics& metrics() const noexcept { return metrics_; }

    // Refresh the gauge snapshots from the EXISTING consensus observables + the disk
    // counters, AND derive the transition counters (elections_started / leader_changes /
    // steps_down / submits_committed) by EDGE-DETECTING against the last-seen snapshot.
    // This is the "metrics READ the observables" boundary — no consensus change. Cheap:
    // role/term/commit_index are member reads; log size uses physical_log_size() (an O(1)
    // member read of log_.size()) — NOT log().size(), which calls rebuild_log_view() and
    // does an O(n) clear+copy of the whole logical log on EVERY call. Because this runs on
    // every admin request, log().size() made the leader's per-request work O(current log
    // size) ⇒ O(n^2) over a sustained run (the comparative bench surfaced this: single-node
    // admin throughput collapsed 12.8k→1.3k from 4k→50k ops). physical_log_size() (the
    // retained physical suffix; bounded after snapshotting) is the right gauge source and is
    // truly O(1). The disk counters are member reads. NEVER walks the durable log. Call it
    // on every admin request + on a METRICS scrape (both O(1)); single reactor thread only.
    void refresh_metrics() noexcept {
        const auto role = static_cast<std::uint64_t>(node_->role());
        const std::uint64_t term = node_->current_term();
        const std::uint64_t ci = node_->commit_index();
        const std::uint64_t lsz = node_->physical_log_size();

        // Edge-detect role/term transitions (counters are monotonic event tallies).
        const auto leader = static_cast<std::uint64_t>(consensus::Role::Leader);
        const auto candidate = static_cast<std::uint64_t>(consensus::Role::Candidate);
        const auto follower = static_cast<std::uint64_t>(consensus::Role::Follower);
        if (term > last_term_) {
            // A new term began — if we entered it as a Candidate, an election started.
            if (role == candidate) {
                metrics_.elections_started.inc();
            }
        }
        if (role == candidate && last_role_ != candidate) {
            // Became a Candidate (a fresh election attempt) regardless of term recording.
            metrics_.elections_started.inc();
        }
        if (role == leader && last_role_ != leader) {
            metrics_.leader_changes.inc();
        }
        if (role == follower && last_role_ != follower &&
            (last_role_ == leader || last_role_ == candidate)) {
            metrics_.steps_down.inc();
        }

        // submits_committed: the count of OUR accepted entries that have reached commit.
        // accepted_index_ is the highest index we accepted as leader; once commit_index
        // covers a previously-uncovered accepted index, count those as committed. We only
        // attribute commits up to our own accepted high-water (entries WE took), so the
        // counter tracks the writes this node accepted that became durable.
        if (ci > committed_seen_ && accepted_index_ > 0) {
            const std::uint64_t up_to = ci < accepted_index_ ? ci : accepted_index_;
            if (up_to > committed_seen_) {
                metrics_.submits_committed.inc(up_to - committed_seen_);
                committed_seen_ = up_to;
            }
        }

        // in_flight: accepted-but-not-yet-committed entries this node took as leader.
        const std::uint64_t inflight =
            accepted_index_ > ci ? (accepted_index_ - ci) : 0;

        metrics_.role.set(role);
        metrics_.current_term.set(term);
        metrics_.commit_index.set(ci);
        metrics_.log_size.set(lsz);
        metrics_.in_flight.set(inflight);
        metrics_.fdatasync_calls.set(disk_.sync_calls());
        metrics_.bytes_appended.set(disk_.bytes_appended());
        metrics_.append_calls.set(disk_.append_calls());

        last_role_ = role;
        last_term_ = term;
    }

    // ---- S8.5 disk profiling passthrough (introspection only) -----------------
    // Surface the ProdDisk fdatasync/append counters so the daemon can report
    // fsyncs-per-committed-op + fsync wall-time on shutdown — the data behind the
    // "is the commit path fsync-bound?" verdict. Off the durability path.
    [[nodiscard]] std::uint64_t disk_append_calls() const noexcept { return disk_.append_calls(); }
    [[nodiscard]] std::uint64_t disk_sync_calls() const noexcept { return disk_.sync_calls(); }
    [[nodiscard]] std::uint64_t disk_sync_total_ns() const noexcept { return disk_.sync_total_ns(); }
    [[nodiscard]] std::uint64_t disk_bytes_appended() const noexcept { return disk_.bytes_appended(); }
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
    // SQL-OVER-RAFT apply pump: a periodic reactor coroutine that, as the committed index
    // advances, invokes apply_fn_ with each newly-committed entry's value IN ORDER (once
    // each). Deterministic state machines (the SqlEngine) reach identical state on every
    // replica. Bounded by the reactor's run deadline (left suspended at teardown).
    static core::Task apply_pump(ProdConsensusNode* self) {
        for (;;) {
            co_await self->reactor_->clock().delay(2'000'000);  // 2ms apply cadence
            const consensus::Index ci = self->node_->commit_index();
            const std::span<const consensus::LogEntry> lg = self->node_->log();
            while (self->applied_ < ci && self->applied_ < static_cast<consensus::Index>(lg.size())) {
                // 1-based log index of this entry = applied_ + 1 (matches SubmitResult.index).
                self->apply_fn_(self->applied_ + 1, lg[static_cast<std::size_t>(self->applied_)].value);
                ++self->applied_;
            }
        }
    }

    static core::Task admin_serve(ProdConsensusNode* self, int budget) {
        ProdNetwork* net = self->admin_net_;
        for (int i = 0; i < budget; ++i) {
            core::Message msg = co_await net->recv();
            // AUTH/RBAC: snapshot the PRINCIPAL (peer cert CN) of the connection that
            // delivered THIS frame right after recv() returns — before any other recv()
            // can overwrite the side-channel. Copy it (the handler may co_await on send).
            const std::string principal = net->last_principal();
            std::vector<std::byte> reply = self->handle_admin(msg.payload, principal);
            if (!reply.empty()) {
                co_await net->send(msg.from, {reply.data(), reply.size()});
            }
        }
        co_return;
    }

    // AUTH/RBAC: the OPERATION CLASS each admin verb belongs to. READ verbs observe
    // (Status/StatCommit/Metrics); WRITE verbs mutate (Submit). An UNKNOWN / malformed
    // verb maps to None -> DENIED before any dispatch (fail-closed). (There is no admin-
    // control verb on this surface yet; membership/config/shutdown ride other paths.)
    [[nodiscard]] static OpClass op_class_of(AdminKind kind) noexcept {
        switch (kind) {
            case AdminKind::Submit:
            case AdminKind::ForceNewCluster:
                return OpClass::Write;  // mutating (recovery) — auth-gated like Submit.
            case AdminKind::Status:
            case AdminKind::StatCommit:
            case AdminKind::Metrics:
            case AdminKind::SqlQuery:
                return OpClass::Read;
            default:
                return OpClass::None;  // unknown/reply kinds -> DENIED (never executed).
        }
    }

    // Build an AUTH-DENIED reply (the op did NOT execute). Carries a short reason.
    [[nodiscard]] static std::vector<std::byte> auth_denied(const std::string& reason) {
        std::vector<std::byte> rep;
        admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::AuthDenied));
        admin_detail::put_str(rep, reason);
        return rep;
    }

    // Decode one admin request payload + produce the reply payload bytes (or empty on
    // a malformed frame). Pure routing over the node's surface — no IO here. `principal`
    // is the authenticated client identity (mTLS cert CN; empty == anonymous/plaintext).
    [[nodiscard]] std::vector<std::byte> handle_admin(std::span<const std::byte> req,
                                                      const std::string& principal) {
        admin_detail::Reader r{req.data(), req.size(), 0, true};
        const auto kind = static_cast<AdminKind>(r.u8());
        std::vector<std::byte> rep;
        // OBSERVABILITY: every admin request handled is a client request; refresh the
        // gauges + transition counters from the observables on each request (O(1)).
        metrics_.client_requests.inc();
        refresh_metrics();
        // AUTH/RBAC ENFORCEMENT — BEFORE any dispatch / submit(). Resolve the op class
        // (None for a malformed/unknown verb) and check the principal's permission. On a
        // miss, return AUTH-DENIED and DO NOT execute (default-deny / fail-closed). When
        // no policy is configured, auth_.allow() is always true (legacy open path).
        if (!r.ok()) {
            return auth_denied("malformed request");  // never dispatch a torn frame.
        }
        const OpClass cls = op_class_of(kind);
        if (!auth_.allow(principal, cls)) {
            return auth_denied("principal '" + principal + "' lacks permission");
        }
        if (kind == AdminKind::Submit) {
            const std::string value = r.str();
            if (!r.ok()) {
                return rep;  // malformed: ignore
            }
            const consensus::SubmitResult sr = node_->submit(value);
            if (sr.accepted) {
                // OBSERVABILITY: count the accepted write + track its index as our commit
                // high-water (refresh_metrics later attributes commits up to this index).
                metrics_.submits_accepted.inc();
                if (sr.index > accepted_index_) {
                    accepted_index_ = sr.index;
                }
                admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::SubmitOk));
                admin_detail::put_u64(rep, sr.term);
                admin_detail::put_u64(rep, sr.index);
            } else {
                admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::NotLeader));
                admin_detail::put_u64(rep, sr.leader_hint);
            }
        } else if (kind == AdminKind::ForceNewCluster) {
            const std::uint64_t new_token = r.u64();
            if (!r.ok()) {
                return rep;  // malformed: ignore
            }
            node_->force_new_cluster(new_token);  // quorum-loss recovery (P5)
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::ForceNewClusterRep));
            admin_detail::put_u64(rep, new_token);
        } else if (kind == AdminKind::SqlQuery) {
            const std::string q = r.str();
            if (!r.ok()) {
                return rep;
            }
            const std::string result = query_fn_ ? query_fn_(q) : std::string("ERR:no SQL engine");
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::SqlQueryRep));
            admin_detail::put_str(rep, result);
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
        } else if (kind == AdminKind::StatCommit) {
            // S8.3 CHEAP commit-index query — O(1): role/term/commit ONLY, no log
            // walk, no per-entry serialization. node_->commit_index() is a member read
            // (the consensus impls track it directly), so this does NOT touch the
            // durable log size — polling it per submit is O(1) regardless of log growth.
            // The full-log Status branch above is left intact for the safety checkers.
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::StatCommitRep));
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(node_->role()));
            admin_detail::put_u64(rep, node_->current_term());
            admin_detail::put_u64(rep, node_->commit_index());
        } else if (kind == AdminKind::Metrics) {
            // Phase 10 OBSERVABILITY: scrape this node's metrics in Prometheus text
            // format. refresh_metrics() (already called above) snapshotted the gauges +
            // transition counters from the observables; encode the fixed metric set.
            // O(#metrics), NOT O(log) — never walks the durable log.
            admin_detail::put_u8(rep, static_cast<std::uint8_t>(AdminKind::MetricsRep));
            admin_detail::put_str(rep, encode_prometheus(metrics_));
        }
        return rep;
    }

    ProdReactor* reactor_;
    std::uint64_t self_id_;
    std::uint64_t admin_id_;
    core::INetwork* net_;        // consensus peer-RPC handle (owned by the bus)
    ProdNetwork* admin_net_;     // admin/client handle (owned by the bus); ProdNetwork so
                                 // the AUTH layer can read last_principal() (peer cert CN)
    ProdRandom rng_;             // election jitter / backoff (seeded)
    ProdDisk disk_;              // the DURABLE consensus log over data_dir (S9.2: reactor-bound)
    std::unique_ptr<consensus::ConsensusNode> node_;  // impl A, UNCHANGED
    std::function<void(consensus::Index, const std::string&)> apply_fn_;  // SQL-over-Raft apply
    std::function<std::string(const std::string&)> query_fn_;  // SQL-over-Raft local read
    consensus::Index applied_ = 0;                       // highest applied index (0 = none)

    AuthPolicy auth_{};  // AUTH/RBAC principal->role->permission policy (empty == OPEN/legacy)

    // Phase 10 OBSERVABILITY state (single reactor thread; single-writer).
    ProdMetrics metrics_;                // per-node/shard registry (labeled in the ctor)
    std::uint64_t last_role_ = 0;        // last-seen role for transition edge-detection
    std::uint64_t last_term_ = 0;        // last-seen term for election edge-detection
    std::uint64_t accepted_index_ = 0;   // highest index WE accepted as leader
    std::uint64_t committed_seen_ = 0;   // highest of our accepted indices counted committed
};

}  // namespace lockstep::prod

#endif  // __linux__
