// sql_upsert_test.cpp — G2 INSERT ... ON CONFLICT gate. DO NOTHING leaves an existing row
// untouched (affected 0); DO UPDATE SET applies col=literal to the existing row; a non-conflicting
// row inserts normally. Row + columnar. Teeth: updating the PK in ON CONFLICT is rejected.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t cell(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -999;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, v INT NOT NULL, w INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id, v, w) VALUES (1, 10, 100)");

    // DO NOTHING: existing row untouched, affected 0.
    const ExecResult dn = e.exec("INSERT INTO t (id, v, w) VALUES (1, 99, 999) ON CONFLICT DO NOTHING");
    check(dn.ok && dn.affected == 0, std::string(tag) + " DO NOTHING affected 0");
    if (columnar) e.flush_columnar("t");
    check(cell(e, "SELECT v FROM t WHERE id = 1") == 10, std::string(tag) + " DO NOTHING kept v=10");

    // DO UPDATE SET v=99 (w untouched), affected 1.
    const ExecResult du =
        e.exec("INSERT INTO t (id, v, w) VALUES (1, 5, 5) ON CONFLICT DO UPDATE SET v = 99");
    check(du.ok && du.affected == 1, std::string(tag) + " DO UPDATE affected 1");
    if (columnar) e.flush_columnar("t");
    check(cell(e, "SELECT v FROM t WHERE id = 1") == 99, std::string(tag) + " DO UPDATE set v=99");
    check(cell(e, "SELECT w FROM t WHERE id = 1") == 100, std::string(tag) + " DO UPDATE kept w=100");

    // multi-col SET.
    e.exec("INSERT INTO t (id, v, w) VALUES (1, 0, 0) ON CONFLICT DO UPDATE SET v = 7, w = 8");
    if (columnar) e.flush_columnar("t");
    check(cell(e, "SELECT v FROM t WHERE id = 1") == 7 && cell(e, "SELECT w FROM t WHERE id = 1") == 8,
          std::string(tag) + " multi-col SET (v=7,w=8)");

    // non-conflicting row inserts normally even with the clause.
    const ExecResult ni = e.exec("INSERT INTO t (id, v, w) VALUES (2, 20, 200) ON CONFLICT DO NOTHING");
    check(ni.ok && ni.affected == 1, std::string(tag) + " non-conflict insert affected 1");
    if (columnar) e.flush_columnar("t");
    check(cell(e, "SELECT v FROM t WHERE id = 2") == 20, std::string(tag) + " new row inserted");

    // teeth: updating the PK in ON CONFLICT is rejected.
    check(!e.exec("INSERT INTO t (id, v, w) VALUES (1,1,1) ON CONFLICT DO UPDATE SET id = 9").ok,
          std::string(tag) + " ON CONFLICT cannot UPDATE the PK");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_upsert_test: FAILED\n"); return 1; }
    std::printf("sql_upsert_test: OK (ON CONFLICT DO NOTHING / DO UPDATE, both modes)\n");
    return 0;
}
