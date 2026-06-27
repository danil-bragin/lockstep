// sql_analyze_test.cpp — I6: ANALYZE recomputes per-column stats (n_distinct), so the cost model
// estimates eq selectivity as n / n_distinct instead of a fixed 1% guess. Plan CHOICE never affects
// correctness (the indexed==full-scan cross-check guarantees that); this checks ANALYZE runs, the
// estimate improves, and results stay correct.
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
// pull the "est rows=N" from an EXPLAIN of the index-scan line.
std::int64_t index_est(SqlEngine& e, const std::string& sql) {
    const ExecResult r = e.exec("EXPLAIN " + sql);
    for (const auto& row : r.rows) {
        if (row.cells.empty()) continue;
        const std::string s = row.cells[0].second.render();
        const auto pos = s.find("Index Scan");
        if (pos == std::string::npos) continue;
        const auto e2 = s.find("est rows=");
        if (e2 != std::string::npos) return std::strtoll(s.c_str() + e2 + 9, nullptr, 10);
    }
    return -1;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
    // 1000 rows; k has only 4 distinct values -> ~250 rows per value (NOT the 1% default guess of ~10).
    for (int i = 0; i < 1000; ++i) {
        char q[128];
        std::snprintf(q, sizeof q, "INSERT INTO t (id,k,v) VALUES (%d,%d,%d)", i, i % 4, i);
        e.exec(q);
    }
    e.exec("CREATE INDEX ik ON t (k)");

    // before ANALYZE: the fixed 1% guess (~ n/100).
    const std::int64_t before = index_est(e, "SELECT v FROM t WHERE k = 1");
    check(e.exec("ANALYZE t").ok, "ANALYZE runs");
    const std::int64_t after = index_est(e, "SELECT v FROM t WHERE k = 1");
    // n_distinct(k)=4 -> est ~= 1000/4 = 250, much larger (more accurate) than the ~10 default.
    check(after >= 200, "post-ANALYZE est reflects n/n_distinct (~250)");
    check(after > before, "ANALYZE raised the (more accurate) estimate");

    // correctness unchanged regardless of the plan.
    std::int64_t truth = 0;
    for (int i = 0; i < 1000; ++i) if (i % 4 == 1) ++truth;
    check(scal(e.exec("SELECT COUNT(*) FROM t WHERE k = 1")) == truth, "count correct post-ANALYZE");

    // a high-cardinality column: n_distinct == row_count -> est ~= 1 (very selective).
    e.exec("CREATE INDEX iv ON t (v)");
    e.exec("ANALYZE t");
    const std::int64_t v_est = index_est(e, "SELECT k FROM t WHERE v = 5");
    check(v_est >= 1 && v_est <= 5, "unique-ish column est ~= 1");

    // ANALYZE on an unknown table errors.
    check(!e.exec("ANALYZE nope").ok, "ANALYZE unknown table errors");

    if (g_fail) { std::printf("sql_analyze_test: FAILED\n"); return 1; }
    std::printf("sql_analyze_test: OK (ANALYZE -> n_distinct selectivity; estimate improves; results "
                "correct)\n");
    return 0;
}
