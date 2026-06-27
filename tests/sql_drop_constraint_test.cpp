// sql_drop_constraint_test.cpp — ALTER TABLE ... DROP CONSTRAINT <name>. Every CHECK / column-UNIQUE
// / column-FOREIGN-KEY constraint carries a name (explicit `CONSTRAINT <name>` or a deterministic
// Postgres-style auto name: <t>_check, <t>_<col>_key, <t>_<col>_fkey). Dropping by name stops the
// constraint being enforced; the rest of the schema is untouched.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
bool ok(SqlEngine& e, const std::string& q) { return e.exec(q).ok; }
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE parent (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO parent (id) VALUES (1),(2),(3)");
    check(ok(e, "CREATE TABLE t (id INT, email TEXT UNIQUE, pid INT REFERENCES parent, age INT, "
               "CONSTRAINT age_ok CHECK (age >= 0), PRIMARY KEY (id))"),
          "create table with named CHECK + column UNIQUE + FK");

    // All three constraints enforce before any drop.
    check(!ok(e, "INSERT INTO t (id,email,pid,age) VALUES (1,'a',1,-5)"), "CHECK rejects age=-5");
    check(ok(e, "INSERT INTO t (id,email,pid,age) VALUES (1,'a',1,5)"), "valid row inserts");
    check(!ok(e, "INSERT INTO t (id,email,pid,age) VALUES (2,'a',2,5)"), "UNIQUE rejects dup email");
    check(!ok(e, "INSERT INTO t (id,email,pid,age) VALUES (3,'b',9,5)"), "FK rejects pid=9 (no parent)");

    // DROP the CHECK by its EXPLICIT name.
    check(ok(e, "ALTER TABLE t DROP CONSTRAINT age_ok"), "drop CHECK age_ok");
    check(ok(e, "INSERT INTO t (id,email,pid,age) VALUES (4,'c',1,-5)"), "negative age now allowed");

    // DROP the column UNIQUE by its AUTO name.
    check(ok(e, "ALTER TABLE t DROP CONSTRAINT t_email_key"), "drop UNIQUE t_email_key");
    check(ok(e, "INSERT INTO t (id,email,pid,age) VALUES (5,'a',1,5)"), "duplicate email now allowed");

    // DROP the FK by its AUTO name.
    check(ok(e, "ALTER TABLE t DROP CONSTRAINT t_pid_fkey"), "drop FK t_pid_fkey");
    check(ok(e, "INSERT INTO t (id,email,pid,age) VALUES (6,'z',99,5)"), "dangling pid now allowed");

    // Errors: unknown name, and re-dropping an already-dropped constraint.
    check(!ok(e, "ALTER TABLE t DROP CONSTRAINT nope"), "unknown constraint name errors");
    check(!ok(e, "ALTER TABLE t DROP CONSTRAINT age_ok"), "re-dropping a gone constraint errors");

    // ADD CONSTRAINT with an explicit name, then drop it by that name (CHECK + UNIQUE).
    {
        SqlEngine f;
        f.exec("CREATE TABLE u (id INT, v INT, PRIMARY KEY (id))");
        f.exec("INSERT INTO u (id,v) VALUES (1,10),(2,20)");
        check(ok(f, "ALTER TABLE u ADD CONSTRAINT v_pos CHECK (v > 0)"), "ADD CONSTRAINT v_pos");
        check(!ok(f, "INSERT INTO u (id,v) VALUES (3,-1)"), "added CHECK enforces");
        check(ok(f, "ALTER TABLE u DROP CONSTRAINT v_pos"), "drop the added CHECK by name");
        check(ok(f, "INSERT INTO u (id,v) VALUES (4,-1)"), "after drop, -1 allowed");
        check(ok(f, "ALTER TABLE u ADD CONSTRAINT v_uniq UNIQUE (v)"), "ADD named UNIQUE (values distinct)");
        check(!ok(f, "INSERT INTO u (id,v) VALUES (5,10)"), "added UNIQUE enforces (dup v=10)");
        check(ok(f, "ALTER TABLE u DROP CONSTRAINT v_uniq"), "drop the added UNIQUE by name");
        check(ok(f, "INSERT INTO u (id,v) VALUES (6,10)"), "after drop, dup v=10 allowed");
    }

    // Two unnamed CHECKs auto-name distinctly (<t>_check, <t>_check2) and both are droppable.
    {
        SqlEngine g;
        g.exec("CREATE TABLE w (id INT, a INT, b INT, CHECK (a >= 0), CHECK (b >= 0), PRIMARY KEY (id))");
        check(!ok(g, "INSERT INTO w (id,a,b) VALUES (1,-1,5)"), "first check enforces");
        check(!ok(g, "INSERT INTO w (id,a,b) VALUES (1,5,-1)"), "second check enforces");
        check(ok(g, "ALTER TABLE w DROP CONSTRAINT w_check"), "drop first auto-named check");
        check(ok(g, "ALTER TABLE w DROP CONSTRAINT w_check2"), "drop second auto-named check");
        check(ok(g, "INSERT INTO w (id,a,b) VALUES (2,-1,-1)"), "both checks gone");
    }

    if (g_fail) { std::printf("sql_drop_constraint_test: FAILED\n"); return 1; }
    std::printf("sql_drop_constraint_test: OK (DROP CONSTRAINT by name: CHECK/UNIQUE/FK, explicit + "
                "auto names, ADD+DROP round-trip, multi-check auto-naming)\n");
    return 0;
}
