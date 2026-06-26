// sql_auto_increment_test.cpp — F6 AUTO_INCREMENT gate. An omitted AUTO_INCREMENT column is
// assigned the next monotonic id; an explicit value advances the counter past it; the counter
// survives a restart (persisted in the catalog). Teeth: AUTO_INCREMENT on a TEXT column is rejected.
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
std::vector<std::int64_t> ids(SqlEngine& e, const std::string& q) {
    std::vector<std::int64_t> out;
    for (const auto& row : e.exec(q).rows) out.push_back(row.cells[0].second.i);
    return out;
}
}  // namespace

int main() {
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT AUTO_INCREMENT, v INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (v) VALUES (10)");           // id 1
        e.exec("INSERT INTO t (v) VALUES (20), (30)");     // ids 2, 3
        e.exec("INSERT INTO t (id, v) VALUES (100, 40)");  // explicit id 100 -> counter jumps to 101
        e.exec("INSERT INTO t (v) VALUES (50)");           // id 101
        const std::vector<std::int64_t> got = ids(e, "SELECT id FROM t ORDER BY id");
        const std::vector<std::int64_t> want = {1, 2, 3, 100, 101};
        check(got == want, "auto ids = {1,2,3,100,101}");

        // teeth: AUTO_INCREMENT on TEXT rejected.
        check(!e.exec("CREATE TABLE w (id TEXT AUTO_INCREMENT, PRIMARY KEY (id))").ok,
              "AUTO_INCREMENT on TEXT rejected");
    }
    // durable: the counter continues after a restart (no id reissue).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT AUTO_INCREMENT, v INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO t (v) VALUES (1), (2), (3)");  // ids 1,2,3
            dl = d.durable_len();
            cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        rec.exec("INSERT INTO t (v) VALUES (4)");  // must be id 4, NOT a reissued 1
        const std::vector<std::int64_t> got = ids(rec, "SELECT id FROM t ORDER BY id");
        check((got == std::vector<std::int64_t>{1, 2, 3, 4}),
              "after recover the next auto id is 4 (counter persisted)");
    }
    if (g_fail) { std::printf("sql_auto_increment_test: FAILED\n"); return 1; }
    std::printf("sql_auto_increment_test: OK (AUTO_INCREMENT assign + explicit bump + durable)\n");
    return 0;
}
