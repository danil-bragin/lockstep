// sql_recursive_cte_test.cpp — WITH RECURSIVE common table expressions.
//
// A recursive CTE `name AS (<base> UNION [ALL] <recursive>)` seeds from the base, then
// repeatedly runs the recursive term (which reads `name`) against the previous iteration's
// new rows, accumulating until no new rows appear. Classic uses: number series and
// hierarchy / transitive-closure traversal. Deterministic; bounded iteration.
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

namespace {

using lockstep::query::sql::ExecResult;
using lockstep::query::sql::SqlEngine;

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

std::vector<std::int64_t> ints(const ExecResult& r) {
    std::vector<std::int64_t> v;
    for (const auto& row : r.rows)
        if (!row.cells.empty()) v.push_back(row.cells[0].second.i);
    return v;
}

}  // namespace

int main() {
    std::printf("=== sql_recursive_cte_test ===\n");
    SqlEngine e;

    // (A) number series 1..5 via UNION ALL.
    e.exec("CREATE TABLE seed (n INT, PRIMARY KEY (n))");
    e.exec("INSERT INTO seed (n) VALUES (1)");
    {
        const ExecResult r = e.exec(
            "WITH RECURSIVE cnt AS ("
            "  SELECT n FROM seed"
            "  UNION ALL"
            "  SELECT n + 1 FROM cnt WHERE n < 5"
            ") SELECT n FROM cnt ORDER BY n");
        const auto v = ints(r);
        check(r.ok, "(A) recursive series query ok");
        check(v == (std::vector<std::int64_t>{1, 2, 3, 4, 5}), "(A) series = 1..5");
    }

    // (B) hierarchy transitive closure: all descendants of node 1 in a parent->child edge table.
    //   1 -> 2, 1 -> 3, 2 -> 4, 3 -> 5, 4 -> 6.  Descendants of 1: 2,3,4,5,6.
    e.exec("CREATE TABLE edge (id INT, parent INT NOT NULL, child INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO edge (id, parent, child) VALUES (1,1,2),(2,1,3),(3,2,4),(4,3,5),(5,4,6)");
    {
        const ExecResult r = e.exec(
            "WITH RECURSIVE reach AS ("
            "  SELECT child AS node FROM edge WHERE parent = 1"
            "  UNION"
            "  SELECT e.child FROM edge e, reach r WHERE e.parent = r.node"
            ") SELECT node FROM reach ORDER BY node");
        const auto v = ints(r);
        check(r.ok, "(B) recursive hierarchy query ok");
        check(v == (std::vector<std::int64_t>{2, 3, 4, 5, 6}), "(B) descendants of 1 = {2,3,4,5,6}");
    }

    // (C) UNION dedup terminates (a cycle would loop forever without dedup): 1<->2 edge.
    e.exec("CREATE TABLE cyc (id INT, a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO cyc (id, a, b) VALUES (1,1,2),(2,2,1)");
    {
        const ExecResult r = e.exec(
            "WITH RECURSIVE walk AS ("
            "  SELECT b AS node FROM cyc WHERE a = 1"
            "  UNION"
            "  SELECT c.b FROM cyc c, walk w WHERE c.a = w.node"
            ") SELECT node FROM walk ORDER BY node");
        const auto v = ints(r);
        check(r.ok && v == (std::vector<std::int64_t>{1, 2}),
              "(C) UNION dedup terminates a cycle -> {1,2}");
    }

    // (D) an empty base term is a clean error (not a crash).
    check(!e.exec("WITH RECURSIVE x AS (SELECT n FROM seed WHERE n > 999 "
                  "UNION ALL SELECT n+1 FROM x WHERE n < 3) SELECT n FROM x").ok,
          "(D) empty base term errors");

    if (g_fail != 0) { std::printf("sql_recursive_cte_test: FAILURES\n"); return 1; }
    std::printf("sql_recursive_cte_test: ALL PASS\n");
    return 0;
}
