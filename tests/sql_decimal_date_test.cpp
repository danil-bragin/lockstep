// sql_decimal_date_test.cpp — F9b DECIMAL (scaled int64 fixed-point) + DATE (days since epoch) +
// TIMESTAMP (seconds since epoch). All physically INT, so storage / keys / comparison / ordering are
// byte-deterministic; only literal-parse and render() use the logical tag. Teeth: FLOAT/DOUBLE stay
// rejected; ordering follows numeric value; durable across restart.
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
std::int64_t scalar(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -1;
}
}  // namespace

int main() {
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE acct (id INT, bal DECIMAL(12,2) NOT NULL, PRIMARY KEY (id))").ok,
              "create DECIMAL table");
        check(e.exec("INSERT INTO acct (id, bal) VALUES (1, '10.50'), (2, '3.05'), (3, '100')").ok,
              "insert decimal literals (string + bare int)");
        // Stored as scaled int: 10.50 -> 1050, 3.05 -> 305, 100 -> 10000.
        check(scalar(e.exec("SELECT bal FROM acct WHERE id = 1")) == 1050, "10.50 -> 1050");
        check(scalar(e.exec("SELECT bal FROM acct WHERE id = 3")) == 10000, "bare 100 -> 10000");
        check(cell0(e.exec("SELECT bal FROM acct WHERE id = 1")) == "10.50", "render 10.50");
        check(cell0(e.exec("SELECT bal FROM acct WHERE id = 2")) == "3.05", "render 3.05");
        check(cell0(e.exec("SELECT bal FROM acct WHERE id = 3")) == "100.00", "render 100.00");
        // SUM keeps scale: 1050+305+10000 = 11355 -> "113.55".
        check(cell0(e.exec("SELECT SUM(bal) FROM acct")) == "113.55", "SUM(bal) = 113.55");
        // arithmetic: 10.50 + 3.05 = 13.55 (same scale aligns).
        check(cell0(e.exec("SELECT bal + bal FROM acct WHERE id = 2")) == "6.10", "3.05+3.05=6.10");
        // ordering follows numeric value (raw int): 3.05 < 10.50 < 100.
        const ExecResult ord = e.exec("SELECT bal FROM acct ORDER BY bal");
        check(ord.ok && ord.rows.size() == 3, "3 ordered rows");
        if (ord.rows.size() == 3) {
            check(ord.rows[0].cells[0].second.render() == "3.05" &&
                      ord.rows[2].cells[0].second.render() == "100.00",
                  "ORDER BY bal: 3.05 .. 100.00");
        }
        // negative decimal renders with a leading sign and padded fraction.
        check(e.exec("INSERT INTO acct (id, bal) VALUES (4, '-7.20')").ok, "insert negative");
        check(cell0(e.exec("SELECT bal FROM acct WHERE id = 4")) == "-7.20", "render -7.20");
    }
    // DATE + TIMESTAMP.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE ev (id INT, d DATE, ts TIMESTAMP, PRIMARY KEY (id))").ok,
              "create DATE/TIMESTAMP table");
        check(e.exec("INSERT INTO ev (id, d, ts) VALUES "
                     "(1, '2026-06-27', '2026-06-27 13:45:09')").ok,
              "insert date + timestamp");
        check(cell0(e.exec("SELECT d FROM ev WHERE id = 1")) == "2026-06-27", "render date");
        check(e.exec("SELECT ts FROM ev WHERE id = 1").rows[0].cells[0].second.render() ==
                  "2026-06-27 13:45:09",
              "render timestamp");
        // 1970-01-01 is day 0; the epoch round-trips.
        check(e.exec("INSERT INTO ev (id, d, ts) VALUES (2, '1970-01-01', '1970-01-01 00:00:00')").ok,
              "insert epoch");
        check(scalar(e.exec("SELECT d FROM ev WHERE id = 2")) == 0, "1970-01-01 -> day 0");
        check(cell0(e.exec("SELECT d FROM ev WHERE id = 2")) == "1970-01-01", "render epoch date");
        // DATE comparison is numeric (chronological).
        check(scalar(e.exec("SELECT COUNT(*) FROM ev WHERE d >= '2000-01-01'")) == 1,
              "1 event on/after 2000");
        // malformed literals fail closed.
        check(!e.exec("INSERT INTO ev (id, d, ts) VALUES (3, 'not-a-date', '2026-06-27 00:00:00')").ok,
              "malformed DATE rejected");
    }
    // teeth: FLOAT/DOUBLE still rejected.
    {
        SqlEngine e;
        // F14: FLOAT / DOUBLE are now accepted (REAL, logical=14); see sql_real_test for coverage.
        check(e.exec("CREATE TABLE f (id INT, x FLOAT, PRIMARY KEY (id))").ok, "FLOAT accepted (F14)");
        check(e.exec("CREATE TABLE g (id INT, x DOUBLE, PRIMARY KEY (id))").ok, "DOUBLE accepted (F14)");
    }
    // durable: DECIMAL + DATE survive a restart (schema logical/scale recovered).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE p (id INT, price DECIMAL(10,3), day DATE, PRIMARY KEY (id))");
            e.exec("INSERT INTO p (id, price, day) VALUES (1, '19.990', '2026-06-27')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT price FROM p WHERE id = 1")) == "19.990",
              "recovered DECIMAL render (scale=3)");
        check(rec.exec("SELECT day FROM p WHERE id = 1").rows[0].cells[0].second.render() ==
                  "2026-06-27",
              "recovered DATE render");
    }
    if (g_fail) { std::printf("sql_decimal_date_test: FAILED\n"); return 1; }
    std::printf("sql_decimal_date_test: OK (DECIMAL fixed-point + DATE/TIMESTAMP, durable, "
                "deterministic INT storage)\n");
    return 0;
}
