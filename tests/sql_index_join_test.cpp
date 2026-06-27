// sql_index_join_test.cpp — I4: index nested-loop JOIN. A 2-table INNER equi-join whose INNER table
// is indexed on the (non-PK) join column probes the index per outer row instead of scanning the inner
// fully. Result must equal the general hash-join (the join conformance gate proves byte-identity);
// last_join_used_index_nl() confirms the path actually fired.
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
}  // namespace

int main() {
    SqlEngine e;
    // outer: orders(id, cust). inner: cust_idx(id, ckey, name) with a NON-PK index on ckey.
    e.exec("CREATE TABLE ord (id INT, cust INT NOT NULL, PRIMARY KEY (id))");
    e.exec("CREATE TABLE cust (id INT, ckey INT NOT NULL, name TEXT NOT NULL, PRIMARY KEY (id))");
    for (int i = 0; i < 200; ++i) {
        char q[128];
        std::snprintf(q, sizeof q, "INSERT INTO cust (id, ckey, name) VALUES (%d, %d, 'c%d')", i, 1000 + i, i);
        e.exec(q);
    }
    for (int i = 0; i < 60; ++i) {
        char q[128];
        std::snprintf(q, sizeof q, "INSERT INTO ord (id, cust) VALUES (%d, %d)", i, 1000 + (i % 200));
        e.exec(q);
    }
    e.exec("CREATE INDEX ck ON cust (ckey)");  // non-PK index on the join key

    // INNER join ord.cust = cust.ckey -> index-NL (cust is the inner, indexed on ckey).
    const ExecResult r = e.exec(
        "SELECT ord.id, cust.name FROM ord JOIN cust ON ord.cust = cust.ckey ORDER BY ord.id");
    check(r.ok && r.rows.size() == 60, "indexed join -> 60 rows (one per order)");
    check(e.last_join_used_index_nl(), "the index nested-loop path fired");
    // correctness: order i maps to ckey 1000 + (i%200) -> name c(i%200).
    if (r.ok && r.rows.size() == 60) {
        bool ok = true;
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            const std::int64_t oid = r.rows[i].cells[0].second.i;
            const std::string nm = r.rows[i].cells[1].second.render();
            if (nm != ("c" + std::to_string(oid % 200))) ok = false;
        }
        check(ok, "every joined row maps order -> the right customer");
    }

    // a WHERE + aggregate over the indexed join still works (finish_joined handles it).
    check(scal(e.exec("SELECT COUNT(*) FROM ord JOIN cust ON ord.cust = cust.ckey WHERE ord.id < 10")) == 10,
          "indexed join + WHERE count");
    check(e.last_join_used_index_nl(), "index-NL fired for the aggregate join too");
    check(scal(e.exec("SELECT COUNT(*) FROM ord JOIN cust ON ord.cust = cust.ckey")) == 60,
          "indexed join full count");

    // teeth: join on the inner PK (no secondary index) does NOT use index-NL (falls back, still correct).
    e.exec("CREATE TABLE p (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO p (id) VALUES (1000),(1001),(1002)");
    const ExecResult r2 = e.exec("SELECT ord.id FROM ord JOIN p ON ord.cust = p.id WHERE ord.id < 5");
    check(r2.ok, "PK-join ok");
    check(!e.last_join_used_index_nl(), "PK join (no secondary index) -> general path");

    if (g_fail) { std::printf("sql_index_join_test: FAILED\n"); return 1; }
    std::printf("sql_index_join_test: OK (index nested-loop join: fired, results == expected, "
                "+WHERE/agg; PK join falls back)\n");
    return 0;
}
