// sql_array_gin_test.cpp — J3: array-element GIN index (`CREATE INDEX ... ON t (arr_col) USING GIN`).
// A GIN index stores ONE entry per array element, so a containment lookup `<const> = ANY(arr_col)`
// (now writable thanks to J1's expression LHS) finds every row whose array holds the value WITHOUT a
// full scan — and stays byte-identical to a scan (the residual WHERE re-checks `= ANY` per row).
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t scal(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
}
bool explain_has(SqlEngine& e, const std::string& sql, const std::string& needle) {
    const ExecResult r = e.exec("EXPLAIN " + sql);
    for (const auto& row : r.rows)
        if (!row.cells.empty() && row.cells[0].second.render().find(needle) != std::string::npos)
            return true;
    return false;
}
void seed_tags(SqlEngine& e) {
    e.exec("CREATE TABLE t (id INT, tags TEXT[] NOT NULL, PRIMARY KEY (id))");
    for (int i = 0; i < 360; ++i) {
        std::string arr = "ARRAY['common'";
        if (i % 12 == 0) arr += ",'rare'";
        if (i % 4 == 0) arr += ",'mid'";
        if (i % 12 == 0) arr += ",'rare'";  // a duplicate element — GIN must dedup to one entry
        arr += "]";
        char q[256];
        std::snprintf(q, sizeof q, "INSERT INTO t (id,tags) VALUES (%d,%s)", i, arr.c_str());
        e.exec(q);
    }
}
}  // namespace

int main() {
    // 1) TEXT[] GIN vs a no-index control — identical answers, GIN actually used.
    {
        SqlEngine idx, ctl;
        seed_tags(idx);
        seed_tags(ctl);
        check(idx.exec("CREATE INDEX tg ON t (tags) USING GIN").ok, "create TEXT[] GIN index");
        idx.exec("ANALYZE t");
        check(explain_has(idx, "SELECT id FROM t WHERE 'rare' = ANY(tags)", "Index Scan"),
              "containment query uses the GIN index");
        for (const char* v : {"common", "rare", "mid", "absent"}) {
            const std::string q = std::string("SELECT COUNT(*) FROM t WHERE '") + v + "' = ANY(tags)";
            check(scal(idx.exec(q)) == scal(ctl.exec(q)),
                  std::string("GIN == scan for '") + v + "' = ANY(tags)");
        }
        // teeth: 'rare' is on every 12th row over [0,360) => 30 (the duplicate element must NOT double it).
        check(scal(idx.exec("SELECT COUNT(*) FROM t WHERE 'rare' = ANY(tags)")) == 30,
              "rare count == 30 (duplicate array element deduped)");
        check(scal(idx.exec("SELECT COUNT(*) FROM t WHERE 'common' = ANY(tags)")) == 360, "common == 360");
    }

    // 2) INT[] GIN — same machinery over an integer element type.
    {
        SqlEngine idx, ctl;
        for (SqlEngine* e : {&idx, &ctl}) {
            e->exec("CREATE TABLE n (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))");
            for (int i = 0; i < 250; ++i) {
                char q[120];
                std::snprintf(q, sizeof q, "INSERT INTO n (id,xs) VALUES (%d,ARRAY[%d,%d])", i, i % 5, i % 7);
                e->exec(q);
            }
        }
        check(idx.exec("CREATE INDEX ng ON n (xs) USING GIN").ok, "create INT[] GIN index");
        idx.exec("ANALYZE n");
        check(explain_has(idx, "SELECT id FROM n WHERE 3 = ANY(xs)", "Index Scan"),
              "INT[] containment uses the GIN index");
        for (int v = 0; v < 7; ++v) {
            const std::string q = "SELECT COUNT(*) FROM n WHERE " + std::to_string(v) + " = ANY(xs)";
            check(scal(idx.exec(q)) == scal(ctl.exec(q)), "GIN == scan for " + std::to_string(v) + " = ANY(xs)");
        }
    }

    // 3) maintenance: UPDATE / DELETE keep the GIN index in lockstep with the array column.
    {
        SqlEngine e;
        e.exec("CREATE TABLE p (id INT, tags TEXT[] NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO p (id,tags) VALUES (1,ARRAY['a','b']),(2,ARRAY['b','c']),(3,ARRAY['a'])");
        e.exec("CREATE INDEX pg ON p (tags) USING GIN");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'a' = ANY(tags)")) == 2, "two rows hold 'a'");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'b' = ANY(tags)")) == 2, "two rows hold 'b'");
        e.exec("UPDATE p SET tags = ARRAY['x','y'] WHERE id = 1");  // drops 'a' and 'b' from id1
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'a' = ANY(tags)")) == 1, "one row holds 'a' after update");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'x' = ANY(tags)")) == 1, "new element 'x' indexed");
        e.exec("DELETE FROM p WHERE id = 2");  // removes the last 'c'
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'c' = ANY(tags)")) == 0, "no row holds 'c' after delete");
        check(scal(e.exec("SELECT COUNT(*) FROM p WHERE 'b' = ANY(tags)")) == 0, "no row holds 'b' after delete");
    }

    // 4) guards: GIN requires an ARRAY column and cannot be UNIQUE.
    {
        SqlEngine e;
        e.exec("CREATE TABLE g (id INT, name TEXT NOT NULL, xs INT[] NOT NULL, PRIMARY KEY (id))");
        check(!e.exec("CREATE INDEX bad ON g (name) USING GIN").ok, "GIN on a non-array column is rejected");
        check(!e.exec("CREATE UNIQUE INDEX bad2 ON g (xs) USING GIN").ok, "a UNIQUE GIN index is rejected");
        check(e.exec("CREATE INDEX ok ON g (xs) USING GIN").ok, "GIN on an array column is accepted");
    }

    if (g_fail) { std::printf("sql_array_gin_test: FAILED\n"); return 1; }
    std::printf("sql_array_gin_test: OK (array-element GIN: <const> = ANY(arr) uses the index, "
                "byte-identical to a scan; TEXT[]/INT[]; maintained on update/delete; guards enforced)\n");
    return 0;
}
