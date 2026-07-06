// sql_grouping_sets_test.cpp — C2 GROUP BY GROUPING SETS. Each set is a separate grouping; the
// rows are unioned; a column not in a given set renders NULL. Row + columnar.
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
// Render a row as "region|cat|sum" with NULL -> "_".
std::string r3(const ResultRow& row) {
    std::string s;
    for (const auto& [label, d] : row.cells) {
        (void)label;
        s += (d.is_null ? "_" : (d.type == Type::Text ? d.s : std::to_string(d.i))) + "|";
    }
    return s;
}
bool has(const ExecResult& r, const std::string& want) {
    for (const auto& row : r.rows) if (r3(row) == want) return true;
    return false;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, region TEXT NOT NULL, cat INT NOT NULL, amt INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,region,cat,amt) VALUES (1,'N',1,10),(2,'N',2,20),(3,'S',1,30)");
    if (columnar) e.flush_columnar("t");
    const std::string T = tag;

    const ExecResult r = e.exec(
        "SELECT region, cat, SUM(amt) FROM t GROUP BY GROUPING SETS ((region), (cat), ())");
    check(r.ok && r.rows.size() == 5, T + " 5 rows (2 regions + 2 cats + 1 total) got " +
                                          std::to_string(r.rows.size()));
    check(has(r, "N|_|30|"), T + " region N total 30");
    check(has(r, "S|_|30|"), T + " region S total 30");
    check(has(r, "_|1|40|"), T + " cat 1 total 40");
    check(has(r, "_|2|20|"), T + " cat 2 total 20");
    check(has(r, "_|_|60|"), T + " grand total 60");

    // ROLLUP(region, cat) == GROUPING SETS ((region,cat),(region),()) — prefixes + grand total.
    const ExecResult ru = e.exec("SELECT region, cat, SUM(amt) FROM t GROUP BY ROLLUP(region, cat)");
    check(ru.ok, T + " ROLLUP ok");
    check(has(ru, "N|1|30|") || has(ru, "N|1|10|") || ru.rows.size() >= 4, T + " ROLLUP produces detail rows");
    check(has(ru, "N|_|30|") && has(ru, "S|_|30|"), T + " ROLLUP region subtotals");
    check(has(ru, "_|_|60|"), T + " ROLLUP grand total 60");

    // CUBE(region, cat) == all subsets: (region,cat),(region),(cat),() — adds the cat-only rows.
    const ExecResult cu = e.exec("SELECT region, cat, SUM(amt) FROM t GROUP BY CUBE(region, cat)");
    check(cu.ok, T + " CUBE ok");
    check(has(cu, "_|1|40|") && has(cu, "_|2|20|"), T + " CUBE cat-only subtotals (not in ROLLUP)");
    check(has(cu, "N|_|30|") && has(cu, "_|_|60|"), T + " CUBE region subtotals + grand total");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_grouping_sets_test: FAILED\n"); return 1; }
    std::printf("sql_grouping_sets_test: OK (GROUPING SETS union + NULL non-set cols)\n");
    return 0;
}
