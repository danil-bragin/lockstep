// sql_matview_test.cpp — CREATE MATERIALIZED VIEW / REFRESH / DROP (K5 non-incremental).
//
// A materialized view is a real table populated from a stored SELECT: it does NOT
// auto-update when the base data changes; REFRESH MATERIALIZED VIEW recomputes it.
// The backing table + the refreshable source both persist (recover on restart).
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <cstdio>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

namespace {

using lockstep::query::sql::ExecResult;
using lockstep::query::sql::SqlEngine;

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

std::int64_t scalar(const ExecResult& r) {
    return (r.ok && r.rows.size() == 1 && !r.rows[0].cells.empty()) ? r.rows[0].cells[0].second.i : -1;
}

void seed(SqlEngine& e) {
    e.exec("CREATE TABLE sales (id INT, region TEXT NOT NULL, amount INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO sales (id, region, amount) VALUES (1, 'east', 100)");
    e.exec("INSERT INTO sales (id, region, amount) VALUES (2, 'east', 200)");
    e.exec("INSERT INTO sales (id, region, amount) VALUES (3, 'west', 50)");
}

}  // namespace

int main() {
    std::printf("=== sql_matview_test (CREATE MATERIALIZED VIEW / REFRESH) ===\n");

    // (A) in-memory: create, stale after a base change, refresh recomputes, drop.
    {
        SqlEngine e;
        seed(e);
        check(e.exec("CREATE MATERIALIZED VIEW region_totals AS "
                     "SELECT region, SUM(amount) AS total FROM sales GROUP BY region").ok,
              "(A) CREATE MATERIALIZED VIEW ok");
        // Query it like a table: east total = 300.
        check(scalar(e.exec("SELECT total FROM region_totals WHERE region = 'east'")) == 300,
              "(A) matview has the materialized east total (300)");

        // Change the base data — the matview is NOT auto-updated (still 300).
        e.exec("INSERT INTO sales (id, region, amount) VALUES (4, 'east', 1000)");
        check(scalar(e.exec("SELECT total FROM region_totals WHERE region = 'east'")) == 300,
              "(A) matview is stale after a base insert (still 300)");

        // REFRESH recomputes it -> 1300.
        check(e.exec("REFRESH MATERIALIZED VIEW region_totals").ok, "(A) REFRESH ok");
        check(scalar(e.exec("SELECT total FROM region_totals WHERE region = 'east'")) == 1300,
              "(A) after REFRESH east total = 1300");

        // REFRESH of a non-matview errors; DROP removes it.
        check(!e.exec("REFRESH MATERIALIZED VIEW sales").ok, "(A) REFRESH of a plain table errors");
        check(e.exec("DROP MATERIALIZED VIEW region_totals").ok, "(A) DROP MATERIALIZED VIEW ok");
        check(!e.exec("SELECT total FROM region_totals").ok, "(A) matview gone after DROP");
        check(!e.exec("REFRESH MATERIALIZED VIEW region_totals").ok, "(A) REFRESH after DROP errors");
    }

    // (B) durability: a matview + its source recover across a restart, and REFRESH still works.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x5A1E5ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        {
            SqlEngine e(sched, data, sched, cat);
            seed(e);
            e.exec("CREATE MATERIALIZED VIEW mv AS SELECT region, SUM(amount) AS total FROM sales GROUP BY region");
        }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            // The materialized data recovered (table): east = 300.
            check(scalar(e.exec("SELECT total FROM mv WHERE region = 'east'")) == 300,
                  "(B) matview data recovered after restart");
            // REFRESH still works (source recovered) after a base change.
            e.exec("INSERT INTO sales (id, region, amount) VALUES (9, 'east', 7)");
            check(e.exec("REFRESH MATERIALIZED VIEW mv").ok, "(B) REFRESH works after restart");
            check(scalar(e.exec("SELECT total FROM mv WHERE region = 'east'")) == 307,
                  "(B) REFRESH recomputed to 307 after restart");
        }
    }

    if (g_fail != 0) { std::printf("sql_matview_test: FAILURES\n"); return 1; }
    std::printf("sql_matview_test: ALL PASS\n");
    return 0;
}
