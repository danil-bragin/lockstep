// sql_orderby_expr_test.cpp — G4 ORDER BY by output POSITION + by computed-column ALIAS.
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
std::vector<std::int64_t> col0(SqlEngine& e, const std::string& q) {
    std::vector<std::int64_t> out;
    const ExecResult r = e.exec(q);
    if (!r.ok) { std::printf("  ERR [%s]: %s\n", q.c_str(), r.error.c_str()); return out; }
    for (const auto& row : r.rows) out.push_back(row.cells[0].second.i);
    return out;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,a,b) VALUES (1,3,30),(2,1,10),(3,2,20)");

    // by position: ORDER BY 1 sorts on the first output column (a).
    check((col0(e, "SELECT a FROM t ORDER BY 1") == std::vector<std::int64_t>{1, 2, 3}),
          "ORDER BY 1 (asc on a)");
    check((col0(e, "SELECT a FROM t ORDER BY 1 DESC") == std::vector<std::int64_t>{3, 2, 1}),
          "ORDER BY 1 DESC");
    // by position on a computed column.
    check((col0(e, "SELECT a + b AS s FROM t ORDER BY 1 DESC") == std::vector<std::int64_t>{33, 22, 11}),
          "ORDER BY 1 on computed column (desc)");
    // by alias.
    check((col0(e, "SELECT a + b AS s FROM t ORDER BY s") == std::vector<std::int64_t>{11, 22, 33}),
          "ORDER BY alias");
    // position 2 references the second output column.
    {
        const ExecResult r = e.exec("SELECT a, b FROM t ORDER BY 2 DESC");
        std::vector<std::int64_t> bs;
        for (const auto& row : r.rows) bs.push_back(row.cells[1].second.i);
        check((bs == std::vector<std::int64_t>{30, 20, 10}), "ORDER BY 2 DESC (on b)");
    }
    // teeth: position 0 / negative rejected.
    check(!e.exec("SELECT a FROM t ORDER BY 0").ok, "ORDER BY 0 rejected");

    if (g_fail) { std::printf("sql_orderby_expr_test: FAILED\n"); return 1; }
    std::printf("sql_orderby_expr_test: OK (ORDER BY position + alias)\n");
    return 0;
}
