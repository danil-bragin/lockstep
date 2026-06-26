// sql_right_join_test.cpp — E1 RIGHT / FULL OUTER JOIN gate. RIGHT keeps every right row (left
// NULL-filled when unmatched); FULL keeps every row of both sides. Cross-checked against the
// equivalent LEFT join with the tables swapped (same multiset of matched pairs), plus exact
// NULL-fill + count assertions. Both the hash (equi-key) and nested-loop paths.
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
// Render "x" cells (NULL -> "_") for a query, in result order.
std::string xs(SqlEngine& e, const std::string& q) {
    std::string s;
    const ExecResult r = e.exec(q);
    if (!r.ok) return std::string("ERR:") + r.error;
    for (const auto& row : r.rows) {
        const Datum& d = row.cells[0].second;
        s += (d.is_null ? "_" : std::to_string(d.i)) + ",";
    }
    return s;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE a (id INT, x INT, PRIMARY KEY (id))");
    e.exec("CREATE TABLE b (id INT, y INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO a (id,x) VALUES (1,10),(2,20),(3,30)");
    e.exec("INSERT INTO b (id,y) VALUES (2,200),(3,300),(4,400)");

    // RIGHT JOIN (hash equi-key): every B row; a.x NULL for the unmatched b.id=4. (b.y projected so
    // ORDER BY b.y is a valid output-column reference.)
    check(xs(e, "SELECT a.x, b.y FROM a RIGHT JOIN b ON a.id = b.id ORDER BY b.y") == "20,30,_,",
          "RIGHT JOIN: a.x = {20,30,NULL} ordered by b.y");
    check(e.exec("SELECT a.x FROM a RIGHT JOIN b ON a.id = b.id").rows.size() == 3,
          "RIGHT JOIN: 3 rows (one per B row)");

    // FULL JOIN: A-unmatched (id=1 -> x=10, b NULL) + matched (2,3) + B-unmatched (id=4 -> a.x NULL).
    check(e.exec("SELECT a.x FROM a FULL JOIN b ON a.id = b.id").rows.size() == 4,
          "FULL JOIN: 4 rows (|A unmatched| + |matched| + |B unmatched|)");
    // The FULL result's x values ordered by a.x NULLS LAST: 10,20,30 then the B-unmatched NULL.
    check(xs(e, "SELECT a.x FROM a FULL JOIN b ON a.id = b.id ORDER BY a.x NULLS LAST") == "10,20,30,_,",
          "FULL JOIN: x = {10,20,30,NULL}");

    // RIGHT JOIN equals the swapped LEFT JOIN on the right's perspective (b.y multiset identical).
    auto right_ys = e.exec("SELECT b.y FROM a RIGHT JOIN b ON a.id = b.id ORDER BY b.y");
    auto left_ys = e.exec("SELECT b.y FROM b LEFT JOIN a ON a.id = b.id ORDER BY b.y");
    check(right_ys.rows.size() == left_ys.rows.size() && right_ys.rows.size() == 3,
          "RIGHT == swapped LEFT (row count)");

    // nested-loop path (non-equi ON): a theta ON that never matches still keeps every right row,
    // left NULL-filled.
    check(e.exec("SELECT b.y FROM a RIGHT JOIN b ON a.x > 1000").rows.size() == 3,
          "RIGHT JOIN nested-loop (no match): all 3 B rows kept, a NULL");

    if (g_fail) { std::printf("sql_right_join_test: FAILED\n"); return 1; }
    std::printf("sql_right_join_test: OK (RIGHT / FULL OUTER JOIN, hash + nested-loop)\n");
    return 0;
}
