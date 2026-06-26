// sql_alter_full_test.cpp — E1: the full ALTER TABLE surface. RENAME TABLE / COLUMN, DROP COLUMN
// (logical drop — slot kept for row-alignment, hidden from SELECT/ref), ALTER COLUMN TYPE (row
// rewrite via cast), SET/DROP DEFAULT, SET/DROP NOT NULL (validate existing), ADD CHECK / UNIQUE
// (validate existing). All over the verified row scan/commit path; durable.
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
std::string cell0(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : std::string("<none>");
}
}  // namespace

int main() {
    // RENAME TABLE + RENAME COLUMN.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, n INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, n) VALUES (1, 100)");
        check(e.exec("ALTER TABLE t RENAME TO t2").ok, "rename table");
        check(!e.exec("SELECT n FROM t").ok, "old name gone");
        check(cell0(e.exec("SELECT n FROM t2 WHERE id = 1")) == "100", "data under new name");
        check(e.exec("ALTER TABLE t2 RENAME COLUMN n TO amount").ok, "rename column");
        check(cell0(e.exec("SELECT amount FROM t2 WHERE id = 1")) == "100", "column under new name");
        check(!e.exec("SELECT n FROM t2").ok, "old column name gone");
    }
    // DROP COLUMN (logical drop: hidden, alignment preserved for other columns).
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b TEXT NOT NULL, c INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, a, b, c) VALUES (1, 10, 'x', 30)");
        check(e.exec("ALTER TABLE t DROP COLUMN b").ok, "drop column b");
        check(!e.exec("SELECT b FROM t").ok, "b no longer referenceable");
        // a and c still read correctly (alignment held despite the dropped middle column).
        check(cell0(e.exec("SELECT a FROM t WHERE id = 1")) == "10", "a intact after drop");
        check(cell0(e.exec("SELECT c FROM t WHERE id = 1")) == "30", "c intact after drop");
        // a new INSERT works (the dropped slot is written as NULL internally).
        check(e.exec("INSERT INTO t (id, a, c) VALUES (2, 11, 31)").ok, "insert after drop");
        check(cell0(e.exec("SELECT c FROM t WHERE id = 2")) == "31", "new row c");
        // can't drop the PK.
        check(!e.exec("ALTER TABLE t DROP COLUMN id").ok, "cannot drop PK");
    }
    // SET/DROP DEFAULT, SET/DROP NOT NULL.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, v INT, PRIMARY KEY (id))");  // v nullable, no default
        e.exec("INSERT INTO t (id, v) VALUES (1, 5)");
        check(e.exec("ALTER TABLE t ALTER COLUMN v SET DEFAULT 99").ok, "set default");
        check(e.exec("INSERT INTO t (id) VALUES (2)").ok, "insert omitting v uses default");
        check(cell0(e.exec("SELECT v FROM t WHERE id = 2")) == "99", "default applied");
        // SET NOT NULL fails while a NULL exists, succeeds once filled.
        e.exec("INSERT INTO t (id, v) VALUES (3, NULL)");
        check(!e.exec("ALTER TABLE t ALTER COLUMN v SET NOT NULL").ok, "SET NOT NULL blocked by NULL");
        e.exec("UPDATE t SET v = 0 WHERE id = 3");
        check(e.exec("ALTER TABLE t ALTER COLUMN v SET NOT NULL").ok, "SET NOT NULL ok after fill");
        check(!e.exec("INSERT INTO t (id, v) VALUES (4, NULL)").ok, "NOT NULL now enforced");
        check(e.exec("ALTER TABLE t ALTER COLUMN v DROP NOT NULL").ok, "drop not null");
        check(e.exec("INSERT INTO t (id, v) VALUES (5, NULL)").ok, "NULL allowed again");
        check(e.exec("ALTER TABLE t ALTER COLUMN v DROP DEFAULT").ok, "drop default");
    }
    // ALTER COLUMN TYPE (rewrite existing rows).
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, code INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, code) VALUES (1, 42), (2, 7)");
        check(e.exec("ALTER TABLE t ALTER COLUMN code TYPE TEXT").ok, "int -> text");
        check(cell0(e.exec("SELECT code FROM t WHERE id = 1")) == "42", "42 rewritten as text");
        // and back: TEXT '42' -> INT 42.
        check(e.exec("ALTER TABLE t ALTER COLUMN code TYPE INT").ok, "text -> int");
        check(cell0(e.exec("SELECT code FROM t WHERE id = 1")) == "42", "42 back as int");
    }
    // ADD CHECK / ADD UNIQUE — validate existing rows.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, age INT NOT NULL, email TEXT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, age, email) VALUES (1, 30, 'a@x'), (2, 40, 'b@x')");
        check(e.exec("ALTER TABLE t ADD CHECK (age >= 0)").ok, "add check (existing rows pass)");
        check(!e.exec("INSERT INTO t (id, age, email) VALUES (3, -1, 'c@x')").ok, "check enforced");
        check(e.exec("ALTER TABLE t ADD UNIQUE (email)").ok, "add unique (existing distinct)");
        check(!e.exec("INSERT INTO t (id, age, email) VALUES (4, 50, 'a@x')").ok, "unique enforced");
        // a CHECK that an existing row violates is rejected.
        e.exec("INSERT INTO t (id, age, email) VALUES (5, 5, 'd@x')");
        check(!e.exec("ALTER TABLE t ADD CHECK (age >= 18)").ok, "add check rejected (existing < 18)");
    }
    // durable: a rename + drop + alter survive a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE orig (id INT, x INT NOT NULL, y INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO orig (id, x, y) VALUES (1, 11, 22)");
            e.exec("ALTER TABLE orig RENAME TO renamed");
            e.exec("ALTER TABLE renamed DROP COLUMN y");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("SELECT x FROM orig").ok, "old table name gone after recover");
        check(cell0(rec.exec("SELECT x FROM renamed WHERE id = 1")) == "11", "renamed table recovered");
        check(!rec.exec("SELECT y FROM renamed").ok, "dropped column stays dropped after recover");
    }
    if (g_fail) { std::printf("sql_alter_full_test: FAILED\n"); return 1; }
    std::printf("sql_alter_full_test: OK (RENAME table/col, DROP COLUMN, ALTER TYPE, SET/DROP "
                "DEFAULT/NOT NULL, ADD CHECK/UNIQUE, durable)\n");
    return 0;
}
