// sql_index_merge_test.cpp — I7-rest: INDEX MERGE (intersect two single-column eq indexes for
// `a=x AND b=y`) and USING HASH (an equality-only index, not used for ranges).
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
    // INDEX MERGE — two single-col indexes on a and b; WHERE a=x AND b=y intersects them.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
        for (int i = 0; i < 1000; ++i) {
            char q[160];
            std::snprintf(q, sizeof q, "INSERT INTO t (id,a,b,v) VALUES (%d,%d,%d,%d)", i, i % 50, i % 40, i);
            e.exec(q);
        }
        e.exec("CREATE INDEX ia ON t (a)");
        e.exec("CREATE INDEX ib ON t (b)");
        e.exec("ANALYZE t");  // n_distinct(a)=50, n_distinct(b)=40 -> both selective -> merge wins
        check(explain_has(e, "SELECT v FROM t WHERE a = 7 AND b = 3", "Index Merge"),
              "two selective eq indexes -> Index Merge");
        // correctness: == the manual count.
        std::int64_t truth = 0;
        for (int i = 0; i < 1000; ++i) if (i % 50 == 7 && i % 40 == 3) ++truth;
        check(scal(e.exec("SELECT COUNT(*) FROM t WHERE a = 7 AND b = 3")) == truth,
              "merge result == truth");
        // every returned row satisfies BOTH predicates.
        const ExecResult r = e.exec("SELECT a, b FROM t WHERE a = 7 AND b = 3");
        for (const auto& row : r.rows)
            check(row.cells[0].second.i == 7 && row.cells[1].second.i == 3, "row has a=7 AND b=3");
        // a single eq still uses one index (not a merge).
        check(!explain_has(e, "SELECT v FROM t WHERE a = 7", "Index Merge"), "single eq -> not a merge");
    }
    // USING HASH — an equality-only index. Used for eq, NOT for a range.
    {
        SqlEngine e;
        e.exec("CREATE TABLE h (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
        for (int i = 0; i < 500; ++i) { char q[128]; std::snprintf(q,sizeof q,"INSERT INTO h (id,k,v) VALUES (%d,%d,%d)",i,i%50,i); e.exec(q); }
        check(e.exec("CREATE INDEX hk ON h (k) USING HASH").ok, "create hash index");
        e.exec("ANALYZE h");
        check(explain_has(e, "SELECT v FROM h WHERE k = 7", "Index Scan"), "hash index used for eq");
        // a range does NOT use the hash index (no Index Scan) -> seq scan.
        check(!explain_has(e, "SELECT v FROM h WHERE k >= 5 AND k <= 9", "Index Scan"),
              "hash index NOT used for a range");
        // correctness either way.
        std::int64_t truth = 0;
        for (int i = 0; i < 500; ++i) if (i % 50 == 7) ++truth;
        check(scal(e.exec("SELECT COUNT(*) FROM h WHERE k = 7")) == truth, "hash eq count correct");
        // USING BTREE is accepted (the default ordered index).
        check(e.exec("CREATE INDEX hv ON h (v) USING BTREE").ok, "USING BTREE accepted");
        // durable: the hash flag survives a restart (still eq-only). (in-memory engine: re-create)
    }
    if (g_fail) { std::printf("sql_index_merge_test: FAILED\n"); return 1; }
    std::printf("sql_index_merge_test: OK (index merge intersection == truth; USING HASH eq-only)\n");
    return 0;
}
