// sql_savepoint_test.cpp — G6 SAVEPOINT / ROLLBACK TO SAVEPOINT / RELEASE SAVEPOINT. Nested
// partial rollback inside a BEGIN..COMMIT transaction: a savepoint marks a point in the
// buffered write set; ROLLBACK TO undoes the writes since it (visible to read-your-writes and
// to the committed set); RELEASE forgets it while keeping the writes. Row mode.
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
bool has(SqlEngine& e, int id) {
    return !e.exec("SELECT id FROM t WHERE id = " + std::to_string(id)).rows.empty();
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, v INT NOT NULL, PRIMARY KEY (id))");

    // NOTE: this engine buffers a txn's writes and applies them atomically at COMMIT; an
    // in-txn SELECT reads the COMMITTED state (mid-txn read-your-writes is a separate v1 gap),
    // so savepoint effects are verified by the COMMITTED result after COMMIT.

    // ROLLBACK TO SAVEPOINT: writes AFTER the savepoint are undone; those before survive COMMIT.
    check(e.exec("BEGIN").ok, "BEGIN");
    check(e.exec("INSERT INTO t (id, v) VALUES (1, 10)").ok, "insert 1");
    check(e.exec("SAVEPOINT s1").ok, "SAVEPOINT s1");
    check(e.exec("INSERT INTO t (id, v) VALUES (2, 20)").ok, "insert 2");
    check(e.exec("ROLLBACK TO SAVEPOINT s1").ok, "ROLLBACK TO s1");
    check(e.exec("COMMIT").ok, "COMMIT");
    check(count(e) == 1, "after COMMIT: only id 1 committed (id 2 undone by rollback-to-s1)");
    check(has(e, 1) && !has(e, 2), "committed: id 1 present, id 2 absent");

    // RELEASE SAVEPOINT: the savepoint is forgotten but its writes are kept and commit.
    check(e.exec("BEGIN").ok, "BEGIN 2");
    check(e.exec("INSERT INTO t (id, v) VALUES (3, 30)").ok, "insert 3");
    check(e.exec("SAVEPOINT s2").ok, "SAVEPOINT s2");
    check(e.exec("INSERT INTO t (id, v) VALUES (4, 40)").ok, "insert 4");
    check(e.exec("RELEASE SAVEPOINT s2").ok, "RELEASE s2");
    check(!e.exec("ROLLBACK TO SAVEPOINT s2").ok, "released savepoint no longer exists");
    check(e.exec("COMMIT").ok, "COMMIT 2");
    check(count(e) == 3, "after COMMIT: id 1,3,4 all committed (release kept the writes)");

    // NESTED savepoints: ROLLBACK TO an OUTER savepoint drops the inner one + all its writes.
    check(e.exec("BEGIN").ok, "BEGIN 3");
    check(e.exec("INSERT INTO t (id, v) VALUES (5, 50)").ok, "insert 5");
    check(e.exec("SAVEPOINT a").ok, "SAVEPOINT a");
    check(e.exec("INSERT INTO t (id, v) VALUES (6, 60)").ok, "insert 6");
    check(e.exec("SAVEPOINT b").ok, "SAVEPOINT b");
    check(e.exec("INSERT INTO t (id, v) VALUES (7, 70)").ok, "insert 7");
    check(e.exec("ROLLBACK TO SAVEPOINT a").ok, "ROLLBACK TO a (outer)");
    check(!e.exec("ROLLBACK TO SAVEPOINT b").ok, "inner savepoint b destroyed by rollback-to-a");
    // Can re-rollback to a savepoint (PG keeps the target after a rollback-to).
    check(e.exec("INSERT INTO t (id, v) VALUES (8, 80)").ok, "insert 8 after rollback");
    check(e.exec("ROLLBACK TO SAVEPOINT a").ok, "rollback-to-a again (target still valid)");
    check(e.exec("COMMIT").ok, "COMMIT 3");
    check(count(e) == 4, "after COMMIT: id 1,3,4,5 committed (6,7,8 undone)");
    check(has(e, 5) && !has(e, 6) && !has(e, 7) && !has(e, 8),
          "committed: id 5 present; 6, 7, 8 undone by the rollbacks-to-a");

    // Errors: unknown savepoint, and savepoint verbs outside a transaction.
    check(e.exec("BEGIN").ok, "BEGIN 4");
    check(!e.exec("ROLLBACK TO SAVEPOINT nope").ok, "ROLLBACK TO unknown savepoint -> error");
    check(!e.exec("RELEASE SAVEPOINT nope").ok, "RELEASE unknown savepoint -> error");
    check(e.exec("ROLLBACK").ok, "ROLLBACK 4");
    check(!e.exec("SAVEPOINT x").ok, "SAVEPOINT outside a transaction -> error");
    check(!e.exec("ROLLBACK TO SAVEPOINT x").ok, "ROLLBACK TO outside a transaction -> error");

    if (g_fail) { std::printf("sql_savepoint_test: FAILED\n"); return 1; }
    std::printf("sql_savepoint_test: ALL PASS (SAVEPOINT / ROLLBACK TO / RELEASE, nested + errors)\n");
    return 0;
}
