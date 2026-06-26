// sql_domain_test.cpp — F10 domain constraints (all deterministic, checked at coerce):
//   * VARCHAR(n)/BLOB(n) length cap; CHAR(n) right-pads to n.
//   * DECIMAL(p,s) precision (integer part bounded).
//   * UNSIGNED: reject a negative value (INT/BIGINT/INT128/DECIMAL).
//   * TINYINT/SMALLINT/INT32: range-validated INT aliases.
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
    // VARCHAR(n) length + CHAR(n) pad.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, v VARCHAR(5) NOT NULL, c CHAR(4), PRIMARY KEY (id))");
        check(e.exec("INSERT INTO t (id, v, c) VALUES (1, 'abc', 'hi')").ok, "fits");
        check(cell0(e.exec("SELECT v FROM t WHERE id = 1")) == "abc", "varchar stored");
        check(cell0(e.exec("SELECT c FROM t WHERE id = 1")) == "hi  ", "CHAR(4) right-padded");
        check(!e.exec("INSERT INTO t (id, v) VALUES (2, 'toolong')").ok, "VARCHAR(5) overflow rejected");
        check(e.exec("INSERT INTO t (id, v) VALUES (3, 'exact')").ok, "exact length ok");
    }
    // DECIMAL(p,s) precision.
    {
        SqlEngine e;
        e.exec("CREATE TABLE d (id INT, m DECIMAL(5,2) NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO d (id, m) VALUES (1, '123.45')").ok, "5-digit decimal fits");
        check(!e.exec("INSERT INTO d (id, m) VALUES (2, '1234.56')").ok,
              "6 significant digits exceeds DECIMAL(5,2)");
        check(cell0(e.exec("SELECT m FROM d WHERE id = 1")) == "123.45", "decimal stored");
    }
    // UNSIGNED on INT / BIGINT.
    {
        SqlEngine e;
        e.exec("CREATE TABLE u (id INT, bal BIGINT UNSIGNED NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO u (id, bal) VALUES (1, 100)").ok, "positive ok");
        check(e.exec("INSERT INTO u (id, bal) VALUES (2, 0)").ok, "zero ok");
        check(!e.exec("INSERT INTO u (id, bal) VALUES (3, -1)").ok, "negative rejected (UNSIGNED)");
    }
    // UNSIGNED on INT128 / DECIMAL128.
    {
        SqlEngine e;
        e.exec("CREATE TABLE w (id INT, bal INT128 UNSIGNED NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO w (id, bal) VALUES (1, '1000000000000000000000000000')").ok,
              "big positive ok");
        check(!e.exec("INSERT INTO w (id, bal) VALUES (2, '-5')").ok, "negative INT128 rejected");
    }
    // integer-width aliases: TINYINT / SMALLINT.
    {
        SqlEngine e;
        e.exec("CREATE TABLE n (id INT, a TINYINT NOT NULL, b SMALLINT NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO n (id, a, b) VALUES (1, 127, 32767)").ok, "max signed 8/16-bit ok");
        check(e.exec("INSERT INTO n (id, a, b) VALUES (2, -128, -32768)").ok, "min signed ok");
        check(!e.exec("INSERT INTO n (id, a, b) VALUES (3, 128, 0)").ok, "TINYINT 128 out of range");
        check(!e.exec("INSERT INTO n (id, a, b) VALUES (4, 0, 40000)").ok, "SMALLINT 40000 out of range");
        // TINYINT UNSIGNED widens the positive range to 0..255.
        e.exec("CREATE TABLE m (id INT, a TINYINT UNSIGNED NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO m (id, a) VALUES (1, 255)").ok, "TINYINT UNSIGNED 255 ok");
        check(!e.exec("INSERT INTO m (id, a) VALUES (2, 256)").ok, "TINYINT UNSIGNED 256 rejected");
        check(!e.exec("INSERT INTO m (id, a) VALUES (3, -1)").ok, "TINYINT UNSIGNED -1 rejected");
    }
    // durable: constraints survive a restart (recovered from the schema).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, v VARCHAR(3) NOT NULL, a TINYINT UNSIGNED NOT NULL, "
                   "PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id, v, a) VALUES (1, 'ok', 200)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(!rec.exec("INSERT INTO t (id, v, a) VALUES (2, 'toolong', 1)").ok,
              "VARCHAR(3) still enforced after recover");
        check(!rec.exec("INSERT INTO t (id, v, a) VALUES (3, 'ok', 300)").ok,
              "TINYINT UNSIGNED still enforced after recover");
        check(rec.exec("INSERT INTO t (id, v, a) VALUES (4, 'ab', 50)").ok, "valid row after recover");
    }
    if (g_fail) { std::printf("sql_domain_test: FAILED\n"); return 1; }
    std::printf("sql_domain_test: OK (VARCHAR/CHAR length, DECIMAL precision, UNSIGNED, "
                "TINYINT/SMALLINT range; durable)\n");
    return 0;
}
