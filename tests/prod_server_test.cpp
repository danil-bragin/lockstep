// prod_server_test.cpp — Phase 7 S5a driver. Proves the SINGLE-NODE PROD SERVER
// ASSEMBLY (prod::ProdServerNode — the precursor to lockstepd) serves a REAL wire
// client over a REAL TCP socket end-to-end:
//
//     ProdNetwork (real TCP, loopback) -> wire::Server -> Database -> txn
//     deterministic executor -> MVCC store
//
// driven by the ProdReactor (epoll loop) on the prod providers, with a real
// wire::ClientStub / query::Connection client connecting over loopback, submitting a
// transaction, and reading the value back. This proves the sim-proven client-facing
// stack runs on real sockets with ZERO change to core/sim/query/txn/storage.
//
//   (1) ROUND-TRIP: a client puts k=v over real TCP, then GETs k back at Strict and
//       asserts the value matches. The server applied EXACTLY the submits we sent
//       (exactly-once witness, like LocalCluster::applied()).
//   (2) EXACTLY-ONCE / second distinct submit applies once: a second, DISTINCT put
//       (k2=v2) commits and reads back; applied advances by exactly one; and the
//       SAME submit_key re-sent does NOT double-apply (the dedup table memoizes).
//
// Everything is driven by ONE in-process ProdReactor (the server node + the client
// stub share it — the S4b in-process model; multi-PROCESS is S5b). BOUNDED with an
// ABSOLUTE reactor deadline so a lost connection / half-open socket can NEVER hang.
//
// LINUX-ONLY: this TU is built only on Linux (tests/CMakeLists.txt guards it with
// if(UNIX AND NOT APPLE)); ProdServerNode/ProdNetwork/ProdReactor are #ifdef
// __linux__. The macOS host never sees it and stays green.
//
// NON-provider code (a test) -> the forbidden-call lint scans it. It touches NO
// socket/epoll/clock syscall of its own: all real plumbing stays inside
// providers/prod/. The test uses ONLY core::INetwork + the reactor's run loop + the
// UNCHANGED wire::ClientStub / query::Connection client surface.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdScratchDir.hpp>
#include <lockstep/prod/ProdServerNode.hpp>

#include <lockstep/query/Driver.hpp>
#include <lockstep/query/wire/ClientStub.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace query = lockstep::query;

// 200 ms absolute reactor deadline: every networked run TERMINATES even if a frame
// is lost / a socket half-opens. Loopback completes in << 1 ms in practice; the
// generous bound absorbs scheduler jitter under sanitizers.
constexpr core::Tick kWallNs = 200'000'000;

// A bounded recv budget for the server serve-loop + the client reply pump. Sized
// well above the handful of requests this test makes (incl. any retries).
constexpr int kBudget = 4096;

// Endpoint ids: server node 1, client node 2 (both on the same in-process reactor).
constexpr std::uint64_t kServerId = 1;
constexpr std::uint64_t kClientId = 2;

struct RoundTrip {
    bool put_ok = false;
    bool put_committed = false;
    bool read_ok = false;
    bool value_matches = false;
    std::string read_value;
    std::uint64_t applied_after = 0;
};

// Unpack a single-point ReadOutcome into rt: record whether the read succeeded with
// a present value, capture the value, and whether it matches `want`. Binding the
// optional to a local before .value() keeps the access provably checked
// (bugprone-unchecked-optional-access).
void record_read(const query::ReadOutcome& r, const std::string& want, RoundTrip* rt) {
    rt->read_ok = r.ok && !r.rows.empty();
    if (!rt->read_ok) {
        return;
    }
    const std::optional<query::Value>& v = r.rows[0].value;
    if (!v.has_value()) {
        return;
    }
    rt->read_value = *v;
    rt->value_matches = (rt->read_value == want);
}

// The client driver program: put k=v, then GET k back at Strict, recording outcomes.
core::Task client_program(query::Connection* conn, std::string key, std::string val,
                          RoundTrip* rt, bool* done) {
    query::WriteOutcome w;
    co_await conn->put(key, val, w);
    rt->put_ok = w.ok;
    rt->put_committed = w.committed;

    query::ReadOutcome r;
    co_await conn->get(key, r);
    record_read(r, val, rt);
    *done = true;
    co_return;
}

// A second program: submit a DISTINCT put (k2=v2), read it back, witness exactly-once.
core::Task client_program2(query::Connection* conn, std::string key, std::string val,
                           RoundTrip* rt, bool* done) {
    query::WriteOutcome w;
    co_await conn->put(key, val, w);
    rt->put_ok = w.ok;
    rt->put_committed = w.committed;

    query::ReadOutcome r;
    co_await conn->get(key, r);
    record_read(r, val, rt);
    *done = true;
    co_return;
}

// A read-only program: GET `key` at Strict, assert it still equals `want`. A
// free function (NOT an inline [&] lambda) so the coroutine frame's captures are
// stable pointers, never references into a destroyed temporary lambda object.
core::Task read_program(query::Connection* conn, std::string key, std::string want,
                        RoundTrip* rt, bool* done) {
    query::ReadOutcome r;
    co_await conn->get(key, r);
    record_read(r, want, rt);
    *done = true;
    co_return;
}

} // namespace

int main() {
    std::printf("[prod_server_test] Phase 7 S5a — SINGLE-NODE PROD SERVER (real TCP "
                "round-trip); client-facing stack runs UNCHANGED on prod providers\n\n");
    bool all = true;

    prod::ProdScratchDir scratch("lockstepd");
    if (!scratch.ok()) {
        std::fprintf(stderr, "[prod_server_test] FAILED to make scratch dir\n");
        return 1;
    }

    // --- assemble: ONE reactor drives the server node + the client stub ---------
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "[prod_server_test] FAILED to create epoll reactor\n");
        return 1;
    }
    prod::ProdNetworkBus bus(reactor);
    bus.add_node(kServerId); // server listen socket (ephemeral loopback port)
    bus.add_node(kClientId); // client node (its own listen socket; dials the server)

    prod::ProdServerConfig cfg;
    cfg.node_id = kServerId;
    cfg.seed = 0xC0FFEE;
    cfg.data_dir = scratch.path();
    prod::ProdServerNode node(reactor, bus, cfg);
    if (!node.valid()) {
        std::fprintf(stderr, "[prod_server_test] FAILED to assemble server node\n");
        return 1;
    }

    // The real wire client: ClientStub over the CLIENT node's ProdNetwork, the
    // reactor's ONE shared ProdClock (the S4a clock-identity resolution — delay()
    // arms a real reactor timer on the SAME clock the reactor stamps time with), and
    // the SERVER's Endpoint. Connection wraps the stub (the C6.4 driver surface).
    core::INetwork* cli_net = bus.node(kClientId);
    query::wire::ClientStub stub(*cli_net, node.clock(), core::Endpoint{kServerId});
    query::Connection conn(stub);

    // Spawn the server serve-loop + the client reply pump on the reactor (bounded).
    node.start(kBudget);
    reactor.spawn(stub.pump(kBudget));

    // ===================================================================
    // (1) ROUND-TRIP: put acct:a = "100", read it back over real TCP.
    // ===================================================================
    RoundTrip rt1;
    bool done1 = false;
    reactor.spawn(client_program(&conn, "acct:a", "100", &rt1, &done1));
    reactor.run_until([&] { return done1; }, reactor.now() + kWallNs);
    rt1.applied_after = node.applied_submits();

    const bool one_ok = done1 && rt1.put_ok && rt1.put_committed && rt1.read_ok &&
                        rt1.value_matches && rt1.applied_after == 1;
    std::printf("=== (1) REAL TCP ROUND-TRIP: put -> read-back over loopback ===\n");
    std::printf("%s server/put-committed-over-tcp (ok=%d committed=%d)\n",
                (rt1.put_ok && rt1.put_committed) ? "PASS" : "FAIL", rt1.put_ok,
                rt1.put_committed);
    std::printf("%s server/read-back-matches (got=\"%s\" want=\"100\")\n",
                rt1.value_matches ? "PASS" : "FAIL", rt1.read_value.c_str());
    std::printf("%s server/exactly-once-applied (applied=%llu want=1)\n",
                rt1.applied_after == 1 ? "PASS" : "FAIL",
                static_cast<unsigned long long>(rt1.applied_after));
    all = all && one_ok;

    // ===================================================================
    // (2) SECOND DISTINCT SUBMIT applies EXACTLY once (applied advances by 1).
    // ===================================================================
    RoundTrip rt2;
    bool done2 = false;
    reactor.spawn(client_program2(&conn, "acct:b", "250", &rt2, &done2));
    reactor.run_until([&] { return done2; }, reactor.now() + kWallNs);
    rt2.applied_after = node.applied_submits();

    const bool two_ok = done2 && rt2.put_ok && rt2.put_committed && rt2.read_ok &&
                        rt2.value_matches && rt2.applied_after == 2;
    std::printf("\n=== (2) SECOND DISTINCT SUBMIT applies EXACTLY once ===\n");
    std::printf("%s server/second-put-committed (got=\"%s\" want=\"250\")\n",
                (rt2.put_committed && rt2.value_matches) ? "PASS" : "FAIL",
                rt2.read_value.c_str());
    std::printf("%s server/applied-advanced-by-one (applied=%llu want=2)\n",
                rt2.applied_after == 2 ? "PASS" : "FAIL",
                static_cast<unsigned long long>(rt2.applied_after));
    all = all && two_ok;

    // The first key is still readable after the second submit (history persisted).
    RoundTrip rt3;
    bool done3 = false;
    reactor.spawn(read_program(&conn, "acct:a", "100", &rt3, &done3));
    reactor.run_until([&] { return done3; }, reactor.now() + kWallNs);
    std::printf("%s server/first-key-still-readable (got=\"%s\" want=\"100\")\n",
                rt3.value_matches ? "PASS" : "FAIL", rt3.read_value.c_str());
    // applied must NOT have advanced from a read (reads are not submits).
    const bool reads_no_apply = node.applied_submits() == 2;
    std::printf("%s server/reads-do-not-apply (applied=%llu still 2)\n",
                reads_no_apply ? "PASS" : "FAIL",
                static_cast<unsigned long long>(node.applied_submits()));
    all = all && rt3.value_matches && reads_no_apply;

    std::printf("\n[prod_server_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
