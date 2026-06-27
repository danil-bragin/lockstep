// sql_columnar_i128_test.cpp — K1/K2: columnar SoA aggregation over 128-bit (INT128/DECIMAL128)
// columns. Previously a 128-bit aggregate column bailed the whole SoA path to full row-AoS
// materialization (~20x slower). Now SUM/AVG/MIN/MAX fold the 16-byte payloads directly over the
// SoA chunks — the generic gather path (compute_agg_soa) and the branchless masked fast path
// (compute_masked_agg_i128, decoding to a contiguous __int128 array). The ONLY thing that matters
// for correctness is that the columnar result is BYTE-IDENTICAL to the row-store result, including
// SUM-overflow errors; this test cross-checks the two engines on the same workload.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
std::string val(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    if (!r.ok) return "ERR:" + r.error;
    std::string s;
    for (const auto& row : r.rows) {
        s += "{";
        for (const auto& c : row.cells) { s += c.second.render(); s += ","; }
        s += "}";
    }
    return s;
}
void same(SqlEngine& col, SqlEngine& row, const std::string& q) {
    const std::string a = val(col, q), b = val(row, q);
    if (a != b) {
        std::printf("DIFF: %s\n  columnar=%s\n  row     =%s\n", q.c_str(), a.c_str(), b.c_str());
        g_fail = 1;
    }
}
}  // namespace

int main() {
    SqlEngine col, row;
    col.set_columnar_default(true);
    col.set_vectorize(true);
    for (SqlEngine* e : {&col, &row}) {
        e->exec("CREATE TABLE t (id INT, grp INT NOT NULL, a INT128 NOT NULL, "
                "d DECIMAL128(30,4) NOT NULL, an INT128, PRIMARY KEY (id))");
        for (int i = 0; i < 6000; ++i) {
            char q[256];
            std::snprintf(q, sizeof q,
                          "INSERT INTO t (id,grp,a,d,an) VALUES (%d,%d,%d,'%d.%04d',%s)",
                          i, i % 8, (i % 1000) - 500, i % 3000, i % 10000,
                          (i % 5 == 0) ? "NULL" : std::to_string((i % 700) - 300).c_str());
            e->exec(q);
        }
    }
    col.flush_columnar("t");

    // Scalar aggregates over a 128-bit column — unfiltered, filtered, INT128 + DECIMAL128.
    const char* qs[] = {
        "SELECT SUM(a) FROM t",
        "SELECT AVG(a) FROM t",
        "SELECT MIN(a), MAX(a) FROM t",
        "SELECT COUNT(a) FROM t",
        "SELECT SUM(a), COUNT(*) FROM t WHERE a > 0",
        "SELECT AVG(a) FROM t WHERE a >= -100 AND a <= 100",
        "SELECT MIN(a), MAX(a) FROM t WHERE a < 0",
        "SELECT SUM(a) FROM t WHERE a = 7",
        "SELECT SUM(a) FROM t WHERE a > 100000",          // zero matches => int 0
        "SELECT SUM(d) FROM t",
        "SELECT AVG(d) FROM t WHERE d > 1000",
        "SELECT MAX(d), MIN(d) FROM t WHERE a > 0",
        "SELECT SUM(an) FROM t",                            // nullable 128-bit (NULLs skipped)
        "SELECT AVG(an) FROM t WHERE an > 0",
        "SELECT COUNT(an), SUM(an) FROM t WHERE a < 0",
        // mixed: a 128-bit filter with an INT aggregate, and an INT filter with a 128-bit aggregate.
        "SELECT COUNT(*) FROM t WHERE a > 0",
        "SELECT SUM(a) FROM t WHERE grp = 3",
        // GROUP BY with a 128-bit aggregate (the gather path).
        "SELECT grp, SUM(a), MIN(a), MAX(d) FROM t GROUP BY grp",
        "SELECT grp, AVG(a) FROM t WHERE a > 0 GROUP BY grp",
    };
    for (const char* q : qs) same(col, row, q);

    // SUM overflow must raise the SAME error on both engines (the columnar path bails to the row
    // path on overflow, so the error text is identical).
    {
        SqlEngine c2, r2;
        c2.set_columnar_default(true);
        c2.set_vectorize(true);
        const char* big = "170141183460469231731687303715884105727";  // INT128_MAX
        for (SqlEngine* e : {&c2, &r2}) {
            e->exec("CREATE TABLE b (id INT, a INT128 NOT NULL, PRIMARY KEY (id))");
            for (int i = 0; i < 16; ++i) {
                char q[160];
                std::snprintf(q, sizeof q, "INSERT INTO b (id,a) VALUES (%d,'%s')", i, big);
                e->exec(q);
            }
        }
        c2.flush_columnar("b");
        const ExecResult a = c2.exec("SELECT SUM(a) FROM b");
        const ExecResult b = r2.exec("SELECT SUM(a) FROM b");
        if (a.ok || b.ok || a.error != b.error) {
            std::printf("FAIL: overflow mismatch col.ok=%d row.ok=%d '%s' vs '%s'\n",
                        a.ok, b.ok, a.error.c_str(), b.error.c_str());
            g_fail = 1;
        }
    }

    if (g_fail) { std::printf("sql_columnar_i128_test: FAILED\n"); return 1; }
    std::printf("sql_columnar_i128_test: OK (columnar SoA 128-bit aggregates == row store: SUM/AVG/"
                "MIN/MAX, INT128/DECIMAL128, filtered/grouped/nullable; overflow errors match)\n");
    return 0;
}
