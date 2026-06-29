// sql_cte_test.cpp — D4 WITH common table expressions. A WITH clause defines one or more NAMED
// subqueries that the engine materializes into ephemeral tables before running the main query; a
// later CTE may reference an earlier one, a CTE may be JOINed/aggregated like any table, and a CTE
// name that clashes with a real table is rejected. Outputs checked exactly.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
// Render column 0 of every row as a comma-joined string (INT or TEXT).
std::string col0(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) return std::string("ERR:") + r.error;
    std::string s;
    for (const auto& row : r.rows) s += row.cells[0].second.render() + ",";
    return s;
}
// Render two columns "a:b," per row.
std::string col2(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) return std::string("ERR:") + r.error;
    std::string s;
    for (const auto& row : r.rows)
        s += row.cells[0].second.render() + ":" + row.cells[1].second.render() + ",";
    return s;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE emp (id INT, dept TEXT NOT NULL, sal INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO emp (id,dept,sal) VALUES "
           "(1,'eng',500),(2,'eng',200),(3,'sales',300),(4,'sales',100),(5,'eng',400)");

    // (1) Basic CTE: filter in the CTE, project from it.
    check(col0(e, "WITH high AS (SELECT id, sal FROM emp WHERE sal > 250) "
                  "SELECT id FROM high ORDER BY id") == "1,3,5,",
          "basic WITH: sal>250 => ids 1,3,5");

    // (2) Aggregating CTE, then read its rolled-up result.
    check(col2(e, "WITH bydept AS (SELECT dept, SUM(sal) AS total FROM emp GROUP BY dept) "
                  "SELECT dept, total FROM bydept ORDER BY dept") == "eng:1100,sales:400,",
          "aggregating WITH: per-dept SUM(sal)");

    // (3) Chained CTEs — the second reads the first.
    check(col0(e, "WITH a AS (SELECT id, sal FROM emp WHERE sal >= 200), "
                  "     b AS (SELECT id FROM a WHERE sal < 450) "
                  "SELECT id FROM b ORDER BY id") == "2,3,5,",
          "chained WITH: a(sal>=200) then b(sal<450) => 2,3,5");

    // (4) CTE joined with a real table.
    check(col0(e, "WITH big AS (SELECT id FROM emp WHERE sal > 300) "
                  "SELECT big.id FROM big JOIN emp ON big.id = emp.id ORDER BY big.id") == "1,5,",
          "WITH joined to a real table: sal>300 => 1,5");

    // (5) Multiple CTEs both used in the main query (a join across two CTEs).
    check(col0(e, "WITH hi AS (SELECT id FROM emp WHERE sal >= 400), "
                  "     eng AS (SELECT id FROM emp WHERE dept = 'eng') "
                  "SELECT hi.id FROM hi JOIN eng ON hi.id = eng.id ORDER BY hi.id") == "1,5,",
          "two CTEs joined: high-sal AND eng => 1,5");

    // (6) A CTE name that clashes with a real table is rejected (no shadowing in this version).
    const ExecResult clash = e.exec("WITH emp AS (SELECT id FROM emp) SELECT id FROM emp");
    check(!clash.ok && clash.error.find("clashes") != std::string::npos,
          "WITH name clashing with a table is rejected");

    // (7) A plain query (no WITH) is unaffected.
    check(col0(e, "SELECT id FROM emp WHERE dept = 'sales' ORDER BY id") == "3,4,",
          "non-WITH query still works");

    if (g_fail) { std::printf("sql_cte_test: FAILED\n"); return 1; }
    std::printf("sql_cte_test: OK (WITH common table expressions)\n");
    return 0;
}
