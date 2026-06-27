// sql_expr_pred_test.cpp — J1: a scalar EXPRESSION may be the left operand of a WHERE/CHECK
// comparison (`a + b = 10`, `doc->>'k' = 'v'`, `UPPER(name) = 'BOB'`), evaluated per row. A bare
// column still parses as a column operand (back-compat). This is the foundation the expression /
// JSON-path index (J2) and array-element GIN (J3) build on: it is what makes such a query writable.
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
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b INT NOT NULL, name TEXT, PRIMARY KEY (id))");
    std::int64_t sum10 = 0, prod_gt = 0;
    for (int i = 0; i < 200; ++i) {
        const int a = i % 13;
        const int b = i % 7;
        char q[200];
        std::snprintf(q, sizeof q,
                      "INSERT INTO t (id,a,b,name) VALUES (%d,%d,%d,'n%d')", i, a, b, a);
        e.exec(q);
        if (a + b == 10) ++sum10;
        if (a * b > 30) ++prod_gt;
    }

    // 1) arithmetic expression LHS: a + b = 10.
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a + b = 10")) == sum10,
          "a + b = 10 matches manual truth");
    // 2) a richer expression: a * b > 30.
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a * b > 30")) == prod_gt,
          "a * b > 30 matches manual truth");
    // 3) a bare column still works (back-compat — must parse as a column operand).
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a = 0")) ==
              scal(e.exec("SELECT COUNT(*) FROM t WHERE a + 0 = 0")),
          "bare column == expression-wrapped column");
    // 4) function expression LHS: UPPER(name).
    check(e.exec("INSERT INTO t (id,a,b,name) VALUES (1000,1,1,'bob')").ok, "insert bob");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE UPPER(name) = 'BOB'")) == 1,
          "UPPER(name) = 'BOB' finds the lowercase row");

    // 5) JSON-path expression LHS — the headline case the expression index will accelerate.
    {
        SqlEngine j;
        j.exec("CREATE TABLE d (id INT, doc JSON NOT NULL, PRIMARY KEY (id))");
        j.exec("INSERT INTO d (id,doc) VALUES (1,'{\"k\":\"red\",\"n\":5}')");
        j.exec("INSERT INTO d (id,doc) VALUES (2,'{\"k\":\"blue\",\"n\":9}')");
        j.exec("INSERT INTO d (id,doc) VALUES (3,'{\"k\":\"red\",\"n\":1}')");
        check(scal(j.exec("SELECT COUNT(*) FROM d WHERE doc->>'k' = 'red'")) == 2,
              "doc->>'k' = 'red' finds 2 rows");
        check(scal(j.exec("SELECT id FROM d WHERE doc->>'k' = 'blue'")) == 2,
              "doc->>'k' = 'blue' returns id 2");
    }

    // 6) expression IS NULL — UPPER of a NULL name is NULL.
    check(e.exec("INSERT INTO t (id,a,b,name) VALUES (1001,1,1,NULL)").ok, "insert null name");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE UPPER(name) IS NULL")) == 1,
          "UPPER(NULL) IS NULL matches the one null-name row");

    // 7) CHECK over an expression (J1 makes the predicate writable here too).
    {
        SqlEngine c;
        check(c.exec("CREATE TABLE g (id INT, lo INT NOT NULL, hi INT NOT NULL, "
                     "CHECK (hi - lo > 0), PRIMARY KEY (id))").ok, "create with expr CHECK");
        check(c.exec("INSERT INTO g (id,lo,hi) VALUES (1,3,9)").ok, "valid row passes expr CHECK");
        check(!c.exec("INSERT INTO g (id,lo,hi) VALUES (2,9,3)").ok, "invalid row fails expr CHECK");
    }

    if (g_fail) { std::printf("sql_expr_pred_test: FAILED\n"); return 1; }
    std::printf("sql_expr_pred_test: OK (expression LHS in WHERE/CHECK: arithmetic, function, "
                "JSON-path, IS NULL; bare column unchanged)\n");
    return 0;
}
