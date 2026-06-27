// sql_json_ops_test.cpp — JSON operators: `@>` containment (deep object/array/scalar), `#>` / `#>>`
// path extraction (JSON / text), and the JSON_AGG aggregate (collect a group into a canonical JSON
// array). `@>` is a predicate operator (CmpOp::Contains); `#>` / `#>>` are postfix expression
// operators; JSON_AGG is a row-AoS aggregate. All deterministic / canonical.
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
std::string str(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : std::string("<ERR>");
}
}  // namespace

int main() {
    SqlEngine e;
    e.exec("CREATE TABLE d (id INT, doc JSON NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO d (id,doc) VALUES (1,'{\"k\":\"red\",\"tags\":[1,2,3],\"o\":{\"x\":9}}')");
    e.exec("INSERT INTO d (id,doc) VALUES (2,'{\"k\":\"blue\",\"tags\":[2,4]}')");
    e.exec("INSERT INTO d (id,doc) VALUES (3,'{\"k\":\"red\",\"tags\":[5]}')");

    // @> containment.
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc @> '{\"k\":\"red\"}'")) == 2, "@> object key");
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc @> '{\"tags\":[2]}'")) == 2, "@> array element");
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc @> '{\"o\":{\"x\":9}}'")) == 1, "@> nested object");
    check(scal(e.exec("SELECT id FROM d WHERE doc @> '{\"k\":\"blue\",\"tags\":[4]}'")) == 2, "@> multi-key");
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc @> '{\"k\":\"green\"}'")) == 0, "@> absent => none");
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc @> '{\"tags\":[2,3]}'")) == 1, "@> two array elems");

    // #> (JSON) and #>> (text) path extraction.
    check(str(e.exec("SELECT doc #> ARRAY['o','x'] FROM d WHERE id = 1")) == "9", "#> nested -> JSON 9");
    check(str(e.exec("SELECT doc #>> ARRAY['k'] FROM d WHERE id = 1")) == "red", "#>> key -> text");
    check(str(e.exec("SELECT doc #>> ARRAY['tags','1'] FROM d WHERE id = 1")) == "2", "#>> array index -> text");
    check(e.exec("SELECT doc #> ARRAY['missing'] FROM d WHERE id = 1").rows[0].cells[0].second.is_null,
          "#> missing path -> NULL");
    // a #>> result is usable in WHERE (it is an expression LHS — J1).
    check(scal(e.exec("SELECT COUNT(*) FROM d WHERE doc #>> ARRAY['k'] = 'red'")) == 2, "#>> in WHERE");

    // JSON_AGG — grouped + ungrouped, INT and TEXT elements, canonical output.
    SqlEngine f;
    f.exec("CREATE TABLE t (id INT, grp INT, v INT, name TEXT, PRIMARY KEY (id))");
    f.exec("INSERT INTO t (id,grp,v,name) VALUES (1,1,10,'a'),(2,1,20,'b'),(3,2,30,'c')");
    check(str(f.exec("SELECT json_agg(v) FROM t")) == "[10,20,30]", "json_agg(int) ungrouped");
    {
        const ExecResult r = f.exec("SELECT grp, json_agg(v) FROM t GROUP BY grp");
        check(r.ok && r.rows.size() == 2, "grouped json_agg returns 2 groups");
        check(r.ok && r.rows[0].cells[1].second.render() == "[10,20]", "grp 1 => [10,20]");
        check(r.ok && r.rows[1].cells[1].second.render() == "[30]", "grp 2 => [30]");
    }
    check(str(f.exec("SELECT json_agg(name) FROM t WHERE grp = 1")) == "[\"a\",\"b\"]",
          "json_agg(text) => JSON string array");
    // NULLs are included as JSON null (Postgres semantics).
    f.exec("INSERT INTO t (id,grp,v,name) VALUES (4,3,40,NULL)");
    check(str(f.exec("SELECT json_agg(name) FROM t WHERE grp = 3")) == "[null]",
          "json_agg includes NULL as JSON null");

    if (g_fail) { std::printf("sql_json_ops_test: FAILED\n"); return 1; }
    std::printf("sql_json_ops_test: OK (@> containment, #> / #>> path extraction, JSON_AGG "
                "grouped/ungrouped INT+TEXT with NULLs, composable)\n");
    return 0;
}
