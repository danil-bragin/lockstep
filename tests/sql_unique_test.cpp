// sql_unique_test.cpp — F2 UNIQUE constraint gate. No two non-NULL rows may share a UNIQUE
// column value (existing or within an atomic multi-row INSERT); multiple NULLs are allowed; the
// constraint survives a restart. Row + columnar.
#include <cstdio>
#include <string>

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
std::int64_t count(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -1;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, email TEXT NOT NULL UNIQUE, tag INT UNIQUE, PRIMARY KEY (id))");
    check(e.exec("INSERT INTO t (id, email, tag) VALUES (1, 'a@x', 100)").ok, std::string(tag) + " insert 1");
    if (columnar) e.flush_columnar("t");
    // duplicate email rejected.
    check(!e.exec("INSERT INTO t (id, email, tag) VALUES (2, 'a@x', 200)").ok,
          std::string(tag) + " dup email rejected");
    // duplicate tag rejected.
    check(!e.exec("INSERT INTO t (id, email, tag) VALUES (2, 'b@x', 100)").ok,
          std::string(tag) + " dup tag rejected");
    // distinct ok.
    check(e.exec("INSERT INTO t (id, email, tag) VALUES (2, 'b@x', 200)").ok,
          std::string(tag) + " distinct insert ok");
    if (columnar) e.flush_columnar("t");
    // within-batch dup rejected (atomic — nothing persists).
    check(!e.exec("INSERT INTO t (id, email, tag) VALUES (3,'c@x',300),(4,'c@x',400)").ok,
          std::string(tag) + " within-batch dup email rejected");
    check(count(e, "SELECT COUNT(*) FROM t") == 2, std::string(tag) + " still 2 rows after failed batch");
    // NULL tag may repeat (UNIQUE allows multiple NULLs).
    check(e.exec("INSERT INTO t (id, email) VALUES (5, 'd@x')").ok, std::string(tag) + " null tag #1");
    check(e.exec("INSERT INTO t (id, email) VALUES (6, 'e@x')").ok, std::string(tag) + " null tag #2 (NULLs repeat)");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");

    // durable: UNIQUE enforced after a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, k INT UNIQUE, PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id, k) VALUES (1, 7)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("INSERT INTO t (id, k) VALUES (2, 7)").ok,
              "UNIQUE still enforced after recover");
        check(rec.exec("INSERT INTO t (id, k) VALUES (2, 8)").ok, "distinct ok after recover");
    }
    if (g_fail) { std::printf("sql_unique_test: FAILED\n"); return 1; }
    std::printf("sql_unique_test: OK (UNIQUE: dup reject, NULLs repeat, atomic, durable)\n");
    return 0;
}
