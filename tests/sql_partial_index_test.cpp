// sql_partial_index_test.cpp — I5: PARTIAL index. `CREATE INDEX ... WHERE <pred>` indexes only rows
// satisfying the predicate. The planner uses it ONLY when the query's WHERE implies the index
// predicate (so the index covers every matchable row). Results stay byte-identical to a full scan.
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
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, active INT NOT NULL, name INT NOT NULL, PRIMARY KEY (id))");
    // 1000 rows; ~5% active (active=1), the rest active=0. name is the lookup key.
    std::int64_t active_with_name7 = 0, all_with_name7 = 0;
    for (int i = 0; i < 1000; ++i) {
        const int active = (i % 20 == 0) ? 1 : 0;
        const int name = i % 50;
        char q[160];
        std::snprintf(q, sizeof q, "INSERT INTO t (id,active,name) VALUES (%d,%d,%d)", i, active, name);
        e.exec(q);
        if (name == 7) { ++all_with_name7; if (active == 1) ++active_with_name7; }
    }
    // a PARTIAL index on `name` covering only active rows.
    check(e.exec("CREATE INDEX inm ON t (name) WHERE active = 1").ok, "create partial index");
    e.exec("ANALYZE t");

    // a query that implies `active = 1` CAN use the partial index, and is correct.
    check(explain_has(e, "SELECT id FROM t WHERE name = 7 AND active = 1", "Index Scan"),
          "query implying the partial pred uses the index");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE name = 7 AND active = 1")) == active_with_name7,
          "partial-indexed count == truth (active only)");

    // a query that does NOT imply `active = 1` must NOT use the partial index (it would miss the
    // inactive name=7 rows), and is still correct via a full scan.
    check(!explain_has(e, "SELECT id FROM t WHERE name = 7", "Index Scan"),
          "query NOT implying the pred does not use the partial index");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE name = 7")) == all_with_name7,
          "non-implying query still counts ALL name=7 rows");

    // teeth: the partial index really is smaller — a full (non-partial) index would be used for the
    // bare `name = 7`. Add one and confirm it IS used (sanity that the planner can pick an index here).
    e.exec("CREATE INDEX fnm ON t (name)");
    e.exec("ANALYZE t");
    check(explain_has(e, "SELECT id FROM t WHERE name = 7", "Index Scan"),
          "a full index on name IS used for the bare query");
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE name = 7")) == all_with_name7,
          "full-index count still == ALL name=7");

    // updates respect the partial set: flipping active in/out of the predicate maintains the index.
    {
        SqlEngine u;
        u.exec("CREATE TABLE p (id INT, a INT NOT NULL, k INT NOT NULL, PRIMARY KEY (id))");
        u.exec("INSERT INTO p (id,a,k) VALUES (1,1,5),(2,0,5),(3,1,5)");
        u.exec("CREATE INDEX pi ON p (k) WHERE a = 1");
        // active+k=5 rows: id 1 and 3.
        check(scal(u.exec("SELECT COUNT(*) FROM p WHERE k = 5 AND a = 1")) == 2, "2 active k=5");
        u.exec("UPDATE p SET a = 0 WHERE id = 1");  // id1 leaves the partial set
        check(scal(u.exec("SELECT COUNT(*) FROM p WHERE k = 5 AND a = 1")) == 1, "1 active k=5 after update");
        u.exec("UPDATE p SET a = 1 WHERE id = 2");  // id2 enters the partial set
        check(scal(u.exec("SELECT COUNT(*) FROM p WHERE k = 5 AND a = 1")) == 2, "2 active k=5 after re-enter");
    }

    if (g_fail) { std::printf("sql_partial_index_test: FAILED\n"); return 1; }
    std::printf("sql_partial_index_test: OK (partial index: used only when WHERE implies the pred; "
                "correct both ways; maintained on update)\n");
    return 0;
}
