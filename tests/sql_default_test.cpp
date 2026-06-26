// sql_default_test.cpp — F4 column DEFAULT gate. An omitted column with a DEFAULT takes the
// default; a provided value overrides it; the default survives a restart (catalog persists it);
// a DEFAULT literal of the wrong type is rejected at CREATE.
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
std::int64_t i_of(const ExecResult& r, std::size_t c = 0) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[c].second.i : -999;
}
std::string s_of(const ExecResult& r, std::size_t c = 0) {
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[c].second.s : "?";
}
}  // namespace

int main() {
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, n INT NOT NULL DEFAULT 7, s TEXT NOT NULL DEFAULT 'hi', "
               "PRIMARY KEY (id))");
        e.exec("INSERT INTO t (id) VALUES (1)");                 // both default
        e.exec("INSERT INTO t (id, n) VALUES (2, 99)");          // n provided, s default
        e.exec("INSERT INTO t (id, s) VALUES (3, 'bye')");       // s provided, n default
        check(i_of(e.exec("SELECT n FROM t WHERE id = 1")) == 7, "id1 n=7 (default)");
        check(s_of(e.exec("SELECT s FROM t WHERE id = 1")) == "hi", "id1 s='hi' (default)");
        check(i_of(e.exec("SELECT n FROM t WHERE id = 2")) == 99, "id2 n=99 (provided)");
        check(s_of(e.exec("SELECT s FROM t WHERE id = 2")) == "hi", "id2 s='hi' (default)");
        check(i_of(e.exec("SELECT n FROM t WHERE id = 3")) == 7, "id3 n=7 (default)");
        check(s_of(e.exec("SELECT s FROM t WHERE id = 3")) == "bye", "id3 s='bye' (provided)");
        // teeth: a NOT NULL column with NO default still requires a value.
        check(!e.exec("CREATE TABLE u (id INT, v INT NOT NULL, PRIMARY KEY (id))").ok ? false : true,
              "control create ok");
        check(!e.exec("INSERT INTO u (id) VALUES (1)").ok, "NOT NULL w/o default still required");
        // teeth: DEFAULT type mismatch rejected.
        check(!e.exec("CREATE TABLE w (id INT, n INT DEFAULT 'x', PRIMARY KEY (id))").ok,
              "DEFAULT type mismatch rejected");
    }
    // durable: default survives a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE t (id INT, n INT NOT NULL DEFAULT 42, PRIMARY KEY (id))");
            e.exec("INSERT INTO t (id) VALUES (1)");
            dl = d.durable_len();
            cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(i_of(rec.exec("SELECT n FROM t WHERE id = 1")) == 42, "recovered default value");
        check(rec.exec("INSERT INTO t (id) VALUES (2)").ok &&
                  i_of(rec.exec("SELECT n FROM t WHERE id = 2")) == 42,
              "default still applies after recover");
    }
    if (g_fail) { std::printf("sql_default_test: FAILED\n"); return 1; }
    std::printf("sql_default_test: OK (DEFAULT fill + override + durable)\n");
    return 0;
}
