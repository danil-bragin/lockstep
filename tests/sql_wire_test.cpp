// sql_wire_test.cpp — SQL-OVER-THE-WIRE end-to-end gate. The wire protocol carries a SQL
// statement string (MsgKind::SqlExec) to the server's PERSISTENT sql::SqlEngine and ships back
// {ok, error, affected, row-count} (MsgKind::SqlResult). This proves SQL runs THROUGH the wire
// (encode -> SimNetwork -> Server::handle_sql -> SqlEngine -> encode -> back), not just
// in-process: a CREATE/INSERT/SELECT/UPDATE/DELETE session driven over a real ClientStub<->Server
// exchange yields results BYTE-IDENTICAL to the same statements run on a twin in-process SqlEngine
// (the oracle). Plus teeth: a malformed statement returns ok=false + a non-empty error over the
// wire, and a dropped-reply retry applies the statement EXACTLY ONCE (the dedup table).
//
// Determinism: the only entropy is the seed (SeededRandom for net faults). Virtual SimClock; no
// <chrono>/<thread>/<random>. query/ is not lint-exempt — all time is the sim clock.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/wire/ClientStub.hpp>
#include <lockstep/query/wire/Protocol.hpp>
#include <lockstep/query/wire/Server.hpp>
#include <lockstep/query/wire/SqlRows.hpp>

namespace {

using lockstep::core::Endpoint;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimNetworkBus;
namespace wire = lockstep::query::wire;

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

constexpr std::uint64_t kServerEp = 1;
constexpr std::uint64_t kClientEp = 2;

// One statement's wire outcome, rendered for byte-comparison vs the in-process oracle.
std::string render_rows(const std::vector<lockstep::query::sql::ResultRow>& rows) {
    std::string o;
    for (const auto& row : rows) {
        o += "{";
        for (const auto& [label, d] : row.cells) o += label + "=" + d.render() + ",";
        o += "}";
    }
    return o;
}

struct SqlOutcome {
    bool replied = false;
    bool ok = false;
    std::string error;
    std::uint64_t affected = 0;
    std::uint64_t rows = 0;
    std::string row_values;  // the actual SELECT cell values (over the wire: from the blob)
    std::string render() const {
        return std::string(replied ? "R" : "-") + (ok ? "ok" : "ERR") + "|err=" + error +
               "|aff=" + std::to_string(affected) + "|rows=" + std::to_string(rows) +
               "|vals=" + row_values;
    }
};

// The session of SQL statements driven over the wire (and, separately, on the oracle).
const std::vector<std::string>& session() {
    static const std::vector<std::string> s = {
        "CREATE TABLE t (id INT, region TEXT, amount INT NOT NULL, PRIMARY KEY (id))",
        "INSERT INTO t (id, region, amount) VALUES (1, 'north', 100)",
        "INSERT INTO t (id, region, amount) VALUES (2, 'south', 250)",
        "INSERT INTO t (id, region, amount) VALUES (3, 'north', 75)",
        "UPDATE t SET amount = 300 WHERE id = 2",
        "DELETE FROM t WHERE id = 3",
        "SELECT region, COUNT(*), SUM(amount) FROM t GROUP BY region",
        "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM t",
        "SELECT amount FROM t WHERE id = 1",
        "SELECT * FROM nonexistent_table",            // teeth: must ERR over the wire
        "THIS IS NOT SQL",                            // teeth: parse error over the wire
    };
    return s;
}

// Drive the whole session over the wire, recording each statement's reply.
Task sql_driver(wire::ClientStub& cli, std::vector<SqlOutcome>* out) {
    for (const std::string& stmt : session()) {
        wire::CallResult cr;
        co_await cli.sql(stmt, cr);
        SqlOutcome o;
        o.replied = cr.ok;
        if (cr.ok) {
            o.ok = cr.response.sql_ok;
            o.error = cr.response.sql_error;
            o.affected = cr.response.sql_affected;
            o.rows = cr.response.sql_rows;
            o.row_values = render_rows(wire::deserialize_rows(cr.response.sql_rows_blob));
        }
        out->push_back(o);
    }
    co_return;
}

// THE WIRE RUN: a real ClientStub<->Server exchange over SimNetwork (optionally faulty).
std::vector<SqlOutcome> run_wire(std::uint64_t seed,
                                 const lockstep::sim::detail::LinkFaults& faults) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    SimNetworkBus bus(sched, rng);
    bus.add_nodes({kServerEp, kClientEp});
    bus.set_faults(faults);
    auto srv_net = std::make_unique<lockstep::sim::SimNetwork>(bus.node(kServerEp));
    auto cli_net = std::make_unique<lockstep::sim::SimNetwork>(bus.node(kClientEp));
    wire::Server srv(*srv_net);  // persistent SqlEngine inside
    wire::ClientStub cli(*cli_net, clock, Endpoint{kServerEp});

    std::vector<SqlOutcome> out;
    const int budget = (static_cast<int>(session().size()) + 4) * 64;
    sched.spawn(srv.serve(budget));
    sched.spawn(cli.pump(budget));
    sched.spawn(sql_driver(cli, &out));
    sched.run();
    return out;
}

// THE ORACLE: the same statements on a twin in-process SqlEngine (no wire). The wire result must
// equal this for every statement (ok / error / affected / row-count).
std::vector<SqlOutcome> run_oracle() {
    lockstep::query::sql::SqlEngine eng;
    std::vector<SqlOutcome> out;
    for (const std::string& stmt : session()) {
        const lockstep::query::sql::ExecResult er = eng.exec(stmt);
        SqlOutcome o;
        o.replied = true;
        o.ok = er.ok;
        o.error = er.error;
        o.affected = er.affected;
        o.rows = static_cast<std::uint64_t>(er.rows.size());
        o.row_values = render_rows(er.rows);
        out.push_back(o);
    }
    return out;
}

}  // namespace

int main() {
    const std::vector<SqlOutcome> oracle = run_oracle();

    // (A) CLEAN WIRE == ORACLE, byte-for-byte, every statement.
    lockstep::sim::detail::LinkFaults clean;
    const std::vector<SqlOutcome> wire = run_wire(1, clean);
    check(wire.size() == oracle.size(), "wire statement count == oracle");
    for (std::size_t i = 0; i < oracle.size() && i < wire.size(); ++i) {
        check(wire[i].replied, "stmt " + std::to_string(i) + " got a reply");
        check(wire[i].render() == oracle[i].render(),
              "stmt " + std::to_string(i) + " wire=[" + wire[i].render() + "] oracle=[" +
                  oracle[i].render() + "] for: " + session()[i]);
    }

    // Spot-check the semantics actually crossed the wire (not vacuous):
    // groupby (index 6) => 2 rows (north,south after the delete leaves north x1 + south),
    // the scalar agg (index 7) => 1 row, the point SELECT (index 8) => 1 row, and BOTH teeth
    // statements (9,10) => ok=false with a non-empty error.
    if (wire.size() == oracle.size() && wire.size() == session().size()) {
        check(wire[6].ok && wire[6].rows == 2, "GROUP BY over wire => ok, 2 region rows");
        check(wire[7].ok && wire[7].rows == 1, "scalar agg over wire => ok, 1 row");
        check(wire[8].ok && wire[8].rows == 1, "point SELECT over wire => ok, 1 row");
        check(!wire[9].ok && !wire[9].error.empty(), "unknown table => ERR + error over wire");
        check(!wire[10].ok && !wire[10].error.empty(), "garbage SQL => ERR + error over wire");
    }

    // (B) EXACTLY-ONCE UNDER FAULTS: with dup/drop/reorder turned up, the persistent engine must
    // not double-apply an INSERT/UPDATE/DELETE (the dedup table dedups a retried SqlExec). Compare
    // the faulty wire run's outcomes to the oracle — identical => no statement applied twice.
    lockstep::sim::detail::LinkFaults faulty;
    faulty.dup_prob = 0.4;
    faulty.drop_prob = 0.25;
    faulty.reorder_prob = 0.3;
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        const std::vector<SqlOutcome> fw = run_wire(seed, faulty);
        check(fw.size() == oracle.size(), "faulty wire count == oracle (seed " + std::to_string(seed) + ")");
        for (std::size_t i = 0; i < oracle.size() && i < fw.size(); ++i) {
            check(fw[i].replied, "faulty stmt " + std::to_string(i) + " replied (seed " +
                                     std::to_string(seed) + ")");
            check(fw[i].render() == oracle[i].render(),
                  "faulty seed " + std::to_string(seed) + " stmt " + std::to_string(i) +
                      " wire=[" + fw[i].render() + "] oracle=[" + oracle[i].render() + "]");
        }
    }

    if (g_fail) {
        std::printf("sql_wire_test: FAILED\n");
        return 1;
    }
    std::printf("sql_wire_test: OK (SQL over the wire == in-process oracle; exactly-once under faults)\n");
    return 0;
}
