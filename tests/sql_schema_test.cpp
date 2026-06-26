// sql_schema_test.cpp — E4: SCHEMAS / namespaces. CREATE/DROP SCHEMA, schema.table qualified names,
// SET search_path. A bare name (default empty search_path) is byte-identical to the pre-schema
// behaviour; schemas are purely additive.
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
std::int64_t scal(const ExecResult& r) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.i : (std::int64_t)-99;
}
}  // namespace

int main() {
    // qualified names + same bare name in two schemas are distinct tables.
    {
        SqlEngine e;
        check(e.exec("CREATE SCHEMA app").ok, "create schema app");
        check(e.exec("CREATE SCHEMA audit").ok, "create schema audit");
        check(e.exec("CREATE TABLE app.users (id INT, n INT NOT NULL, PRIMARY KEY (id))").ok,
              "create app.users");
        check(e.exec("CREATE TABLE audit.users (id INT, n INT NOT NULL, PRIMARY KEY (id))").ok,
              "create audit.users (same bare name, different schema)");
        e.exec("INSERT INTO app.users (id, n) VALUES (1, 100)");
        e.exec("INSERT INTO audit.users (id, n) VALUES (1, 999)");
        check(scal(e.exec("SELECT n FROM app.users WHERE id = 1")) == 100, "app.users row");
        check(scal(e.exec("SELECT n FROM audit.users WHERE id = 1")) == 999, "audit.users row distinct");
        // a qualified table with a qualified column alias.
        check(scal(e.exec("SELECT users.n FROM app.users WHERE id = 1")) == 100, "alias defaults to 'users'");
    }
    // search_path: a bare name resolves to the current schema.
    {
        SqlEngine e;
        e.exec("CREATE SCHEMA s1");
        e.exec("CREATE TABLE s1.t (id INT, v INT NOT NULL, PRIMARY KEY (id))");
        check(!e.exec("SELECT v FROM t").ok, "bare 't' not found (search_path empty)");
        check(e.exec("SET search_path TO s1").ok, "set search_path");
        e.exec("INSERT INTO t (id, v) VALUES (1, 7)");  // resolves to s1.t
        check(scal(e.exec("SELECT v FROM t WHERE id = 1")) == 7, "bare 't' resolves to s1.t");
        check(scal(e.exec("SELECT v FROM s1.t WHERE id = 1")) == 7, "qualified also works");
        check(e.exec("SET search_path TO DEFAULT").ok, "reset search_path");
        check(!e.exec("SELECT v FROM t").ok, "bare 't' unresolved again after reset");
        check(!e.exec("SET search_path TO ghost").ok, "set to unknown schema rejected");
    }
    // DROP SCHEMA + IF [NOT] EXISTS.
    {
        SqlEngine e;
        check(e.exec("CREATE SCHEMA x").ok, "create x");
        check(!e.exec("CREATE SCHEMA x").ok, "duplicate schema errors");
        check(e.exec("CREATE SCHEMA IF NOT EXISTS x").ok, "IF NOT EXISTS no-op");
        check(e.exec("DROP SCHEMA x").ok, "drop x");
        check(!e.exec("DROP SCHEMA x").ok, "drop missing errors");
        check(e.exec("DROP SCHEMA IF EXISTS x").ok, "DROP IF EXISTS no-op");
    }
    // durable: schema + qualified table survive a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE SCHEMA tenant");
            e.exec("CREATE TABLE tenant.acct (id INT, bal INT NOT NULL, PRIMARY KEY (id))");
            e.exec("INSERT INTO tenant.acct (id, bal) VALUES (1, 500)");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(scal(rec.exec("SELECT bal FROM tenant.acct WHERE id = 1")) == 500,
              "recovered qualified table");
        check(rec.exec("SET search_path TO tenant").ok, "schema recovered (search_path works)");
        check(scal(rec.exec("SELECT bal FROM acct WHERE id = 1")) == 500, "bare resolves post-recover");
    }
    if (g_fail) { std::printf("sql_schema_test: FAILED\n"); return 1; }
    std::printf("sql_schema_test: OK (schemas: qualified names, search_path, CREATE/DROP, durable)\n");
    return 0;
}
