// sql_check_test.cpp — F5 CHECK constraint gate. A row that violates a column- or table-level CHECK
// is rejected; valid rows insert; the constraint survives a restart (the source text is persisted
// and re-evaluated). Row + columnar.
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
std::int64_t cnt(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -1;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, age INT NOT NULL CHECK (age >= 0), "
           "score INT NOT NULL, CHECK (score <= 100), PRIMARY KEY (id))");
    const std::string T = tag;
    check(e.exec("INSERT INTO t (id, age, score) VALUES (1, 20, 90)").ok, T + " valid insert");
    check(!e.exec("INSERT INTO t (id, age, score) VALUES (2, -1, 50)").ok, T + " age<0 rejected (column CHECK)");
    check(!e.exec("INSERT INTO t (id, age, score) VALUES (3, 30, 200)").ok, T + " score>100 rejected (table CHECK)");
    check(e.exec("INSERT INTO t (id, age, score) VALUES (4, 0, 100)").ok, T + " boundary values ok");
    if (columnar) e.flush_columnar("t");
    check(cnt(e, "SELECT COUNT(*) FROM t") == 2, T + " only the 2 valid rows present");
    // multi-row atomic: one bad row rejects the whole batch.
    check(!e.exec("INSERT INTO t (id, age, score) VALUES (5, 5, 5),(6, -9, 5)").ok, T + " batch with a bad row rejected");
    check(cnt(e, "SELECT COUNT(*) FROM t") == 2, T + " batch left table unchanged");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");

    // durable: CHECK enforced after a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, v INT NOT NULL CHECK (v > 10), PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id, v) VALUES (1, 20)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("INSERT INTO t (id, v) VALUES (2, 5)").ok, "CHECK still enforced after recover");
        check(rec.exec("INSERT INTO t (id, v) VALUES (2, 99)").ok, "valid insert ok after recover");
    }
    if (g_fail) { std::printf("sql_check_test: FAILED\n"); return 1; }
    std::printf("sql_check_test: OK (column + table CHECK, atomic, durable)\n");
    return 0;
}
