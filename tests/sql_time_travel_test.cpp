// sql_time_travel_test.cpp — K10 TIME TRAVEL gate: SELECT ... AS OF SEQ n.
//
// AS OF SEQ n is a PostgreSQL/Cockroach-style alias for the existing AT SNAPSHOT n
// (MVCC read as-of a committed version). Each committed write advances the version
// (Seq) deterministically, so a query AS OF an earlier version observes the value
// that was current then — "query the database as of just before the bad write".
//
// WHAT IT PROVES:
//   (A) AS OF SEQ n sees the as-of-n value; the tip sees the latest.
//   (B) the optional keyword (SEQ / SNAPSHOT / VERSION / none) all parse the same.
//   (C) AS OF is exactly equivalent to AT SNAPSHOT (same rows).
//   (D) a bad AS OF (negative / non-integer) is a parse error.
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

// The single visible text cell of a one-row result (empty if not a single row).
std::string one_text(const ExecResult& r) {
    if (!r.ok || r.rows.size() != 1) return "";
    for (const auto& cell : r.rows[0].cells) {
        if (cell.second.type == lockstep::query::sql::Type::Text) return cell.second.s;
    }
    return "";
}

}  // namespace

int main() {
    std::printf("=== sql_time_travel_test (K10 AS OF SEQ) ===\n");

    SqlEngine e;
    e.exec("CREATE TABLE kv (id INT, name TEXT, PRIMARY KEY (id))");
    e.exec("INSERT INTO kv (id, name) VALUES (1, 'first')");   // commit -> version 1
    e.exec("UPDATE kv SET name = 'second' WHERE id = 1");        // commit -> version 2

    // (A) AS OF SEQ 1 sees 'first'; the tip sees 'second'.
    check(one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AS OF SEQ 1")) == "first",
          "(A) AS OF SEQ 1 observes the as-of-version-1 value 'first'");
    check(one_text(e.exec("SELECT id, name FROM kv WHERE id = 1")) == "second",
          "(A) the tip observes the latest value 'second'");

    // (B) the version keyword is optional / interchangeable.
    check(one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AS OF 1")) == "first",
          "(B) AS OF 1 (no keyword) observes 'first'");
    check(one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AS OF SNAPSHOT 1")) == "first",
          "(B) AS OF SNAPSHOT 1 observes 'first'");
    check(one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AS OF VERSION 1")) == "first",
          "(B) AS OF VERSION 1 observes 'first'");

    // (C) AS OF SEQ n is exactly equivalent to AT SNAPSHOT n.
    const std::string as_of = one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AS OF SEQ 1"));
    const std::string at_snap = one_text(e.exec("SELECT id, name FROM kv WHERE id = 1 AT SNAPSHOT 1"));
    check(as_of == at_snap && as_of == "first",
          "(C) AS OF SEQ 1 == AT SNAPSHOT 1 (both 'first')");

    // (D) malformed AS OF is a parse error.
    check(!e.exec("SELECT id FROM kv AS OF SEQ -1").ok, "(D) negative AS OF is rejected");
    check(!e.exec("SELECT id FROM kv AS OF SEQ abc").ok, "(D) non-integer AS OF is rejected");

    if (g_fail != 0) {
        std::printf("sql_time_travel_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_time_travel_test: ALL PASS\n");
    return 0;
}
