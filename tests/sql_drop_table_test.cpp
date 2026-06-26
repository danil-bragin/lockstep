// sql_drop_table_test.cpp — F8 DROP TABLE gate. After DROP TABLE the name is unknown; a re-CREATE
// of the same name starts EMPTY (not the old rows); the drop SURVIVES a restart (the durable schema
// record is tombstoned). Teeth: DROP of an unknown table errors.
#include <cstdio>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
}  // namespace

int main() {
    // ---- in-memory: drop semantics ----
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, v INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, v) VALUES (1, 100), (2, 200)");
        check(e.exec("SELECT COUNT(*) FROM t").rows[0].cells[0].second.i == 2, "2 rows before drop");

        check(e.exec("DROP TABLE t").ok, "DROP TABLE ok");
        check(!e.exec("SELECT * FROM t").ok, "after DROP: table unknown");
        check(!e.exec("INSERT INTO t (id, v) VALUES (9, 9)").ok, "after DROP: INSERT unknown table");
        check(!e.exec("DROP TABLE t").ok, "teeth: DROP of unknown table errors");

        // re-CREATE same name → EMPTY (old rows gone).
        check(e.exec("CREATE TABLE t (id INT, v INT NOT NULL, PRIMARY KEY (id))").ok, "re-CREATE ok");
        check(e.exec("SELECT COUNT(*) FROM t").rows[0].cells[0].second.i == 0,
              "re-created table is EMPTY (old rows not resurrected)");
    }

    // ---- durable: DROP survives a restart (catalog tombstone recovered) ----
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE keep (id INT, PRIMARY KEY (id))");
            e.exec("CREATE TABLE gone (id INT, PRIMARY KEY (id))");
            e.exec("INSERT INTO keep (id) VALUES (1)");
            e.exec("DROP TABLE gone");
            dl = d.durable_len();
            cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(rec.exec("SELECT * FROM keep").ok, "recover: kept table present");
        check(!rec.exec("SELECT * FROM gone").ok, "recover: DROPped table stays gone");
    }

    if (g_fail) { std::printf("sql_drop_table_test: FAILED\n"); return 1; }
    std::printf("sql_drop_table_test: OK (DROP TABLE + re-create empty + durable)\n");
    return 0;
}
