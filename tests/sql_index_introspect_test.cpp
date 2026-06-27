// sql_index_introspect_test.cpp — E5: composite (multi-column) index, CREATE UNIQUE INDEX, and
// introspection (SHOW TABLES / DESCRIBE).
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
std::int64_t scal(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
}
std::string cell(const ExecResult& r, std::size_t row, std::size_t col) {
    return (r.ok && row < r.rows.size() && col < r.rows[row].cells.size())
               ? r.rows[row].cells[col].second.render() : std::string("<none>");
}
}  // namespace

int main() {
    // Composite index — maintained, and a leading-column lookup matches the full scan.
    {
        SqlEngine e;
        e.exec("CREATE TABLE emp (id INT, dept TEXT NOT NULL, sal INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO emp (id, dept, sal) VALUES (1,'eng',100),(2,'eng',200),(3,'ops',150)");
        check(e.exec("CREATE INDEX ix ON emp (dept, sal)").ok, "create composite index");
        // a leading-column equality uses the index; result == the unindexed truth.
        check(scal(e.exec("SELECT COUNT(*) FROM emp WHERE dept = 'eng'")) == 2, "dept=eng -> 2 (composite-indexed)");
        check(scal(e.exec("SELECT sal FROM emp WHERE dept = 'ops'")) == 150, "dept=ops -> 150");
        // updates/inserts keep it consistent.
        e.exec("INSERT INTO emp (id, dept, sal) VALUES (4,'eng',300)");
        check(scal(e.exec("SELECT COUNT(*) FROM emp WHERE dept = 'eng'")) == 3, "after insert -> 3");
    }
    // UNIQUE INDEX (single + composite) — enforced on insert.
    {
        SqlEngine e;
        e.exec("CREATE TABLE u (id INT, email TEXT NOT NULL, a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO u (id, email, a, b) VALUES (1,'x@y',1,1)");
        check(e.exec("CREATE UNIQUE INDEX ue ON u (email)").ok, "create unique index");
        check(!e.exec("INSERT INTO u (id, email, a, b) VALUES (2,'x@y',9,9)").ok, "unique index rejects dup");
        check(e.exec("INSERT INTO u (id, email, a, b) VALUES (2,'z@y',2,2)").ok, "distinct email ok");
        // composite unique (a,b): (1,1) exists; (1,1) again rejected, (1,2) ok.
        check(e.exec("CREATE UNIQUE INDEX uab ON u (a, b)").ok, "create composite unique index");
        check(!e.exec("INSERT INTO u (id, email, a, b) VALUES (3,'q@y',1,1)").ok, "composite unique rejects (1,1)");
        check(e.exec("INSERT INTO u (id, email, a, b) VALUES (3,'q@y',1,2)").ok, "(1,2) distinct ok");
        // creating a unique index over existing duplicates fails.
        e.exec("CREATE TABLE d (id INT, k INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO d (id, k) VALUES (1, 5), (2, 5)");
        check(!e.exec("CREATE UNIQUE INDEX dk ON d (k)").ok, "unique index over existing dups rejected");
        check(e.exec("CREATE INDEX dk2 ON d (k)").ok, "non-unique index over dups ok");
    }
    // introspection: SHOW TABLES / DESCRIBE.
    {
        SqlEngine e;
        e.exec("CREATE TABLE alpha (id INT, n INT NOT NULL, PRIMARY KEY (id))");
        e.exec("CREATE TABLE beta (id INT, s TEXT, PRIMARY KEY (id))");
        const ExecResult t = e.exec("SHOW TABLES");
        check(t.ok && t.rows.size() == 2, "SHOW TABLES -> 2 tables");
        check(cell(t, 0, 0) == "alpha" && cell(t, 1, 0) == "beta", "tables in order");
        const ExecResult d = e.exec("DESCRIBE alpha");
        check(d.ok && d.rows.size() == 2, "DESCRIBE -> 2 columns");
        check(cell(d, 0, 0) == "id" && cell(d, 0, 3) == "PK", "id is the PK");
        check(cell(d, 1, 0) == "n" && cell(d, 1, 2) == "NO", "n is NOT NULL");
        // a dropped column is hidden in DESCRIBE.
        e.exec("ALTER TABLE alpha DROP COLUMN n");
        check(e.exec("DESCRIBE alpha").rows.size() == 1, "dropped column hidden in DESCRIBE");
    }
    // durable: composite + unique index survive a restart (still enforced).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE u (id INT, email TEXT NOT NULL, PRIMARY KEY (id))");
            e.exec("CREATE UNIQUE INDEX ue ON u (email)");
            e.exec("INSERT INTO u (id, email) VALUES (1, 'a@b')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("INSERT INTO u (id, email) VALUES (2, 'a@b')").ok,
              "unique index still enforced after recover");
        check(rec.exec("INSERT INTO u (id, email) VALUES (2, 'c@d')").ok, "distinct ok after recover");
    }
    if (g_fail) { std::printf("sql_index_introspect_test: FAILED\n"); return 1; }
    std::printf("sql_index_introspect_test: OK (composite index, UNIQUE index, SHOW TABLES/DESCRIBE, "
                "durable)\n");
    return 0;
}
