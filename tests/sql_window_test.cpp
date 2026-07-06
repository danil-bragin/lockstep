// sql_window_test.cpp — C3 window functions. ROW_NUMBER()/RANK() and SUM/COUNT OVER a partition,
// with PARTITION BY + ORDER BY. Exact per-row values; row + columnar.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, dept TEXT NOT NULL, sal INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,dept,sal) VALUES (1,'A',100),(2,'A',200),(3,'B',150),(4,'A',200)");
    if (columnar) e.flush_columnar("t");
    const std::string T = tag;

    // id, ROW_NUMBER within dept by sal desc, SUM(sal) over dept, COUNT(*) over dept.
    const ExecResult r = e.exec(
        "SELECT id, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY sal DESC) AS rn, "
        "SUM(sal) OVER (PARTITION BY dept) AS tot, COUNT(*) OVER (PARTITION BY dept) AS cnt "
        "FROM t ORDER BY id");
    check(r.ok && r.rows.size() == 4, T + " 4 rows");
    if (r.rows.size() == 4) {
        // expected by id: id1 rn3 tot500 cnt3 ; id2 rn1 tot500 cnt3 ; id3 rn1 tot150 cnt1 ; id4 rn2 tot500 cnt3
        auto row = [&](int i, std::int64_t rn, std::int64_t tot, std::int64_t cnt) {
            const auto& c = r.rows[i].cells;
            check(c[1].second.i == rn, T + " id" + std::to_string(i + 1) + " rn=" + std::to_string(rn));
            check(c[2].second.i == tot, T + " id" + std::to_string(i + 1) + " tot=" + std::to_string(tot));
            check(c[3].second.i == cnt, T + " id" + std::to_string(i + 1) + " cnt=" + std::to_string(cnt));
        };
        row(0, 3, 500, 3);
        row(1, 1, 500, 3);
        row(2, 1, 150, 1);
        row(3, 2, 500, 3);
    }

    // RANK() with a tie (sal 200 appears twice in dept A): ranks 1,1,3 (gap).
    const ExecResult rk = e.exec(
        "SELECT id, RANK() OVER (PARTITION BY dept ORDER BY sal DESC) AS rk FROM t WHERE dept = 'A' ORDER BY id");
    // dept A by sal desc: id2(200),id4(200),id1(100) -> ranks 1,1,3. By id: id1->3, id2->1, id4->1.
    if (rk.ok && rk.rows.size() == 3) {
        check(rk.rows[0].cells[1].second.i == 3, T + " RANK id1 = 3");
        check(rk.rows[1].cells[1].second.i == 1, T + " RANK id2 = 1");
        check(rk.rows[2].cells[1].second.i == 1, T + " RANK id4 = 1 (tie)");
    } else {
        check(false, T + " RANK query");
    }

    // DENSE_RANK() — same tie, no gap: dept A by sal desc -> 1,1,2 (id1->2, id2->1, id4->1).
    const ExecResult dr = e.exec(
        "SELECT id, DENSE_RANK() OVER (PARTITION BY dept ORDER BY sal DESC) AS dr FROM t WHERE dept = 'A' ORDER BY id");
    if (dr.ok && dr.rows.size() == 3) {
        check(dr.rows[0].cells[1].second.i == 2, T + " DENSE_RANK id1 = 2 (no gap)");
        check(dr.rows[1].cells[1].second.i == 1, T + " DENSE_RANK id2 = 1");
        check(dr.rows[2].cells[1].second.i == 1, T + " DENSE_RANK id4 = 1");
    } else {
        check(false, T + " DENSE_RANK query");
    }

    // LAG/LEAD over dept A by id asc (id1,id2,id4 with sal 100,200,200).
    const ExecResult lg = e.exec(
        "SELECT id, LAG(sal) OVER (PARTITION BY dept ORDER BY id) AS pv, "
        "LEAD(sal) OVER (PARTITION BY dept ORDER BY id) AS nv, "
        "AVG(sal) OVER (PARTITION BY dept) AS av FROM t WHERE dept = 'A' ORDER BY id");
    if (lg.ok && lg.rows.size() == 3) {
        // id1: LAG=NULL, LEAD=200, AVG=(100+200+200)/3=166
        check(lg.rows[0].cells[1].second.is_null, T + " LAG id1 = NULL (partition start)");
        check(lg.rows[0].cells[2].second.i == 200, T + " LEAD id1 = 200");
        check(lg.rows[0].cells[3].second.i == 166, T + " AVG dept A = 166 (int trunc)");
        // id2: LAG=100, LEAD=200
        check(lg.rows[1].cells[1].second.i == 100, T + " LAG id2 = 100");
        check(lg.rows[1].cells[2].second.i == 200, T + " LEAD id2 = 200");
        // id4 (last): LEAD=NULL
        check(lg.rows[2].cells[2].second.is_null, T + " LEAD id4 = NULL (partition end)");
    } else {
        check(false, T + " LAG/LEAD/AVG query");
    }

    // FIRST_VALUE / LAST_VALUE / NTILE over dept A by sal asc (id1=100,id2=200,id4=200).
    const ExecResult fv = e.exec(
        "SELECT id, FIRST_VALUE(sal) OVER (PARTITION BY dept ORDER BY sal) AS fv, "
        "LAST_VALUE(sal) OVER (PARTITION BY dept ORDER BY sal) AS lv, "
        "NTILE(2) OVER (PARTITION BY dept ORDER BY sal) AS nt FROM t WHERE dept = 'A' ORDER BY id");
    if (fv.ok && fv.rows.size() == 3) {
        // partition first sal = 100 (id1), last = 200. NTILE(2) over 3 rows -> buckets 1,1,2.
        check(fv.rows[0].cells[1].second.i == 100, T + " FIRST_VALUE dept A = 100");
        check(fv.rows[0].cells[2].second.i == 200, T + " LAST_VALUE dept A = 200");
        // by sal asc: id1(bucket1), then id2/id4(200) — id1 in bucket1; the two 200s in buckets 1,2.
        std::int64_t total_buckets = 0;
        for (const auto& row : fv.rows) total_buckets += row.cells[3].second.i;
        check(total_buckets == 1 + 1 + 2, T + " NTILE(2) buckets sum to 4 (1,1,2)");
    } else {
        check(false, T + " FIRST_VALUE/LAST_VALUE/NTILE query");
    }
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_window_test: FAILED\n"); return 1; }
    std::printf("sql_window_test: OK (ROW_NUMBER/RANK + SUM/COUNT OVER partition)\n");
    return 0;
}
