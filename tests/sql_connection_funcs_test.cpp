// sql_connection_funcs_test.cpp — W9 (PG compatibility): session/connection functions.
//
// Drivers and ORMs issue SELECT version() / current_database() / current_schema() /
// current_user() during the connection handshake and for feature detection. These are
// zero-arg, pure, constant-per-session scalar functions.
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

// The single text cell of a one-row, one-column result.
std::string scalar_text(const ExecResult& r) {
    if (!r.ok || r.rows.size() != 1 || r.rows[0].cells.empty()) return "";
    return r.rows[0].cells[0].second.s;
}

}  // namespace

int main() {
    std::printf("=== sql_connection_funcs_test (W9 session functions) ===\n");

    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id) VALUES (1)");

    // version() reports a PostgreSQL-compatible string (drivers parse 'PostgreSQL <major>').
    const std::string v = scalar_text(e.exec("SELECT version() FROM t WHERE id = 1"));
    check(v.rfind("PostgreSQL ", 0) == 0, "version() starts with 'PostgreSQL '");

    check(scalar_text(e.exec("SELECT current_database() FROM t WHERE id = 1")) == "lockstep",
          "current_database() is 'lockstep'");
    check(scalar_text(e.exec("SELECT current_catalog() FROM t WHERE id = 1")) == "lockstep",
          "current_catalog() is 'lockstep'");
    check(scalar_text(e.exec("SELECT current_schema() FROM t WHERE id = 1")) == "public",
          "current_schema() is 'public'");
    check(scalar_text(e.exec("SELECT current_user() FROM t WHERE id = 1")) == "lockstep",
          "current_user() is 'lockstep'");
    check(scalar_text(e.exec("SELECT session_user() FROM t WHERE id = 1")) == "lockstep",
          "session_user() is 'lockstep'");

    if (g_fail != 0) {
        std::printf("sql_connection_funcs_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_connection_funcs_test: ALL PASS\n");
    return 0;
}
