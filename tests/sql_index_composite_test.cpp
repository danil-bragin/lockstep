// sql_index_composite_test.cpp — I1: composite PREFIX index lookup. A `WHERE a = x AND b = y` over a
// composite index (a, b) ranges tightly over the (a,b) prefix (not leading-col + residual). The
// indexed result must equal the full-scan truth; EXPLAIN must show the index is chosen.
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
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
    // 400 rows; a in 0..19, b in 0..6 — enough that the index beats a full scan.
    std::int64_t truth_ab = 0, truth_a = 0;
    for (int i = 0; i < 400; ++i) {
        const int a = i % 20, b = i % 7;
        char q[160];
        std::snprintf(q, sizeof q, "INSERT INTO t (id,a,b,v) VALUES (%d,%d,%d,%d)", i, a, b, i);
        e.exec(q);
        if (a == 5 && b == 3) ++truth_ab;
        if (a == 5) ++truth_a;
    }
    e.exec("CREATE INDEX ix ON t (a, b)");

    // (1) the composite index is chosen for the two-column equality.
    check(explain_has(e, "SELECT v FROM t WHERE a = 5 AND b = 3", "Index Scan"),
          "composite (a,b) eq uses the index");
    // (2) correctness: indexed result == the manually counted truth.
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a = 5 AND b = 3")) == truth_ab,
          "WHERE a=5 AND b=3 count matches truth");
    // a leading-column-only equality also uses the index (prefix length 1).
    check(explain_has(e, "SELECT v FROM t WHERE a = 5", "Index Scan"), "leading-col eq uses the index");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a = 5")) == truth_a, "WHERE a=5 count matches truth");
    // (3) the exact rows fetched are correct (every returned row satisfies the predicate).
    const ExecResult rows = e.exec("SELECT id, b, v FROM t WHERE a = 5 AND b = 3");
    check(rows.ok, "rows query ok");
    for (const auto& r : rows.rows) {
        // b is cell 1; it must equal 3 (the index narrowed + residual filtered correctly).
        check(r.cells[1].second.i == 3, "every fetched row has b=3");
    }
    // (4) a partial composite eq + a range on the 2nd col still correct (prefix on a, residual on b).
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a = 5 AND b >= 3")) ==
              [&] { std::int64_t c = 0; for (int i = 0; i < 400; ++i) if (i % 20 == 5 && i % 7 >= 3) ++c; return c; }(),
          "a=5 AND b>=3 matches truth");
    // (5) a 3-column composite index, full + partial prefix.
    e.exec("CREATE TABLE u (id INT, x INT NOT NULL, y INT NOT NULL, z INT NOT NULL, PRIMARY KEY (id))");
    for (int i = 0; i < 300; ++i) {
        char q[160];
        std::snprintf(q, sizeof q, "INSERT INTO u (id,x,y,z) VALUES (%d,%d,%d,%d)", i, i % 10, i % 5, i % 3);
        e.exec(q);
    }
    e.exec("CREATE INDEX ixyz ON u (x, y, z)");
    check(scal(e.exec("SELECT COUNT(*) FROM u WHERE x = 2 AND y = 1 AND z = 0")) ==
              [&] { std::int64_t c = 0; for (int i = 0; i < 300; ++i) if (i % 10 == 2 && i % 5 == 1 && i % 3 == 0) ++c; return c; }(),
          "3-col full prefix matches truth");
    check(scal(e.exec("SELECT COUNT(*) FROM u WHERE x = 2 AND y = 1")) ==
              [&] { std::int64_t c = 0; for (int i = 0; i < 300; ++i) if (i % 10 == 2 && i % 5 == 1) ++c; return c; }(),
          "3-col 2-prefix matches truth");

    if (g_fail) { std::printf("sql_index_composite_test: FAILED\n"); return 1; }
    std::printf("sql_index_composite_test: OK (composite prefix lookup: index chosen, results == "
                "full-scan truth, 2- and 3-column)\n");
    return 0;
}
