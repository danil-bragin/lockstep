// pg_wire_test.cpp — the PostgreSQL wire shim (query/wire/PgWire.hpp) driven end to end
// against a real SqlEngine, entirely in memory (no socket). It plays a psql-like session
// as a byte stream — SSLRequest, StartupMessage, then CREATE / INSERT / SELECT / a bad
// query — and decodes the backend reply bytes, asserting the handshake, RowDescription
// columns, DataRow values, CommandComplete tags, and ErrorResponse are correct PG v3.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/wire/PgWire.hpp>

using lockstep::query::sql::ExecResult;
using lockstep::query::sql::SqlEngine;
namespace pw = lockstep::query::wire;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}

struct Backing {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xB0A1'0001ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
    lockstep::core::Scheduler cat_sched;
    lockstep::core::SimClock cat_clock{cat_sched};
    lockstep::sim::SeededRandom cat_rng{0xB0A1'0002ULL};
    lockstep::sim::SimDisk cat_disk{cat_sched, cat_clock, cat_rng};
};

// A decoded backend message: its type byte + payload (the bytes after the int32 length).
struct Msg { char type; std::vector<std::byte> body; };
std::vector<Msg> parse_backend(const std::vector<std::byte>& bytes) {
    std::vector<Msg> out;
    std::size_t i = 0;
    while (i + 5 <= bytes.size()) {
        const char type = std::to_integer<char>(bytes[i]);
        const std::int32_t len = pw::pg_get_i32(bytes.data() + i + 1);
        if (len < 4 || i + 1 + static_cast<std::size_t>(len) > bytes.size()) break;
        Msg m;
        m.type = type;
        m.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(i + 5),
                      bytes.begin() + static_cast<std::ptrdiff_t>(i + 1 + static_cast<std::size_t>(len)));
        out.push_back(std::move(m));
        i += 1 + static_cast<std::size_t>(len);
    }
    return out;
}
bool has_type(const std::vector<Msg>& ms, char t) {
    for (const Msg& m : ms) if (m.type == t) return true;
    return false;
}
std::string cstring(const std::vector<std::byte>& b, std::size_t& pos) {
    std::string s;
    while (pos < b.size() && std::to_integer<char>(b[pos]) != 0) s += std::to_integer<char>(b[pos++]);
    if (pos < b.size()) ++pos;  // skip NUL
    return s;
}
std::int16_t geti16(const std::byte* p) {
    return static_cast<std::int16_t>((std::to_integer<unsigned>(p[0]) << 8) | std::to_integer<unsigned>(p[1]));
}

std::span<const std::byte> sp(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }

// Build a client StartupMessage (protocol 3.0) with a user parameter.
std::vector<std::byte> startup() {
    std::vector<std::byte> payload;
    pw::pg_put_i32(payload, 196608);  // protocol 3.0
    pw::pg_put_str(payload, "user");
    pw::pg_put_str(payload, "lockstep");
    payload.push_back(std::byte{0});  // end of params
    std::vector<std::byte> msg;
    pw::pg_put_i32(msg, static_cast<std::int32_t>(payload.size() + 4));
    msg.insert(msg.end(), payload.begin(), payload.end());
    return msg;
}
std::vector<std::byte> ssl_request() {
    std::vector<std::byte> msg;
    pw::pg_put_i32(msg, 8);
    pw::pg_put_i32(msg, 80877103);
    return msg;
}
std::vector<std::byte> query(const std::string& sql) {
    std::vector<std::byte> msg;
    msg.push_back(static_cast<std::byte>('Q'));
    pw::pg_put_i32(msg, static_cast<std::int32_t>(sql.size() + 1 + 4));  // len + sql + NUL
    for (char c : sql) msg.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    msg.push_back(std::byte{0});
    return msg;
}
}  // namespace

int main() {
    Backing b;
    SqlEngine engine(b.sched, b.disk, b.cat_sched, b.cat_disk);
    pw::PgSession session([&engine](const std::string& s) -> ExecResult { return engine.exec(s); });

    // (0) SSLRequest -> 'N' (decline), then StartupMessage -> AuthOk + ParameterStatus + ReadyForQuery.
    {
        const std::vector<std::byte> r = session.feed(sp(ssl_request()));
        check(r.size() == 1 && std::to_integer<char>(r[0]) == 'N', "SSLRequest declined with 'N'");
    }
    {
        const auto ms = parse_backend(session.feed(sp(startup())));
        check(has_type(ms, 'R'), "startup -> AuthenticationOk 'R'");
        check(has_type(ms, 'Z'), "startup -> ReadyForQuery 'Z'");
    }

    // (1) CREATE TABLE -> CommandComplete "CREATE TABLE" + ReadyForQuery.
    {
        const auto ms = parse_backend(session.feed(sp(query("CREATE TABLE t (id INT, name TEXT, PRIMARY KEY (id))"))));
        bool tag_ok = false;
        for (const Msg& m : ms)
            if (m.type == 'C') { std::size_t p = 0; tag_ok = (cstring(m.body, p) == "CREATE TABLE"); }
        check(tag_ok, "CREATE TABLE -> CommandComplete 'CREATE TABLE'");
        check(has_type(ms, 'Z'), "CREATE -> ReadyForQuery");
    }

    // (2) INSERT -> "INSERT 0 2".
    {
        const auto ms = parse_backend(session.feed(sp(query("INSERT INTO t (id, name) VALUES (1, 'alice'), (2, 'bob')"))));
        bool tag_ok = false;
        for (const Msg& m : ms)
            if (m.type == 'C') { std::size_t p = 0; tag_ok = (cstring(m.body, p) == "INSERT 0 2"); }
        check(tag_ok, "INSERT -> CommandComplete 'INSERT 0 2'");
    }

    // (3) SELECT -> RowDescription(id, name) + 2 DataRows + "SELECT 2".
    {
        const auto ms = parse_backend(session.feed(sp(query("SELECT id, name FROM t"))));
        // RowDescription 'T' with two columns named id, name.
        bool cols_ok = false;
        for (const Msg& m : ms)
            if (m.type == 'T') {
                std::size_t p = 2;  // skip the int16 field count
                const std::string c0 = cstring(m.body, p);
                p += 18;  // skip the 18 fixed field bytes
                const std::string c1 = cstring(m.body, p);
                cols_ok = (geti16(m.body.data()) == 2 && c0 == "id" && c1 == "name");
            }
        check(cols_ok, "SELECT -> RowDescription columns [id, name]");
        int datarows = 0;
        std::vector<std::string> first_vals;
        for (const Msg& m : ms)
            if (m.type == 'D') {
                ++datarows;
                if (datarows == 1) {
                    std::size_t p = 2;  // skip col count
                    for (int c = 0; c < 2; ++c) {
                        const std::int32_t vlen = pw::pg_get_i32(m.body.data() + p);
                        p += 4;
                        first_vals.emplace_back(reinterpret_cast<const char*>(m.body.data() + p),
                                                vlen < 0 ? 0 : static_cast<std::size_t>(vlen));
                        if (vlen > 0) p += static_cast<std::size_t>(vlen);
                    }
                }
            }
        check(datarows == 2, "SELECT -> two DataRows");
        check(first_vals.size() == 2 && first_vals[0] == "1" && first_vals[1] == "alice",
              "SELECT -> first DataRow values [1, alice]");
        bool sel_tag = false;
        for (const Msg& m : ms)
            if (m.type == 'C') { std::size_t p = 0; sel_tag = (cstring(m.body, p) == "SELECT 2"); }
        check(sel_tag, "SELECT -> CommandComplete 'SELECT 2'");
    }

    // (4) a bad query -> ErrorResponse 'E' + ReadyForQuery (session stays usable).
    {
        const auto ms = parse_backend(session.feed(sp(query("SELECT * FROM does_not_exist"))));
        check(has_type(ms, 'E'), "bad query -> ErrorResponse 'E'");
        check(has_type(ms, 'Z'), "bad query -> ReadyForQuery (session recovers)");
    }

    // (5) Terminate -> session closed.
    {
        std::vector<std::byte> term;
        term.push_back(static_cast<std::byte>('X'));
        pw::pg_put_i32(term, 4);
        (void)session.feed(sp(term));
        check(session.closed(), "Terminate 'X' closes the session");
    }

    if (g_fail) { std::printf("pg_wire_test: FAILED\n"); return 1; }
    std::printf("pg_wire_test: OK (PG v3 handshake + simple query: CREATE/INSERT/SELECT/error over the shim)\n");
    return 0;
}
