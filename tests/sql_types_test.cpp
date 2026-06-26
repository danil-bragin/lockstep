// sql_types_test.cpp — F9 extra types (integer/text-backed, byte-deterministic): BIGINT/INTEGER and
// BOOL (with TRUE/FALSE literals) over INT, VARCHAR/CHAR over TEXT. FLOAT/DOUBLE/DECIMAL are rejected.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
}  // namespace

int main() {
    SqlEngine e;
    check(e.exec("CREATE TABLE t (id BIGINT, active BOOL NOT NULL, name VARCHAR(50) NOT NULL, "
                 "big BIGINT NOT NULL, PRIMARY KEY (id))").ok, "CREATE with BIGINT/BOOL/VARCHAR");
    check(e.exec("INSERT INTO t (id, active, name, big) VALUES (1, TRUE, 'alice', 9000000000)").ok,
          "insert TRUE + big int");
    check(e.exec("INSERT INTO t (id, active, name, big) VALUES (2, FALSE, 'bob', 5)").ok,
          "insert FALSE");

    // BOOL is INT-backed: TRUE=1, FALSE=0.
    check(e.exec("SELECT active FROM t WHERE id = 1").rows[0].cells[0].second.i == 1, "TRUE stored as 1");
    check(e.exec("SELECT active FROM t WHERE id = 2").rows[0].cells[0].second.i == 0, "FALSE stored as 0");
    // filter by a BOOL literal.
    {
        const ExecResult r = e.exec("SELECT id FROM t WHERE active = TRUE");
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[0].second.i == 1, "WHERE active = TRUE -> id 1");
    }
    // BIGINT holds a value beyond 32 bits.
    check(e.exec("SELECT big FROM t WHERE id = 1").rows[0].cells[0].second.i == 9000000000LL,
          "BIGINT 9e9 round-trips");
    // VARCHAR behaves as TEXT.
    check(e.exec("SELECT name FROM t WHERE id = 2").rows[0].cells[0].second.s == "bob", "VARCHAR text");

    // teeth: FLOAT / DOUBLE / DECIMAL rejected.
    check(!e.exec("CREATE TABLE f (id INT, x FLOAT, PRIMARY KEY (id))").ok, "FLOAT rejected");
    check(!e.exec("CREATE TABLE f (id INT, x DOUBLE, PRIMARY KEY (id))").ok, "DOUBLE rejected");
    check(!e.exec("CREATE TABLE f (id INT, x DECIMAL, PRIMARY KEY (id))").ok, "DECIMAL rejected");

    if (g_fail) { std::printf("sql_types_test: FAILED\n"); return 1; }
    std::printf("sql_types_test: OK (BIGINT/BOOL/VARCHAR + TRUE/FALSE; FLOAT/DECIMAL OUT)\n");
    return 0;
}
