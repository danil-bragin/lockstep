// sql_agg_fusion_test.cpp — AGGREGATE FUSION: when a query selects several aggregates of the SAME
// INT column (e.g. SUM(a), MIN(a), MAX(a), AVG(a), COUNT(a)), the columnar engine folds the column
// ONCE per group / per filtered scan instead of once per aggregate. The ONLY correctness contract is
// that the result is byte-identical to the row store; this cross-checks a columnar engine against a
// row-mode control over grouped, filtered, and HAVING shapes with multiple same-column aggregates.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
std::string dump(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) return "ERR:" + r.error;
    std::string s;
    for (const auto& row : r.rows) {
        s += "{";
        for (const auto& c : row.cells) { s += c.second.render(); s += ","; }
        s += "}";
    }
    return s;
}
void same(SqlEngine& col, SqlEngine& row, const std::string& q) {
    const std::string a = dump(col, q), b = dump(row, q);
    if (a != b) {
        std::printf("DIFF: %s\n  columnar=%s\n  row     =%s\n", q.c_str(), a.c_str(), b.c_str());
        g_fail = 1;
    }
}
}  // namespace

int main() {
    SqlEngine col, row;
    col.set_columnar_default(true);
    col.set_vectorize(true);
    for (SqlEngine* e : {&col, &row}) {
        e->exec("CREATE TABLE t (id INT, g INT NOT NULL, a INT NOT NULL, b INT NOT NULL, "
                "av INT, PRIMARY KEY (id))");
        for (int i = 0; i < 4000; ++i) {
            char q[200];
            std::snprintf(q, sizeof q,
                          "INSERT INTO t (id,g,a,b,av) VALUES (%d,%d,%d,%d,%s)", i, i % 7,
                          (i * 31) % 1000 - 500, (i * 17) % 400, (i % 4 == 0) ? "NULL" : std::to_string(i % 90).c_str());
            e->exec(q);
        }
    }
    col.flush_columnar("t");

    const char* qs[] = {
        // multiple aggregates of the SAME column — the fusion case.
        "SELECT SUM(a), MIN(a), MAX(a), AVG(a), COUNT(a) FROM t",
        "SELECT SUM(a), MIN(a), MAX(a) FROM t WHERE a > 0",
        "SELECT SUM(a), MIN(a), MAX(a), AVG(a) FROM t WHERE a >= -100 AND a <= 100",
        "SELECT g, SUM(a), MIN(a), MAX(a), AVG(a) FROM t GROUP BY g",
        "SELECT g, SUM(a), MIN(a), MAX(a) FROM t WHERE b > 100 GROUP BY g",
        "SELECT g, COUNT(*), SUM(a), MIN(a), MAX(a), AVG(a) FROM t GROUP BY g",
        // mixed columns + same-column aggregates together.
        "SELECT g, SUM(a), MAX(a), SUM(b), MIN(b) FROM t GROUP BY g",
        // HAVING over a same-column aggregate.
        "SELECT g, SUM(a), MIN(a) FROM t GROUP BY g HAVING SUM(a) > 0",
        "SELECT g, MAX(a), MIN(a) FROM t GROUP BY g HAVING COUNT(*) > 500",
        // a nullable column's aggregates (NOT fusable — must fall back, still correct).
        "SELECT g, SUM(av), MIN(av), MAX(av), COUNT(av) FROM t GROUP BY g",
        // empty result groups / zero matches (the int-0 empty rule must hold under fusion).
        "SELECT SUM(a), MIN(a), MAX(a) FROM t WHERE a > 100000",
        "SELECT g, SUM(a), MIN(a), MAX(a) FROM t WHERE a > 100000 GROUP BY g",
        // single aggregate (fusion path with one column) still matches.
        "SELECT g, MIN(a) FROM t GROUP BY g",
        "SELECT MAX(a) FROM t WHERE a < 0",
    };
    for (const char* q : qs) same(col, row, q);

    if (g_fail) { std::printf("sql_agg_fusion_test: FAILED\n"); return 1; }
    std::printf("sql_agg_fusion_test: OK (fused multi-aggregate over one column == row store: grouped, "
                "filtered, HAVING, mixed, nullable-fallback, empty groups)\n");
    return 0;
}
