// sql_real_test.cpp — F14: the REAL (double-precision float) column type.
//
// REAL is a logical=14 value carried as an 8-byte IEEE-754 payload over TEXT physical storage
// (like DECIMAL128's 16-byte payload). It stores, renders (shortest round-trip via to_chars),
// compares NUMERICALLY (negatives ordered correctly, not by raw bit pattern), and is durable.
// This first slice covers store / render / WHERE / ORDER BY / durability — no arithmetic yet.
//
// Non-provider TU where in-memory; the durability case uses the sim providers.

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
    return (r.ok && r.rows.size() == 1 && !r.rows[0].cells.empty()) ? r.rows[0].cells[0].second.render() : "?";
}
}  // namespace

int main() {
    std::printf("=== sql_real_test (F14 REAL type) ===\n");

    {
        SqlEngine e;
        check(e.exec("CREATE TABLE m (id INT, x REAL NOT NULL, PRIMARY KEY (id))").ok, "CREATE REAL column");
        check(e.exec("INSERT INTO m (id,x) VALUES (1,'3.14'),(2,'2.71'),(3,'-1.5'),(4,'100')").ok,
              "INSERT REAL (quoted string + bare int)");

        // Render: shortest round-trip (no trailing zeros, negatives, integers as-is).
        check(cell0(e.exec("SELECT x FROM m WHERE id = 1")) == "3.14", "render 3.14");
        check(cell0(e.exec("SELECT x FROM m WHERE id = 3")) == "-1.5", "render -1.5");
        check(cell0(e.exec("SELECT x FROM m WHERE id = 4")) == "100", "render 100 (whole)");

        // ORDER BY compares NUMERICALLY — the negative sorts first (a raw bit-pattern compare
        // would put it last). Expected id order: 3 (-1.5), 2 (2.71), 1 (3.14), 4 (100).
        // (The ORDER BY key is projected: this engine sorts on projected columns.)
        const ExecResult o = e.exec("SELECT id, x FROM m ORDER BY x");
        check(o.ok && o.rows.size() == 4, "ORDER BY REAL 4 rows");
        if (o.rows.size() == 4) {
            check(o.rows[0].cells[0].second.i == 3, "ORDER BY: -1.5 first (numeric, not bit-pattern)");
            check(o.rows[1].cells[0].second.i == 2, "ORDER BY: 2.71 second");
            check(o.rows[2].cells[0].second.i == 1, "ORDER BY: 3.14 third");
            check(o.rows[3].cells[0].second.i == 4, "ORDER BY: 100 last");
        }

        // WHERE comparisons against a numeric-string literal.
        const ExecResult w = e.exec("SELECT id FROM m WHERE x > '2.0' ORDER BY id");
        check(w.ok && w.rows.size() == 3, "WHERE x > 2.0 -> {2.71,3.14,100} = 3 rows");
        check(cell0(e.exec("SELECT COUNT(*) FROM m WHERE x = '3.14'")) == "1", "WHERE x = 3.14 exact");
        check(cell0(e.exec("SELECT COUNT(*) FROM m WHERE x < '0'")) == "1", "WHERE x < 0 -> only -1.5");

        // A non-numeric string is a clean error, not a crash / silent 0.
        check(!e.exec("INSERT INTO m (id,x) VALUES (9,'not-a-number')").ok, "invalid REAL literal errors");
    }

    // Bare (unquoted) float literals + REAL arithmetic.
    {
        SqlEngine e;
        e.exec("CREATE TABLE a (id INT, x REAL NOT NULL, y REAL NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO a (id,x,y) VALUES (1, 3.5, 2.0), (2, -1.5, 4.0), (3, 10, 3)").ok,
              "INSERT bare float literals (3.5, -1.5) + bare int (10)");
        // Arithmetic widens a plain INT operand into double; results render shortest round-trip.
        check(cell0(e.exec("SELECT x * 2 FROM a WHERE id = 1")) == "7", "x*2 = 7 (INT widens)");
        check(cell0(e.exec("SELECT x + 0.5 FROM a WHERE id = 1")) == "4", "x + 0.5 = 4");
        check(cell0(e.exec("SELECT x / y FROM a WHERE id = 3")) == "3.3333333333333335", "10/3 double");
        check(cell0(e.exec("SELECT x / y FROM a WHERE id = 2")) == "-0.375", "-1.5/4 = -0.375");
        // Exponent literal.
        check(cell0(e.exec("SELECT x FROM a WHERE id = 1")) == "3.5" &&
                  e.exec("INSERT INTO a (id,x,y) VALUES (4, 1.5e3, 1)").ok,
              "exponent literal 1.5e3 accepted");
        check(cell0(e.exec("SELECT x FROM a WHERE id = 4")) == "1500", "1.5e3 = 1500");
        // Division by zero is a clean error.
        check(!e.exec("SELECT x / 0.0 FROM a WHERE id = 1").ok, "REAL division by zero errors");
        // Expression in WHERE (REAL compared to an INT literal). x*2: 7, -3, 20, 3000 -> >5 keeps 1,3,4.
        const ExecResult w = e.exec("SELECT id FROM a WHERE x * 2 > 5 ORDER BY id");
        check(w.ok && w.rows.size() == 3, "WHERE x*2 > 5 -> id 1 (7), 3 (20), 4 (3000)");
    }

    // REAL aggregates (SUM/AVG fold in double; MIN/MAX via cmp_datum). Row + columnar.
    for (bool columnar : {false, true}) {
        SqlEngine e;
        if (columnar) e.set_columnar_default(true);
        e.exec("CREATE TABLE g (id INT, grp TEXT NOT NULL, x REAL NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO g (id,grp,x) VALUES (1,'a',1.5),(2,'a',2.5),(3,'b',10.0),(4,'b',-4.0)");
        if (columnar) e.flush_columnar("g");
        const std::string tag = columnar ? "columnar " : "row ";
        check(cell0(e.exec("SELECT SUM(x) FROM g")) == "10", tag + "SUM = 10");
        check(cell0(e.exec("SELECT AVG(x) FROM g")) == "2.5", tag + "AVG = 2.5");
        check(cell0(e.exec("SELECT MIN(x) FROM g")) == "-4", tag + "MIN = -4");
        check(cell0(e.exec("SELECT MAX(x) FROM g")) == "10", tag + "MAX = 10");
        const ExecResult gr = e.exec("SELECT grp, SUM(x) FROM g GROUP BY grp ORDER BY grp");
        check(gr.ok && gr.rows.size() == 2 && gr.rows[0].cells[1].second.render() == "4" &&
                  gr.rows[1].cells[1].second.render() == "6",
              tag + "GROUP BY REAL SUM (a=4, b=6)");
    }

    // Durability: a REAL column recovers byte-exact across a restart.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x4EA1ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        {
            SqlEngine e(sched, data, sched, cat);
            e.exec("CREATE TABLE r (id INT, v REAL NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO r (id,v) VALUES (1,'2.5'),(2,'-9.75')");
        }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            check(cell0(e.exec("SELECT v FROM r WHERE id = 1")) == "2.5", "REAL recovered 2.5 after restart");
            check(cell0(e.exec("SELECT v FROM r WHERE id = 2")) == "-9.75", "REAL recovered -9.75 after restart");
        }
    }

    if (g_fail != 0) { std::printf("sql_real_test: FAILURES\n"); return 1; }
    std::printf("sql_real_test: ALL PASS\n");
    return 0;
}
