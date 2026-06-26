// sql_composite_pk_test.cpp — F1 composite PRIMARY KEY (all-INT, row mode). The key is the PK
// columns' order-preserving INT encodings concatenated; rows sort by the PK tuple; the full tuple is
// the uniqueness/identity. PK columns round-trip from the key. Durable. Teeth: TEXT/columnar/dup.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t scalar(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -1;
}
}  // namespace

int main() {
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE t (a INT, b INT, v INT NOT NULL, PRIMARY KEY (a, b))").ok,
              "create composite PK");
        check(e.exec("INSERT INTO t (a, b, v) VALUES (2,1,30),(1,2,20),(1,1,10)").ok, "insert 3 rows");
        // rows come back in composite-key order: (1,1),(1,2),(2,1).
        const ExecResult all = e.exec("SELECT a, b, v FROM t");
        check(all.ok && all.rows.size() == 3, "3 rows");
        if (all.rows.size() == 3) {
            auto eq = [&](int i, std::int64_t a, std::int64_t b, std::int64_t v) {
                const auto& c = all.rows[i].cells;
                check(c[0].second.i == a && c[1].second.i == b && c[2].second.i == v,
                      "row " + std::to_string(i) + " = (" + std::to_string(a) + "," + std::to_string(b) +
                          "," + std::to_string(v) + ")");
            };
            eq(0, 1, 1, 10);
            eq(1, 1, 2, 20);
            eq(2, 2, 1, 30);
        }
        // composite uniqueness: same a, different b is allowed; an exact dup is rejected.
        check(e.exec("INSERT INTO t (a, b, v) VALUES (1, 3, 99)").ok, "(1,3) distinct from (1,1)/(1,2)");
        check(!e.exec("INSERT INTO t (a, b, v) VALUES (1, 1, 99)").ok, "exact composite dup rejected");
        // a partial filter (WHERE a = 1) full-scans + filters: 3 rows now (1,1),(1,2),(1,3).
        check(scalar(e.exec("SELECT COUNT(*) FROM t WHERE a = 1")) == 3, "WHERE a=1 -> 3 rows");
        check(scalar(e.exec("SELECT v FROM t WHERE a = 1 AND b = 2")) == 20, "(1,2) -> v=20");

        // teeth: a composite PK with a TEXT column, and a columnar composite, are rejected.
        check(!e.exec("CREATE TABLE w (a INT, b TEXT, PRIMARY KEY (a, b))").ok, "composite TEXT rejected");
    }
    // columnar composite rejected.
    {
        SqlEngine e;
        e.set_columnar_default(true);
        check(!e.exec("CREATE TABLE c (a INT, b INT, PRIMARY KEY (a, b))").ok, "columnar composite rejected");
    }
    // durable: composite PK survives a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (a INT, b INT, v INT NOT NULL, PRIMARY KEY (a, b))");
            e.exec("INSERT INTO t (a, b, v) VALUES (5, 6, 70)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(scalar(rec.exec("SELECT v FROM t WHERE a = 5 AND b = 6")) == 70, "recovered composite row");
        check(!rec.exec("INSERT INTO t (a, b, v) VALUES (5, 6, 1)").ok, "dup composite PK after recover");
        check(rec.exec("INSERT INTO t (a, b, v) VALUES (5, 7, 1)").ok, "distinct composite after recover");
    }
    if (g_fail) { std::printf("sql_composite_pk_test: FAILED\n"); return 1; }
    std::printf("sql_composite_pk_test: OK (composite PK: order, uniqueness, round-trip, durable)\n");
    return 0;
}
