// sql_uint256_test.cpp — UINT256: a 256-bit UNSIGNED integer for crypto-scale amounts (e.g. Ethereum
// wei / token balances). Physically a 32-byte BIG-ENDIAN order-preserving TEXT payload (logical 13),
// so ORDER BY / comparisons / MIN/MAX / indexes order it correctly via the existing TEXT codec; +/-/
// */ // % and SUM accumulate in u256 with overflow / underflow / div-by-zero reported as errors.
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string val(SqlEngine& e, const std::string& q) {
    const ExecResult r = e.exec(q);
    return (r.ok && !r.rows.empty()) ? r.rows[0].cells[0].second.render() : (r.ok ? "<none>" : "ERR");
}
bool ok(SqlEngine& e, const std::string& q) { return e.exec(q).ok; }
}  // namespace

int main() {
    const std::string MAX =
        "115792089237316195423570985008687907853269984665640564039457584007913129639935";  // 2^256-1
    SqlEngine e;
    e.exec("CREATE TABLE w (id INT, bal UINT256 NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO w (id,bal) VALUES (1,'1000000000000000000')");   // 1 ETH (1e18 wei)
    e.exec("INSERT INTO w (id,bal) VALUES (2,'" + MAX + "')");
    e.exec("INSERT INTO w (id,bal) VALUES (3,'5000000000000000000')");   // 5 ETH
    e.exec("INSERT INTO w (id,bal) VALUES (4,42)");                      // a small INT widens in

    // store + render (lossless across the full 256-bit range).
    check(val(e, "SELECT bal FROM w WHERE id = 2") == MAX, "2^256-1 stored + rendered losslessly");
    check(val(e, "SELECT bal FROM w WHERE id = 4") == "42", "small INT value widens to UINT256");

    // ORDER BY uses the order-preserving payload (numeric order, not lexicographic of the decimal text).
    {
        const ExecResult r = e.exec("SELECT id, bal FROM w ORDER BY bal");
        check(r.ok && r.rows.size() == 4 && r.rows[0].cells[0].second.i == 4 &&
                  r.rows[1].cells[0].second.i == 1 && r.rows[2].cells[0].second.i == 3 &&
                  r.rows[3].cells[0].second.i == 2,
              "ORDER BY bal => 42 < 1e18 < 5e18 < 2^256-1");
    }

    // arithmetic (+, -, *, /, %).
    check(val(e, "SELECT bal + 1000000000000000000 FROM w WHERE id = 1") == "2000000000000000000", "add");
    check(val(e, "SELECT bal * 3 FROM w WHERE id = 3") == "15000000000000000000", "mul");
    check(val(e, "SELECT bal - 1 FROM w WHERE id = 2") ==
              "115792089237316195423570985008687907853269984665640564039457584007913129639934", "sub");
    check(val(e, "SELECT bal / 2 FROM w WHERE id = 3") == "2500000000000000000", "div");
    check(val(e, "SELECT bal % 1000000000 FROM w WHERE id = 1") == "0", "mod");

    // overflow / underflow / division-by-zero are errors.
    check(!ok(e, "SELECT bal + 1 FROM w WHERE id = 2"), "MAX + 1 overflows");
    check(!ok(e, "SELECT bal - 100 FROM w WHERE id = 4"), "42 - 100 underflows (unsigned)");
    check(!ok(e, "SELECT bal / 0 FROM w WHERE id = 1"), "division by zero");
    check(!ok(e, "SELECT -bal FROM w WHERE id = 1"), "negation of an unsigned value errors");

    // filter + aggregate.
    check(e.exec("SELECT COUNT(*) FROM w WHERE bal > 4000000000000000000").rows[0].cells[0].second.i == 2,
          "filter bal > 4e18 => 2 rows (id 2,3)");
    check(val(e, "SELECT SUM(bal) FROM w WHERE id <> 2") == "6000000000000000042", "SUM over UINT256 (1e18+5e18+42)");
    check(val(e, "SELECT MAX(bal) FROM w") == MAX, "MAX = 2^256-1");
    check(val(e, "SELECT MIN(bal) FROM w") == "42", "MIN = 42");
    check(val(e, "SELECT AVG(bal) FROM w WHERE id <> 2") == "2000000000000000014", "AVG truncates (6e18+42)/3");
    check(!ok(e, "SELECT SUM(bal) FROM w"), "SUM including 2^256-1 overflows (checked)");

    // input validation.
    check(!ok(e, "INSERT INTO w (id,bal) VALUES (5,-7)"), "negative literal rejected");
    check(!ok(e, "INSERT INTO w (id,bal) VALUES (6,'12x')"), "non-numeric literal rejected");
    check(!ok(e, "INSERT INTO w (id,bal) VALUES (7,'" + MAX + "0')"), "a 2^256+ literal rejected (overflow)");

    // a UINT256 secondary index orders + looks up correctly (the order-preserving payload).
    e.exec("CREATE INDEX wb ON w (bal)");
    check(e.exec("SELECT id FROM w WHERE bal = 42").rows[0].cells[0].second.i == 4, "indexed lookup by UINT256");

    if (g_fail) { std::printf("sql_uint256_test: FAILED\n"); return 1; }
    std::printf("sql_uint256_test: OK (UINT256 256-bit: lossless store/render, ordered, +/-/*///%% with "
                "overflow/underflow checks, SUM/AVG/MIN/MAX, indexed, validated)\n");
    return 0;
}
