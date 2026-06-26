// sql_wire_catalog_recover_test.cpp — the wire::Server SQL CATALOG survives a restart.
//
// Validates the durable-SQL-catalog wiring that ProdServerNode/lockstepd use: a wire::Server
// built with SEPARATE durable IDisks for keyed state, SQL DATA, and the SQL CATALOG (schema)
// must, after a "restart" (drop the server, keep the disks, reconstruct + recover_sql), still
// know its tables AND return the rows. Drives SQL through dispatch() (no transport needed; the
// dispatch path runs the SAME SqlEngine the wire serve loop does).
//
// This is the wire/prod analogue of sql_catalog_recover_test (which proves it at the SqlEngine
// level): here the catalog lives on its OWN WAL/Seq line so DDL never shifts the data MVCC
// version, and recover_sql replays BOTH the data and catalog images.
//
// Determinism: seeded SimDisk, virtual time. No clock/thread/rng beyond the seed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/query/wire/Protocol.hpp>
#include <lockstep/query/wire/Server.hpp>
#include <lockstep/query/wire/SqlRows.hpp>

using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
namespace wire = lockstep::query::wire;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

// One durable backing: an own scheduler + SimDisk (its bytes persist across a server restart
// because the SimDisk object outlives the wire::Server constructed over it).
struct Disk {
    Scheduler sched;
    SimClock clock{sched};
    SeededRandom rng;
    SimDisk disk{sched, clock, rng};
    explicit Disk(std::uint64_t seed) : rng(seed) {}
};

// Run one SQL statement through the server's dispatch path (no transport). submit_key makes a
// write idempotent (unused here — each call is unique).
wire::Response run_sql(wire::Server& srv, const std::string& sql, std::uint64_t key) {
    wire::Request req;
    req.kind = wire::MsgKind::SqlExec;
    req.sql = sql;
    req.submit_key = key;
    return srv.dispatch(req);
}
}  // namespace

int main() {
    Disk kd(0xDA7A'0001ULL);  // keyed state (unused by SQL but required by the ctor)
    Disk sd(0x5DA7'0002ULL);  // SQL DATA WAL
    Disk cd(0xCA7A'0003ULL);  // SQL CATALOG WAL (separate Seq line)

    std::size_t data_len = 0;
    std::size_t cat_len = 0;

    // ---- PHASE 1: a durable server runs DDL + INSERTs, then "crashes" (dropped) ----
    {
        wire::Server srv(kd.sched, kd.disk, sd.sched, sd.disk, cd.sched, cd.disk);
        check(run_sql(srv, "CREATE TABLE t (id INT, name TEXT NOT NULL, PRIMARY KEY (id))", 1).sql_ok,
              "phase1 CREATE TABLE ok");
        check(run_sql(srv, "INSERT INTO t (id, name) VALUES (1, 'alpha')", 2).sql_ok,
              "phase1 INSERT 1 ok");
        check(run_sql(srv, "INSERT INTO t (id, name) VALUES (2, 'beta')", 3).sql_ok,
              "phase1 INSERT 2 ok");
        data_len = sd.disk.durable_len();
        cat_len = cd.disk.durable_len();
        check(cat_len > 0, "phase1 catalog WAL has durable bytes (schema persisted)");
    }

    // ---- PHASE 2: reconstruct over the SAME disks; recover; the table is back ----
    wire::Server srv2(kd.sched, kd.disk, sd.sched, sd.disk, cd.sched, cd.disk);
    // TEETH: before recover, a fresh engine does NOT know the table.
    check(!run_sql(srv2, "SELECT id, name FROM t", 10).sql_ok,
          "pre-recover: table unknown (catalog not yet replayed)");

    srv2.recover_sql(data_len, cat_len);

    const wire::Response sel = run_sql(srv2, "SELECT id, name FROM t", 11);
    check(sel.sql_ok, "post-recover SELECT ok (catalog + data recovered)");
    check(sel.sql_affected == 2, "post-recover SELECT sees both rows (affected=" +
                                     std::to_string(sel.sql_affected) + ")");

    // A post-recover INSERT + read-back still works (the recovered catalog is fully live).
    check(run_sql(srv2, "INSERT INTO t (id, name) VALUES (3, 'gamma')", 12).sql_ok,
          "post-recover INSERT ok");
    check(run_sql(srv2, "SELECT id FROM t", 13).sql_affected == 3,
          "post-recover row count is 3 after the new INSERT");

    if (g_fail) {
        std::printf("sql_wire_catalog_recover_test: FAILED\n");
        return 1;
    }
    std::printf("sql_wire_catalog_recover_test: OK (wire SQL catalog + data survive a restart)\n");
    return 0;
}
