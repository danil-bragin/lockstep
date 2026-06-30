// sql_sqldump_test.cpp — SQL-TEXT (pg_dump-style) backup / restore. The engine emits a portable .sql
// script (CREATE SCHEMA / CREATE TABLE / CREATE INDEX, plus INSERTs for Full) that, replayed through
// the verified exec() path, rebuilds the WHOLE database. This gate proves the text dump round-trips
// the core relational surface and that recovery from the script reproduces the source exactly.
//
// Coverage:
//   (1) FULL dump -> restore (string path) -> every query byte-identical to the source. Workload:
//       a CREATE SCHEMA; an FK parent/child pair (dependency-ordered); a table with AUTO_INCREMENT,
//       UNIQUE, VARCHAR/CHAR/TINYINT/SMALLINT and a CHECK; a columnar table (via the `-- lockstep:
//       columnar` directive); a composite-PK table; and secondary indexes (plain + unique).
//   (2) FULL dump -> restore via the framed DISK path (dump_sql / restore_sql).
//   (3) SCHEMA-ONLY dump -> restore -> tables/indexes exist, NO rows.
//   (4) FRESH-ENGINE guard — restoring a script onto a populated engine is rejected.
//   (5) REFUSAL — a table with a type the text dump can't round-trip yet (DATE) makes dump_sql error
//       (pointing at the binary backup); the binary path remains full-fidelity.
//   (6) Framing teeth — a bad-magic / truncated framed dump is rejected by restore_sql.
// See SqlEngine::dump_sql / restore_sql.

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
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

struct Backing {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xD00D'0001ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
    lockstep::core::Scheduler cat_sched;
    lockstep::core::SimClock cat_clock{cat_sched};
    lockstep::sim::SeededRandom cat_rng{0xD00D'0002ULL};
    lockstep::sim::SimDisk cat_disk{cat_sched, cat_clock, cat_rng};
};

struct DiskBox {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xD00D'0100ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
};

// Run a DDL statement and assert it succeeded (guards against a vacuous pass where a table that
// fails to create in BOTH source and restore makes "unknown table" queries match trivially).
void ddl(SqlEngine& e, const std::string& sql) {
    const ExecResult r = e.exec(sql);
    check(r.ok, "DDL must succeed: [" + sql + "] -> " + render(r));
}

void load(SqlEngine& e) {
    ddl(e, "CREATE SCHEMA app");

    // FK parent + child (must be dependency-ordered in the dump).
    ddl(e, "CREATE TABLE cust (id INT, name TEXT NOT NULL, PRIMARY KEY (id))");
    ddl(e, "CREATE TABLE ord (id INT, cust_id INT NOT NULL REFERENCES cust(id), "
           "total BIGINT NOT NULL DEFAULT 0, PRIMARY KEY (id))");
    const char* names[] = {"ann", "bob", "cid", "dee", "eve"};
    for (int i = 0; i < 5; ++i)
        e.exec("INSERT INTO cust (id, name) VALUES (" + std::to_string(i) + ", '" + names[i] + "')");
    for (int i = 0; i < 40; ++i)
        e.exec("INSERT INTO ord (id, cust_id, total) VALUES (" + std::to_string(i) + ", " +
               std::to_string(i % 5) + ", " + std::to_string((i * 131) % 1000) + ")");

    // AUTO_INCREMENT + UNIQUE + VARCHAR/CHAR/TINYINT/SMALLINT + CHECK (table-level CHECK must precede
    // the PRIMARY KEY clause — the dumper emits it in that order too).
    ddl(e, "CREATE TABLE acct (id INT AUTO_INCREMENT, code VARCHAR(8) UNIQUE, tag CHAR(3), "
           "age TINYINT NOT NULL, score SMALLINT, CHECK (age >= 0), PRIMARY KEY (id))");
    for (int i = 0; i < 20; ++i)
        e.exec("INSERT INTO acct (id, code, tag, age, score) VALUES (" + std::to_string(i) + ", 'c" +
               std::to_string(i) + "', 'abc', " + std::to_string(i % 90) + ", " +
               std::to_string((i * 7) % 30000) + ")");

    // Columnar table (carried by the directive) with a flush.
    e.set_columnar_default(true);
    ddl(e, "CREATE TABLE sale (id INT, region TEXT NOT NULL, amount BIGINT NOT NULL, PRIMARY KEY (id))");
    e.set_columnar_default(false);
    const char* regs[] = {"north", "south", "east", "west"};
    for (int i = 0; i < 60; ++i)
        e.exec("INSERT INTO sale (id, region, amount) VALUES (" + std::to_string(i) + ", '" +
               regs[i % 4] + "', " + std::to_string((i * 2654435761ULL) % 100000) + ")");
    e.flush_columnar("sale");

    // Composite PK.
    ddl(e, "CREATE TABLE pos (a INT, b INT, v INT NOT NULL, PRIMARY KEY (a, b))");
    for (int a = 0; a < 6; ++a)
        for (int b = 0; b < 3; ++b)
            e.exec("INSERT INTO pos (a, b, v) VALUES (" + std::to_string(a) + ", " + std::to_string(b) +
                   ", " + std::to_string(a * 10 + b) + ")");

    // Secondary indexes (plain + unique).
    ddl(e, "CREATE INDEX ix_name ON cust (name)");
    ddl(e, "CREATE UNIQUE INDEX ux_code ON acct (code)");
}

// Assert a table actually exists with rows (a non-vacuous sanity check before round-tripping).
void assert_populated(SqlEngine& e) {
    const ExecResult r = e.exec("SELECT COUNT(*) FROM acct");
    check(r.ok && render(r) != "OK aff=1 | COUNT(*)=0", "source acct exists and has rows — " + render(r));
}

const std::vector<std::string>& queries() {
    static const std::vector<std::string> q = {
        "SELECT COUNT(*), SUM(total), MIN(total), MAX(total) FROM ord",
        "SELECT cust_id, COUNT(*), SUM(total) FROM ord GROUP BY cust_id",
        "SELECT id, code, tag, age, score FROM acct WHERE id = 7",
        "SELECT COUNT(*) FROM acct WHERE age >= 50",
        "SELECT region, COUNT(*), SUM(amount) FROM sale GROUP BY region",
        "SELECT COUNT(*), SUM(amount) FROM sale",
        "SELECT a, b, v FROM pos WHERE a = 3",
        "SELECT name FROM cust WHERE id = 3",        // exercises ix_name
        "SELECT id FROM acct WHERE code = 'c5'",     // exercises ux_code
    };
    return q;
}

std::vector<std::string> snapshot(SqlEngine& e) {
    std::vector<std::string> out;
    for (const std::string& q : queries()) out.push_back(render(e.exec(q)));
    return out;
}

lockstep::core::Task write_raw(lockstep::core::IDisk& d, std::vector<std::byte> bytes,
                               lockstep::core::Error& result) {
    lockstep::core::Offset off = 0;
    const lockstep::core::Error ae =
        co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (!ae.ok()) {
        result = ae;
        co_return;
    }
    result = co_await d.sync();
    co_return;
}

}  // namespace

int main() {
    Backing sb;
    SqlEngine src(sb.sched, sb.disk, sb.cat_sched, sb.cat_disk);
    load(src);
    assert_populated(src);
    const std::vector<std::string> want = snapshot(src);

    // ===== (1) FULL dump (string) -> restore into a FRESH engine -> identical =====
    std::string script;
    check(src.dump_sql_string(SqlBackupScope::Full, script).ok(), "full dump_sql_string ok");
    check(script.find("CREATE TABLE cust") != std::string::npos, "dump contains CREATE TABLE cust");
    check(script.find("-- lockstep:columnar") != std::string::npos, "dump carries the columnar directive");
    // FK ordering: parent cust must be emitted before child ord.
    check(script.find("CREATE TABLE cust") < script.find("CREATE TABLE ord"),
          "FK parent (cust) dumped before child (ord)");
    {
        Backing rb;
        SqlEngine restored(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
        check(restored.restore_sql_string(script).ok(), "restore_sql_string ok");
        const std::vector<std::string> got = snapshot(restored);
        for (std::size_t i = 0; i < queries().size(); ++i)
            check(got[i] == want[i], "sql-restore query " + std::to_string(i) + ":\n  want=[" +
                                         want[i] + "]\n  got =[" + got[i] + "]  for: " + queries()[i]);
        // Post-restore DML works; the columnar table re-flushes.
        check(restored.exec("INSERT INTO cust (id, name) VALUES (100, 'zoe')").ok, "post-restore INSERT ok");
        restored.flush_columnar("sale");
        check(restored.exec("SELECT COUNT(*) FROM sale").ok, "post-restore re-flush + query ok");
    }

    // ===== (2) FULL dump -> restore via the framed DISK path =====
    {
        DiskBox dump_disk;
        check(src.dump_sql(dump_disk.sched, dump_disk.disk, SqlBackupScope::Full).ok(), "dump_sql (disk) ok");
        Backing rb;
        SqlEngine restored(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
        check(restored.restore_sql(dump_disk.sched, dump_disk.disk).ok(), "restore_sql (disk) ok");
        check(snapshot(restored) == want, "disk-path sql restore reproduces the source");
    }

    // ===== (3) SCHEMA-ONLY -> restore -> tables exist, NO rows =====
    {
        std::string ddl;
        check(src.dump_sql_string(SqlBackupScope::SchemaOnly, ddl).ok(), "schema-only dump ok");
        check(ddl.find("INSERT INTO") == std::string::npos, "schema-only dump has NO INSERTs");
        Backing rb;
        SqlEngine restored(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
        check(restored.restore_sql_string(ddl).ok(), "schema-only restore ok");
        check(render(restored.exec("SELECT COUNT(*) FROM ord")) == "OK aff=1 | COUNT(*)=0",
              "schema-only: table exists, no rows");
        check(restored.exec("INSERT INTO cust (id, name) VALUES (1, 'ann')").ok,
              "schema-only: post-restore INSERT ok (schema intact)");
        check(restored.exec("SELECT name FROM cust WHERE id = 1").ok,
              "schema-only: indexed query ok (index restored)");
    }

    // ===== (4) FRESH-ENGINE guard =====
    check(!src.restore_sql_string(script).ok(),
          "restore_sql onto a populated engine is rejected (fresh-engine contract)");

    // ===== (5) REFUSAL — an unsupported column type errors (pointing at the binary backup) =====
    {
        Backing db;
        SqlEngine dated(db.sched, db.disk, db.cat_sched, db.cat_disk);
        dated.exec("CREATE TABLE evt (id INT, when_d DATE NOT NULL, PRIMARY KEY (id))");
        std::string s;
        const lockstep::core::Error e = dated.dump_sql_string(SqlBackupScope::SchemaOnly, s);
        check(!e.ok(), "dump_sql refuses a DATE column (unsupported by the text dump)");
        // The binary backup still handles it (full fidelity).
        DiskBox bin;
        check(dated.backup(bin.sched, bin.disk, SqlBackupScope::Full).ok(),
              "binary backup() still covers the unsupported-type table");
    }

    // ===== (6) Framing teeth — bad magic / truncated framed dump is rejected =====
    {
        DiskBox good;
        check(src.dump_sql(good.sched, good.disk, SqlBackupScope::Full).ok(), "framed dump ok");
        std::vector<std::byte> img = good.disk.durable_snapshot();

        std::vector<std::byte> bad = img;
        bad[0] ^= std::byte{0xFF};  // corrupt the "LSQL" magic
        DiskBox bd;
        lockstep::core::Error w{lockstep::core::ErrorCode::Unknown, "norun"};
        bd.sched.spawn(write_raw(bd.disk, std::move(bad), w));
        bd.sched.run();
        Backing rb;
        SqlEngine dst(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
        check(!dst.restore_sql(bd.sched, bd.disk).ok(), "bad-magic framed dump rejected");

        std::vector<std::byte> trunc = img;
        trunc.resize(trunc.size() - 32);  // drop the tail of the script
        DiskBox td;
        lockstep::core::Error w2{lockstep::core::ErrorCode::Unknown, "norun"};
        td.sched.spawn(write_raw(td.disk, std::move(trunc), w2));
        td.sched.run();
        Backing rb2;
        SqlEngine dst2(rb2.sched, rb2.disk, rb2.cat_sched, rb2.cat_disk);
        check(!dst2.restore_sql(td.sched, td.disk).ok(), "truncated framed dump rejected");
    }

    if (g_fail) {
        std::printf("sql_sqldump_test: FAILED\n");
        return 1;
    }
    std::printf("sql_sqldump_test: OK (pg_dump-style text backup: full/schema-only round-trip + disk "
                "framing + FK order + columnar directive + fresh-guard + refusal + teeth)\n");
    return 0;
}
