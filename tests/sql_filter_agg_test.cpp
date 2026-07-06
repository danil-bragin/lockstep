// sql_filter_agg_test.cpp — aggregate FILTER (WHERE ...) clause (conditional aggregation).
//
// agg(x) FILTER (WHERE pred) folds only the group's rows that pass pred. Verified for the
// whole-table case and per-group, in both the row and columnar engines (columnar bails a
// filtered aggregate to the row-AoS fold). Deterministic; no time/threads/random here.

#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t at(const ExecResult& r, std::size_t row, std::size_t col) {
    return (r.ok && row < r.rows.size() && col < r.rows[row].cells.size())
               ? r.rows[row].cells[col].second.i : -999;
}

void run_mode(bool columnar, const std::string& T) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, region TEXT NOT NULL, amt INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,region,amt) VALUES (1,'east',100),(2,'east',200),(3,'west',50),(4,'west',300)");
    if (columnar) e.flush_columnar("t");

    // Whole-table conditional aggregates.
    const ExecResult r = e.exec(
        "SELECT COUNT(*) FILTER (WHERE amt > 100) AS big, "
        "SUM(amt) FILTER (WHERE region = 'east') AS east, "
        "SUM(amt) AS total FROM t");
    check(r.ok && r.rows.size() == 1, T + " whole-table FILTER query ok");
    check(at(r, 0, 0) == 2, T + " COUNT(*) FILTER amt>100 = 2 (rows 200,300)");
    check(at(r, 0, 1) == 300, T + " SUM FILTER region=east = 300 (100+200)");
    check(at(r, 0, 2) == 650, T + " SUM total = 650 (unfiltered)");

    // Per-group FILTER: within each region, SUM of amt>=100 only.
    const ExecResult g = e.exec(
        "SELECT region, SUM(amt) FILTER (WHERE amt >= 100) AS big_sum FROM t GROUP BY region ORDER BY region");
    check(g.ok && g.rows.size() == 2, T + " grouped FILTER query ok");
    // east: 100+200=300 ; west: only 300 (>=100), 50 excluded.
    check(at(g, 0, 1) == 300, T + " east big_sum = 300");
    check(at(g, 1, 1) == 300, T + " west big_sum = 300 (50 filtered out)");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail != 0) { std::printf("sql_filter_agg_test: FAILED\n"); return 1; }
    std::printf("sql_filter_agg_test: ALL PASS (FILTER conditional aggregates, row + columnar)\n");
    return 0;
}
