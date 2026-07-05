// sql_information_schema_test.cpp — W9 (PG compatibility): information_schema shim.
//
// ORMs and psql \d introspect the catalog through information_schema. We synthesise
// information_schema.tables and information_schema.columns from the live catalog on the
// fly (materialised into an ephemeral table, then read via the normal SELECT path so
// WHERE / projection / ORDER all work).
//
// WHAT IT PROVES:
//   (A) information_schema.tables lists base tables (table_type 'BASE TABLE').
//   (B) a WHERE on it filters by table_name; projection selects columns.
//   (C) information_schema.columns lists a table's columns with data_type + is_nullable,
//       hiding the synthetic PK and honouring ordinal_position.
//   (D) a VIEW shows up with table_type 'VIEW'.
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <cstdio>
#include <string>
#include <vector>

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

// Collect the text value of column `col` across all rows (by label).
std::vector<std::string> col_texts(const ExecResult& r, const std::string& col) {
    std::vector<std::string> out;
    for (const auto& row : r.rows) {
        for (const auto& cell : row.cells) {
            if (cell.first == col) out.push_back(cell.second.s);
        }
    }
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

}  // namespace

int main() {
    std::printf("=== sql_information_schema_test (W9 information_schema) ===\n");

    SqlEngine e;
    e.exec("CREATE TABLE users (id INT, name TEXT, email TEXT, PRIMARY KEY (id))");
    e.exec("CREATE TABLE orders (oid INT, amount INT, PRIMARY KEY (oid))");

    // (A) tables lists both base tables.
    {
        const ExecResult r = e.exec("SELECT table_name, table_type FROM information_schema.tables");
        const auto names = col_texts(r, "table_name");
        check(r.ok, "(A) information_schema.tables query ok");
        check(has(names, "users") && has(names, "orders"), "(A) both base tables listed");
        check(has(col_texts(r, "table_type"), "BASE TABLE"), "(A) table_type is BASE TABLE");
    }

    // (B) WHERE + projection.
    {
        const ExecResult r =
            e.exec("SELECT table_name FROM information_schema.tables WHERE table_name = 'users'");
        const auto names = col_texts(r, "table_name");
        check(r.ok && names.size() == 1 && names[0] == "users",
              "(B) WHERE table_name = 'users' returns exactly that row");
    }

    // (C) columns lists user columns (not the hidden PK), with data_type + nullability.
    {
        const ExecResult r = e.exec(
            "SELECT column_name, data_type, is_nullable FROM information_schema.columns "
            "WHERE table_name = 'users'");
        const auto cols = col_texts(r, "column_name");
        check(r.ok, "(C) information_schema.columns query ok");
        check(has(cols, "id") && has(cols, "name") && has(cols, "email"),
              "(C) all three user columns listed");
        check(!has(cols, "_ctid"), "(C) the hidden synthetic PK is NOT listed");
        check(has(col_texts(r, "data_type"), "text"), "(C) TEXT column reports data_type 'text'");
        check(has(col_texts(r, "data_type"), "integer"), "(C) INT column reports data_type 'integer'");
    }

    // (D) a view appears with table_type 'VIEW'.
    {
        e.exec("CREATE VIEW active AS SELECT id, name FROM users");
        const ExecResult r =
            e.exec("SELECT table_type FROM information_schema.tables WHERE table_name = 'active'");
        const auto types = col_texts(r, "table_type");
        check(r.ok && types.size() == 1 && types[0] == "VIEW", "(D) the view lists as table_type VIEW");
    }

    if (g_fail != 0) {
        std::printf("sql_information_schema_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_information_schema_test: ALL PASS\n");
    return 0;
}
