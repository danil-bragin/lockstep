// sql_correlated_test.cpp — B2 CORRELATED subqueries. The inner SELECT references an outer column
// and is re-evaluated per outer row (the outer column is substituted with the row's value). Tests
// correlated EXISTS / NOT EXISTS and a correlated scalar subquery.
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
std::vector<std::string> names(SqlEngine& e, const std::string& q) {
    std::vector<std::string> out;
    const ExecResult r = e.exec(q);
    if (!r.ok) { std::printf("  ERR [%s]: %s\n", q.c_str(), r.error.c_str()); return out; }
    for (const auto& row : r.rows) out.push_back(row.cells[0].second.s);
    return out;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE parent (pid INT, name TEXT NOT NULL, PRIMARY KEY (pid))");
    e.exec("CREATE TABLE child (cid INT, parent_ref INT NOT NULL, amt INT NOT NULL, PRIMARY KEY (cid))");
    e.exec("INSERT INTO parent (pid, name) VALUES (1,'a'),(2,'b'),(3,'c')");
    // parent 1 has 2 children, parent 2 has 1, parent 3 has none.
    e.exec("INSERT INTO child (cid, parent_ref, amt) VALUES (1,1,100),(2,1,20),(3,2,200)");

    // correlated EXISTS: parents that have at least one child.
    check((names(e, "SELECT name FROM parent WHERE EXISTS "
                    "(SELECT 1 FROM child WHERE parent_ref = pid) ORDER BY pid")
           == std::vector<std::string>{"a", "b"}),
          "correlated EXISTS -> {a,b}");
    // correlated NOT EXISTS: parents with no children.
    check((names(e, "SELECT name FROM parent WHERE NOT EXISTS "
                    "(SELECT 1 FROM child WHERE parent_ref = pid) ORDER BY pid")
           == std::vector<std::string>{"c"}),
          "correlated NOT EXISTS -> {c}");
    // correlated scalar subquery (RHS form, the supported `col <op> (SELECT ...)`): a parent matches
    // when its pid equals the min parent_ref of its own children; parent 3 (no children -> NULL) drops.
    check((names(e, "SELECT name FROM parent WHERE pid = "
                    "(SELECT MIN(parent_ref) FROM child WHERE parent_ref = pid) ORDER BY pid")
           == std::vector<std::string>{"a", "b"}),
          "correlated scalar subquery (RHS) -> {a,b}");
    // correlated with a further inner filter: parents having a child with amt > 50.
    check((names(e, "SELECT name FROM parent WHERE EXISTS "
                    "(SELECT 1 FROM child WHERE parent_ref = pid AND amt > 50) ORDER BY pid")
           == std::vector<std::string>{"a", "b"}),
          "correlated EXISTS + inner filter -> {a,b}");
    // teeth: an UNcorrelated EXISTS still works (always true here since child is non-empty).
    check(names(e, "SELECT name FROM parent WHERE EXISTS (SELECT 1 FROM child) ORDER BY pid").size() == 3,
          "uncorrelated EXISTS still works");

    if (g_fail) { std::printf("sql_correlated_test: FAILED\n"); return 1; }
    std::printf("sql_correlated_test: OK (correlated EXISTS / NOT EXISTS / scalar)\n");
    return 0;
}
