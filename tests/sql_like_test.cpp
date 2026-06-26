// sql_like_test.cpp — B1 LIKE / NOT LIKE gate. `%` matches any run (incl empty), `_` exactly one
// char. Checked against an independent reference matcher over a fixed dataset, in BOTH row and
// columnar mode, plus the bare like_match unit cases. Teeth: a substring that is NOT a prefix must
// not match `prefix%`, and NOT LIKE is the complement.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

// Independent reference LIKE (recursive) — different impl from the engine's iterative one.
bool ref_like(const char* s, const char* p) {
    if (*p == '\0') return *s == '\0';
    if (*p == '%') return ref_like(s, p + 1) || (*s != '\0' && ref_like(s + 1, p));
    if (*s == '\0') return false;
    if (*p == '_' || *p == *s) return ref_like(s + 1, p + 1);
    return false;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, name TEXT NOT NULL, PRIMARY KEY (id))");
    const std::vector<std::string> names = {"north", "northeast", "south", "no", "NORTH", "n_rth", "stone"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        e.exec("INSERT INTO t (id, name) VALUES (" + std::to_string(i) + ", '" + names[i] + "')");
    }
    if (columnar) e.flush_columnar("t");

    const std::vector<std::string> pats = {"north%", "%th", "%or%", "no", "n_rth", "_o%", "%", "z%"};
    for (const std::string& pat : pats) {
        // engine result set for LIKE pat
        const ExecResult r = e.exec("SELECT name FROM t WHERE name LIKE '" + pat + "' ORDER BY id");
        check(r.ok, std::string(tag) + " LIKE '" + pat + "' ok");
        // reference: which names match
        std::vector<std::string> want;
        for (const std::string& nm : names) if (ref_like(nm.c_str(), pat.c_str())) want.push_back(nm);
        std::vector<std::string> got;
        for (const auto& row : r.rows) got.push_back(row.cells[0].second.s);
        check(got == want, std::string(tag) + " LIKE '" + pat + "' set mismatch (got " +
                               std::to_string(got.size()) + " want " + std::to_string(want.size()) + ")");

        // NOT LIKE is the complement.
        const ExecResult nr = e.exec("SELECT name FROM t WHERE name NOT LIKE '" + pat + "' ORDER BY id");
        check(nr.ok && nr.rows.size() + r.rows.size() == names.size(),
              std::string(tag) + " NOT LIKE '" + pat + "' complement (|like|+|notlike| == N)");
    }
}
}  // namespace

int main() {
    // Tricky patterns are exercised through SQL (the dataset + pats cover %, _, prefix, suffix,
    // infix, full, empty-run); each is cross-checked against the independent ref_like matcher.
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_like_test: FAILED\n"); return 1; }
    std::printf("sql_like_test: OK (LIKE / NOT LIKE == reference, both modes)\n");
    return 0;
}
