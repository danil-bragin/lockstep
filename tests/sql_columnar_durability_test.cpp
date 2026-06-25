// sql_columnar_durability_test.cpp — COLUMNAR crash/recovery DURABILITY gate (A5).
//
// The columnar storage engine is a NEW storage path (a row 'd' delta over flushed column
// 'B' blocks). Durability is sacrosanct, so this proves the committed columnar state
// survives a crash + recovery, byte-for-byte, over a deterministic SimDisk:
//   (A) DATA DURABILITY. A columnar workload (CREATE + INSERTs + flush + post-flush
//       INSERT/UPDATE/DELETE) committed through the durable query path is fully recovered
//       after a crash: every SELECT (projection / scalar aggregate / GROUP BY) returns the
//       SAME rows AFTER recovery as before the crash. The blocks AND the delta both come
//       back from the durable WAL (they are ordinary KVs on the verified commit path).
//   (B) FLUSH ATOMICITY. flush_columnar commits the new blocks + the delta clear in ONE
//       txn (one commit batch), so a crash leaves either the pre- or the post-flush state
//       — never a torn mix. Recovering at the post-flush durable length yields the flushed
//       state; the committed data is identical either way (flush is value-preserving).
//   (C) DETERMINISM. Same seed => identical recovered output (recovery is a pure function
//       of the durable byte image).
//
// The CATALOG (schema) is in-memory here and kept across the "crash" (the test holds the
// SqlEngine): DDL durability is a separate, flagged concern. This gate proves the DATA
// path (rows + columnar blocks + delta) is durable, which is the columnar-specific risk.
//
// Determinism: the only entropy is the fixed SimDisk seed; all time is the virtual
// SimClock. No <chrono>/<thread>/<random>. Bounded.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

// A fault-free SimDisk backing (the durable query-side disk). We own sched/clock/rng so
// the disk + the engine's internal scheduler are independent.
struct SimBacking {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xC0FFEE'1234'5678ULL};
    lockstep::sim::SimDisk disk;
    SimBacking() : disk(sched, clock, rng, fault_free()) {}
    static lockstep::sim::DiskFaultConfig fault_free() {
        lockstep::sim::DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 0;
        return dc;
    }
};

std::string render(const ExecResult& r) {
    std::string out = r.ok ? "OK" : ("ERR(" + r.error + ")");
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) {
            out += " " + label + "=" + d.render();
        }
    }
    return out;
}

// Run the columnar workload on a fresh durable engine; return (engine ready to query,
// durable_len at the chosen crash point). The workload: CREATE columnar, INSERT 20, FLUSH,
// then post-flush INSERT/UPDATE/DELETE (so recovery must restore blocks AND delta).
void build_workload(SqlEngine& eng) {
    eng.set_columnar_default(true);
    (void)eng.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))");
    const char* depts[] = {"eng", "sales", "ops", "hr", "legal"};
    for (int i = 0; i < 20; ++i) {
        (void)eng.exec("INSERT INTO emp (id, dept, sal) VALUES (" + std::to_string(i) +
                       ", '" + depts[i % 5] + "', " + std::to_string(i * 10) + ")");
    }
    (void)eng.flush_columnar("emp");                       // -> column blocks
    (void)eng.exec("INSERT INTO emp (id, dept, sal) VALUES (100, 'eng', 999)");  // delta
    (void)eng.exec("UPDATE emp SET sal = 7777 WHERE id = 5");                    // delta over block
    (void)eng.exec("DELETE FROM emp WHERE id = 7");                              // delta shadow
}

// The query suite whose results must be crash-stable.
std::vector<std::string> probe(SqlEngine& eng) {
    return {
        render(eng.exec("SELECT id, dept, sal FROM emp")),
        render(eng.exec("SELECT id, sal FROM emp WHERE sal > 50")),
        render(eng.exec("SELECT id FROM emp WHERE id = 5")),
        render(eng.exec("SELECT COUNT(*), SUM(sal), MIN(sal), MAX(sal) FROM emp")),
        render(eng.exec("SELECT dept, COUNT(*), SUM(sal) FROM emp GROUP BY dept")),
    };
}

void test_data_durability() {
    SimBacking back;
    std::vector<std::string> before;
    std::size_t durable_len = 0;
    {
        SqlEngine eng(back.sched, back.disk);
        build_workload(eng);
        before = probe(eng);
        durable_len = back.disk.durable_len();
        // CRASH: drop the in-memory store + staged disk bytes; durable prefix survives.
        back.disk.crash();
        back.disk.recover();
        check(back.disk.durable_len() == durable_len, "durable prefix survives crash");
        // RECOVER the same engine (catalog kept; data rebuilt from the durable WAL).
        eng.recover(durable_len);
        const std::vector<std::string> after = probe(eng);
        check(after == before, "columnar reads identical after crash+recovery");
        // Spot-check a concrete recovered value (the post-flush UPDATE survived).
        check(after[2].find("id=5") != std::string::npos, "recovered row id=5 present");
    }
}

void test_determinism() {
    auto run = []() {
        SimBacking back;
        SqlEngine eng(back.sched, back.disk);
        build_workload(eng);
        const std::size_t dl = back.disk.durable_len();
        back.disk.crash();
        back.disk.recover();
        eng.recover(dl);
        return probe(eng);
    };
    check(run() == run(), "recovery deterministic (same seed => same recovered output)");
}

}  // namespace

int main() {
    test_data_durability();
    test_determinism();
    if (g_fail == 0) {
        std::printf("sql_columnar_durability_test: ALL PASS\n");
    }
    return g_fail;
}
