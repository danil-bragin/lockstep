// sql_date_part_test.cpp — W9: DATE_PART('field', date|timestamp) component extraction.
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

namespace {

using lockstep::query::sql::ExecResult;
using lockstep::query::sql::SqlEngine;

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

SqlEngine* g_e = nullptr;
std::int64_t part(const std::string& field) {
    const ExecResult r =
        g_e->exec("SELECT DATE_PART('" + field + "', d) FROM ev WHERE id = 1");
    if (!r.ok || r.rows.size() != 1 || r.rows[0].cells.empty()) return -999999;
    return r.rows[0].cells[0].second.i;
}
std::int64_t tpart(const std::string& field) {
    const ExecResult r =
        g_e->exec("SELECT DATE_PART('" + field + "', ts) FROM ev WHERE id = 1");
    if (!r.ok || r.rows.size() != 1 || r.rows[0].cells.empty()) return -999999;
    return r.rows[0].cells[0].second.i;
}

}  // namespace

int main() {
    std::printf("=== sql_date_part_test (W9 DATE_PART) ===\n");
    SqlEngine e;
    g_e = &e;
    e.exec("CREATE TABLE ev (id INT, d DATE NOT NULL, ts TIMESTAMP NOT NULL, PRIMARY KEY (id))");
    // 2026-06-27 is a Saturday (dow 6); it is the 178th day of 2026.
    e.exec("INSERT INTO ev (id, d, ts) VALUES (1, '2026-06-27', '2026-06-27 13:45:09')");

    // DATE components.
    check(part("year") == 2026, "DATE_PART year = 2026");
    check(part("month") == 6, "DATE_PART month = 6");
    check(part("day") == 27, "DATE_PART day = 27");
    check(part("dow") == 6, "DATE_PART dow = 6 (Saturday)");
    check(part("doy") == 178, "DATE_PART doy = 178");
    check(part("hour") == 0, "DATE_PART hour of a bare DATE = 0");

    // TIMESTAMP components (13:45:09).
    check(tpart("year") == 2026, "TIMESTAMP year = 2026");
    check(tpart("hour") == 13, "TIMESTAMP hour = 13");
    check(tpart("minute") == 45, "TIMESTAMP minute = 45");
    check(tpart("second") == 9, "TIMESTAMP second = 9");

    // EXTRACT(field FROM expr) — SQL-standard syntax, same result as DATE_PART.
    check(e.exec("SELECT EXTRACT(YEAR FROM d) FROM ev WHERE id = 1").rows[0].cells[0].second.i == 2026,
          "EXTRACT(YEAR FROM date) = 2026");
    check(e.exec("SELECT EXTRACT(HOUR FROM ts) FROM ev WHERE id = 1").rows[0].cells[0].second.i == 13,
          "EXTRACT(HOUR FROM timestamp) = 13");
    check(e.exec("SELECT EXTRACT(dow FROM d) FROM ev WHERE id = 1").rows[0].cells[0].second.i == 6,
          "EXTRACT(dow FROM date) = 6");

    // Bad field is an error; a non-date argument is an error.
    check(!e.exec("SELECT DATE_PART('century', d) FROM ev WHERE id = 1").ok,
          "unknown DATE_PART field errors");
    check(!e.exec("SELECT DATE_PART('year', id) FROM ev WHERE id = 1").ok,
          "DATE_PART on a plain INT errors");

    if (g_fail != 0) {
        std::printf("sql_date_part_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_date_part_test: ALL PASS\n");
    return 0;
}
