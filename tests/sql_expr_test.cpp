// sql_expr_test.cpp — A1 arithmetic + A2 string fns + A3 CASE + A4 CAST in the SELECT projection
// (computed columns), single-table, row + columnar. Exact expected values.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t I(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) { std::printf("  ERR [%s]: %s\n", q.c_str(), r.error.c_str()); return -999; }
    return r.rows.empty() ? -998 : r.rows[0].cells[0].second.i;
}
std::string S(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) return std::string("ERR:") + r.error;
    return r.rows.empty() ? "?" : r.rows[0].cells[0].second.s;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, a INT NOT NULL, b INT NOT NULL, name TEXT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO t (id,a,b,name) VALUES (1, 7, 3, 'Hello')");
    if (columnar) e.flush_columnar("t");
    const std::string T = tag;
    // A1 arithmetic
    check(I(e, "SELECT a + b FROM t") == 10, T + " a+b=10");
    check(I(e, "SELECT a - b FROM t") == 4, T + " a-b=4");
    check(I(e, "SELECT a * b FROM t") == 21, T + " a*b=21");
    check(I(e, "SELECT a / b FROM t") == 2, T + " a/b=2 (int trunc)");
    check(I(e, "SELECT a % b FROM t") == 1, T + " a%b=1");
    check(I(e, "SELECT a * 2 + b FROM t") == 17, T + " precedence a*2+b=17");
    check(I(e, "SELECT (a + b) * 2 FROM t") == 20, T + " parens (a+b)*2=20");
    check(I(e, "SELECT -a FROM t") == -7, T + " unary -a=-7");
    check(!e.exec("SELECT a / 0 FROM t").ok, T + " div by zero error");
    // A2 string fns
    check(S(e, "SELECT UPPER(name) FROM t") == "HELLO", T + " UPPER");
    check(S(e, "SELECT LOWER(name) FROM t") == "hello", T + " LOWER");
    check(I(e, "SELECT LENGTH(name) FROM t") == 5, T + " LENGTH=5");
    check(S(e, "SELECT SUBSTR(name, 2, 3) FROM t") == "ell", T + " SUBSTR(2,3)=ell");
    check(S(e, "SELECT CONCAT(name, '!') FROM t") == "Hello!", T + " CONCAT");
    check(I(e, "SELECT ABS(b - a) FROM t") == 4, T + " ABS(b-a)=4");
    check(I(e, "SELECT COALESCE(a, b) FROM t") == 7, T + " COALESCE first non-null");
    // A3 CASE
    check(I(e, "SELECT CASE WHEN a > b THEN 1 ELSE 0 END FROM t") == 1, T + " CASE a>b -> 1");
    check(I(e, "SELECT CASE WHEN a < b THEN 1 ELSE 0 END FROM t") == 0, T + " CASE a<b -> else 0");
    check(I(e, "SELECT CASE WHEN a = 1 THEN 10 WHEN a = 7 THEN 70 ELSE 0 END FROM t") == 70,
          T + " CASE multi-WHEN -> 70");
    // A4 CAST
    check(S(e, "SELECT CAST(a AS TEXT) FROM t") == "7", T + " CAST int->text");
    check(I(e, "SELECT CAST('42' AS INT) FROM t") == 42, T + " CAST text->int");
    // AS alias label
    {
        const ExecResult r = e.exec("SELECT a + b AS total FROM t");
        check(r.ok && !r.rows.empty() && r.rows[0].cells[0].first == "total", T + " AS alias label");
    }
    // computed column composes with WHERE + a plain column
    check(I(e, "SELECT a * b FROM t WHERE a > 5") == 21, T + " expr + WHERE");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_expr_test: FAILED\n"); return 1; }
    std::printf("sql_expr_test: OK (arithmetic + string fns + CASE + CAST in projection)\n");
    return 0;
}
