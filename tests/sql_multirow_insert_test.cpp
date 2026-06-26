// sql_multirow_insert_test.cpp — D6 MULTI-ROW INSERT gate.
// `INSERT INTO t (..) VALUES (..),(..),(..)` must be EQUIVALENT to the same rows inserted one at a
// time (byte-identical table state), ATOMIC (a dup PK anywhere in the batch commits NOTHING), and
// report affected == the row count. Checked in BOTH row mode and columnar mode.
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
std::string render(const ExecResult& r) {
    std::string out = r.ok ? "OK" : "ERR";
    if (!r.ok) return out + "(" + r.error + ")";
    out += " aff=" + std::to_string(r.affected);
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) out += " " + label + "=" + d.render();
    }
    return out;
}
const char* kDDL = "CREATE TABLE t (id INT, name TEXT NOT NULL, qty INT NOT NULL, PRIMARY KEY (id))";

void run_mode(bool columnar, const char* tag) {
    SqlEngine multi, single;
    if (columnar) { multi.set_columnar_default(true); single.set_columnar_default(true); }
    check(multi.exec(kDDL).ok && single.exec(kDDL).ok, std::string(tag) + " create");

    // multi-row in ONE statement vs the same rows one-by-one.
    const ExecResult m = multi.exec(
        "INSERT INTO t (id, name, qty) VALUES (1,'a',10),(2,'b',20),(3,'c',30)");
    check(m.ok && m.affected == 3, std::string(tag) + " multi-row affected=3 (got " + render(m) + ")");
    single.exec("INSERT INTO t (id, name, qty) VALUES (1,'a',10)");
    single.exec("INSERT INTO t (id, name, qty) VALUES (2,'b',20)");
    single.exec("INSERT INTO t (id, name, qty) VALUES (3,'c',30)");
    if (columnar) { multi.flush_columnar("t"); single.flush_columnar("t"); }

    const std::vector<std::string> qs = {
        "SELECT id, name, qty FROM t",
        "SELECT COUNT(*), SUM(qty), MIN(qty), MAX(qty) FROM t",
        "SELECT name, qty FROM t WHERE id = 2",
    };
    for (const std::string& q : qs) {
        const std::string a = render(multi.exec(q));
        const std::string b = render(single.exec(q));
        check(a == b, std::string(tag) + " multi != single for [" + q + "]\n  multi=[" + a +
                          "]\n  single=[" + b + "]");
    }

    // ATOMICITY: a dup PK in the batch (3 collides with the existing 3) rejects the WHOLE statement
    // — none of 7/8/9 may persist.
    const ExecResult bad = multi.exec(
        "INSERT INTO t (id, name, qty) VALUES (7,'x',1),(3,'dup',2),(9,'z',3)");
    check(!bad.ok, std::string(tag) + " dup-in-batch rejected (got " + render(bad) + ")");
    check(multi.exec("SELECT id FROM t WHERE id = 7").rows.empty(),
          std::string(tag) + " atomic: row 7 NOT committed after the failed batch");
    const ExecResult cnt = multi.exec("SELECT COUNT(*) FROM t");
    check(cnt.ok && !cnt.rows.empty() && cnt.rows[0].cells[0].second.i == 3,
          std::string(tag) + " atomic: still exactly 3 rows (got " + render(cnt) + ")");

    // within-batch dup (two 5s) also rejects.
    check(!multi.exec("INSERT INTO t (id, name, qty) VALUES (5,'p',1),(5,'q',2)").ok,
          std::string(tag) + " within-batch dup PK rejected");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_multirow_insert_test: FAILED\n"); return 1; }
    std::printf("sql_multirow_insert_test: OK (multi-row == single-row, atomic, both modes)\n");
    return 0;
}
