#pragma once

// ProdServerNode.hpp — Phase 7 S5a. THE SINGLE-NODE PROD SERVER ASSEMBLY: the
// precursor to lockstepd. It is the PROD analogue of query::LocalCluster's server
// half — it stands up the EXISTING client-facing DB stack
//
//     ProdNetwork (real TCP) -> wire::Server -> Database -> txn deterministic
//     executor -> MVCC store
//
// on the PROD providers (ProdReactor epoll loop + the reactor's ONE shared
// ProdClock + ProdRandom(seed) + ProdDisk(data dir)), driven by ProdReactor, and
// spawns the server's recv/serve coroutine on that reactor. A real wire client
// (wire::ClientStub / query::Connection over a ProdNetwork client endpoint) then
// connects over loopback, submits a txn, and reads it back — proving the prod
// stack serves real clients end-to-end over a real socket. Multi-node Raft
// consensus is S5b; this is the SINGLE-NODE precursor.
//
// ============================================================================
// WHAT THIS ASSEMBLES (mirroring LocalCluster, but on prod providers)
//   LocalCluster (sim)                     ProdServerNode (prod)
//   ------------------------------------   ------------------------------------
//   core::Scheduler                        prod::ProdReactor (epoll loop)
//   core::SimClock(sched)                  reactor.clock() (the reactor's OWN
//                                            ProdClock — ONE shared identity)
//   sim::SeededRandom(seed)                prod::ProdRandom(seed)
//   sim::SimNetworkBus(sched,rng)          prod::ProdNetworkBus(reactor)  [real TCP]
//   wire::Server(srv_net)                  wire::Server(srv_net)          [UNCHANGED]
//   (server's Database is in-memory MVCC; see DESIGN NOTE below)
//
// The wire::Server + Database + txn executor + MVCC store are taken UNCHANGED —
// they are provider-agnostic (wire::Server takes a core::INetwork&, and ProdNetwork
// IS a core::INetwork). That is the whole point of the boundary architecture: the
// sim-proven client-facing stack runs on real sockets with ZERO core/query/txn/
// storage change.
//
// ----------------------------------------------------------------------------
// STORAGE SEAM (Phase 7 S5a flag CLOSED): wire::Server now exposes a disk-injection
// seam and query::Database backs its committed query state with ONE persistent
// storage::WalEngine over an injected core::IDisk (Database.hpp). This node injects
// the durable ProdDisk over the data dir, so the committed query state is WAL'd to
// disk as it commits and RECOVERS on a restart (recover() rebuilds the store from
// the durable WAL prefix — the verified crash-recovery path) WITHOUT replaying the
// consensus log.
//
// SCHEDULER PUMPING (the correctness point): the persistent engine runs on
// disk_sched_ (the ProdDisk's OWN scheduler), and Database::apply_committed() /
// run() / recover() PUMP disk_sched_ INLINE (PersistentStore::apply_committed does
// sched_->run() to drain the WAL append+sync). ProdDisk does synchronous inline IO,
// so its Futures resolve without needing the reactor's epoll/timer loop — the
// commit completes within the handle_submit call. disk_sched_ is therefore NOT
// dependent on the ProdReactor pumping it; it is self-driven by the query layer.
//
// ----------------------------------------------------------------------------
// LINUX-ONLY (epoll + sockets). The whole class is compiled only under __linux__
// (ProdReactor/ProdNetwork are __linux__-guarded); the CMake target + its test are
// added only on Linux, so the macOS host build stays green (this assembly absent).
//
// providers/prod/ is the lint-exempt boundary zone. ProdServerNode itself touches
// NO raw syscall — it only assembles the provider surfaces + wire::Server. The run
// loop is BOUNDED: run_until(stop-pred, absolute-deadline) so a lost connection can
// NEVER hang. RAII everywhere (the reactor closes its epoll fd; ProdNetwork closes
// its sockets; ProdDisk closes its fd) — no fd leak under ASan/LSan. No real threads
// (the reactor is the one thread).

#ifdef __linux__

#include <cstddef>
#include <cstdint>
#include <string>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdRandom.hpp>
#include <lockstep/prod/ProdReactor.hpp>

#include <lockstep/query/wire/Server.hpp>

namespace lockstep::prod {

// The UNCHANGED client-facing protocol server lives in lockstep::query::wire; alias
// it locally so this prod assembly reads like LocalCluster (wire::Server / Seq).
namespace wire = ::lockstep::query::wire;
using ::lockstep::query::Seq;

// Config a ProdServerNode is assembled from (the thin lockstepd config too).
struct ProdServerConfig {
    std::uint64_t node_id = 1;       // this node's Endpoint id (single-node: 1)
    std::uint64_t seed = 0;          // ProdRandom seed (election jitter / backoff)
    std::string data_dir;            // ProdDisk data directory (the durability anchor)
};

// ---------------------------------------------------------------------------
// ProdServerNode — owns the reactor binding (passed in, shared with any client on
// the SAME in-process reactor for the round-trip test), the node's ProdNetwork
// handle, the ProdRandom, the ProdDisk over the data dir, and the wire::Server. It
// spawns the server's serve() coroutine and offers BOUNDED run loops.
//
// One in-process ProdReactor drives both the server node AND the client stub for
// the loopback round-trip test (S4b's in-process model). Multi-PROCESS is S5b; a
// real lockstepd main() (cli/lockstepd.cpp) owns its own reactor + node + run loop.
// ---------------------------------------------------------------------------
class ProdServerNode {
public:
    // Assemble on an EXISTING reactor + network bus (so a client on the same reactor
    // can dial this node over loopback). The bus must already have add_node(node_id)
    // called (the bus binds the listen socket + records the ephemeral port). The
    // node's ProdNetwork handle is fetched from the bus.
    ProdServerNode(ProdReactor& reactor, ProdNetworkBus& bus, const ProdServerConfig& cfg)
        : reactor_(&reactor),
          net_(bus.node(cfg.node_id)),
          rng_(cfg.seed),
          disk_(disk_sched_, cfg.data_dir.empty()
                                 ? std::string("/dev/null")
                                 : (cfg.data_dir + "/lockstepd.wal")),
          // The committed QUERY STATE is backed by the durable ProdDisk over the data
          // dir (S5a closure), driven on disk_sched_ (the disk's own scheduler, pumped
          // INLINE by the query layer — not by the reactor). A restart recovers it via
          // recover(). When no data dir is given the disk is /dev/null (no durability).
          server_(net_ != nullptr ? wire::Server(*net_, disk_sched_, disk_)
                                   : wire::Server(disk_sched_, disk_)),
          cfg_(cfg) {}

    ProdServerNode(const ProdServerNode&) = delete;
    ProdServerNode& operator=(const ProdServerNode&) = delete;
    ProdServerNode(ProdServerNode&&) = delete;
    ProdServerNode& operator=(ProdServerNode&&) = delete;

    // True if the node's transport + durability anchor are both usable.
    [[nodiscard]] bool valid() const noexcept {
        return net_ != nullptr && reactor_ != nullptr;
    }

    // The node's listen Endpoint (so a client knows where to dial).
    [[nodiscard]] core::Endpoint endpoint() const noexcept {
        return net_ != nullptr ? net_->local() : core::Endpoint{cfg_.node_id};
    }

    // Spawn the server's recv/serve coroutine on the reactor. `max_msgs` is the
    // bounded recv budget (the server quiesces after that many frames — NEVER an
    // unbounded loop; size it >= the total requests the test could send incl. dups).
    void start(int max_msgs) {
        if (net_ == nullptr) {
            return;
        }
        reactor_->spawn(server_.serve(max_msgs));
    }

    // Pump the reactor until `pred()` (a stop flag) OR an ABSOLUTE now()-deadline (ns)
    // — a HARD wall guard so a lost connection / half-open socket can NEVER hang. The
    // long-lived lockstepd run loop uses this with its own stop flag + a generous (or
    // absent) deadline; a test uses it with a tight deadline. Returns true if pred
    // fired, false if the deadline tripped first.
    bool run_until(const std::function<bool()>& pred, core::Tick deadline_ns) {
        return reactor_->run_until(pred, deadline_ns);
    }

    // Pump until an absolute now()-deadline (ns) or quiescence. The simplest bounded
    // server run for a test (no separate stop flag — the deadline IS the bound).
    void run_with_deadline(core::Tick deadline_ns) {
        reactor_->run_with_deadline(deadline_ns);
    }

    // ---- exactly-once / liveness witnesses (mirrors LocalCluster) ----------
    [[nodiscard]] std::uint64_t applied_submits() const noexcept {
        return server_.applied_submits();
    }
    [[nodiscard]] std::uint64_t rejected() const noexcept { return server_.rejected(); }

    // RECOVER the committed query state from the durable ProdDisk after a restart
    // (the node was re-constructed over the SAME data dir). `durable_len` is the
    // on-disk WAL file length. After this, a Query returns every committed value
    // WITHOUT replaying the consensus log (S5a closure).
    void recover(std::size_t durable_len) { server_.recover(durable_len); }
    [[nodiscard]] Seq tip() const noexcept { return server_.tip(); }

    // The reactor's shared clock (ONE identity) — handed to a client stub's IClock&.
    [[nodiscard]] ProdClock& clock() noexcept { return reactor_->clock(); }
    [[nodiscard]] ProdReactor& reactor() noexcept { return *reactor_; }
    [[nodiscard]] core::INetwork* network() noexcept { return net_; }
    [[nodiscard]] const ProdServerConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] bool disk_valid() const noexcept { return disk_.valid(); }

    // The node's seeded entropy source (election jitter / backoff for S5b consensus).
    // Single-node S5a does not yet draw from it; exposed so the seed is observable and
    // the member is a live part of the assembly (not dead weight).
    [[nodiscard]] ProdRandom& random() noexcept { return rng_; }

private:
    ProdReactor* reactor_;       // the one event-loop thread (shared, not owned)
    core::INetwork* net_;        // this node's ProdNetwork handle (owned by the bus)
    ProdRandom rng_;             // election jitter / backoff entropy (seeded)
    core::Scheduler disk_sched_; // mints ProdDisk's inline-ready Futures (harness only)
    ProdDisk disk_;              // durability anchor over the data dir (RAII-closed)
    wire::Server server_;        // the UNCHANGED client-facing protocol server
    ProdServerConfig cfg_;
};

} // namespace lockstep::prod

#endif // __linux__
