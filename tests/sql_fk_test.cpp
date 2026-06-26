// sql_fk_test.cpp — F3 FOREIGN KEY gate. An INSERT with a non-NULL FK must reference an existing
// parent PK (NULL FK allowed); a parent row still referenced by a child cannot be DELETEd (RESTRICT);
// the constraint survives a restart. Row + columnar.
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
    e.exec("CREATE TABLE dept (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO dept (id) VALUES (1), (2)");
    e.exec("CREATE TABLE emp (id INT, dept_id INT REFERENCES dept(id), PRIMARY KEY (id))");
    const std::string T = tag;

    check(e.exec("INSERT INTO emp (id, dept_id) VALUES (1, 1)").ok, T + " FK to existing parent ok");
    check(!e.exec("INSERT INTO emp (id, dept_id) VALUES (2, 99)").ok, T + " FK to missing parent rejected");
    check(e.exec("INSERT INTO emp (id) VALUES (3)").ok, T + " NULL FK allowed");
    if (columnar) { e.flush_columnar("dept"); e.flush_columnar("emp"); }
    check(cnt(e, "SELECT COUNT(*) FROM emp") == 2, T + " 2 emp rows (the bad one rejected)");

    // DELETE RESTRICT: dept 1 is referenced by emp 1 -> cannot delete; dept 2 has no child -> ok.
    check(!e.exec("DELETE FROM dept WHERE id = 1").ok, T + " DELETE of referenced parent restricted");
    check(e.exec("DELETE FROM dept WHERE id = 2").ok, T + " DELETE of unreferenced parent ok");
    check(cnt(e, "SELECT COUNT(*) FROM dept") == 1, T + " dept still has row 1");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");

    // durable: FK enforced after a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE p (id INT, PRIMARY KEY (id))");
            e.exec("INSERT INTO p (id) VALUES (1)");
            e.exec("CREATE TABLE c (id INT, pid INT REFERENCES p(id), PRIMARY KEY (id))");
            e.exec("INSERT INTO c (id, pid) VALUES (1, 1)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("INSERT INTO c (id, pid) VALUES (2, 7)").ok, "FK enforced after recover (missing parent)");
        check(!rec.exec("DELETE FROM p WHERE id = 1").ok, "DELETE RESTRICT after recover");
    }
    if (g_fail) { std::printf("sql_fk_test: FAILED\n"); return 1; }
    std::printf("sql_fk_test: OK (FK insert-check + DELETE restrict + durable)\n");
    return 0;
}
