// sql_distinct_agg_test.cpp — C1 COUNT/SUM/AVG(DISTINCT col) gate. Exact expected values over a
// dataset with duplicates, grouped + ungrouped, in BOTH row and columnar mode. Teeth: DISTINCT
// differs from the plain aggregate when duplicates exist, and COUNT(DISTINCT *) is rejected.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
// Read a single scalar cell (row 0, given col) as int; -999 sentinel if absent.
std::int64_t scalar(const ExecResult& r, std::size_t col = 0) {
    if (!r.ok || r.rows.empty() || r.rows[0].cells.size() <= col) return -999;
    return r.rows[0].cells[col].second.i;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, g INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
    // g=1: v {10,10,20}; g=2: v {5,5,5}
    e.exec("INSERT INTO t (id,g,v) VALUES (1,1,10),(2,1,10),(3,1,20),(4,2,5),(5,2,5),(6,2,5)");
    if (columnar) e.flush_columnar("t");

    // ungrouped: distinct v over all = {10,20,5} -> count 3, sum 35, avg 11 (35/3 trunc)
    check(scalar(e.exec("SELECT COUNT(DISTINCT v) FROM t")) == 3, std::string(tag) + " uCOUNT(DISTINCT)=3");
    check(scalar(e.exec("SELECT SUM(DISTINCT v) FROM t")) == 35, std::string(tag) + " uSUM(DISTINCT)=35");
    check(scalar(e.exec("SELECT AVG(DISTINCT v) FROM t")) == 11, std::string(tag) + " uAVG(DISTINCT)=11");
    // plain (teeth: must differ): COUNT(v)=6, SUM(v)=55
    check(scalar(e.exec("SELECT COUNT(v) FROM t")) == 6, std::string(tag) + " plain COUNT=6 (teeth)");
    check(scalar(e.exec("SELECT SUM(v) FROM t")) == 55, std::string(tag) + " plain SUM=55 (teeth)");

    // grouped: g=1 distinct {10,20} -> cnt2 sum30 avg15 ; g=2 distinct {5} -> cnt1 sum5 avg5
    const ExecResult gq = e.exec(
        "SELECT g, COUNT(DISTINCT v), SUM(DISTINCT v), AVG(DISTINCT v) FROM t GROUP BY g");
    check(gq.ok && gq.rows.size() == 2, std::string(tag) + " grouped 2 rows");
    if (gq.rows.size() == 2) {
        check(gq.rows[0].cells[1].second.i == 2 && gq.rows[0].cells[2].second.i == 30 &&
                  gq.rows[0].cells[3].second.i == 15, std::string(tag) + " g=1 distinct {2,30,15}");
        check(gq.rows[1].cells[1].second.i == 1 && gq.rows[1].cells[2].second.i == 5 &&
                  gq.rows[1].cells[3].second.i == 5, std::string(tag) + " g=2 distinct {1,5,5}");
    }

    // teeth: COUNT(DISTINCT *) rejected at parse.
    check(!e.exec("SELECT COUNT(DISTINCT *) FROM t").ok, std::string(tag) + " COUNT(DISTINCT *) rejected");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_distinct_agg_test: FAILED\n"); return 1; }
    std::printf("sql_distinct_agg_test: OK (COUNT/SUM/AVG DISTINCT, grouped+ungrouped, both modes)\n");
    return 0;
}
