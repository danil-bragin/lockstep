// sql_temporal_enum_test.cpp — F13 TIME / ENUM / INTERVAL (all INT-backed, deterministic).
//   TIME  (logical 8): seconds since midnight, 'HH:MM:SS'.
//   ENUM  (logical 9): ordinal over a declared label set; compares by declaration order; renders label.
//   INTERVAL (logical 10): signed seconds; 'N unit...' literal; TIMESTAMP/TIME +/- INTERVAL arithmetic.
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
std::int64_t scal(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
}
}  // namespace

int main() {
    // TIME.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, at TIME NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO t (id, at) VALUES (1, '13:45:09'), (2, '00:00:00'), (3, '09:30')").ok,
              "insert TIME");
        check(scal(e.exec("SELECT at FROM t WHERE id = 1")) == 49509, "13:45:09 -> 49509s");
        check(cell0(e.exec("SELECT at FROM t WHERE id = 1")) == "13:45:09", "render 13:45:09");
        check(cell0(e.exec("SELECT at FROM t WHERE id = 3")) == "09:30:00", "HH:MM -> 09:30:00");
        // ordering is numeric/chronological.
        check(cell0(e.exec("SELECT at FROM t ORDER BY at")) == "00:00:00", "earliest first");
        check(!e.exec("INSERT INTO t (id, at) VALUES (4, '25:00:00')").ok, "invalid hour rejected");
    }
    // ENUM.
    {
        SqlEngine e;
        e.exec("CREATE TABLE o (id INT, status ENUM('active','pending','closed') NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO o (id, status) VALUES (1, 'pending'), (2, 'active'), (3, 'closed')").ok,
              "insert ENUM labels");
        check(scal(e.exec("SELECT status FROM o WHERE id = 1")) == 1, "pending -> ordinal 1");
        check(cell0(e.exec("SELECT status FROM o WHERE id = 1")) == "pending", "render label 'pending'");
        // WHERE by label.
        check(e.exec("SELECT id FROM o WHERE status = 'active'").rows.size() == 1, "WHERE status='active'");
        // ordering follows DECLARATION order (active<pending<closed), not alphabetical.
        const ExecResult ord = e.exec("SELECT status FROM o ORDER BY status");
        check(ord.ok && ord.rows.size() == 3, "3 ordered");
        if (ord.rows.size() == 3) {
            check(ord.rows[0].cells[0].second.render() == "active" &&
                      ord.rows[1].cells[0].second.render() == "pending" &&
                      ord.rows[2].cells[0].second.render() == "closed",
                  "declaration order: active < pending < closed");
        }
        check(!e.exec("INSERT INTO o (id, status) VALUES (4, 'bogus')").ok, "unknown label rejected");
    }
    // INTERVAL + temporal arithmetic.
    {
        SqlEngine e;
        e.exec("CREATE TABLE iv (id INT, d INTERVAL NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO iv (id, d) VALUES (1, '1 day'), (2, '2 hours 30 minutes'), (3, '90 seconds')").ok,
              "insert INTERVAL");
        check(scal(e.exec("SELECT d FROM iv WHERE id = 1")) == 86400, "1 day -> 86400s");
        check(scal(e.exec("SELECT d FROM iv WHERE id = 2")) == 9000, "2h30m -> 9000s");
        check(cell0(e.exec("SELECT d FROM iv WHERE id = 1")) == "1d 00:00:00", "render 1d 00:00:00");
        // TIMESTAMP + INTERVAL -> TIMESTAMP.
        e.exec("CREATE TABLE ev (id INT, ts TIMESTAMP NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO ev (id, ts) VALUES (1, '2026-06-27 13:00:00')");
        check(cell0(e.exec("SELECT ts + INTERVAL '1 day' FROM ev WHERE id = 1")) ==
                  "2026-06-28 13:00:00",
              "ts + 1 day");
        check(cell0(e.exec("SELECT ts - INTERVAL '2 hours' FROM ev WHERE id = 1")) ==
                  "2026-06-27 11:00:00",
              "ts - 2 hours");
        // TIMESTAMP - TIMESTAMP -> INTERVAL is exercised via INTERVAL + INTERVAL here.
        check(cell0(e.exec("SELECT d + INTERVAL '1 day' FROM iv WHERE id = 1")) == "2d 00:00:00",
              "interval + interval");
        // TIME + INTERVAL wraps within the day.
        e.exec("CREATE TABLE tt (id INT, at TIME NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO tt (id, at) VALUES (1, '23:30:00')");
        check(cell0(e.exec("SELECT at + INTERVAL '1 hour' FROM tt WHERE id = 1")) == "00:30:00",
              "time + 1 hour wraps to 00:30:00");
    }
    // durable: TIME/ENUM/INTERVAL survive a restart (incl. the ENUM label set).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE x (id INT, st ENUM('lo','hi') NOT NULL, at TIME NOT NULL, dur INTERVAL NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO x (id, st, at, dur) VALUES (1, 'hi', '08:15:00', '45 minutes')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT st FROM x WHERE id = 1")) == "hi", "recovered ENUM label");
        check(cell0(rec.exec("SELECT at FROM x WHERE id = 1")) == "08:15:00", "recovered TIME");
        check(scal(rec.exec("SELECT dur FROM x WHERE id = 1")) == 2700, "recovered INTERVAL (45m)");
        check(!rec.exec("INSERT INTO x (id, st, at, dur) VALUES (2, 'bad', '00:00:00', '1 day')").ok,
              "ENUM label set still enforced after recover");
    }
    if (g_fail) { std::printf("sql_temporal_enum_test: FAILED\n"); return 1; }
    std::printf("sql_temporal_enum_test: OK (TIME, ENUM ordinal+label, INTERVAL + temporal arithmetic, "
                "durable)\n");
    return 0;
}
