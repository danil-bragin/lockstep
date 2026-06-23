// prod_consensus_test.cpp — Phase 7 S5b-1 driver. Proves a REAL Raft ConsensusNode
// (impl A) runs on the PROD providers as a 1-NODE cluster, commits client SUBMITs
// over a real socket admin protocol, and — the durability payload — RECOVERS its
// committed log from the real ProdDisk after a PROCESS-RESTART simulation.
//
// This is the consensus analogue of prod_server_test (S5a). Where S5a ran the
// in-memory MVCC DB stack, THIS exercises real DURABILITY: the consensus LOG lives
// on a real ProdDisk file (data_dir/consensus.wal via the node's IDisk), and a fresh
// process incarnation over the SAME data_dir rebuilds term/vote/log from those bytes
// (ConsensusNode::restart() / start() -> recover_from_disk()).
//
//   (1) ELECT + APPEND: a 1-node cluster elects itself Leader (quorum=1), a client
//       SUBMITs values over the real admin socket, each is ACCEPTED at its expected
//       log index, and each is PERSISTED to the durable consensus.wal (the leader's
//       FIFO persist worker appends + syncs every entry to the real ProdDisk).
//   (2) DURABLE CRASH/RESTART RECOVERY [the durability payload]: the node + its
//       reactor + bus + ProdDisk are DESTROYED (simulating process exit), then a FRESH
//       reactor + bus + node are built over the SAME data_dir and started.
//       recover_from_disk() replays consensus.wal, rebuilding the durable LOG FROM
//       DISK (not memory — there is none). We assert the recovered log values + terms
//       MATCH what was appended before, and read them back over the real admin socket.
//
// N=1 SELF-COMMIT (fixed): the consensus core now advances commit_index for a
// peerless 1-node cluster — the lone leader self-commits its own appended entry the
// instant it is durable (advance_commit_index() is re-evaluated after the leader's own
// append; it self-counts and quorum()==1). This deployment is exactly what surfaced the
// original gap (RaftNodeA only advanced commit_index on a peer AppendEntries ack, which
// never arrives with no peers). The fix is gated on quorum()==1, so it is a strict no-op
// for N>=2 (where commitment stays ack-driven). We now ASSERT commit_index advances to
// the durable log length here, in addition to the durable-LOG recovery payload.
//
// Everything is driven by ONE in-process ProdReactor per incarnation (the node + the
// admin client share it — the S4b in-process model; multi-PROCESS is S5b-2). BOUNDED
// by an ABSOLUTE reactor deadline ABOVE the 1-node election window so the node
// reaches Leader yet a lost frame / spinning election can NEVER hang.
//
// LINUX-ONLY: built only on Linux (tests/CMakeLists.txt guards with if(UNIX AND NOT
// APPLE)); Prod* providers are #ifdef __linux__. The macOS host never sees it.
//
// NON-provider code (a test) -> the forbidden-call lint scans it. It touches NO
// socket/epoll/clock/file syscall of its own: all real plumbing stays inside
// providers/prod/. ASAN: every coroutine is a FREE FUNCTION over stable pointers,
// NEVER an inline [&] lambda Task.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdConsensusNode.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdScratchDir.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace consensus = lockstep::consensus;

// Endpoint ids: consensus node 1, admin listen 2, admin client 3 (all in-process).
constexpr std::uint64_t kNodeId = 1;
constexpr std::uint64_t kAdminId = 2;
constexpr std::uint64_t kClientId = 3;

// Wall guard: 800 ms absolute deadline. The 1-node election window is <= 20 ms
// (ProdConsensusNode::kElectionMaxNs), so the node is Leader long before this; the
// generous bound absorbs sanitizer/scheduler jitter while still killing any hang.
constexpr core::Tick kWallNs = 800'000'000;

// A bounded recv budget for the admin serve-loop + the client reply pump.
constexpr int kBudget = 4096;

// ----- admin client over real TCP -----------------------------------------
// A client SUBMIT round-trip: send the SUBMIT frame, await one reply, decode whether
// it was accepted (SubmitOk) and at which index. A FREE FUNCTION over stable
// pointers (no [&] capture) — the coroutine frame holds plain pointers only.
struct SubmitOutcome {
    bool replied = false;
    bool accepted = false;
    std::uint64_t term = 0;
    std::uint64_t index = 0;
};

core::Task submit_client(core::INetwork* cli, core::Endpoint admin, std::string value,
                         SubmitOutcome* out, bool* done) {
    const std::vector<std::byte> req = prod::encode_submit(value);
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    prod::admin_detail::Reader r{rep.payload.data(), rep.payload.size(), 0, true};
    const auto kind = static_cast<prod::AdminKind>(r.u8());
    out->replied = true;
    if (kind == prod::AdminKind::SubmitOk) {
        out->accepted = true;
        out->term = r.u64();
        out->index = r.u64();
    }
    *done = true;
    co_return;
}

// A client STATUS round-trip: send STATUS, await one reply, decode it.
core::Task status_client(core::INetwork* cli, core::Endpoint admin,
                         prod::AdminStatus* out, bool* ok, bool* done) {
    const std::vector<std::byte> req = prod::encode_status();
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    *ok = prod::decode_status(rep.payload, *out);
    *done = true;
    co_return;
}

// ----- one node incarnation over a data_dir --------------------------------
// Owns a fresh reactor + bus + node over `data_dir`. A SEPARATE struct so an
// incarnation can be fully DESTROYED (closing every fd) before the next is built —
// the honest process-restart simulation (the only durable carrier is the data_dir).
struct Incarnation {
    prod::ProdReactor reactor;
    prod::ProdNetworkBus bus{reactor};
    std::unique_ptr<prod::ProdConsensusNode> node;
    bool built = false;

    explicit Incarnation(const std::string& data_dir, std::uint64_t seed) {
        if (!reactor.valid()) {
            return;
        }
        if (!bus.add_node(kNodeId) || !bus.add_node(kAdminId) ||
            !bus.add_node(kClientId)) {
            return;
        }
        node = std::make_unique<prod::ProdConsensusNode>(
            reactor, bus, kNodeId, kAdminId, data_dir, seed,
            std::vector<std::uint64_t>{kNodeId});  // 1-node cluster
        built = node->valid();
    }

    [[nodiscard]] core::INetwork* client_net() { return bus.node(kClientId); }
    [[nodiscard]] core::Endpoint admin_ep() const {
        return node ? node->admin_endpoint() : core::Endpoint{kAdminId};
    }
};

// Pump the reactor until the node is Leader OR the wall guard trips. A free function
// over a stable pointer (a predicate closure capturing only `node` by value pointer).
bool drive_until_leader(prod::ProdConsensusNode* node) {
    return node->run_until(
        [node] { return node->role() == consensus::Role::Leader; },
        node->reactor().now() + kWallNs);
}

}  // namespace

int main() {
    std::printf("[prod_consensus_test] Phase 7 S5b-1 — REAL Raft ConsensusNode on "
                "PROD providers (1-node), admin protocol over real TCP, DURABLE "
                "crash/restart recovery from ProdDisk\n\n");
    bool all = true;

    // ONE scratch dir survives BOTH incarnations (the durable carrier). Its dtor
    // rmdir()s at the very end; the consensus.wal inside it persists across the
    // process-restart simulation (incarnation A destroyed, B built over the same dir).
    prod::ProdScratchDir scratch("lockstep_consensus");
    if (!scratch.ok()) {
        std::fprintf(stderr, "[prod_consensus_test] FAILED to make scratch dir\n");
        return 1;
    }
    const std::string& data_dir = scratch.path();
    constexpr std::uint64_t kSeed = 0xC0FFEE;

    // The values the client submits (committed before crash; recovered after).
    const std::vector<std::string> values = {"alpha", "bravo", "charlie"};

    // Captured from incarnation A so incarnation B can be cross-checked against it.
    std::vector<consensus::LogEntry> durable_before;
    std::uint64_t term_before = 0;

    // ===================================================================
    // (1) INCARNATION A: elect, SUBMIT over real TCP, persist to ProdDisk.
    // ===================================================================
    {
        auto inc = std::make_unique<Incarnation>(data_dir, kSeed);
        if (!inc->built) {
            std::fprintf(stderr, "[prod_consensus_test] FAILED to build incarnation A\n");
            return 1;
        }
        prod::ProdConsensusNode* node = inc->node.get();
        node->start(kBudget);

        // The 1-node cluster must elect ITSELF Leader (quorum=1) on its real timer.
        const bool became_leader = drive_until_leader(node);
        std::printf("=== (1) ELECT + APPEND + PERSIST (1-node cluster, quorum=1) ===\n");
        std::printf("%s consensus/self-elected-leader (role=%s term=%llu)\n",
                    became_leader ? "PASS" : "FAIL",
                    consensus::role_name(node->role()),
                    static_cast<unsigned long long>(node->term()));
        all = all && became_leader;

        core::INetwork* cli = inc->client_net();
        const core::Endpoint admin = inc->admin_ep();

        // SUBMIT each value over the real admin socket; each must be ACCEPTED at the
        // expected 1-based index, then COMMIT (1-node leader commits immediately).
        bool all_submitted = true;
        for (std::size_t i = 0; i < values.size(); ++i) {
            SubmitOutcome so;
            bool done = false;
            node->reactor().spawn(submit_client(cli, admin, values[i], &so, &done));
            node->run_until([&done] { return done; }, node->reactor().now() + kWallNs);
            const std::uint64_t want_idx = static_cast<std::uint64_t>(i) + 1;
            const bool ok = so.replied && so.accepted && so.index == want_idx;
            std::printf("%s consensus/submit-accepted \"%s\" (idx=%llu want=%llu)\n",
                        ok ? "PASS" : "FAIL", values[i].c_str(),
                        static_cast<unsigned long long>(so.index),
                        static_cast<unsigned long long>(want_idx));
            all_submitted = all_submitted && ok;
        }
        all = all && all_submitted;

        // Pump the reactor so the durable persist worker drains (each appended entry
        // is pwrite+fdatasync'd to consensus.wal). The node's log now holds every
        // submitted entry; they are durable on the real ProdDisk file.
        node->run_until(
            [node, &values] { return node->durable_entries().size() >= values.size(); },
            node->reactor().now() + kWallNs);

        // STATUS over the real socket: the durable LOG digest must hold our values.
        prod::AdminStatus st;
        bool st_ok = false;
        bool st_done = false;
        node->reactor().spawn(status_client(cli, admin, &st, &st_ok, &st_done));
        node->run_until([&st_done] { return st_done; }, node->reactor().now() + kWallNs);

        const bool log_over_socket =
            st_ok && st.committed.size() >= values.size();  // .committed = durable log
        bool values_ok = log_over_socket;
        for (std::size_t i = 0; i < values.size() && values_ok; ++i) {
            values_ok = values_ok && st.committed[i] == values[i];
        }
        std::printf("%s consensus/durable-log-over-status (log_len=%llu role=%u)\n",
                    log_over_socket ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(st.committed.size()),
                    static_cast<unsigned>(st.role));
        std::printf("%s consensus/log-values-match-over-socket\n",
                    values_ok ? "PASS" : "FAIL");
        all = all && log_over_socket && values_ok;

        // N=1 SELF-COMMIT (fixed): the lone leader self-commits its own entries with no
        // peer ack, so commit_index advances to the durable log length. ASSERTED here —
        // the exact deployment that surfaced the original gap now commits.
        const std::uint64_t ci = node->commit_index();
        const bool commit_ok = ci == static_cast<std::uint64_t>(values.size());
        std::printf("%s consensus/N=1-self-commit (commit_index=%llu want=%llu; lone "
                    "leader self-commits, no peer ack)\n",
                    commit_ok ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(ci),
                    static_cast<unsigned long long>(values.size()));
        all = all && commit_ok;

        // Snapshot the durable log + term DIRECTLY from the node (the oracle to
        // cross-check the post-restart recovery against).
        durable_before = node->durable_entries();
        term_before = node->term();
        std::printf("    [incarnation A] durable=%llu entries, term=%llu\n",
                    static_cast<unsigned long long>(durable_before.size()),
                    static_cast<unsigned long long>(term_before));

        // DESTROY incarnation A: closes every fd (reactor epoll, sockets, ProdDisk),
        // dropping ALL in-memory consensus state. Only data_dir/consensus.wal survives
        // — the honest process-exit simulation. The durable consensus log is now the
        // SOLE carrier of the committed entries into incarnation B.
        inc.reset();
        std::printf("    [incarnation A] destroyed (all fds closed; only "
                    "consensus.wal on disk survives)\n\n");
    }

    // ===================================================================
    // (2) INCARNATION B: fresh process over the SAME data_dir; recover from DISK.
    // ===================================================================
    {
        // A BRAND-NEW reactor + bus + ProdDisk over the SAME data_dir. The node's
        // start() calls recover_from_disk(), which reads consensus.wal from offset 0
        // and replays its framed records to rebuild (term, vote, log) — SOURCED FROM
        // THE REAL ProdDisk FILE, not from any surviving memory (there is none).
        auto inc = std::make_unique<Incarnation>(data_dir, kSeed);
        if (!inc->built) {
            std::fprintf(stderr, "[prod_consensus_test] FAILED to build incarnation B\n");
            return 1;
        }
        prod::ProdConsensusNode* node = inc->node.get();
        node->start(kBudget);

        std::printf("=== (2) DURABLE CRASH/RESTART RECOVERY (fresh node, same "
                    "data_dir, replay from ProdDisk) ===\n");

        // Drive until the durable LOG is recovered from disk. start() ->
        // recover_from_disk() reads consensus.wal from offset 0 and replays its framed
        // records to rebuild the log. We pump the reactor until the recovered log
        // reaches the expected length (or the wall guard trips).
        node->run_until(
            [node, &durable_before] {
                return node->durable_entries().size() >= durable_before.size();
            },
            node->reactor().now() + kWallNs);

        // The RECOVERED log (sourced from the ProdDisk file — no memory survived the
        // incarnation-A destruction) must MATCH what incarnation A appended: same
        // values, same terms, same length.
        const std::vector<consensus::LogEntry> recovered = node->durable_entries();

        const bool len_ok = recovered.size() == durable_before.size() &&
                            durable_before.size() == values.size();
        std::printf("%s consensus/recovered-log-length (after=%llu before=%llu)\n",
                    len_ok ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(recovered.size()),
                    static_cast<unsigned long long>(durable_before.size()));
        all = all && len_ok;

        bool entries_ok = len_ok;
        for (std::size_t i = 0; i < recovered.size() && entries_ok; ++i) {
            const bool match = recovered[i].value == durable_before[i].value &&
                               recovered[i].term == durable_before[i].term;
            if (!match) {
                entries_ok = false;
            }
        }
        std::printf("%s consensus/recovered-entries-match-disk (value+term identical)\n",
                    entries_ok ? "PASS" : "FAIL");
        all = all && entries_ok;

        // The recovered values must also equal the ORIGINAL submitted strings (the
        // end-to-end durability proof: what the client put in, disk gave back).
        bool original_ok = recovered.size() == values.size();
        for (std::size_t i = 0; i < recovered.size() && original_ok; ++i) {
            original_ok = original_ok && recovered[i].value == values[i];
        }
        std::printf("%s consensus/recovered-equals-original-submits\n",
                    original_ok ? "PASS" : "FAIL");
        all = all && original_ok;

        // The recovered terms must be NON-ZERO (a real durable Raft term survived; a
        // term-0 log would betray a fabricated/empty recovery rather than a real one).
        bool terms_real = !recovered.empty();
        for (const consensus::LogEntry& e : recovered) {
            if (e.term == 0) {
                terms_real = false;
            }
        }
        std::printf("%s consensus/recovered-terms-nonzero (real durable term, term=%llu)\n",
                    terms_real ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(
                        recovered.empty() ? 0 : recovered[0].term));
        all = all && terms_real;

        // Confirm the recovery is sourced from DISK: read it back over the real admin
        // socket from the FRESH incarnation (no memory could have carried it).
        prod::AdminStatus st;
        bool st_ok = false;
        bool st_done = false;
        node->reactor().spawn(
            status_client(inc->client_net(), inc->admin_ep(), &st, &st_ok, &st_done));
        node->run_until([&st_done] { return st_done; }, node->reactor().now() + kWallNs);
        bool socket_ok = st_ok && st.committed.size() == values.size();
        for (std::size_t i = 0; i < values.size() && socket_ok; ++i) {
            socket_ok = socket_ok && st.committed[i] == values[i];
        }
        std::printf("%s consensus/recovered-readable-over-socket (log_len=%llu)\n",
                    socket_ok ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(st.committed.size()));
        all = all && socket_ok;

        (void)term_before;
        inc.reset();
    }

    std::printf("\n[prod_consensus_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
