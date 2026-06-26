// sql_txn_test.cpp — G1 BEGIN / COMMIT / ROLLBACK. Buffered writes commit atomically at COMMIT and
// are discarded by ROLLBACK; read-your-writes within the txn catches a duplicate PK. Row mode.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t count(SqlEngine& e) {
    const ExecResult r = e.exec("SELECT COUNT(*) FROM t");
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -1;
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, v INT NOT NULL, PRIMARY KEY (id))");

    // COMMIT: a multi-statement transaction is applied atomically.
    check(e.exec("BEGIN").ok, "BEGIN");
    check(e.exec("INSERT INTO t (id, v) VALUES (1, 10)").ok, "txn insert 1");
    check(e.exec("INSERT INTO t (id, v) VALUES (2, 20)").ok, "txn insert 2");
    check(e.exec("COMMIT").ok, "COMMIT");
    check(count(e) == 2, "after COMMIT: 2 rows");

    // ROLLBACK: buffered writes are discarded.
    check(e.exec("BEGIN").ok, "BEGIN 2");
    check(e.exec("INSERT INTO t (id, v) VALUES (3, 30)").ok, "txn insert 3");
    check(e.exec("INSERT INTO t (id, v) VALUES (4, 40)").ok, "txn insert 4");
    check(e.exec("ROLLBACK").ok, "ROLLBACK");
    check(count(e) == 2, "after ROLLBACK: still 2 rows (3,4 discarded)");
    check(e.exec("SELECT v FROM t WHERE id = 3").rows.empty(), "row 3 absent after rollback");

    // read-your-writes: a duplicate PK within the txn is caught.
    check(e.exec("BEGIN").ok, "BEGIN 3");
    check(e.exec("INSERT INTO t (id, v) VALUES (5, 50)").ok, "txn insert 5");
    check(!e.exec("INSERT INTO t (id, v) VALUES (5, 99)").ok, "within-txn dup PK rejected");
    check(e.exec("COMMIT").ok, "COMMIT 3");
    check(count(e) == 3, "after COMMIT: 3 rows (the dup did not double-insert)");
    check(e.exec("SELECT v FROM t WHERE id = 5").rows[0].cells[0].second.i == 50, "id 5 = 50");

    // a write outside a txn auto-commits (unchanged behavior).
    check(e.exec("INSERT INTO t (id, v) VALUES (6, 60)").ok, "auto-commit insert");
    check(count(e) == 4, "auto-commit: 4 rows");

    // teeth: COMMIT/ROLLBACK with no active transaction errors.
    check(!e.exec("COMMIT").ok, "COMMIT with no txn errors");
    check(!e.exec("ROLLBACK").ok, "ROLLBACK with no txn errors");

    if (g_fail) { std::printf("sql_txn_test: FAILED\n"); return 1; }
    std::printf("sql_txn_test: OK (BEGIN/COMMIT/ROLLBACK, read-your-writes dup)\n");
    return 0;
}
