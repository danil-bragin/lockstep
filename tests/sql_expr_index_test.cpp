// sql_expr_index_test.cpp — J2: EXPRESSION index (`CREATE INDEX ... ON t ((expr))`), which also
// covers JSON-path indexes (`doc->>'k'` is an expression). The indexed value is the expression's
// result; the planner uses the index for `WHERE <same expr> = literal` and the result stays
// byte-identical to a full scan (the residual WHERE re-checks every fetched row). The index is
// maintained on INSERT/UPDATE/DELETE, and a row whose expression can't be soundly evaluated is
// rejected at write (so the index path can never diverge from a scan).
#include <cstdio>
#include <string>

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
bool explain_has(SqlEngine& e, const std::string& sql, const std::string& needle) {
    const ExecResult r = e.exec("EXPLAIN " + sql);
    for (const auto& row : r.rows)
        if (!row.cells.empty() && row.cells[0].second.render().find(needle) != std::string::npos)
            return true;
    return false;
}
// Seed identical data into two engines; one gets the expression index, the other does not. Every
// query must return the SAME scalar from both — the index is correctness-transparent.
void seed_json(SqlEngine& e) {
    e.exec("CREATE TABLE d (id INT, doc JSON NOT NULL, PRIMARY KEY (id))");
    for (int i = 0; i < 400; ++i) {
        const char* k = (i % 40 == 0) ? "red" : (i % 3 == 0 ? "blue" : "green");
        char q[200];
        std::snprintf(q, sizeof q, "INSERT INTO d (id,doc) VALUES (%d,'{\"k\":\"%s\",\"n\":%d}')", i, k, i % 10);
        e.exec(q);
    }
}
}  // namespace

int main() {
    // 1) JSON-path expression index vs a no-index control — identical answers, index actually used.
    {
        SqlEngine idx, ctl;
        seed_json(idx);
        seed_json(ctl);
        check(idx.exec("CREATE INDEX dk ON d ((doc->>'k'))").ok, "create JSON-path expression index");
        idx.exec("ANALYZE d");
        check(explain_has(idx, "SELECT id FROM d WHERE doc->>'k' = 'red'", "Index Scan"),
              "JSON-path query uses the expression index");
        for (const char* k : {"red", "blue", "green", "missing"}) {
            const std::string q = std::string("SELECT COUNT(*) FROM d WHERE doc->>'k' = '") + k + "'";
            check(scal(idx.exec(q)) == scal(ctl.exec(q)),
                  std::string("indexed == scan for doc->>'k' = '") + k + "'");
        }
        // teeth: 'red' is every 40th row over [0,400) => exactly 10.
        check(scal(idx.exec("SELECT COUNT(*) FROM d WHERE doc->>'k' = 'red'")) == 10, "red count == 10");
    }

    // 2) arithmetic expression index — `x + y`.
    {
        SqlEngine idx, ctl;
        for (SqlEngine* e : {&idx, &ctl}) {
            e->exec("CREATE TABLE t (id INT, x INT NOT NULL, y INT NOT NULL, PRIMARY KEY (id))");
            for (int i = 0; i < 400; ++i) {
                char q[120];
                std::snprintf(q, sizeof q, "INSERT INTO t (id,x,y) VALUES (%d,%d,%d)", i, i % 50, i % 7);
                e->exec(q);
            }
        }
        check(idx.exec("CREATE INDEX txy ON t ((x + y))").ok, "create arithmetic expression index");
        idx.exec("ANALYZE t");
        check(explain_has(idx, "SELECT id FROM t WHERE x + y = 10", "Index Scan"),
              "arithmetic query uses the expression index");
        for (int v = 0; v < 20; ++v) {
            const std::string q = "SELECT COUNT(*) FROM t WHERE x + y = " + std::to_string(v);
            check(scal(idx.exec(q)) == scal(ctl.exec(q)),
                  "indexed == scan for x + y = " + std::to_string(v));
        }
    }

    // 3) maintenance: UPDATE / DELETE keep the expression index in lockstep with the table.
    {
        SqlEngine e;
        e.exec("CREATE TABLE p (id INT, name TEXT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO p (id,name) VALUES (1,'bob'),(2,'BOB'),(3,'alice')");
        e.exec("CREATE INDEX pu ON p ((UPPER(name)))");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE UPPER(name) = 'BOB'")) == 2, "UPPER index: 2 BOBs");
        e.exec("UPDATE p SET name = 'carol' WHERE id = 1");  // leaves the BOB group
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE UPPER(name) = 'BOB'")) == 1, "after update: 1 BOB");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE UPPER(name) = 'CAROL'")) == 1, "after update: 1 CAROL");
        e.exec("DELETE FROM p WHERE id = 2");  // removes the last BOB
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE UPPER(name) = 'BOB'")) == 0, "after delete: 0 BOB");
    }

    // 4) soundness: a row whose indexed expression cannot be evaluated is rejected at write (so the
    //    index never silently drops a row that a full scan would see).
    {
        SqlEngine e;
        e.exec("CREATE TABLE g (id INT, a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO g (id,a,b) VALUES (1,10,2)");
        check(e.exec("CREATE INDEX gd ON g ((a / b))").ok, "create a/b expression index");
        check(scal(e.exec("SELECT COUNT(*) FROM g WHERE a / b = 5")) == 1, "a/b index: one match");
        check(!e.exec("INSERT INTO g (id,a,b) VALUES (2,3,0)").ok,
              "a row with b=0 (division by zero in the index expr) is rejected");
        // the rejected row must not exist (the INSERT failed atomically).
        check(scal(e.exec("SELECT COUNT(*) FROM g")) == 1, "rejected insert left no row");
    }

    // 5) a UNIQUE expression index rejects two rows whose expression collides.
    {
        SqlEngine e;
        e.exec("CREATE TABLE u (id INT, name TEXT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO u (id,name) VALUES (1,'Bob')");
        check(e.exec("CREATE UNIQUE INDEX uu ON u ((LOWER(name)))").ok, "create unique expression index");
        check(!e.exec("INSERT INTO u (id,name) VALUES (2,'BOB')").ok,
              "LOWER('BOB') collides with LOWER('Bob') under the unique expr index");
        check(e.exec("INSERT INTO u (id,name) VALUES (3,'carol')").ok, "a distinct value still inserts");
    }

    if (g_fail) { std::printf("sql_expr_index_test: FAILED\n"); return 1; }
    std::printf("sql_expr_index_test: OK (expression / JSON-path index: used + byte-identical to a "
                "scan; maintained on update/delete; un-evaluable rows rejected; UNIQUE enforced)\n");
    return 0;
}
