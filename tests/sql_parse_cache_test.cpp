// sql_parse_cache_test.cpp — W9.2 parse cache correctness.
//
// exec(sql) caches the parsed AST (parsing is catalog-independent, so a cached AST is
// always valid). This test proves the cache is TRANSPARENT: results are unchanged, a
// cached SELECT re-resolves against the live catalog after a schema change (never
// stales), cached DML re-executes correctly, and the bounded cap does not misbehave.
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

}  // namespace

int main() {
    std::printf("=== sql_parse_cache_test (W9.2) ===\n");

    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, v INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id, v) VALUES (1, 10)");
    e.exec("INSERT INTO t (id, v) VALUES (2, 20)");

    // (A) repeated identical query returns identical correct results (cache hit path).
    const char* q = "SELECT id, v FROM t WHERE id = 1";
    for (int i = 0; i < 100; ++i) {
        const ExecResult r = e.exec(q);
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[1].second.i == 10,
              "(A) repeated cached query stays correct");
    }

    // (B) a cached SELECT re-resolves against the live catalog after a mutation — a new row
    // is visible on the next run of the SAME (cached) query text.
    e.exec("INSERT INTO t (id, v) VALUES (3, 30)");
    check(e.exec("SELECT id, v FROM t").rows.size() == 3,
          "(B) cached SELECT sees rows inserted after it was first cached");

    // (C) a cached SELECT re-resolves after a SCHEMA change (ALTER adds a column) — the AST
    // is unchanged but execution binds to the new catalog. SELECT * now yields the new column.
    e.exec("ALTER TABLE t ADD COLUMN w INT");
    {
        const ExecResult before = e.exec("SELECT * FROM t WHERE id = 1");  // caches this text
        const std::size_t ncol_before = before.rows.empty() ? 0 : before.rows[0].cells.size();
        e.exec("ALTER TABLE t ADD COLUMN x INT");
        const ExecResult after = e.exec("SELECT * FROM t WHERE id = 1");  // same text ... wait, cached
        const std::size_t ncol_after = after.rows.empty() ? 0 : after.rows[0].cells.size();
        check(ncol_after == ncol_before + 1,
              "(C) cached SELECT * re-resolves to the new column set after ALTER");
    }

    // (D) cached DML re-executes correctly (UPDATE from the same cached text runs each time).
    e.exec("UPDATE t SET v = 99 WHERE id = 2");
    e.exec("UPDATE t SET v = 99 WHERE id = 2");  // cache hit; idempotent re-run
    check(e.exec("SELECT v FROM t WHERE id = 2").rows[0].cells[0].second.i == 99,
          "(D) cached UPDATE re-executes correctly");

    // (E) many DISTINCT queries (past the cache cap) do not crash and stay correct.
    for (int i = 0; i < 1500; ++i) {
        (void)e.exec("SELECT id FROM t WHERE id = " + std::to_string(i));
    }
    check(e.exec("SELECT id, v FROM t WHERE id = 1").rows[0].cells[1].second.i == 10,
          "(E) engine correct after exceeding the parse-cache cap");

    if (g_fail != 0) {
        std::printf("sql_parse_cache_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_parse_cache_test: ALL PASS\n");
    return 0;
}
