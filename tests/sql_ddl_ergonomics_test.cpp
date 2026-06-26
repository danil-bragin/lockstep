// sql_ddl_ergonomics_test.cpp — E2/E3: IF [NOT] EXISTS, TRUNCATE, CREATE TABLE LIKE, CTAS.
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
std::string cell0(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : std::string("<none>");
}
std::int64_t scal(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
}
}  // namespace

int main() {
    // IF NOT EXISTS / IF EXISTS.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE t (id INT PRIMARY KEY)").ok == false, "bare PK inline still rejected");
        check(e.exec("CREATE TABLE t (id INT, PRIMARY KEY (id))").ok, "create t");
        check(!e.exec("CREATE TABLE t (id INT, PRIMARY KEY (id))").ok, "duplicate create errors");
        check(e.exec("CREATE TABLE IF NOT EXISTS t (id INT, PRIMARY KEY (id))").ok,
              "CREATE IF NOT EXISTS is a no-op when present");
        check(e.exec("DROP TABLE IF EXISTS nope").ok, "DROP IF EXISTS missing is a no-op");
        check(!e.exec("DROP TABLE nope").ok, "DROP missing errors without IF EXISTS");
        check(e.exec("DROP TABLE IF EXISTS t").ok, "DROP IF EXISTS present works");
        check(!e.exec("SELECT id FROM t").ok, "t gone");
    }
    // TRUNCATE.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, v INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, v) VALUES (1, 10), (2, 20), (3, 30)");
        check(scal(e.exec("SELECT COUNT(*) FROM t")) == 3, "3 rows before truncate");
        check(e.exec("TRUNCATE TABLE t").ok, "truncate");
        check(scal(e.exec("SELECT COUNT(*) FROM t")) == 0, "0 rows after truncate");
        // schema kept: can insert again.
        check(e.exec("INSERT INTO t (id, v) VALUES (1, 99)").ok, "insert after truncate");
        check(scal(e.exec("SELECT v FROM t WHERE id = 1")) == 99, "new row present");
    }
    // CREATE TABLE LIKE — copy schema (constraints), no data.
    {
        SqlEngine e;
        e.exec("CREATE TABLE src (id INT, name TEXT NOT NULL, age INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO src (id, name, age) VALUES (1, 'a', 20)");
        check(e.exec("CREATE TABLE copy LIKE src").ok, "create like");
        check(scal(e.exec("SELECT COUNT(*) FROM copy")) == 0, "LIKE copies no data");
        // the NOT NULL constraint carried over.
        check(!e.exec("INSERT INTO copy (id) VALUES (5)").ok, "NOT NULL carried by LIKE");
        check(e.exec("INSERT INTO copy (id, name, age) VALUES (5, 'z', 9)").ok, "valid insert into copy");
        check(scal(e.exec("SELECT age FROM copy WHERE id = 5")) == 9, "copy row");
    }
    // CREATE TABLE AS SELECT.
    {
        SqlEngine e;
        e.exec("CREATE TABLE src (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO src (id, k, v) VALUES (1, 1, 100), (2, 1, 200), (3, 2, 300)");
        check(e.exec("CREATE TABLE agg AS SELECT k, SUM(v) AS total FROM src GROUP BY k").ok, "CTAS");
        check(scal(e.exec("SELECT COUNT(*) FROM agg")) == 2, "CTAS -> 2 grouped rows");
        // the synthetic PK is hidden: SELECT * shows only k + total.
        const ExecResult star = e.exec("SELECT * FROM agg ORDER BY k");
        check(star.ok && star.rows.size() == 2 && star.rows[0].cells.size() == 2,
              "SELECT * shows 2 columns (hidden PK)");
        if (star.ok && star.rows.size() == 2) {
            check(star.rows[0].cells[0].second.i == 1 && star.rows[0].cells[1].second.i == 300,
                  "k=1 total=300");
            check(star.rows[1].cells[1].second.i == 300, "k=2 total=300");
        }
        check(cell0(e.exec("SELECT total FROM agg WHERE k = 1")) == "300", "CTAS column queryable");
        // plain CTAS without GROUP BY.
        check(e.exec("CREATE TABLE copy2 AS SELECT v FROM src").ok, "CTAS no group");
        check(scal(e.exec("SELECT COUNT(*) FROM copy2")) == 3, "copy2 has 3 rows");
    }
    // durable: LIKE/CTAS tables survive a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE src (id INT, v INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO src (id, v) VALUES (1, 7), (2, 8)");
            e.exec("CREATE TABLE made AS SELECT v FROM src");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(scal(rec.exec("SELECT COUNT(*) FROM made")) == 2, "recovered CTAS table");
    }
    if (g_fail) { std::printf("sql_ddl_ergonomics_test: FAILED\n"); return 1; }
    std::printf("sql_ddl_ergonomics_test: OK (IF [NOT] EXISTS, TRUNCATE, CREATE LIKE, CTAS, durable)\n");
    return 0;
}
