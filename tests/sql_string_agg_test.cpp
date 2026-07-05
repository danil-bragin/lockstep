// sql_string_agg_test.cpp — STRING_AGG / GROUP_CONCAT aggregate.
//
// STRING_AGG(col, delim) joins a group's non-NULL values (scan order == PK order, so
// deterministic) with the separator; GROUP_CONCAT is the same with a default ",". NULLs
// are skipped; an all-NULL/empty group yields SQL NULL.
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

// The text of the (single) cell in the row whose first cell equals `key`.
std::string agg_for(const ExecResult& r, const std::string& key) {
    for (const auto& row : r.rows) {
        if (!row.cells.empty() && row.cells[0].second.s == key && row.cells.size() >= 2) {
            return row.cells[1].second.s;
        }
    }
    return "<none>";
}

}  // namespace

int main() {
    std::printf("=== sql_string_agg_test (STRING_AGG / GROUP_CONCAT) ===\n");
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, grp TEXT NOT NULL, name TEXT, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id, grp, name) VALUES (1, 'a', 'x')");
    e.exec("INSERT INTO t (id, grp, name) VALUES (2, 'a', 'y')");
    e.exec("INSERT INTO t (id, grp, name) VALUES (3, 'b', 'z')");
    e.exec("INSERT INTO t (id, grp, name) VALUES (4, 'a', 'w')");

    // STRING_AGG with an explicit delimiter (scan/PK order within each group).
    {
        const ExecResult r =
            e.exec("SELECT grp, STRING_AGG(name, '-') FROM t GROUP BY grp");
        check(r.ok, "STRING_AGG query ok");
        check(agg_for(r, "a") == "x-y-w", "STRING_AGG group a = 'x-y-w' (PK order)");
        check(agg_for(r, "b") == "z", "STRING_AGG group b = 'z'");
    }

    // GROUP_CONCAT — default "," separator.
    {
        const ExecResult r = e.exec("SELECT grp, GROUP_CONCAT(name) FROM t GROUP BY grp");
        check(agg_for(r, "a") == "x,y,w", "GROUP_CONCAT group a = 'x,y,w'");
    }

    // NULLs are skipped; an all-NULL group yields NULL.
    {
        e.exec("INSERT INTO t (id, grp, name) VALUES (5, 'c', NULL)");
        e.exec("INSERT INTO t (id, grp, name) VALUES (6, 'a', NULL)");
        const ExecResult r = e.exec("SELECT grp, STRING_AGG(name, ',') FROM t GROUP BY grp");
        check(agg_for(r, "a") == "x,y,w", "STRING_AGG skips NULL (group a unchanged)");
        // group c is all-NULL -> the aggregate cell is NULL (empty rendered text).
        for (const auto& row : r.rows) {
            if (row.cells[0].second.s == "c") {
                check(row.cells[1].second.is_null, "STRING_AGG of an all-NULL group is NULL");
            }
        }
    }

    if (g_fail != 0) {
        std::printf("sql_string_agg_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_string_agg_test: ALL PASS\n");
    return 0;
}
