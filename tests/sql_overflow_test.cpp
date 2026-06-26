// sql_overflow_test.cpp — F9d checked int64 arithmetic. INT/BIGINT is 64-bit; an operation whose
// true result exceeds int64 used to be signed-overflow UB (UBSan trap / silent wrap). The row-mode
// expression + aggregate paths now raise a clean "integer overflow" error instead. A literal beyond
// int64 saturates at parse (parse_i64). Non-overflowing arithmetic is unchanged.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
const char* MAX = "9223372036854775807";  // INT64_MAX
}  // namespace

int main() {
    SqlEngine e;
    check(e.exec("CREATE TABLE t (id INT, v BIGINT NOT NULL, PRIMARY KEY (id))").ok, "create");
    check(e.exec(std::string("INSERT INTO t (id, v) VALUES (1, ") + MAX + ")").ok, "insert max");
    check(e.exec(std::string("INSERT INTO t (id, v) VALUES (2, ") + MAX + ")").ok, "insert max #2");

    // expression overflow: max + max, max * 2 -> error (not UB, not a wrapped value).
    {
        const ExecResult r = e.exec("SELECT v + v FROM t WHERE id = 1");
        check(!r.ok && r.error.find("overflow") != std::string::npos, "v+v overflow errors");
    }
    {
        const ExecResult r = e.exec("SELECT v * v FROM t WHERE id = 1");
        check(!r.ok && r.error.find("overflow") != std::string::npos, "v*v overflow errors");
    }
    // aggregate overflow: SUM(max, max) -> error.
    {
        const ExecResult r = e.exec("SELECT SUM(v) FROM t");
        check(!r.ok && r.error.find("overflow") != std::string::npos, "SUM overflow errors");
    }
    // non-overflow arithmetic is unaffected.
    {
        SqlEngine e2;
        e2.exec("CREATE TABLE u (id INT, a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (id))");
        e2.exec("INSERT INTO u (id, a, b) VALUES (1, 1000000, 2000000)");
        const ExecResult r = e2.exec("SELECT a + b, a * b, b - a FROM u WHERE id = 1");
        check(r.ok && r.rows.size() == 1, "normal arithmetic ok");
        if (r.ok && !r.rows.empty()) {
            const auto& c = r.rows[0].cells;
            check(c[0].second.i == 3000000 && c[1].second.i == 2000000000000LL &&
                      c[2].second.i == 1000000,
                  "normal arithmetic values correct");
        }
        const ExecResult s = e2.exec("SELECT SUM(a), SUM(b) FROM u");
        check(s.ok && s.rows[0].cells[0].second.i == 1000000, "normal SUM ok");
    }
    // F11: a bare literal beyond int64 into a BIGINT column is REJECTED (clean type error — no
    // silent saturation). A value that fits is still accepted unchanged. (For values past int64 use
    // an INT128 column; see sql_int128_test.)
    {
        SqlEngine e3;
        e3.exec("CREATE TABLE s (id INT, v BIGINT NOT NULL, PRIMARY KEY (id))");
        check(!e3.exec("INSERT INTO s (id, v) VALUES (1, 99999999999999999999)").ok,
              "huge bare literal rejected for BIGINT (no silent saturate)");
        check(e3.exec("INSERT INTO s (id, v) VALUES (2, 9223372036854775807)").ok, "INT64_MAX fits");
        const ExecResult r = e3.exec("SELECT v FROM s WHERE id = 2");
        check(r.ok && !r.rows.empty() && r.rows[0].cells[0].second.i == 9223372036854775807LL,
              "INT64_MAX stored exactly");
    }
    // DECIMAL value out of range is rejected (no UB in the scale multiply).
    {
        SqlEngine e4;
        e4.exec("CREATE TABLE d (id INT, m DECIMAL(18,6) NOT NULL, PRIMARY KEY (id))");
        const ExecResult r =
            e4.exec("INSERT INTO d (id, m) VALUES (1, 99999999999999)");  // *1e6 overflows int64
        check(!r.ok && r.error.find("range") != std::string::npos, "DECIMAL out-of-range rejected");
    }
    if (g_fail) { std::printf("sql_overflow_test: FAILED\n"); return 1; }
    std::printf("sql_overflow_test: OK (checked int64: expr/SUM overflow -> error; bare literal past "
                "int64 rejected; normal arithmetic unaffected)\n");
    return 0;
}
