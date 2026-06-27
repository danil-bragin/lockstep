// sql_interval_month_test.cpp — F13b: INTERVAL year/month support. A pure year/month interval is a
// MONTH-INTERVAL (logical 12, the int counts months); pure day/time intervals stay seconds (logical
// 10, unchanged). DATE / TIMESTAMP ± a month interval does CALENDAR arithmetic (day clamped to the
// target month's length); month intervals add/subtract/scale; mixing months with days in ONE literal
// is rejected (no composite representation). Deterministic (proleptic-Gregorian civil math).
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string val(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : "<ERR>";
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, d DATE, ts TIMESTAMP, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,d,ts) VALUES (1,'2020-01-31','2020-01-31 12:30:00')");

    // DATE ± month interval with end-of-month day clamping.
    check(val(e, "SELECT d + INTERVAL '1 month' FROM t") == "2020-02-29", "Jan31 +1mon => Feb29 (leap clamp)");
    check(val(e, "SELECT d + INTERVAL '1 year' FROM t") == "2021-01-31", "+1 year");
    check(val(e, "SELECT d + INTERVAL '13 months' FROM t") == "2021-02-28", "+13 months => Feb28 (non-leap)");
    check(val(e, "SELECT d - INTERVAL '2 months' FROM t") == "2019-11-30", "-2 months => Nov30 (clamp)");
    check(val(e, "SELECT d + INTERVAL '1 year 2 months' FROM t") == "2021-03-31", "+1y2m => Mar31");
    check(val(e, "SELECT d - INTERVAL '14 months' FROM t") == "2018-11-30", "-14 months");

    // TIMESTAMP keeps the clock part across a month add.
    check(val(e, "SELECT ts + INTERVAL '1 month' FROM t") == "2020-02-29 12:30:00", "TIMESTAMP +1mon keeps clock");
    check(val(e, "SELECT ts + INTERVAL '2 years' FROM t") == "2022-01-31 12:30:00", "TIMESTAMP +2 years");

    // Month-interval values render + arithmetic (normalize to years+months).
    check(val(e, "SELECT INTERVAL '2 years' FROM t") == "2 years", "render 2 years");
    check(val(e, "SELECT INTERVAL '18 months' FROM t") == "1 year 6 mons", "render 18 months => 1 year 6 mons");
    check(val(e, "SELECT INTERVAL '1 year' + INTERVAL '6 months' FROM t") == "1 year 6 mons", "interval+interval");
    check(val(e, "SELECT INTERVAL '24 months' - INTERVAL '1 year' FROM t") == "1 year", "interval-interval");
    check(val(e, "SELECT INTERVAL '2 months' * 3 FROM t") == "6 mons", "interval * int");
    check(val(e, "SELECT INTERVAL '1 mon' FROM t") == "1 mon", "abbrev 'mon'");

    // Plain second/day intervals are untouched (logical 10, seconds).
    check(val(e, "SELECT INTERVAL '90 seconds' FROM t") == "0d 00:01:30", "seconds interval unchanged");
    check(e.exec("SELECT d + INTERVAL '10 days' FROM t").ok, "DATE + day interval still works");

    // A literal mixing months with days/time is rejected (no composite type).
    check(!e.exec("SELECT INTERVAL '1 month 3 days' FROM t").ok, "month+day literal rejected");
    check(!e.exec("SELECT INTERVAL '1 year 1 hour' FROM t").ok, "year+time literal rejected");

    // A month interval can't be added to a TIME-of-day or another seconds interval at runtime.
    check(!e.exec("SELECT INTERVAL '1 month' + INTERVAL '1 day' FROM t").ok, "month + day interval rejected");

    if (g_fail) { std::printf("sql_interval_month_test: FAILED\n"); return 1; }
    std::printf("sql_interval_month_test: OK (INTERVAL year/month: calendar DATE/TIMESTAMP arithmetic "
                "with day clamp, interval add/sub/scale, render; seconds intervals unchanged)\n");
    return 0;
}
