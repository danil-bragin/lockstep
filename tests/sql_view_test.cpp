// sql_view_test.cpp — H1 CREATE VIEW gate. A view is a named, stored SELECT: a FROM reference
// expands to its query (like an inline derived table). Covers: basic projection + a view WHERE,
// an outer filter on the view, a view in a JOIN, an aggregate view, explicit column renames,
// CREATE OR REPLACE, IF NOT EXISTS, nested views (a view over a view), and DROP VIEW. Durability:
// a view survives a restart (its catalog record is recovered). Teeth: unknown-view DROP errors,
// a view/table name clash is refused both ways, and a self-referential (recursive) view errors.
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
// Seed a small `users` table (id, name, region, active) on a fresh engine.
void seed(SqlEngine& e) {
    e.exec("CREATE TABLE users (id INT, name TEXT, region TEXT, active INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO users (id, name, region, active) VALUES "
           "(1, 'alice', 'eu', 1), (2, 'bob', 'us', 0), "
           "(3, 'carol', 'eu', 1), (4, 'dave', 'us', 1)");
}
}  // namespace

int main() {
    // ---- basic view: projection + a WHERE inside the definition ----
    {
        SqlEngine e;
        seed(e);
        check(e.exec("CREATE VIEW active_users AS "
                     "SELECT id, name FROM users WHERE active = 1").ok, "CREATE VIEW ok");
        const ExecResult r = e.exec("SELECT id, name FROM active_users");
        check(r.ok, "SELECT FROM view ok");
        check(r.rows.size() == 3, "view yields the 3 active users");
        // an OUTER filter composes with the view's own WHERE
        const ExecResult r2 = e.exec("SELECT name FROM active_users WHERE id = 3");
        check(r2.ok && r2.rows.size() == 1 && r2.rows[0].cells[0].second.s == "carol",
              "outer WHERE on the view composes with its inner WHERE");
        // COUNT over the view
        check(e.exec("SELECT COUNT(*) FROM active_users").rows[0].cells[0].second.i == 3,
              "COUNT(*) over the view");
    }

    // ---- an aggregate view (GROUP BY inside) ----
    {
        SqlEngine e;
        seed(e);
        check(e.exec("CREATE VIEW region_counts AS "
                     "SELECT region, COUNT(*) AS n FROM users GROUP BY region").ok,
              "CREATE aggregate VIEW ok");
        const ExecResult r = e.exec("SELECT region, n FROM region_counts WHERE region = 'eu'");
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[1].second.i == 2,
              "aggregate view: eu has 2 users");
    }

    // ---- a view in a JOIN against a base table ----
    {
        SqlEngine e;
        seed(e);
        e.exec("CREATE TABLE orders (oid INT, uid INT NOT NULL, amt INT NOT NULL, PRIMARY KEY (oid))");
        e.exec("INSERT INTO orders (oid, uid, amt) VALUES (10, 1, 50), (11, 3, 70), (12, 2, 30)");
        e.exec("CREATE VIEW active_users AS SELECT id, name FROM users WHERE active = 1");
        const ExecResult r = e.exec(
            "SELECT a.name, o.amt FROM active_users a JOIN orders o ON a.id = o.uid ORDER BY o.amt");
        check(r.ok, "JOIN view <-> table ok");
        check(r.rows.size() == 2, "only active users (alice, carol) join to orders");
        check(r.rows[0].cells[0].second.s == "alice" && r.rows[0].cells[1].second.i == 50,
              "first joined row alice/50");
        check(r.rows[1].cells[0].second.s == "carol" && r.rows[1].cells[1].second.i == 70,
              "second joined row carol/70");
    }

    // ---- explicit column renames: CREATE VIEW v (a, b) AS ... ----
    {
        SqlEngine e;
        seed(e);
        check(e.exec("CREATE VIEW u2 (uid, uname) AS SELECT id, name FROM users WHERE active = 1").ok,
              "CREATE VIEW with explicit column list ok");
        const ExecResult r = e.exec("SELECT uid, uname FROM u2 WHERE uid = 1");
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[1].second.s == "alice",
              "renamed columns are addressable");
        // teeth: wrong arity in the column list is rejected at query time
        e.exec("CREATE VIEW bad (only_one) AS SELECT id, name FROM users");
        check(!e.exec("SELECT * FROM bad").ok, "teeth: column-list arity mismatch errors");
    }

    // ---- nested view (a view over a view) ----
    {
        SqlEngine e;
        seed(e);
        e.exec("CREATE VIEW active_users AS SELECT id, name, region FROM users WHERE active = 1");
        check(e.exec("CREATE VIEW active_eu AS "
                     "SELECT id, name FROM active_users WHERE region = 'eu'").ok,
              "CREATE nested VIEW ok");
        const ExecResult r = e.exec("SELECT name FROM active_eu ORDER BY id");
        check(r.ok && r.rows.size() == 2 && r.rows[0].cells[0].second.s == "alice" &&
                  r.rows[1].cells[0].second.s == "carol",
              "nested view resolves through both layers");
    }

    // ---- CREATE OR REPLACE + IF NOT EXISTS ----
    {
        SqlEngine e;
        seed(e);
        e.exec("CREATE VIEW v AS SELECT id FROM users WHERE active = 1");
        check(e.exec("SELECT COUNT(*) FROM v").rows[0].cells[0].second.i == 3, "v = 3 before replace");
        check(e.exec("CREATE OR REPLACE VIEW v AS SELECT id FROM users WHERE active = 0").ok,
              "CREATE OR REPLACE VIEW ok");
        check(e.exec("SELECT COUNT(*) FROM v").rows[0].cells[0].second.i == 1, "v = 1 after replace");
        // plain re-CREATE errors; IF NOT EXISTS is a no-op that keeps the current definition
        check(!e.exec("CREATE VIEW v AS SELECT id FROM users").ok, "teeth: duplicate CREATE VIEW errors");
        check(e.exec("CREATE VIEW IF NOT EXISTS v AS SELECT id FROM users").ok, "IF NOT EXISTS no-op ok");
        check(e.exec("SELECT COUNT(*) FROM v").rows[0].cells[0].second.i == 1,
              "IF NOT EXISTS did NOT overwrite the existing view");
    }

    // ---- DROP VIEW + namespace / recursion teeth ----
    {
        SqlEngine e;
        seed(e);
        e.exec("CREATE VIEW v AS SELECT id FROM users");
        check(e.exec("DROP VIEW v").ok, "DROP VIEW ok");
        check(!e.exec("SELECT * FROM v").ok, "after DROP: view unknown");
        check(!e.exec("DROP VIEW v").ok, "teeth: DROP of unknown view errors");
        check(e.exec("DROP VIEW IF EXISTS v").ok, "DROP VIEW IF EXISTS no-op ok");

        // a view and a table cannot share a name (both directions)
        e.exec("CREATE VIEW users_v AS SELECT id FROM users");
        check(!e.exec("CREATE TABLE users_v (id INT, PRIMARY KEY (id))").ok,
              "teeth: CREATE TABLE over a view name is refused");
        check(!e.exec("CREATE VIEW users AS SELECT id FROM users").ok,
              "teeth: CREATE VIEW over a table name is refused");

        // a self-referential view must error, not loop forever
        e.exec("CREATE VIEW cyc AS SELECT id FROM users");
        e.exec("CREATE OR REPLACE VIEW cyc AS SELECT id FROM cyc");
        check(!e.exec("SELECT * FROM cyc").ok, "teeth: recursive view errors (no infinite loop)");
    }

    // ---- durable: a view survives a restart ----
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE users (id INT, name TEXT, active INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO users (id, name, active) VALUES (1, 'alice', 1), (2, 'bob', 0)");
            e.exec("CREATE VIEW active_users AS SELECT id, name FROM users WHERE active = 1");
            e.exec("CREATE VIEW doomed AS SELECT id FROM users");
            e.exec("DROP VIEW doomed");
            dl = d.durable_len();
            cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        const ExecResult r2 = rec.exec("SELECT name FROM active_users");
        check(r2.ok && r2.rows.size() == 1 && r2.rows[0].cells[0].second.s == "alice",
              "recover: view still resolves after restart");
        check(!rec.exec("SELECT * FROM doomed").ok, "recover: DROPped view stays gone");
    }

    if (g_fail) { std::printf("sql_view_test: FAILED\n"); return 1; }
    std::printf("sql_view_test: OK (views: basic/aggregate/join/rename/nested/replace/drop/durable)\n");
    return 0;
}
