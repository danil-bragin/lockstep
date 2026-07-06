// sql_array_test.cpp — F12 ARRAY type (logical=7 over physical TEXT, self-describing element payload).
// Deterministic (ordered elements of a deterministic element type). Phase A: type T[], ARRAY[...] and
// '{...}' literals, store/render '{...}', element subscript arr[i] (1-based), element-wise ordering +
// equality, element types INT/TEXT/DECIMAL/INT128, durable.
#include <cstdio>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string cell0(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : std::string("<none>");
}
}  // namespace

int main() {
    // INT[] — ARRAY[...] literal + '{...}' text literal, render, subscript.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE t (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))").ok,
              "create INT[] column");
        check(e.exec("INSERT INTO t (id, xs) VALUES (1, ARRAY[3, 1, 2])").ok, "insert ARRAY[...]");
        check(e.exec("INSERT INTO t (id, xs) VALUES (2, '{10,20}')").ok, "insert '{...}' literal");
        check(cell0(e.exec("SELECT xs FROM t WHERE id = 1")) == "{3,1,2}", "render {3,1,2}");
        check(cell0(e.exec("SELECT xs FROM t WHERE id = 2")) == "{10,20}", "render {10,20}");
        // subscript is 1-based; out of range -> NULL.
        check(cell0(e.exec("SELECT xs[1] FROM t WHERE id = 1")) == "3", "xs[1] = 3");
        check(cell0(e.exec("SELECT xs[3] FROM t WHERE id = 1")) == "2", "xs[3] = 2");
        check(cell0(e.exec("SELECT xs[9] FROM t WHERE id = 1")) == "NULL", "xs[9] out of range -> NULL");
        // empty array.
        check(e.exec("INSERT INTO t (id, xs) VALUES (3, ARRAY[])").ok, "insert empty array");
        check(cell0(e.exec("SELECT xs FROM t WHERE id = 3")) == "{}", "render {}");
    }
    // element-wise ordering + equality.
    {
        SqlEngine e;
        e.exec("CREATE TABLE a (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO a (id, xs) VALUES (1, '{1,2,3}'), (2, '{1,2}'), (3, '{1,3}')");
        // ORDER BY: {1,2} < {1,2,3} < {1,3} (element-wise, then shorter<longer).
        const ExecResult ord = e.exec("SELECT xs FROM a ORDER BY xs");
        check(ord.ok && ord.rows.size() == 3, "3 ordered arrays");
        if (ord.rows.size() == 3) {
            check(ord.rows[0].cells[0].second.render() == "{1,2}" &&
                      ord.rows[1].cells[0].second.render() == "{1,2,3}" &&
                      ord.rows[2].cells[0].second.render() == "{1,3}",
                  "element-wise order: {1,2} < {1,2,3} < {1,3}");
        }
        // equality via WHERE.
        check(e.exec("SELECT id FROM a WHERE xs = '{1,3}'").rows.size() == 1, "WHERE xs = {1,3}");
        check(e.exec("SELECT id FROM a WHERE xs = ARRAY[1,2,3]").rows.size() == 1, "WHERE xs = ARRAY[1,2,3]");
    }
    // TEXT[] elements.
    {
        SqlEngine e;
        e.exec("CREATE TABLE g (id INT, tags TEXT[] NOT NULL, PRIMARY KEY (id))");
        check(e.exec("INSERT INTO g (id, tags) VALUES (1, ARRAY['red', 'green'])").ok, "insert TEXT[]");
        check(cell0(e.exec("SELECT tags FROM g WHERE id = 1")) == "{red,green}", "render {red,green}");
        check(cell0(e.exec("SELECT tags[2] FROM g WHERE id = 1")) == "green", "tags[2] = green");
    }
    // DECIMAL[] + INT128[] element types render with their scale / full width.
    {
        SqlEngine e;
        e.exec("CREATE TABLE d (id INT, prices DECIMAL(10,2)[] NOT NULL, big INT128[] NOT NULL, "
               "PRIMARY KEY (id))");
        check(e.exec("INSERT INTO d (id, prices, big) VALUES "
                     "(1, ARRAY['1.50', '2.05'], ARRAY['10000000000000000000000'])").ok,
              "insert DECIMAL[]/INT128[]");
        check(cell0(e.exec("SELECT prices FROM d WHERE id = 1")) == "{1.50,2.05}", "DECIMAL[] render");
        check(cell0(e.exec("SELECT prices[2] FROM d WHERE id = 1")) == "2.05", "prices[2] = 2.05");
        check(cell0(e.exec("SELECT big[1] FROM d WHERE id = 1")) == "10000000000000000000000",
              "INT128[] element exact");
    }
    // F12b: array functions + ANY/ALL.
    {
        SqlEngine e;
        std::int64_t (*scal)(const ExecResult&) = [](const ExecResult& r) {
            return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
        };
        e.exec("CREATE TABLE t (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, xs) VALUES (1, '{10,20,30}')");
        check(scal(e.exec("SELECT array_length(xs) FROM t WHERE id = 1")) == 3, "array_length = 3");
        check(scal(e.exec("SELECT cardinality(xs) FROM t WHERE id = 1")) == 3, "cardinality = 3");
        check(scal(e.exec("SELECT array_contains(xs, 20) FROM t WHERE id = 1")) == 1, "contains 20");
        check(scal(e.exec("SELECT array_contains(xs, 99) FROM t WHERE id = 1")) == 0, "not contains 99");
        check(scal(e.exec("SELECT array_position(xs, 30) FROM t WHERE id = 1")) == 3, "position of 30 = 3");
        check(cell0(e.exec("SELECT array_append(xs, 40) FROM t WHERE id = 1")) == "{10,20,30,40}",
              "array_append");
        check(cell0(e.exec("SELECT array_cat(xs, ARRAY[40,50]) FROM t WHERE id = 1")) ==
                  "{10,20,30,40,50}",
              "array_cat");
        // = ANY / ALL — the scalar LHS is a COLUMN (a literal LHS like `20 = ANY(xs)` needs general
        // literal-LHS predicate support, a follow-on). RHS is an array column or an ARRAY[...] literal.
        e.exec("CREATE TABLE m (id INT, v INT NOT NULL, xs INT[] NOT NULL, lo INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO m (id, v, xs, lo) VALUES (1, 20, '{10,20,30}', 5), (2, 99, '{10,20,30}', 15)");
        check(e.exec("SELECT id FROM m WHERE v = ANY(xs)").rows.size() == 1, "v=ANY(xs): only id 1");
        check(e.exec("SELECT id FROM m WHERE lo < ALL(xs)").rows.size() == 1, "lo<ALL(xs): only id 1 (5<all)");
        check(e.exec("SELECT id FROM m WHERE v = ANY(ARRAY[99,100])").rows.size() == 1,
              "v = ANY(literal array): only id 2");
    }
    // F12c: ARRAY_AGG — collect grouped values into an array.
    {
        SqlEngine e;
        e.exec("CREATE TABLE s (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO s (id, k, v) VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30)");
        // ungrouped: all values in scan (PK-ascending) order.
        check(cell0(e.exec("SELECT array_agg(v) FROM s")) == "{10,20,30}", "array_agg ungrouped");
        // grouped.
        const ExecResult gb = e.exec("SELECT k, array_agg(v) FROM s GROUP BY k ORDER BY k");
        check(gb.ok && gb.rows.size() == 2, "array_agg grouped -> 2 groups");
        if (gb.rows.size() == 2) {
            check(gb.rows[0].cells[1].second.render() == "{10,20}", "group k=1 -> {10,20}");
            check(gb.rows[1].cells[1].second.render() == "{30}", "group k=2 -> {30}");
        }
        // array_agg over TEXT, then index the result.
        e.exec("CREATE TABLE g (id INT, tag TEXT NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO g (id, tag) VALUES (1, 'a'), (2, 'b')");
        check(cell0(e.exec("SELECT array_agg(tag) FROM g")) == "{a,b}", "array_agg(TEXT)");
    }
    // F12d: UNNEST — expand an array column into one row per element.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id, xs) VALUES (1, '{10,20,30}'), (2, '{99}')");
        const ExecResult r = e.exec("SELECT UNNEST(xs) FROM t");
        check(r.ok && r.rows.size() == 4, "UNNEST -> 4 rows (3 + 1)");
        if (r.rows.size() == 4) {
            check(r.rows[0].cells[0].second.render() == "10" &&
                      r.rows[2].cells[0].second.render() == "30" &&
                      r.rows[3].cells[0].second.render() == "99",
                  "UNNEST element values");
        }
        // UNNEST alongside a repeated per-row column.
        const ExecResult r2 = e.exec("SELECT id, UNNEST(xs) FROM t WHERE id = 1");
        check(r2.ok && r2.rows.size() == 3, "id + UNNEST -> 3 rows");
        if (r2.rows.size() == 3) {
            check(r2.rows[0].cells[0].second.i == 1 && r2.rows[0].cells[1].second.render() == "10",
                  "row 0 = (1, 10)");
            check(r2.rows[2].cells[1].second.render() == "30", "row 2 element = 30");
        }
        // an empty array contributes zero rows.
        e.exec("INSERT INTO t (id, xs) VALUES (3, ARRAY[])");
        check(e.exec("SELECT UNNEST(xs) FROM t WHERE id = 3").rows.empty(), "empty array -> 0 rows");
    }
    // teeth: nested arrays rejected. F14: FLOAT[] (a REAL array) is now accepted and round-trips
    // (the array codec is generic over the element's logical type).
    {
        SqlEngine e;
        check(!e.exec("CREATE TABLE x (id INT, m INT[][], PRIMARY KEY (id))").ok, "nested array rejected");
        check(e.exec("CREATE TABLE y (id INT, m FLOAT[], PRIMARY KEY (id))").ok, "FLOAT[] accepted (F14)");
        check(e.exec("INSERT INTO y (id,m) VALUES (1, ARRAY[1.5, 2.5])").ok, "insert REAL array");
        check(e.exec("SELECT m FROM y WHERE id = 1").rows.at(0).cells.at(0).second.render() == "{1.5,2.5}",
              "REAL array renders {1.5,2.5}");
    }
    // durable: array column survives a restart (element type recovered).
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, xs INT[] NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id, xs) VALUES (1, '{7,8,9}')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT xs FROM t WHERE id = 1")) == "{7,8,9}", "recovered array");
        check(cell0(rec.exec("SELECT xs[2] FROM t WHERE id = 1")) == "8", "recovered subscript");
    }
    if (g_fail) { std::printf("sql_array_test: FAILED\n"); return 1; }
    std::printf("sql_array_test: OK (ARRAY type: literals, render, subscript, element-wise order, "
                "element types, durable)\n");
    return 0;
}
