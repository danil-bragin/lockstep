// sql_distributed_wire_test.cpp — DISTRIBUTED SQL OVER THE WIRE (B5) gate. The capstone of the
// distributed-SQL milestone: the SAME scatter-gather coordinator (DistributedSql), but each shard
// is a REMOTE SqlEngine reached over the wire (wire::Server <-> ClientStub on SimNetwork), with the
// SELECT rows shipped back in the SqlResult blob and merged. Asserts the distributed-over-wire
// result is BYTE-IDENTICAL to one in-process SqlEngine holding all the rows — proving SQL is no
// longer in-process-only and the coordinator is transport-agnostic.
//
// Determinism: only the seed (inlined SplitMix); virtual SimClock; no clock/thread/rng outside the
// sim. Each shard runs a persistent serve()/pump() on the shared scheduler; the coordinator's
// synchronous exec() drives the scheduler to quiescence per statement.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/query/sql/DistributedSql.hpp>
#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/wire/ClientStub.hpp>
#include <lockstep/query/wire/Server.hpp>
#include <lockstep/query/wire/SqlRows.hpp>

using namespace lockstep::query::sql;
namespace wire = lockstep::query::wire;
using lockstep::core::Endpoint;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimNetworkBus;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

std::string render(const ExecResult& r) {
    std::string out = r.ok ? "OK" : "ERR";
    if (!r.ok) return out + "(" + r.error + ")";
    out += " aff=" + std::to_string(r.affected);
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) out += " " + label + "=" + d.render();
    }
    return out;
}

class SplitMix {
public:
    explicit SplitMix(std::uint64_t s) : s_(s) {}
    std::uint64_t next() {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
private:
    std::uint64_t s_;
};

Task do_sql(wire::ClientStub* stub, std::string sql, wire::CallResult* cr) {
    co_await stub->sql(std::move(sql), *cr);
    co_return;
}

// A shard reached over the (sim) wire: send the SQL, drive the scheduler to get the reply, rebuild
// the ExecResult from {sql_ok, sql_error, sql_affected, deserialized rows blob}.
class WireSqlShard final : public ISqlShard {
public:
    WireSqlShard(Scheduler& sched, wire::ClientStub& stub) : sched_(&sched), stub_(&stub) {}
    ExecResult exec(const std::string& sql) override {
        wire::CallResult cr;
        sched_->spawn(do_sql(stub_, sql, &cr));
        sched_->run();  // drives this client call + the persistent server serve loops
        ExecResult r;
        if (!cr.ok) {
            r.ok = false;
            r.error = "wire: no reply";
            return r;
        }
        r.ok = cr.response.sql_ok;
        r.error = cr.response.sql_error;
        r.affected = cr.response.sql_affected;
        r.rows = wire::deserialize_rows(cr.response.sql_rows_blob);
        return r;
    }
private:
    Scheduler* sched_;
    wire::ClientStub* stub_;
};

}  // namespace

int main() {
    constexpr std::size_t kShards = 3;
    constexpr std::size_t kRows = 600;
    const char* regs[] = {"north", "south", "east", "west", "central"};

    // ---- ORACLE: one in-process engine with all the rows ----
    SqlEngine solo;
    solo.set_columnar_default(true);

    // ---- DISTRIBUTED OVER THE WIRE: M shard servers + the coordinator over SimNetwork ----
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0xB5B5'1234ULL);
    SimNetworkBus bus(sched, rng);
    for (std::size_t i = 0; i < kShards; ++i) {
        bus.add_node(100 + i);  // server endpoint
        bus.add_node(200 + i);  // client endpoint
    }

    // Stable storage for the per-shard nets / servers / stubs (referenced for the run's lifetime).
    std::vector<std::unique_ptr<lockstep::sim::SimNetwork>> nets;
    std::vector<std::unique_ptr<wire::Server>> servers;
    std::vector<std::unique_ptr<wire::ClientStub>> stubs;
    std::vector<std::unique_ptr<WireSqlShard>> wshards;
    std::vector<ISqlShard*> shard_ptrs;
    const int budget = 1'000'000;
    for (std::size_t i = 0; i < kShards; ++i) {
        nets.push_back(std::make_unique<lockstep::sim::SimNetwork>(bus.node(100 + i)));
        nets.push_back(std::make_unique<lockstep::sim::SimNetwork>(bus.node(200 + i)));
        auto& srv_net = *nets[nets.size() - 2];
        auto& cli_net = *nets[nets.size() - 1];
        servers.push_back(std::make_unique<wire::Server>(srv_net));  // in-memory shard engine
        stubs.push_back(
            std::make_unique<wire::ClientStub>(cli_net, clock, Endpoint{100 + i}));
        sched.spawn(servers.back()->serve(budget));
        sched.spawn(stubs.back()->pump(budget));
        wshards.push_back(std::make_unique<WireSqlShard>(sched, *stubs.back()));
        shard_ptrs.push_back(wshards.back().get());
    }
    DistributedSql dist(shard_ptrs);

    // DDL (broadcast) + INSERTs (routed by PK hash) on BOTH the oracle and the wire cluster.
    const char* ddl =
        "CREATE TABLE t (id INT, amount INT NOT NULL, cat INT NOT NULL, region TEXT NOT NULL, "
        "PRIMARY KEY (id))";
    check(solo.exec(ddl).ok, "solo create");
    check(dist.exec(ddl).ok, "dist-over-wire create (broadcast)");
    SplitMix mix(999);
    for (std::size_t i = 0; i < kRows; ++i) {
        const std::int64_t amount = static_cast<std::int64_t>(mix.next() % 1000);
        const std::string sql =
            "INSERT INTO t (id, amount, cat, region) VALUES (" + std::to_string(i) + ", " +
            std::to_string(amount) + ", " + std::to_string(i % 8) + ", '" + regs[i % 5] + "')";
        check(solo.exec(sql).ok, "solo insert");
        check(dist.exec(sql).ok, "dist-over-wire insert (routed)");
    }

    // Distributed-over-wire == single-node oracle, byte-for-byte (rows shipped + merged).
    const std::vector<std::string> queries = {
        "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM t",
        "SELECT cat, COUNT(*), SUM(amount) FROM t GROUP BY cat",
        "SELECT region, COUNT(*), SUM(amount) FROM t GROUP BY region",
        "SELECT COUNT(*), SUM(amount) FROM t WHERE amount > 500",
        "SELECT amount FROM t WHERE id = 321",  // point read -> one shard, rows shipped back
    };
    for (const std::string& q : queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "over-wire != solo for [" + q + "]\n  solo=[" + s + "]\n  wire=[" + d + "]");
    }

    if (g_fail) {
        std::printf("sql_distributed_wire_test: FAILED\n");
        return 1;
    }
    std::printf("sql_distributed_wire_test: OK (distributed SQL OVER THE WIRE == single-node, %zu shards)\n",
                kShards);
    return 0;
}
