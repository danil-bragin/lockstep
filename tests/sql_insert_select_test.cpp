// sql_insert_select_test.cpp — D5 INSERT ... SELECT gate. Rows produced by a query are inserted
// into the target (atomic, like multi-row VALUES). Arity mismatch errors; a dup PK from the query
// rejects the whole statement. Row + columnar target.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::int64_t scalar(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : -999;
}

void run_mode(bool columnar, const char* tag) {
    SqlEngine e;
    if (columnar) e.set_columnar_default(true);
    e.exec("CREATE TABLE src (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))");
    e.exec("CREATE TABLE dst (id INT, total INT NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO src (id,k,v) VALUES (1,5,10),(2,5,20),(3,9,30),(4,9,40)");
    if (columnar) e.flush_columnar("src");

    // INSERT ... SELECT: copy a filtered/projected set.
    const ExecResult ins = e.exec("INSERT INTO dst (id, total) SELECT id, v FROM src WHERE v > 15");
    check(ins.ok && ins.affected == 3, std::string(tag) + " inserted 3 (got " +
                                           std::to_string(ins.affected) + " ok=" + (ins.ok?"1":"0") + ")");
    if (columnar) e.flush_columnar("dst");
    check(scalar(e.exec("SELECT COUNT(*) FROM dst")) == 3, std::string(tag) + " dst has 3 rows");
    check(scalar(e.exec("SELECT total FROM dst WHERE id = 3")) == 30, std::string(tag) + " id=3 total=30");
    check(e.exec("SELECT id FROM dst WHERE id = 1").rows.empty(), std::string(tag) + " id=1 filtered out");

    // INSERT ... SELECT with a GROUP BY aggregate source: k=5 -> SUM(v)=30, k=9 -> SUM(v)=70.
    e.exec("CREATE TABLE agg (id INT, s INT NOT NULL, PRIMARY KEY (id))");
    check(e.exec("INSERT INTO agg (id, s) SELECT k, SUM(v) FROM src GROUP BY k").ok,
          std::string(tag) + " insert-select aggregate ok");
    check(scalar(e.exec("SELECT s FROM agg WHERE id = 5")) == 30, std::string(tag) + " agg k=5 sum=30");
    check(scalar(e.exec("SELECT s FROM agg WHERE id = 9")) == 70, std::string(tag) + " agg k=9 sum=70");

    // teeth: arity mismatch.
    check(!e.exec("INSERT INTO dst (id, total) SELECT id FROM src").ok,
          std::string(tag) + " arity mismatch rejected");
    // teeth: a dup PK from the query rejects all (re-running the same SELECT collides id 3).
    check(!e.exec("INSERT INTO dst (id, total) SELECT id, v FROM src WHERE v > 15").ok,
          std::string(tag) + " dup PK from select rejected (atomic)");
    check(scalar(e.exec("SELECT COUNT(*) FROM dst")) == 3, std::string(tag) + " still 3 after failed insert");
}
}  // namespace

int main() {
    run_mode(false, "row");
    run_mode(true, "columnar");
    if (g_fail) { std::printf("sql_insert_select_test: FAILED\n"); return 1; }
    std::printf("sql_insert_select_test: OK (INSERT ... SELECT, atomic, both modes)\n");
    return 0;
}
