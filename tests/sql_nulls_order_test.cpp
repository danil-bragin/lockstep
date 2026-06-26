// sql_nulls_order_test.cpp — G3 ORDER BY ... NULLS FIRST/LAST gate. Explicit NULL placement, plus
// the DEFAULT (NULL is the smallest value: FIRST under ASC, LAST under DESC) unchanged. Row mode
// (columnar sort path is exercised by the existing aggregate/order tests).
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
// id order of the result of a query.
std::vector<std::int64_t> ids(SqlEngine& e, const std::string& q) {
    std::vector<std::int64_t> out;
    const ExecResult r = e.exec(q);
    for (const auto& row : r.rows) out.push_back(row.cells[0].second.i);
    return out;
}
std::string show(const std::vector<std::int64_t>& v) {
    std::string s;
    for (auto x : v) s += std::to_string(x) + " ";
    return s;
}
void expect(SqlEngine& e, const std::string& q, std::vector<std::int64_t> want) {
    const std::vector<std::int64_t> got = ids(e, q);
    check(got == want, "[" + q + "] got=[" + show(got) + "] want=[" + show(want) + "]");
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, v INT, PRIMARY KEY (id))");  // v NULLABLE
    e.exec("INSERT INTO t (id, v) VALUES (1, 10)");
    e.exec("INSERT INTO t (id) VALUES (2)");      // v = NULL
    e.exec("INSERT INTO t (id, v) VALUES (3, 5)");
    e.exec("INSERT INTO t (id) VALUES (4)");      // v = NULL
    e.exec("INSERT INTO t (id, v) VALUES (5, 20)");

    // explicit NULLS placement
    expect(e, "SELECT id, v FROM t ORDER BY v ASC NULLS LAST",  {3, 1, 5, 2, 4});
    expect(e, "SELECT id, v FROM t ORDER BY v ASC NULLS FIRST", {2, 4, 3, 1, 5});
    expect(e, "SELECT id, v FROM t ORDER BY v DESC NULLS LAST", {5, 1, 3, 2, 4});
    expect(e, "SELECT id, v FROM t ORDER BY v DESC NULLS FIRST",{2, 4, 5, 1, 3});
    // DEFAULT (no NULLS clause) — NULL is smallest: FIRST under ASC, LAST under DESC
    expect(e, "SELECT id, v FROM t ORDER BY v ASC",  {2, 4, 3, 1, 5});
    expect(e, "SELECT id, v FROM t ORDER BY v DESC", {5, 1, 3, 2, 4});

    // teeth: a bad keyword after NULLS errors
    check(!e.exec("SELECT id FROM t ORDER BY v NULLS MIDDLE").ok, "teeth: NULLS MIDDLE rejected");

    if (g_fail) { std::printf("sql_nulls_order_test: FAILED\n"); return 1; }
    std::printf("sql_nulls_order_test: OK (NULLS FIRST/LAST + default placement)\n");
    return 0;
}
