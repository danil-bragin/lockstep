// sql_alter_test.cpp — F7 ALTER TABLE ADD COLUMN (row mode). Existing rows read the new column as
// NULL (or its DEFAULT) with no data rewrite; new rows set it; the schema change is durable. Teeth:
// adding a NOT NULL column without a DEFAULT to a non-empty table is rejected; columnar is rejected.
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
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, a INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, a) VALUES (1, 10), (2, 20)");
        // ADD a nullable column: existing rows read NULL.
        check(e.exec("ALTER TABLE t ADD COLUMN note TEXT").ok, "ALTER ADD nullable column");
        const ExecResult r1 = e.exec("SELECT note FROM t WHERE id = 1");
        check(r1.ok && !r1.rows.empty() && r1.rows[0].cells[0].second.is_null, "old row note IS NULL");
        // ADD with a DEFAULT: existing rows read the default.
        check(e.exec("ALTER TABLE t ADD COLUMN status INT NOT NULL DEFAULT 5").ok,
              "ALTER ADD NOT NULL with DEFAULT");
        check(e.exec("SELECT status FROM t WHERE id = 1").rows[0].cells[0].second.i == 5,
              "old row status = DEFAULT 5");
        // new INSERT sets the new columns.
        check(e.exec("INSERT INTO t (id, a, note, status) VALUES (3, 30, 'hi', 9)").ok, "insert with new cols");
        check(e.exec("SELECT note FROM t WHERE id = 3").rows[0].cells[0].second.s == "hi", "new row note");
        check(e.exec("SELECT status FROM t WHERE id = 3").rows[0].cells[0].second.i == 9, "new row status");
        // an INSERT omitting the new NOT NULL DEFAULT column uses the default.
        check(e.exec("INSERT INTO t (id, a) VALUES (4, 40)").ok, "insert omitting defaulted col");
        check(e.exec("SELECT status FROM t WHERE id = 4").rows[0].cells[0].second.i == 5, "omitted -> default 5");

        // teeth: NOT NULL without DEFAULT on a non-empty table is rejected; duplicate column rejected.
        check(!e.exec("ALTER TABLE t ADD COLUMN req INT NOT NULL").ok, "NOT NULL w/o DEFAULT rejected");
        check(!e.exec("ALTER TABLE t ADD COLUMN a INT").ok, "duplicate column rejected");
        check(!e.exec("ALTER TABLE nope ADD COLUMN x INT").ok, "unknown table rejected");
    }
    // columnar ALTER rejected (OUT).
    {
        SqlEngine e;
        e.set_columnar_default(true);
        e.exec("CREATE TABLE c (id INT, a INT NOT NULL, PRIMARY KEY (id))");
        check(!e.exec("ALTER TABLE c ADD COLUMN x INT").ok, "columnar ALTER rejected");
    }
    // durable: the added column survives a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, a INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id, a) VALUES (1, 10)");
            e.exec("ALTER TABLE t ADD COLUMN extra INT NOT NULL DEFAULT 7");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(rec.exec("SELECT extra FROM t WHERE id = 1").rows[0].cells[0].second.i == 7,
              "added column + default recovered");
    }
    if (g_fail) { std::printf("sql_alter_test: FAILED\n"); return 1; }
    std::printf("sql_alter_test: OK (ALTER ADD COLUMN: pad old rows, default, durable)\n");
    return 0;
}
