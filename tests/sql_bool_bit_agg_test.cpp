// sql_bool_bit_agg_test.cpp — BOOL_AND / BOOL_OR / BIT_AND / BIT_OR aggregates.
//
// Fold a group's non-NULL INT values (0 = false): BOOL_AND (all nonzero), BOOL_OR (any
// nonzero), BIT_AND / BIT_OR (bitwise). Empty / all-NULL group -> SQL NULL.
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
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

// value of the aggregate cell for the group whose key (cell 0) == key
std::int64_t agg(const ExecResult& r, std::int64_t key) {
    for (const auto& row : r.rows)
        if (!row.cells.empty() && row.cells[0].second.i == key && row.cells.size() >= 2)
            return row.cells[1].second.i;
    return -999;
}

}  // namespace

int main() {
    std::printf("=== sql_bool_bit_agg_test ===\n");
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, g INT NOT NULL, flag INT, bits INT, PRIMARY KEY (id))");
    // group 1: flags 1,1,1 (all true); bits 6,3 -> AND=2, OR=7
    e.exec("INSERT INTO t (id, g, flag, bits) VALUES (1, 1, 1, 6)");
    e.exec("INSERT INTO t (id, g, flag, bits) VALUES (2, 1, 1, 3)");
    e.exec("INSERT INTO t (id, g, flag, bits) VALUES (3, 1, 1, 6)");
    // group 2: flags 1,0 (mixed); bits 12,10 -> AND=8, OR=14
    e.exec("INSERT INTO t (id, g, flag, bits) VALUES (4, 2, 1, 12)");
    e.exec("INSERT INTO t (id, g, flag, bits) VALUES (5, 2, 0, 10)");

    {
        const ExecResult r = e.exec("SELECT g, BOOL_AND(flag) FROM t GROUP BY g");
        check(agg(r, 1) == 1, "BOOL_AND group1 (all true) = 1");
        check(agg(r, 2) == 0, "BOOL_AND group2 (mixed) = 0");
    }
    {
        const ExecResult r = e.exec("SELECT g, BOOL_OR(flag) FROM t GROUP BY g");
        check(agg(r, 1) == 1, "BOOL_OR group1 = 1");
        check(agg(r, 2) == 1, "BOOL_OR group2 (has a true) = 1");
    }
    {
        const ExecResult r = e.exec("SELECT g, BIT_AND(bits) FROM t GROUP BY g");
        check(agg(r, 1) == 2, "BIT_AND group1 (6&3&6) = 2");
        check(agg(r, 2) == 8, "BIT_AND group2 (12&10) = 8");
    }
    {
        const ExecResult r = e.exec("SELECT g, BIT_OR(bits) FROM t GROUP BY g");
        check(agg(r, 1) == 7, "BIT_OR group1 (6|3|6) = 7");
        check(agg(r, 2) == 14, "BIT_OR group2 (12|10) = 14");
    }
    // NULL handling: a group whose values are all NULL -> NULL.
    {
        e.exec("INSERT INTO t (id, g, flag, bits) VALUES (6, 3, NULL, NULL)");
        const ExecResult r = e.exec("SELECT g, BIT_OR(bits) FROM t GROUP BY g");
        for (const auto& row : r.rows)
            if (row.cells[0].second.i == 3)
                check(row.cells[1].second.is_null, "BIT_OR of an all-NULL group is NULL");
    }

    if (g_fail != 0) { std::printf("sql_bool_bit_agg_test: FAILURES\n"); return 1; }
    std::printf("sql_bool_bit_agg_test: ALL PASS\n");
    return 0;
}
