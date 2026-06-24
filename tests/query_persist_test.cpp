// query_persist_test.cpp — Phase 7 (S5a flag closure). THE QUERY-PATH PERSISTENCE
// + CRASH-RECOVERY GATE.
//
// THE GAP it proves closed (flagged since Phase 7 S5a, see ProdServerNode.hpp):
// the query-visible committed state used to live ONLY in memory — a server restart
// lost it (only the consensus log was durable). The seam now backs the committed
// query state with ONE persistent storage::WalEngine over an INJECTED core::IDisk
// (Database.hpp / wire::Server.hpp), reusing the verified WAL + crash-recovery.
//
// WHAT IT ASSERTS (deterministic, SimDisk so a crash/recovery is byte-stable):
//   (A) DURABLE WRITE + LIVE READ. A committed write-set applied via the durable
//       query path (Database(IDisk&)::apply_committed) is queryable from the LIVE
//       persistent engine (no per-query rebuild).
//   (B) RECOVERY FROM THE QUERY-SIDE DISK. Drop the Database (a "crash"), keep the
//       SimDisk image, reopen a FRESH Database over the SAME IDisk, recover() it,
//       and assert EVERY committed value is queryable AFTER recovery — recovered
//       from the query-side disk, NOT by replaying the consensus log.
//   (C) END-TO-END THROUGH THE WIRE SERVER. The same crash/recover proof driven
//       through wire::Server(IDisk&)::dispatch (Submit -> durable apply; crash;
//       reopen + recover; Query returns the committed values).
//   (D) DETERMINISM. Same seed => identical recovered values (the recovery path is
//       a pure function of the durable byte image).
//
// Determinism (this TU is NOT lint-exempt): the only entropy is the fixed SimDisk
// seed; all time is the virtual SimClock. No <chrono>/<thread>/<random>. Bounded.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/Protocol.hpp>
#include <lockstep/query/wire/Server.hpp>

#include <lockstep/txn/Transaction.hpp>

namespace {

namespace q = lockstep::query;
namespace txn = lockstep::txn;
namespace wire = lockstep::query::wire;

int g_failures = 0;

#define P_CHECK(cond, msg)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "query_persist_test FAIL [%s:%d]: %s\n",   \
                         __FILE__, __LINE__, (msg));                        \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// A fault-free SimDisk used as the injected query-side durability backing. We own
// the scheduler/clock/rng so the disk + the Database's internal engine scheduler
// are independent (the engine drives its own scheduler over this disk).
struct SimBacking {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xABCD'1234'ABCD'1234ULL};
    lockstep::sim::SimDisk disk;
    SimBacking() : disk(sched, clock, rng, fault_free()) {}
    static lockstep::sim::DiskFaultConfig fault_free() {
        lockstep::sim::DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 0;
        return dc;
    }
};

// Read one key at the strict (live tip) level from a Database and return its value.
std::optional<std::string> read_key(q::Database& db, const std::string& key) {
    q::Query<q::Strict> query;
    query.get(key);
    const q::QueryResult r = db.run(query);
    if (r.points.empty()) {
        return std::nullopt;
    }
    return r.points[0].value;
}

// ---------------------------------------------------------------------------
// (A)+(B) Direct durable-path crash/recovery over the Database(IDisk&) seam.
// ---------------------------------------------------------------------------
void test_database_recovery() {
    // The committed history we apply through the durable query path. Each entry is
    // one committed write-set (one commit prefix).
    std::vector<txn::WriteSet> history;
    history.push_back({{"acct:a", "100"}, {"acct:b", "50"}});
    history.push_back({{"acct:a", "90"}, {"acct:b", "60"}});   // a transfer of 10
    history.push_back({{"acct:c", "777"}});                    // a new key later

    SimBacking back;

    // Phase 1: live server writes the committed state durably + reads it live.
    {
        q::Database db(back.sched, back.disk);
        for (const txn::WriteSet& ws : history) {
            (void)db.apply_committed(ws);
        }
        // (A) LIVE read of the durable store reflects the latest committed values.
        P_CHECK(read_key(db, "acct:a") == std::optional<std::string>("90"),
                "live: acct:a should be 90 after the transfer");
        P_CHECK(read_key(db, "acct:b") == std::optional<std::string>("60"),
                "live: acct:b should be 60 after the transfer");
        P_CHECK(read_key(db, "acct:c") == std::optional<std::string>("777"),
                "live: acct:c should be 777");
        P_CHECK(db.tip() == 3, "live tip should be 3 committed write-sets");
    }  // db destroyed — the in-memory Database is GONE (the "crash").

    // Capture the durable WAL length the crash leaves behind, then crash+recover the
    // disk image (drops staged/lying bytes; durable prefix survives — the verified
    // SimDisk crash semantics).
    const std::size_t durable_len = back.disk.durable_len();
    back.disk.crash();
    back.disk.recover();
    P_CHECK(back.disk.durable_len() == durable_len,
            "durable prefix must survive the crash unchanged");

    // Phase 2: a FRESH Database over the SAME IDisk recovers the committed query
    // state from the durable WAL — WITHOUT replaying any consensus log.
    {
        q::Database db2(back.sched, back.disk);
        P_CHECK(db2.tip() == 0, "a freshly reopened DB starts empty before recover");
        db2.recover(durable_len);
        // (B) EVERY committed value is queryable after recovery from the disk.
        // Recovery models the durable history as a SINGLE committed prefix at the
        // live tip (prefix boundaries are not separately persisted), so tip == 1.
        P_CHECK(db2.tip() == 1,
                "recovered tip must be a single non-empty committed prefix");
        P_CHECK(read_key(db2, "acct:a") == std::optional<std::string>("90"),
                "recovered: acct:a should be 90");
        P_CHECK(read_key(db2, "acct:b") == std::optional<std::string>("60"),
                "recovered: acct:b should be 60");
        P_CHECK(read_key(db2, "acct:c") == std::optional<std::string>("777"),
                "recovered: acct:c should be 777");
    }
}

// ---------------------------------------------------------------------------
// (C) End-to-end crash/recovery through the wire::Server(IDisk&) surface.
// ---------------------------------------------------------------------------
wire::Request put_req(std::uint64_t key_tag, const std::string& key,
                      const std::string& value) {
    wire::Request req;
    req.kind = wire::MsgKind::Submit;
    req.op = wire::SubmitOp::Put;
    req.submit_key = key_tag;
    req.req_id = key_tag;
    wire::OpParam p;
    p.key = key;
    p.value = value;
    req.params.push_back(p);
    return req;
}

wire::Request get_req(const std::string& key) {
    wire::Request req;
    req.kind = wire::MsgKind::Query;
    req.level = wire::Level::StrictSerializable;
    wire::Step s;
    s.kind = q::StepKind::Point;
    s.key = key;
    req.steps.push_back(s);
    return req;
}

std::optional<std::string> server_get(wire::Server& srv, const std::string& key) {
    const wire::Response r = srv.dispatch(get_req(key));
    if (r.points.empty() || !r.points[0].present) {
        return std::nullopt;
    }
    return r.points[0].value;
}

void test_server_recovery() {
    SimBacking back;

    // Phase 1: a durable-backed server applies a few Puts via dispatch().
    {
        wire::Server srv(back.sched, back.disk);
        (void)srv.dispatch(put_req(1, "x", "11"));
        (void)srv.dispatch(put_req(2, "y", "22"));
        (void)srv.dispatch(put_req(3, "x", "33"));  // overwrite x
        P_CHECK(server_get(srv, "x") == std::optional<std::string>("33"),
                "live server: x should be 33");
        P_CHECK(server_get(srv, "y") == std::optional<std::string>("22"),
                "live server: y should be 22");
        P_CHECK(srv.applied_submits() == 3, "three distinct submits applied");
    }  // server gone — the crash.

    const std::size_t durable_len = back.disk.durable_len();
    back.disk.crash();
    back.disk.recover();

    // Phase 2: a fresh server over the SAME disk recovers + serves the committed
    // values — proving the query state came back from the query-side disk.
    {
        wire::Server srv2(back.sched, back.disk);
        srv2.recover(durable_len);
        P_CHECK(server_get(srv2, "x") == std::optional<std::string>("33"),
                "recovered server: x should be 33");
        P_CHECK(server_get(srv2, "y") == std::optional<std::string>("22"),
                "recovered server: y should be 22");
    }
}

// ---------------------------------------------------------------------------
// (D) Determinism: two runs of the same recovery produce identical values.
// ---------------------------------------------------------------------------
void test_determinism() {
    auto run_once = []() -> std::vector<std::optional<std::string>> {
        std::vector<txn::WriteSet> history;
        history.push_back({{"k1", "alpha"}});
        history.push_back({{"k2", "beta"}, {"k1", "gamma"}});
        SimBacking back;
        std::size_t durable_len = 0;
        {
            q::Database db(back.sched, back.disk);
            for (const txn::WriteSet& ws : history) {
                (void)db.apply_committed(ws);
            }
            durable_len = back.disk.durable_len();
        }
        back.disk.crash();
        back.disk.recover();
        q::Database db2(back.sched, back.disk);
        db2.recover(durable_len);
        return {read_key(db2, "k1"), read_key(db2, "k2")};
    };
    const auto a = run_once();
    const auto b = run_once();
    P_CHECK(a == b, "recovery must be deterministic (same seed => same values)");
    P_CHECK(a[0] == std::optional<std::string>("gamma"), "k1 recovers to gamma");
    P_CHECK(a[1] == std::optional<std::string>("beta"), "k2 recovers to beta");
}

}  // namespace

int main() {
    test_database_recovery();
    test_server_recovery();
    test_determinism();
    if (g_failures != 0) {
        std::fprintf(stderr, "query_persist_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "query_persist_test: all checks passed\n");
    return 0;
}
