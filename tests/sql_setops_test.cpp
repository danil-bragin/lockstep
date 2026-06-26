// sql_setops_test.cpp — D1 UNION/UNION ALL + D2 INTERSECT/EXCEPT gate. Combine two SELECTs;
// non-ALL deduplicates whole rows; a trailing ORDER BY/LIMIT applies to the combined result;
// arity mismatch errors. Values checked exactly against the set semantics.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string vs(SqlEngine& e, const std::string& q) {
    std::string s;
    const ExecResult r = e.exec(q);
    if (!r.ok) return std::string("ERR:") + r.error;
    for (const auto& row : r.rows) s += std::to_string(row.cells[0].second.i) + ",";
    return s;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t1 (id INT, v INT NOT NULL, PRIMARY KEY (id))");
    e.exec("CREATE TABLE t2 (id INT, v INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t1 (id,v) VALUES (1,10),(2,20),(3,30)");
    e.exec("INSERT INTO t2 (id,v) VALUES (1,30),(2,40)");

    // D1: UNION dedups; UNION ALL keeps dups; ORDER BY applies to the whole.
    check(vs(e, "SELECT v FROM t1 UNION SELECT v FROM t2 ORDER BY v") == "10,20,30,40,",
          "UNION (dedup) ordered = {10,20,30,40}");
    check(vs(e, "SELECT v FROM t1 UNION ALL SELECT v FROM t2 ORDER BY v") == "10,20,30,30,40,",
          "UNION ALL (keep dups) ordered = {10,20,30,30,40}");
    // D2: INTERSECT = common; EXCEPT = left minus right.
    check(vs(e, "SELECT v FROM t1 INTERSECT SELECT v FROM t2 ORDER BY v") == "30,",
          "INTERSECT = {30}");
    check(vs(e, "SELECT v FROM t1 EXCEPT SELECT v FROM t2 ORDER BY v") == "10,20,",
          "EXCEPT = {10,20}");
    // LIMIT on the combined result.
    check(vs(e, "SELECT v FROM t1 UNION SELECT v FROM t2 ORDER BY v LIMIT 2") == "10,20,",
          "UNION ... ORDER BY v LIMIT 2");
    // 3-arm chain (third arm empty).
    check(vs(e, "SELECT v FROM t1 UNION SELECT v FROM t2 UNION SELECT v FROM t2 WHERE v < 0 ORDER BY v")
              == "10,20,30,40,", "3-arm UNION (empty third arm)");

    // teeth: arity mismatch rejected.
    check(!e.exec("SELECT v FROM t1 UNION SELECT id, v FROM t2").ok,
          "arity mismatch rejected");

    if (g_fail) { std::printf("sql_setops_test: FAILED\n"); return 1; }
    std::printf("sql_setops_test: OK (UNION/ALL + INTERSECT + EXCEPT, dedup, ORDER/LIMIT)\n");
    return 0;
}
