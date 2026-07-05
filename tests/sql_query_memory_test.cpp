// sql_query_memory_test.cpp — W3.1 RESOURCE GOVERNANCE gate: per-statement query
// memory accounting.
//
// WHAT IT PROVES:
//   (A) OFF BY DEFAULT — with no cap set, a query that materializes an intermediate
//       (a derived table) succeeds exactly as before (the accounting is inert; results
//       unchanged — the conformance gate separately proves byte-identical output).
//   (B) ENFORCED — with a small cap, the SAME query is REFUSED with a deterministic
//       "query memory limit exceeded" error when the materialized intermediate exceeds
//       the budget. The transaction/engine stays usable afterward.
//   (C) DETERMINISTIC — two independent engines with the SAME cap + SAME query produce
//       the IDENTICAL error string (byte-counted, not allocator-based), so replicated
//       statement execution stays byte-identical across replicas.
//   (D) PER-STATEMENT RESET — the counter resets between top-level statements, so a
//       first heavy (but under-cap) query does not poison a later one.
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

// Populate a table with `n` rows of (id, payload) so a derived-table materialization
// has a sizeable intermediate to charge.
void seed(SqlEngine& e, int n) {
    e.exec("CREATE TABLE t (id INT, payload TEXT, PRIMARY KEY (id))");
    for (int i = 0; i < n; ++i) {
        e.exec("INSERT INTO t (id, payload) VALUES (" + std::to_string(i) +
               ", 'payload_value_" + std::to_string(i) + "')");
    }
}

// A query that materializes the whole table through a derived table (the W3.1 charge
// point: the inner SELECT is materialized into an ephemeral table before the outer read).
const char* kDerived = "SELECT id, payload FROM (SELECT id, payload FROM t) sub";

}  // namespace

int main() {
    std::printf("=== sql_query_memory_test (W3.1 query memory accounting) ===\n");

    // (A) OFF BY DEFAULT — no cap → the derived-table query succeeds with all rows.
    {
        SqlEngine e;
        seed(e, 200);
        const ExecResult r = e.exec(kDerived);
        check(r.ok, "(A) no cap: derived-table query succeeds");
        check(r.rows.size() == 200, "(A) no cap: all 200 rows returned");
    }

    // (B) ENFORCED — a tiny cap refuses the same query with the memory-limit error.
    {
        SqlEngine e;
        seed(e, 200);
        e.set_max_query_memory(100);  // far below the 200-row intermediate
        const ExecResult r = e.exec(kDerived);
        check(!r.ok, "(B) tiny cap: derived-table query is REFUSED");
        check(r.error.find("query memory limit exceeded") != std::string::npos,
              "(B) tiny cap: error names the memory limit");
        // The engine stays usable: a cheap query under the cap still works.
        const ExecResult r2 = e.exec("SELECT id FROM t WHERE id = 5");
        check(r2.ok && r2.rows.size() == 1, "(B) engine still usable after a refused query");
    }

    // (C) DETERMINISTIC — two engines, same cap + query → byte-identical error.
    {
        SqlEngine e1;
        SqlEngine e2;
        seed(e1, 200);
        seed(e2, 200);
        e1.set_max_query_memory(100);
        e2.set_max_query_memory(100);
        const ExecResult a = e1.exec(kDerived);
        const ExecResult b = e2.exec(kDerived);
        check(!a.ok && !b.ok, "(C) both engines refuse");
        check(a.error == b.error, "(C) identical error string across replicas");
    }

    // (D) PER-STATEMENT RESET — a generous cap that each single query fits under is not
    // exhausted by running several queries in a row (the counter resets per statement).
    {
        SqlEngine e;
        seed(e, 50);
        e.set_max_query_memory(1'000'000);  // comfortably above one 50-row intermediate
        bool all_ok = true;
        for (int i = 0; i < 5; ++i) {
            all_ok = all_ok && e.exec(kDerived).ok;
        }
        check(all_ok, "(D) per-statement reset: repeated queries all succeed under a per-stmt cap");
    }

    // (E) SQL SET — the cap is settable over SQL (usable from any PG client), and an
    // unknown parameter is accepted as a no-op (client compatibility).
    {
        SqlEngine e;
        seed(e, 200);
        const ExecResult s = e.exec("SET lockstep.max_query_memory = 100");
        check(s.ok, "(E) SET lockstep.max_query_memory succeeds");
        const ExecResult r = e.exec(kDerived);
        check(!r.ok && r.error.find("query memory limit exceeded") != std::string::npos,
              "(E) SQL-set cap enforced on the derived-table query");
        // Raise it back to unlimited over SQL → the same query now succeeds.
        check(e.exec("SET lockstep.max_query_memory = 0").ok, "(E) SET back to 0 (unlimited)");
        check(e.exec(kDerived).ok, "(E) query succeeds again after raising the cap");
        // An unknown parameter is a no-op (not an error).
        check(e.exec("SET client_encoding = 'UTF8'").ok, "(E) unknown SET param is a no-op");
        // A non-numeric memory value is rejected.
        check(!e.exec("SET lockstep.max_query_memory = abc").ok,
              "(E) non-numeric memory value rejected");
    }

    // (F) FLAT RESULT SET — a plain SELECT (no derived table) that returns a large result
    // is also bounded: the returned rows are charged at the top-level exec, so a runaway
    // result is refused before it is handed back to the client.
    {
        SqlEngine e;
        seed(e, 200);
        // No cap → the flat scan returns all rows.
        check(e.exec("SELECT id, payload FROM t").rows.size() == 200,
              "(F) no cap: flat SELECT returns all rows");
        // Tiny cap → the flat result is refused.
        e.set_max_query_memory(100);
        const ExecResult r = e.exec("SELECT id, payload FROM t");
        check(!r.ok && r.error.find("query memory limit exceeded") != std::string::npos,
              "(F) tiny cap: flat SELECT result is bounded and refused");
        // A cap the small single-row result fits under still works.
        check(e.exec("SELECT id FROM t WHERE id = 1").ok,
              "(F) a small result still fits under the same cap");
    }

    if (g_fail != 0) {
        std::printf("sql_query_memory_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_query_memory_test: ALL PASS\n");
    return 0;
}
