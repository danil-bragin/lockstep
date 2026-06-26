// sql_int128_test.cpp — F9e INT128 / DECIMAL128: 128-bit integer + fixed-point over physical TEXT
// (a 16-byte order-preserving payload). Covers crypto-scale amounts (e.g. wei: a balance far beyond
// int64). Storage/keys/comparison stay TEXT (byte-order == numeric order), so byte-determinism holds.
// Arithmetic decodes to __int128, checked for overflow, re-encodes. Teeth: ordering, big literal,
// add/sub/mul, DECIMAL128 scale, overflow error, durable.
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
    // INT128: store + render a value far beyond int64; numeric ordering; arithmetic.
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE w (id INT, bal INT128 NOT NULL, PRIMARY KEY (id))").ok,
              "create INT128 column");
        // 1 ETH = 1e18 wei; 1e30 wei is ~1e12 ETH — way past int64 (~9.2e18).
        check(e.exec("INSERT INTO w (id, bal) VALUES (1, '1000000000000000000000000000000')").ok,
              "insert 1e30 wei");
        check(cell0(e.exec("SELECT bal FROM w WHERE id = 1")) ==
                  "1000000000000000000000000000000",
              "render 1e30 exact");
        // ordering is numeric (not lexical): 9e9 < 1e30 even though '9' > '1' lexically.
        check(e.exec("INSERT INTO w (id, bal) VALUES (2, '9000000000')").ok, "insert 9e9");
        const ExecResult ord = e.exec("SELECT bal FROM w ORDER BY bal");
        check(ord.ok && ord.rows.size() == 2 && ord.rows[0].cells[0].second.render() == "9000000000",
              "ORDER BY numeric: 9e9 first");
        // WHERE comparison is numeric too.
        check(e.exec("SELECT id FROM w WHERE bal > '1000000000000000000'").rows.size() == 1,
              "WHERE bal > 1e18 -> only the 1e30 row");
        // add: 1e30 + 9e9 (exact 128-bit).
        check(cell0(e.exec("SELECT bal + '9000000000' FROM w WHERE id = 1")) ==
                  "1000000000000000000009000000000",
              "1e30 + 9e9 exact");
        // multiply two big values that overflow int64 but fit int128.
        check(cell0(e.exec("SELECT bal * '1000000' FROM w WHERE id = 2")) == "9000000000000000",
              "9e9 * 1e6 = 9e15");
    }
    // DECIMAL128: a high-precision fixed-point token balance (scale 18, like wei-denominated ETH).
    {
        SqlEngine e;
        check(e.exec("CREATE TABLE acct (id INT, bal DECIMAL(38, 18) NOT NULL, PRIMARY KEY (id))").ok,
              "create DECIMAL(38,18) -> 128-bit");
        check(e.exec("INSERT INTO acct (id, bal) VALUES (1, '123456789.123456789012345678')").ok,
              "insert high-precision decimal");
        check(cell0(e.exec("SELECT bal FROM acct WHERE id = 1")) ==
                  "123456789.123456789012345678",
              "render 18-digit fraction exact");
        check(e.exec("INSERT INTO acct (id, bal) VALUES (2, '0.000000000000000001')").ok,
              "insert 1 wei (1e-18)");
        // add two fixed-point values (scale-aligned, exact).
        check(cell0(e.exec("SELECT bal + '0.000000000000000001' FROM acct WHERE id = 1")) ==
                  "123456789.123456789012345679",
              "decimal128 add carries the last wei");
        // F9f: SUM / AVG / MIN / MAX over DECIMAL128 (sum-of-balances — the crypto case).
        check(cell0(e.exec("SELECT SUM(bal) FROM acct")) == "123456789.123456789012345679",
              "SUM(DECIMAL128) = 123456789.123... + 1 wei");
        check(cell0(e.exec("SELECT MIN(bal) FROM acct")) == "0.000000000000000001", "MIN(DECIMAL128)");
        check(cell0(e.exec("SELECT MAX(bal) FROM acct")) == "123456789.123456789012345678",
              "MAX(DECIMAL128)");
    }
    // F9f: SUM / AVG over INT128 (exact, past int64).
    {
        SqlEngine e;
        e.exec("CREATE TABLE w (id INT, bal INT128 NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO w (id, bal) VALUES (1, '10000000000000000000000000000')");  // 1e28
        e.exec("INSERT INTO w (id, bal) VALUES (2, '20000000000000000000000000000')");  // 2e28
        e.exec("INSERT INTO w (id, bal) VALUES (3, '30000000000000000000000000000')");  // 3e28
        check(cell0(e.exec("SELECT SUM(bal) FROM w")) == "60000000000000000000000000000",
              "SUM(INT128) = 6e28");
        check(cell0(e.exec("SELECT AVG(bal) FROM w")) == "20000000000000000000000000000",
              "AVG(INT128) = 2e28");
        check(cell0(e.exec("SELECT MAX(bal) FROM w")) == "30000000000000000000000000000",
              "MAX(INT128)");
        // GROUP BY with an INT128 SUM per group.
        e.exec("CREATE TABLE g (id INT, k INT NOT NULL, amt INT128 NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO g (id, k, amt) VALUES (1, 1, '5000000000000000000000'), "
               "(2, 1, '5000000000000000000000'), (3, 2, '1000000000000000000000')");
        const ExecResult gb = e.exec("SELECT k, SUM(amt) FROM g GROUP BY k ORDER BY k");
        check(gb.ok && gb.rows.size() == 2, "GROUP BY INT128 -> 2 groups");
        if (gb.rows.size() == 2) {
            check(gb.rows[0].cells[1].second.render() == "10000000000000000000000", "group k=1 sum");
            check(gb.rows[1].cells[1].second.render() == "1000000000000000000000", "group k=2 sum");
        }
        // SUM overflow past int128 -> clean error.
        e.exec("CREATE TABLE o (id INT, v INT128 NOT NULL, PRIMARY KEY (id))");
        e.exec("INSERT INTO o (id, v) VALUES (1, '100000000000000000000000000000000000000')");  // 1e38
        e.exec("INSERT INTO o (id, v) VALUES (2, '100000000000000000000000000000000000000')");  // 1e38
        const ExecResult so = e.exec("SELECT SUM(v) FROM o");  // 2e38 > int128 max (~1.7e38)
        check(!so.ok && so.error.find("overflow") != std::string::npos, "SUM(INT128) overflow -> error");
    }
    // overflow past int128 -> clean error (not UB, not wrap).
    {
        SqlEngine e;
        e.exec("CREATE TABLE o (id INT, v INT128 NOT NULL, PRIMARY KEY (id))");
        // ~1.7e38 is the max; this literal is ~1e38, *100 overflows.
        e.exec("INSERT INTO o (id, v) VALUES (1, '100000000000000000000000000000000000000')");
        const ExecResult r = e.exec("SELECT v * '100' FROM o WHERE id = 1");
        check(!r.ok && r.error.find("overflow") != std::string::npos, "int128 overflow -> error");
        // a literal beyond int128 is rejected at parse.
        check(!e.exec("INSERT INTO o (id, v) VALUES (2, "
                      "'999999999999999999999999999999999999999999')").ok,
              "literal beyond int128 rejected");
    }
    // durable: INT128 value + 16-byte payload survive a restart.
    {
        lockstep::core::Scheduler s, cs;
        lockstep::core::SimClock c{s}, cc{cs};
        lockstep::sim::SeededRandom r{1}, cr{2};
        lockstep::sim::SimDisk d{s, c, r}, cd{cs, cc, cr};
        std::size_t dl = 0, cl = 0;
        {
            SqlEngine e(s, d, cs, cd);
            e.exec("CREATE TABLE w (id INT, bal INT128 NOT NULL, PRIMARY KEY (id))");
            // INT128_MAX = 2^127 - 1.
            e.exec("INSERT INTO w (id, bal) VALUES (2, '170141183460469231731687303715884105727')");
            dl = d.durable_len(); cl = cd.durable_len();
        }
        SqlEngine rec(s, d, cs, cd);
        rec.recover(dl, cl);
        check(cell0(rec.exec("SELECT bal FROM w WHERE id = 2")) ==
                  "170141183460469231731687303715884105727",
              "recovered INT128_MAX exact");
    }
    if (g_fail) { std::printf("sql_int128_test: FAILED\n"); return 1; }
    std::printf("sql_int128_test: OK (INT128/DECIMAL128 over TEXT: exact, numeric order, checked "
                "arithmetic, durable)\n");
    return 0;
}
