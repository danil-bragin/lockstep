// sql_scalar_funcs_test.cpp — W9: additional pure scalar functions.
//
// NULLIF / GREATEST / LEAST / MOD / SIGN / REVERSE / REPEAT / LEFT / RIGHT / LTRIM /
// RTRIM / STRPOS — deterministic, NULL-propagating (GREATEST/LEAST/NULLIF follow the
// PostgreSQL NULL rules). Verified via a single-row scratch table so the function
// evaluates over the normal expression path.
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

namespace {

using lockstep::query::sql::Datum;
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

// Evaluate `expr` and return the single result cell.
Datum val(const std::string& expr) {
    const ExecResult r = g_e->exec("SELECT " + expr + " FROM t WHERE id = 1");
    if (!r.ok || r.rows.size() != 1 || r.rows[0].cells.empty()) return Datum::make_null(lockstep::query::sql::Type::Int);
    return r.rows[0].cells[0].second;
}
std::int64_t vi(const std::string& expr) { return val(expr).i; }
std::string vs(const std::string& expr) { return val(expr).s; }
bool vnull(const std::string& expr) { return val(expr).is_null; }

}  // namespace

int main() {
    std::printf("=== sql_scalar_funcs_test (W9 scalar functions) ===\n");
    SqlEngine e;
    g_e = &e;
    e.exec("CREATE TABLE t (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id) VALUES (1)");

    // NULLIF
    check(vnull("NULLIF(5, 5)"), "NULLIF(5,5) is NULL");
    check(vi("NULLIF(5, 3)") == 5, "NULLIF(5,3) is 5");

    // GREATEST / LEAST (NULLs ignored)
    check(vi("GREATEST(3, 7, 1)") == 7, "GREATEST(3,7,1) = 7");
    check(vi("LEAST(3, 7, 1)") == 1, "LEAST(3,7,1) = 1");
    check(vi("GREATEST(3, NULL, 5)") == 5, "GREATEST ignores NULL");

    // MOD / SIGN
    check(vi("MOD(17, 5)") == 2, "MOD(17,5) = 2");
    check(vi("SIGN(-4)") == -1 && vi("SIGN(0)") == 0 && vi("SIGN(9)") == 1, "SIGN of -/0/+");
    check(!e.exec("SELECT MOD(1, 0) FROM t WHERE id = 1").ok, "MOD by zero errors");

    // String functions
    check(vs("REVERSE('abc')") == "cba", "REVERSE('abc') = 'cba'");
    check(vs("REPEAT('ab', 3)") == "ababab", "REPEAT('ab',3) = 'ababab'");
    check(vs("LEFT('hello', 3)") == "hel", "LEFT('hello',3) = 'hel'");
    check(vs("RIGHT('hello', 2)") == "lo", "RIGHT('hello',2) = 'lo'");
    check(vs("LTRIM('  hi')") == "hi", "LTRIM trims leading spaces");
    check(vs("RTRIM('hi  ')") == "hi", "RTRIM trims trailing spaces");
    check(vi("STRPOS('hello', 'll')") == 3, "STRPOS('hello','ll') = 3 (1-based)");
    check(vi("STRPOS('hello', 'z')") == 0, "STRPOS not found = 0");

    // SPLIT_PART / INITCAP / LPAD / RPAD / ASCII / CHR
    check(vs("SPLIT_PART('a,b,c', ',', 2)") == "b", "SPLIT_PART field 2 = 'b'");
    check(vs("SPLIT_PART('a,b,c', ',', 9)") == "", "SPLIT_PART out of range = ''");
    check(vs("INITCAP('hello world')") == "Hello World", "INITCAP capitalises each word");
    check(vs("LPAD('7', 3, '0')") == "007", "LPAD pads left with '0'");
    check(vs("RPAD('7', 3, '0')") == "700", "RPAD pads right with '0'");
    check(vs("LPAD('hello', 3)") == "hel", "LPAD truncates to length");
    check(vi("ASCII('A')") == 65, "ASCII('A') = 65");
    check(vs("CHR(65)") == "A", "CHR(65) = 'A'");

    if (g_fail != 0) {
        std::printf("sql_scalar_funcs_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_scalar_funcs_test: ALL PASS\n");
    return 0;
}
