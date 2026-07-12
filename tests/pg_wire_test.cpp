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
std::vector<std::byte> password_msg(const std::string& pwd) {
    std::vector<std::byte> m;
    m.push_back(static_cast<std::byte>('p'));
    pw::pg_put_i32(m, static_cast<std::int32_t>(pwd.size() + 1 + 4));
    for (char c : pwd) m.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    m.push_back(std::byte{0});
    return m;
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
        bool cols_ok = false, oids_ok = false;
        for (const Msg& m : ms)
            if (m.type == 'T') {
                std::size_t p = 2;  // skip the int16 field count
                const std::string c0 = cstring(m.body, p);
                const std::int32_t oid0 = pw::pg_get_i32(m.body.data() + p + 6);  // typeOID @ +6
                p += 18;  // skip the 18 fixed field bytes
                const std::string c1 = cstring(m.body, p);
                const std::int32_t oid1 = pw::pg_get_i32(m.body.data() + p + 6);
                cols_ok = (geti16(m.body.data()) == 2 && c0 == "id" && c1 == "name");
                oids_ok = (oid0 == 20 && oid1 == 25);  // id=int8(20), name=text(25)
            }
        check(cols_ok, "SELECT -> RowDescription columns [id, name]");
        check(oids_ok, "SELECT -> per-type OIDs: id=int8(20), name=text(25)");
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

    // (4b) EXTENDED protocol: Parse / Bind($1=1) / Describe / Execute / Sync for a prepared
    //      "SELECT id, name FROM t WHERE id = $1" -> ParseComplete, BindComplete,
    //      RowDescription, one DataRow [1, alice], CommandComplete "SELECT 1", ReadyForQuery.
    {
        std::vector<std::byte> ext;
        // Parse 'P': unnamed stmt, query, 0 param types.
        {
            std::vector<std::byte> p;
            pw::pg_put_str(p, "");  // statement name (unnamed)
            pw::pg_put_str(p, "SELECT id, name FROM t WHERE id = $1");
            pw::pg_put_i16(p, 0);  // number of parameter type OIDs
            ext.push_back(static_cast<std::byte>('P'));
            pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
            ext.insert(ext.end(), p.begin(), p.end());
        }
        // Bind 'B': unnamed portal, unnamed stmt, 0 fmt codes, 1 param = "1", 0 result fmts.
        {
            std::vector<std::byte> p;
            pw::pg_put_str(p, "");  // portal
            pw::pg_put_str(p, "");  // statement
            pw::pg_put_i16(p, 0);   // format codes
            pw::pg_put_i16(p, 1);   // one parameter
            pw::pg_put_i32(p, 1);   // param length
            p.push_back(static_cast<std::byte>('1'));
            pw::pg_put_i16(p, 0);   // result format codes
            ext.push_back(static_cast<std::byte>('B'));
            pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
            ext.insert(ext.end(), p.begin(), p.end());
        }
        // Describe 'D': portal ''.
        {
            std::vector<std::byte> p;
            p.push_back(static_cast<std::byte>('P'));
            pw::pg_put_str(p, "");
            ext.push_back(static_cast<std::byte>('D'));
            pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
            ext.insert(ext.end(), p.begin(), p.end());
        }
        // Execute 'E': portal '', 0 max rows.
        {
            std::vector<std::byte> p;
            pw::pg_put_str(p, "");
            pw::pg_put_i32(p, 0);
            ext.push_back(static_cast<std::byte>('E'));
            pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
            ext.insert(ext.end(), p.begin(), p.end());
        }
        // Sync 'S'.
        ext.push_back(static_cast<std::byte>('S'));
        pw::pg_put_i32(ext, 4);

        const auto ms = parse_backend(session.feed(sp(ext)));
        check(has_type(ms, '1'), "extended: ParseComplete '1'");
        check(has_type(ms, '2'), "extended: BindComplete '2'");
        check(has_type(ms, 'T'), "extended: RowDescription 'T' (from Describe)");
        int drows = 0;
        std::vector<std::string> vals;
        for (const Msg& m : ms)
            if (m.type == 'D') {
                ++drows;
                std::size_t p = 2;
                for (int c = 0; c < 2; ++c) {
                    const std::int32_t vl = pw::pg_get_i32(m.body.data() + p);
                    p += 4;
                    vals.emplace_back(reinterpret_cast<const char*>(m.body.data() + p),
                                      vl < 0 ? 0 : static_cast<std::size_t>(vl));
                    if (vl > 0) p += static_cast<std::size_t>(vl);
                }
            }
        check(drows == 1 && vals.size() == 2 && vals[0] == "1" && vals[1] == "alice",
              "extended: bound $1=1 -> one DataRow [1, alice]");
        bool tag = false;
        for (const Msg& m : ms)
            if (m.type == 'C') { std::size_t p = 0; tag = (cstring(m.body, p) == "SELECT 1"); }
        check(tag, "extended: CommandComplete 'SELECT 1'");
        check(has_type(ms, 'Z'), "extended: Sync -> ReadyForQuery");
    }

    // (4c) AUTH: with an authenticator set, startup requests a cleartext password.
    {
        pw::PgSession as([&engine](const std::string& s) -> ExecResult { return engine.exec(s); },
                         [](const std::string& u, const std::string& p) { return u == "lockstep" && p == "secret"; });
        const auto r1 = parse_backend(as.feed(sp(startup())));
        bool req = false;
        for (const Msg& m : r1)
            if (m.type == 'R' && m.body.size() >= 4 && pw::pg_get_i32(m.body.data()) == 3) req = true;
        check(req, "auth: startup -> AuthenticationCleartextPassword (code 3)");
        const auto r2 = parse_backend(as.feed(sp(password_msg("secret"))));
        bool ok = false;
        for (const Msg& m : r2)
            if (m.type == 'R' && m.body.size() >= 4 && pw::pg_get_i32(m.body.data()) == 0) ok = true;
        check(ok && has_type(r2, 'Z'), "auth: correct password -> AuthenticationOk + ReadyForQuery");
        check(!as.closed(), "auth: session open after successful auth");
    }
    {
        pw::PgSession bad([&engine](const std::string& s) -> ExecResult { return engine.exec(s); },
                          [](const std::string&, const std::string&) { return false; });
        (void)bad.feed(sp(startup()));
        const auto r = parse_backend(bad.feed(sp(password_msg("wrong"))));
        check(has_type(r, 'E'), "auth: wrong password -> ErrorResponse");
        check(bad.closed(), "auth: wrong password closes the session");
    }

    // (5) Terminate -> session closed.
    {
        std::vector<std::byte> term;
        term.push_back(static_cast<std::byte>('X'));
        pw::pg_put_i32(term, 4);
        (void)session.feed(sp(term));
        check(session.closed(), "Terminate 'X' closes the session");
    }

    // (W3.4) BackendKeyData on accept + CancelRequest recognition.
    {
        pw::PgSession keyed([&engine](const std::string& s) -> ExecResult { return engine.exec(s); });
        keyed.set_backend_key(4242, 0x1337BEEF);
        const auto ms = parse_backend(keyed.feed(sp(startup())));
        check(has_type(ms, 'K'), "W3.4: startup with a backend key emits BackendKeyData 'K'");

        // A CancelRequest packet: [len=16][code=80877102][pid][secret].
        std::vector<std::byte> cr;
        pw::pg_put_i32(cr, 16);
        pw::pg_put_i32(cr, 80877102);
        pw::pg_put_i32(cr, 4242);
        pw::pg_put_i32(cr, 0x1337BEEF);
        pw::PgSession canceler([&engine](const std::string& s) -> ExecResult { return engine.exec(s); });
        (void)canceler.feed(sp(cr));
        check(canceler.is_cancel_request(), "W3.4: CancelRequest recognized");
        check(canceler.cancel_target_pid() == 4242 && canceler.cancel_target_secret() == 0x1337BEEF,
              "W3.4: CancelRequest carries the target (pid, secret)");
        check(canceler.closed(), "W3.4: a CancelRequest connection is closed");
    }

    // (W3.5) backpressure: a frame claiming more than the cap is rejected + the connection
    // closed BEFORE its bytes are accumulated (a firehose can't grow the buffer unbounded).
    {
        pw::PgSession bp([&engine](const std::string& s) -> ExecResult { return engine.exec(s); });
        (void)bp.feed(sp(startup()));  // complete the handshake -> regular phase
        // Regular-phase frame 'Q' with a bogus huge length (only the 5-byte header is sent).
        std::vector<std::byte> hdr;
        hdr.push_back(static_cast<std::byte>('Q'));
        pw::pg_put_i32(hdr, 200 * 1024 * 1024);  // 200 MiB > 64 MiB cap
        const auto ms = parse_backend(bp.feed(sp(hdr)));
        check(has_type(ms, 'E'), "W3.5: oversized frame -> ErrorResponse 'E'");
        check(bp.closed(), "W3.5: oversized frame closes the connection (no unbounded buffering)");
    }

    // (K1) VECTOR over the wire: the vector type OID, pg_type discovery, and pgvector's
    // BINARY parameter format through the extended protocol.
    {
        (void)session.feed(sp(query(
            "CREATE TABLE docs (id INT, emb VECTOR(3) NOT NULL, PRIMARY KEY (id))")));
        (void)session.feed(sp(query("INSERT INTO docs (id,emb) VALUES (1, '[1,0,0]')")));
        // RowDescription advertises the vector OID; DataRow carries the pgvector text form.
        {
            const auto ms = parse_backend(session.feed(sp(query("SELECT emb FROM docs WHERE id = 1"))));
            bool oid_ok = false;
            for (const Msg& m : ms)
                if (m.type == 'T') {
                    std::size_t p = 2;
                    (void)cstring(m.body, p);
                    oid_ok = (pw::pg_get_i32(m.body.data() + p + 6) == pw::kPgVectorOid);
                }
            check(oid_ok, "K1: VECTOR column advertises the vector OID (16388)");
            bool val_ok = false;
            for (const Msg& m : ms)
                if (m.type == 'D') {
                    std::size_t p = 2;
                    const std::int32_t vl = pw::pg_get_i32(m.body.data() + p);
                    p += 4;
                    val_ok = vl > 0 && std::string(reinterpret_cast<const char*>(m.body.data() + p),
                                                   static_cast<std::size_t>(vl)) == "[1,0,0]";
                }
            check(val_ok, "K1: VECTOR DataRow is pgvector text '[1,0,0]'");
        }
        // pg_type discovery — the query the pgvector client adapters run at registration.
        {
            const auto ms = parse_backend(
                session.feed(sp(query("SELECT oid FROM pg_type WHERE typname = 'vector'"))));
            bool ok = false;
            for (const Msg& m : ms)
                if (m.type == 'D') {
                    std::size_t p = 2;
                    const std::int32_t vl = pw::pg_get_i32(m.body.data() + p);
                    p += 4;
                    ok = vl > 0 && std::string(reinterpret_cast<const char*>(m.body.data() + p),
                                               static_cast<std::size_t>(vl)) == "16388";
                }
            check(ok, "K1: pg_type discovery returns the vector OID");
        }
        // Extended protocol with a BINARY vector parameter (pgvector's binary format:
        // uint16 dim, uint16 unused, dim x float4 big-endian) declared via the Parse OID.
        {
            std::vector<std::byte> ext;
            {
                std::vector<std::byte> p;
                pw::pg_put_str(p, "");
                pw::pg_put_str(p, "INSERT INTO docs (id, emb) VALUES ($1, $2)");
                pw::pg_put_i16(p, 2);   // two declared param types
                pw::pg_put_i32(p, 20);  // $1 int8
                pw::pg_put_i32(p, pw::kPgVectorOid);  // $2 vector
                ext.push_back(static_cast<std::byte>('P'));
                pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
                ext.insert(ext.end(), p.begin(), p.end());
            }
            {
                std::vector<std::byte> p;
                pw::pg_put_str(p, "");
                pw::pg_put_str(p, "");
                pw::pg_put_i16(p, 2);  // per-param format codes
                pw::pg_put_i16(p, 1);  // $1 binary
                pw::pg_put_i16(p, 1);  // $2 binary
                pw::pg_put_i16(p, 2);  // two parameters
                pw::pg_put_i32(p, 8);  // $1: int8 binary = 8 bytes
                for (int i = 7; i >= 0; --i)
                    p.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(2) >> (8 * i)) & 0xFF));
                pw::pg_put_i32(p, 4 + 3 * 4);  // $2: vector binary
                pw::pg_put_i16(p, 3);          // dim
                pw::pg_put_i16(p, 0);          // unused
                const float fv[3] = {0.0F, 1.0F, 0.0F};
                for (const float f : fv) {
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &f, sizeof(bits));
                    for (int i = 3; i >= 0; --i)
                        p.push_back(static_cast<std::byte>((bits >> (8 * i)) & 0xFF));
                }
                pw::pg_put_i16(p, 0);  // result format codes
                ext.push_back(static_cast<std::byte>('B'));
                pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
                ext.insert(ext.end(), p.begin(), p.end());
            }
            {
                std::vector<std::byte> p;
                pw::pg_put_str(p, "");
                pw::pg_put_i32(p, 0);
                ext.push_back(static_cast<std::byte>('E'));
                pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
                ext.insert(ext.end(), p.begin(), p.end());
            }
            ext.push_back(static_cast<std::byte>('S'));
            pw::pg_put_i32(ext, 4);
            const auto ms = parse_backend(session.feed(sp(ext)));
            check(has_type(ms, '2') && !has_type(ms, 'E'),
                  "K1: binary int8 + binary vector params bind clean");
            // The row landed with the decoded vector.
            const auto sel = parse_backend(session.feed(sp(query("SELECT emb FROM docs WHERE id = 2"))));
            bool ok = false;
            for (const Msg& m : sel)
                if (m.type == 'D') {
                    std::size_t p = 2;
                    const std::int32_t vl = pw::pg_get_i32(m.body.data() + p);
                    p += 4;
                    ok = vl > 0 && std::string(reinterpret_cast<const char*>(m.body.data() + p),
                                               static_cast<std::size_t>(vl)) == "[0,1,0]";
                }
            check(ok, "K1: binary vector param round-trips as '[0,1,0]'");
        }
        // A binary RESULT-format request is a clean error (text-only results today).
        {
            std::vector<std::byte> ext;
            {
                std::vector<std::byte> p;
                pw::pg_put_str(p, "");
                pw::pg_put_str(p, "SELECT id FROM docs");
                pw::pg_put_i16(p, 0);
                ext.push_back(static_cast<std::byte>('P'));
                pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
                ext.insert(ext.end(), p.begin(), p.end());
            }
            {
                std::vector<std::byte> p;
                pw::pg_put_str(p, "");
                pw::pg_put_str(p, "");
                pw::pg_put_i16(p, 0);  // no param formats
                pw::pg_put_i16(p, 0);  // no params
                pw::pg_put_i16(p, 1);  // one result format code...
                pw::pg_put_i16(p, 1);  // ...binary -> rejected
                ext.push_back(static_cast<std::byte>('B'));
                pw::pg_put_i32(ext, static_cast<std::int32_t>(p.size() + 4));
                ext.insert(ext.end(), p.begin(), p.end());
            }
            ext.push_back(static_cast<std::byte>('S'));
            pw::pg_put_i32(ext, 4);
            const auto ms = parse_backend(session.feed(sp(ext)));
            check(has_type(ms, 'E'), "K1: binary result-format request -> clean ErrorResponse");
        }
    }
    // (K4.5) LISTEN/NOTIFY over named changefeeds: push-wake + pull-batch.
    {
        auto notif = [](const std::vector<Msg>& ms) -> std::string {
            for (const Msg& m : ms) {
                if (m.type != 'A') continue;
                // [int32 pid][channel cstr][payload cstr]
                std::string chan, pay;
                std::size_t i = 4;
                while (i < m.body.size() && m.body[i] != std::byte{0})
                    chan += std::to_integer<char>(m.body[i++]);
                ++i;
                while (i < m.body.size() && m.body[i] != std::byte{0})
                    pay += std::to_integer<char>(m.body[i++]);
                return chan + "=" + pay;
            }
            return "";
        };
        (void)session.feed(sp(query("CREATE CHANGEFEED cf FOR t")));
        {
            const auto ms = parse_backend(session.feed(sp(query("LISTEN ghost"))));
            check(has_type(ms, 'E'), "LISTEN on an unknown feed errors");
        }
        {
            // Rows already exist and are unacked -> LISTEN's own boundary announces them.
            const auto ms = parse_backend(session.feed(sp(query("LISTEN cf"))));
            check(has_type(ms, 'C') && has_type(ms, 'A'),
                  "LISTEN ok + immediate 'A' for the existing unacked backlog");
        }
        {
            const auto ms = parse_backend(session.feed(sp(query("SELECT id FROM t WHERE id = 1"))));
            const std::string n = notif(ms);
            check(n.empty(), "announced batch is not re-announced (de-dup)");
        }
        {
            // ACK everything, then a quiet command: no notification.
            const auto f = engine.exec("FETCH cf");
            const std::int64_t tip = f.rows.back().cells[0].second.i;
            (void)session.feed(sp(query("ACK CHANGEFEED cf AT " + std::to_string(tip))));
            const auto ms = parse_backend(session.feed(sp(query("SELECT id FROM t WHERE id = 1"))));
            check(notif(ms).empty(), "fully acked feed is silent");
        }
        {
            // New commit -> next boundary carries 'A' with the first unacked _seq.
            const auto ins = parse_backend(
                session.feed(sp(query("INSERT INTO t (id, name) VALUES (7, 'push')"))));
            const std::string n = notif(ins);
            check(!n.empty() && n.rfind("cf=", 0) == 0, "new write announced on channel cf");
            const auto f = engine.exec("FETCH cf LIMIT 1");
            check(n == "cf=" + std::to_string(f.rows[0].cells[0].second.i),
                  "payload = first unacked _seq");
        }
        {
            const auto ms = parse_backend(session.feed(sp(query("UNLISTEN cf"))));
            check(has_type(ms, 'C'), "UNLISTEN ok");
            (void)session.feed(sp(query("INSERT INTO t (id, name) VALUES (8, 'quiet')")));
            const auto ms2 = parse_backend(session.feed(sp(query("SELECT id FROM t WHERE id = 8"))));
            check(notif(ms2).empty(), "after UNLISTEN: silence");
        }
    }


    if (g_fail) { std::printf("pg_wire_test: FAILED\n"); return 1; }
    std::printf("pg_wire_test: OK (handshake + simple query + EXTENDED protocol (bound $1) + "
                "per-type OIDs + cleartext-password auth)\n");
    return 0;
}
