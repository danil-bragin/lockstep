// sql_over_raft_test.cpp — SQL-over-Raft: the HA PostgreSQL story. SQL WRITE statements
// are committed through Raft; because the SqlEngine is DETERMINISTIC (same statements in
// the same order -> byte-identical state), every replica that applies the committed log
// reaches the SAME database — so a PG client on ANY node sees one consistent, replicated
// SQL database that survives node failures with zero data loss.
//
// This proves the core: over a REAL fault-storm Raft cluster (partitions / crashes / lossy
// bus), each live replica applies its committed prefix of SQL DML into an independent
// SqlEngine, and a canonical SELECT returns IDENTICAL results on every replica — the
// state-machine replication invariant, at the SQL layer. It reuses the P3 apply-seam idea
// (ClusterDriver value_fn = the committed value) with value = a SQL statement and apply =
// SqlEngine::exec.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

#include <lockstep/query/sql/Engine.hpp>

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::Index;
using lockstep::consensus::NodeSnapshot;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::run_cluster;
using lockstep::consensus::raft_a::make_raft_a_factory;
using lockstep::query::sql::ExecResult;
using lockstep::query::sql::ResultRow;
using lockstep::query::sql::SqlEngine;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

const char* kSchema = "CREATE TABLE kv (k INT, v TEXT, PRIMARY KEY (k))";

// A DETERMINISTIC SQL-DML workload keyed on the global op id: unique-key INSERTs, plus
// occasional UPDATE / DELETE on earlier keys. Every statement is a pure function of op_id,
// so replaying the same committed sequence yields the same state (the whole point).
std::string sql_workload(std::uint64_t /*client*/, std::uint64_t /*i*/, std::uint64_t op) {
    if (op % 6 == 0 && op > 6) return "DELETE FROM kv WHERE k = " + std::to_string(op - 5);
    if (op % 4 == 0 && op > 4)
        return "UPDATE kv SET v = 'u" + std::to_string(op) + "' WHERE k = " + std::to_string(op - 3);
    return "INSERT INTO kv (k, v) VALUES (" + std::to_string(op) + ", 'v" + std::to_string(op) + "')";
}

// One SqlEngine + its backing (two stores on two schedulers).
struct Node {
    lockstep::core::Scheduler d_sched;
    lockstep::core::SimClock d_clock{d_sched};
    lockstep::sim::SeededRandom d_rng{0xD01};
    lockstep::sim::SimDisk d_disk{d_sched, d_clock, d_rng};
    lockstep::core::Scheduler c_sched;
    lockstep::core::SimClock c_clock{c_sched};
    lockstep::sim::SeededRandom c_rng{0xD02};
    lockstep::sim::SimDisk c_disk{c_sched, c_clock, c_rng};
    SqlEngine engine{d_sched, d_disk, c_sched, c_disk};
};

// The canonical read used to compare replicas — a stable, fully-ordered projection.
std::string canonical_state(SqlEngine& e) {
    const ExecResult r = e.exec("SELECT k, v FROM kv ORDER BY k");
    if (!r.ok) return "ERR:" + r.error;
    std::string s;
    for (const ResultRow& row : r.rows) {
        for (const auto& [name, d] : row.cells) s += name + "=" + d.render() + ",";
        s += ";";
    }
    return s;
}
}  // namespace

int main() {
    int seeds_with_prefix = 0;
    for (std::uint64_t seed = 0x300; seed <= 0x313; ++seed) {
        ClusterConfig cfg;                 // 3 nodes, full fault envelope
        cfg.value_fn = sql_workload;       // commit SQL DML statements through Raft
        const ObservedRun run = run_cluster(seed, make_raft_a_factory(), cfg);
        if (run.snapshots.empty()) continue;
        const auto& fin = run.snapshots.back();

        std::vector<const NodeSnapshot*> live;
        Index common = UINT64_MAX;
        for (const NodeSnapshot& n : fin.nodes) {
            if (!n.live || n.commit_index == 0) continue;
            live.push_back(&n);
            common = std::min(common, n.commit_index);
        }
        if (live.size() < 2 || common == 0 || common == UINT64_MAX) continue;
        ++seeds_with_prefix;

        // Committed prefixes agree (the Raft baseline the SQL layer builds on).
        bool logs_agree = true;
        for (std::size_t k = 1; k < live.size(); ++k)
            for (Index j = 0; j < common; ++j)
                if (live[k]->log[static_cast<std::size_t>(j)].value !=
                    live[0]->log[static_cast<std::size_t>(j)].value)
                    logs_agree = false;
        check(logs_agree, "committed SQL-statement prefixes agree across replicas");

        // Each replica applies the SAME committed prefix into its own SqlEngine.
        std::vector<std::unique_ptr<Node>> nodes;
        std::vector<std::string> states;
        for (const NodeSnapshot* n : live) {
            auto node = std::make_unique<Node>();
            (void)node->engine.exec(kSchema);  // fixed schema (DDL) on every replica
            for (Index j = 0; j < common; ++j)
                (void)node->engine.exec(n->log[static_cast<std::size_t>(j)].value);
            states.push_back(canonical_state(node->engine));
            nodes.push_back(std::move(node));
        }

        // The HA invariant: every replica's SQL database is IDENTICAL.
        bool all_agree = true;
        for (std::size_t k = 1; k < states.size(); ++k)
            if (states[k] != states[0]) all_agree = false;
        check(all_agree, "SQL-over-Raft: every replica's applied SQL state is IDENTICAL");
        check(!states.empty() && !states[0].empty(),
              "the replicated SQL state is non-trivial (rows committed)");
    }
    check(seeds_with_prefix >= 3, "several fault-storm seeds produced a committed SQL prefix");

    if (g_fail) { std::printf("sql_over_raft_test: FAILED\n"); return 1; }
    std::printf("sql_over_raft_test: OK (deterministic SQL DML replicated over fault-storm Raft -> "
                "identical SQL database on every replica; %d seeds)\n",
                seeds_with_prefix);
    return 0;
}
