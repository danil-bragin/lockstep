#pragma once
// PgWire.hpp — a PostgreSQL v3 wire-protocol SHIM in front of the SqlEngine, so an
// UNMODIFIED PostgreSQL client (psql, JDBC/psycopg/pgx/... any driver) can talk to
// Lockstep. It is a THIN translation: PG frontend messages -> a SQL string -> the
// verified SqlEngine::exec() path -> PG backend messages (RowDescription / DataRow /
// CommandComplete / ErrorResponse / ReadyForQuery). No new query semantics.
//
// This header is the PURE protocol + a byte-stream session state machine (PgSession):
// feed() takes raw bytes off the wire and returns the raw bytes to write back, framing
// PG messages itself (PG self-frames with a type byte + int32 length; the transport is a
// plain byte STREAM, not our length-prefixed frames). The prod daemon pipes a TCP socket
// through feed(); tests drive it in memory. `exec` is injected as a callback so this
// stays decoupled from the SqlEngine concrete type (and forbidden-lint clean — no IO,
// no clock, no threads here).
//
// SCOPE (increment 1): the SIMPLE query protocol (Query 'Q') + startup/SSL/terminate.
// All result columns are advertised as `text` (OID 25) in TEXT format — every driver
// accepts text and parses client-side; proper per-type OIDs are a follow-on. The
// EXTENDED protocol (Parse/Bind/Execute prepared statements) is a follow-on too.
#include <cstddef>
#include <cstdint>
#include <cstring>  // K1: memcpy for the binary float4/float8 parameter decode
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

namespace lockstep::query::wire {

// PG uses network byte order (BIG-endian) for its int16/int32 fields.
inline void pg_put_i16(std::vector<std::byte>& b, std::int16_t v) {
    b.push_back(static_cast<std::byte>((static_cast<std::uint16_t>(v) >> 8) & 0xFFu));
    b.push_back(static_cast<std::byte>(static_cast<std::uint16_t>(v) & 0xFFu));
}
inline void pg_put_i32(std::vector<std::byte>& b, std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    b.push_back(static_cast<std::byte>((u >> 24) & 0xFFu));
    b.push_back(static_cast<std::byte>((u >> 16) & 0xFFu));
    b.push_back(static_cast<std::byte>((u >> 8) & 0xFFu));
    b.push_back(static_cast<std::byte>(u & 0xFFu));
}
[[nodiscard]] inline std::int16_t pg_get_i16(const std::byte* p) {
    return static_cast<std::int16_t>((std::to_integer<unsigned>(p[0]) << 8) |
                                     std::to_integer<unsigned>(p[1]));
}
[[nodiscard]] inline std::int32_t pg_get_i32(const std::byte* p) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(std::to_integer<unsigned>(p[0])) << 24) |
                                     (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[1])) << 16) |
                                     (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[2])) << 8) |
                                     static_cast<std::uint32_t>(std::to_integer<unsigned>(p[3])));
}
inline void pg_put_str(std::vector<std::byte>& b, const std::string& s) {
    for (char c : s) b.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    b.push_back(std::byte{0});  // C-string NUL terminator
}
inline void pg_put_bytes(std::vector<std::byte>& b, const std::string& s) {
    for (char c : s) b.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
}

// The `text` type OID (25) — the fallback when a column's type is unknown.
inline constexpr std::int32_t kPgTextOid = 25;
// K1: the `vector` type OID. pgvector's OID is per-database (extension-assigned); ours is
// FIXED at a value in the extension range, and pg_catalog.pg_type advertises it — a client
// doing `SELECT oid FROM pg_type WHERE typname = 'vector'` (psycopg register_vector, the
// pgvector client adapters) discovers it exactly like on real PostgreSQL.
inline constexpr std::int32_t kPgVectorOid = 16388;

// Map a Lockstep Datum's type to the closest PostgreSQL type OID. Values are still sent
// in TEXT format; the OID tells a type-aware client how to PARSE that text (int vs string
// vs date vs numeric), so a driver returns a native int/Decimal/date instead of a string.
[[nodiscard]] inline std::int32_t pg_oid_for_datum(const sql::Datum& d) {
    if (d.type == sql::Type::Text) {
        switch (d.logical) {           // TEXT-physical logical subtypes
            case 4: return 2950;       // uuid
            case 11: return 114;       // json
            case 14: return 701;       // float8 (REAL renders as PG float8 text)
            case 15: return kPgVectorOid;  // K1: vector — text form '[x,y,z]' matches pgvector
            default: return kPgTextOid;    // text / varchar / char / enum label / arrays
        }
    }
    switch (d.logical) {                                // Int base, logical subtype (F9b/F13)
        case 1: return 1700;  // numeric  (DECIMAL)
        case 2: return 1082;  // date
        case 3: return 1114;  // timestamp
        case 8: return 1083;  // time
        default: return 20;   // int8 (Lockstep ints are 64-bit)
    }
}

// K1: decode ONE binary-format Bind parameter into its TEXT form by its DECLARED
// Parse-time type OID (network byte order per PG v3). Returns nullopt for an OID we do
// not know how to decode — the caller errors clean instead of splicing garbage bytes.
// The vector shape is pgvector's binary format: uint16 dim, uint16 unused, dim x float4.
[[nodiscard]] inline std::optional<std::string> pg_decode_binary_param(std::int32_t oid,
                                                                       const std::byte* p,
                                                                       std::size_t len) {
    const auto u16 = [&](std::size_t off) {
        return (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[off])) << 8) |
               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[off + 1]));
    };
    const auto u32 = [&](std::size_t off) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(p[off + static_cast<std::size_t>(i)]);
        return v;
    };
    const auto u64 = [&](std::size_t off) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint8_t>(p[off + static_cast<std::size_t>(i)]);
        return v;
    };
    const auto f32 = [&](std::size_t off) {
        const std::uint32_t bits = u32(off);
        float f = 0.0F;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };
    switch (oid) {
        case 16:  // bool
            return len == 1 ? std::optional<std::string>(std::to_integer<std::uint8_t>(p[0]) != 0 ? "TRUE" : "FALSE")
                            : std::nullopt;
        case 21:  // int2
            return len == 2 ? std::optional<std::string>(std::to_string(static_cast<std::int16_t>(u16(0))))
                            : std::nullopt;
        case 23:  // int4
            return len == 4 ? std::optional<std::string>(std::to_string(static_cast<std::int32_t>(u32(0))))
                            : std::nullopt;
        case 20:  // int8
            return len == 8 ? std::optional<std::string>(std::to_string(static_cast<std::int64_t>(u64(0))))
                            : std::nullopt;
        case 700:  // float4
            return len == 4 ? std::optional<std::string>(sql::Datum::render_double(static_cast<double>(f32(0))))
                            : std::nullopt;
        case 701: {  // float8
            if (len != 8) return std::nullopt;
            const std::uint64_t bits = u64(0);
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            return sql::Datum::render_double(d);
        }
        case kPgVectorOid: {  // K1: pgvector binary — [dim u16][unused u16][dim x float4 BE]
            if (len < 4) return std::nullopt;
            const std::uint16_t dim = u16(0);
            if (len != 4 + static_cast<std::size_t>(dim) * 4) return std::nullopt;
            std::string s = "[";
            for (std::uint16_t i = 0; i < dim; ++i) {
                if (i != 0) s += ",";
                s += sql::Datum::render_double(static_cast<double>(f32(4 + static_cast<std::size_t>(i) * 4)));
            }
            s += "]";
            return s;
        }
        case 0:     // unspecified — PG treats the bytes as the text form
        case 25:    // text
        case 1043:  // varchar
            return std::string(reinterpret_cast<const char*>(p), len);
        default:
            return std::nullopt;
    }
}
// The (column name, type OID) list for a result — from the first row's cells (the OID is
// the same for a column across rows). Empty for a 0-row result (types then default text).
[[nodiscard]] inline std::vector<std::pair<std::string, std::int32_t>>
pg_cols_of(const sql::ExecResult& r) {
    std::vector<std::pair<std::string, std::int32_t>> cols;
    if (!r.rows.empty())
        for (const auto& cell : r.rows.front().cells) cols.emplace_back(cell.first, pg_oid_for_datum(cell.second));
    return cols;
}

// ---- backend message builders (each writes ONE self-framed message) --------------------
// A message is: [type byte][int32 length-incl-self][payload]. `emit` writes the header +
// payload; `len` counts the 4 length bytes + the payload (NOT the type byte).
namespace pg_detail {
inline void emit(std::vector<std::byte>& out, char type, const std::vector<std::byte>& payload) {
    out.push_back(static_cast<std::byte>(type));
    pg_put_i32(out, static_cast<std::int32_t>(payload.size() + 4));
    out.insert(out.end(), payload.begin(), payload.end());
}
}  // namespace pg_detail

inline void pg_auth_ok(std::vector<std::byte>& out) {
    std::vector<std::byte> p;
    pg_put_i32(p, 0);  // AuthenticationOk
    pg_detail::emit(out, 'R', p);
}
inline void pg_parameter_status(std::vector<std::byte>& out, const std::string& k, const std::string& v) {
    std::vector<std::byte> p;
    pg_put_str(p, k);
    pg_put_str(p, v);
    pg_detail::emit(out, 'S', p);
}
inline void pg_ready_for_query(std::vector<std::byte>& out) {
    std::vector<std::byte> p;
    p.push_back(static_cast<std::byte>('I'));  // 'I' = idle (not in a transaction block)
    pg_detail::emit(out, 'Z', p);
}
// RowDescription: one field per column (name + type OID), all in text format.
inline void pg_row_description(std::vector<std::byte>& out,
                              const std::vector<std::pair<std::string, std::int32_t>>& cols) {
    std::vector<std::byte> p;
    pg_put_i16(p, static_cast<std::int16_t>(cols.size()));
    for (const auto& [name, oid] : cols) {
        pg_put_str(p, name);
        pg_put_i32(p, 0);       // table OID (unknown)
        pg_put_i16(p, 0);       // column attribute number
        pg_put_i32(p, oid);     // type OID
        pg_put_i16(p, -1);      // type size (variable)
        pg_put_i32(p, -1);      // type modifier
        pg_put_i16(p, 0);       // format code: 0 = text
    }
    pg_detail::emit(out, 'T', p);
}
// DataRow: one column value per cell; a NULL cell is length -1 (no bytes).
inline void pg_data_row(std::vector<std::byte>& out, const sql::ResultRow& row) {
    std::vector<std::byte> p;
    pg_put_i16(p, static_cast<std::int16_t>(row.cells.size()));
    for (const auto& cell : row.cells) {
        if (cell.second.is_null) {
            pg_put_i32(p, -1);  // SQL NULL
        } else {
            const std::string s = cell.second.render();
            pg_put_i32(p, static_cast<std::int32_t>(s.size()));
            pg_put_bytes(p, s);
        }
    }
    pg_detail::emit(out, 'D', p);
}
inline void pg_command_complete(std::vector<std::byte>& out, const std::string& tag) {
    std::vector<std::byte> p;
    pg_put_str(p, tag);
    pg_detail::emit(out, 'C', p);
}
// ErrorResponse with an explicit SQLSTATE code: Severity, Code, Message, then a NUL.
inline void pg_error_response_code(std::vector<std::byte>& out, const std::string& code,
                                   const std::string& msg) {
    std::vector<std::byte> p;
    p.push_back(static_cast<std::byte>('S'));
    pg_put_str(p, "ERROR");
    p.push_back(static_cast<std::byte>('C'));
    pg_put_str(p, code);
    p.push_back(static_cast<std::byte>('M'));
    pg_put_str(p, msg.empty() ? "query failed" : msg);
    p.push_back(std::byte{0});  // field terminator
    pg_detail::emit(out, 'E', p);
}
// ErrorResponse with the generic internal_error SQLSTATE (XX000).
inline void pg_error_response(std::vector<std::byte>& out, const std::string& msg) {
    pg_error_response_code(out, "XX000", msg);
}

// The command tag PG expects in CommandComplete, derived from the statement + counts.
[[nodiscard]] inline std::string pg_command_tag(const std::string& sql, const sql::ExecResult& r,
                                                bool is_select) {
    // First whitespace-delimited keyword, uppercased.
    std::size_t i = 0;
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r')) ++i;
    std::size_t j = i;
    while (j < sql.size() && sql[j] != ' ' && sql[j] != '\t' && sql[j] != '\n' && sql[j] != '\r' &&
           sql[j] != '(')
        ++j;
    std::string kw = sql.substr(i, j - i);
    for (char& c : kw) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
    if (is_select) return "SELECT " + std::to_string(r.rows.size());
    if (kw == "INSERT") return "INSERT 0 " + std::to_string(r.affected);
    if (kw == "UPDATE") return "UPDATE " + std::to_string(r.affected);
    if (kw == "DELETE") return "DELETE " + std::to_string(r.affected);
    // CREATE TABLE / DROP TABLE / BEGIN / COMMIT / ... — the keyword(s) suffice.
    if (kw == "CREATE" || kw == "DROP" || kw == "ALTER") {
        // include the object keyword (TABLE/INDEX/...) if present.
        std::size_t k = j;
        while (k < sql.size() && (sql[k] == ' ' || sql[k] == '\t')) ++k;
        std::size_t m = k;
        while (m < sql.size() && sql[m] != ' ' && sql[m] != '\t' && sql[m] != '(' && sql[m] != '\n') ++m;
        std::string obj = sql.substr(k, m - k);
        for (char& c : obj) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
        return kw + (obj.empty() ? "" : " " + obj);
    }
    return kw.empty() ? "OK" : kw;
}

// Does this statement produce a result set (rows to describe)? By first keyword.
[[nodiscard]] inline bool pg_is_select(const std::string& sql) {
    std::size_t i = 0;
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r' ||
                              sql[i] == '(')) ++i;
    auto up = [&](std::size_t off, const char* w) {
        std::size_t k = 0;
        while (w[k]) {
            if (off + k >= sql.size()) return false;
            char c = sql[off + k];
            c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
            if (c != w[k]) return false;
            ++k;
        }
        return true;
    };
    return up(i, "SELECT") || up(i, "WITH") || up(i, "VALUES") || up(i, "TABLE") || up(i, "EXPLAIN") ||
           up(i, "SHOW");
}

// ---- EXTENDED protocol (prepared statements) helpers ----------------------------------
// A bound parameter value -> a SQL literal to splice into the query text (our SqlEngine
// takes a SQL string, so extended-protocol params are substituted, not truly bound). A
// NULL is the keyword NULL; a numeric-looking text is spliced verbatim; anything else is a
// single-quoted string literal with '' escaping. Handles TEXT-format params (format 0);
// binary-format params are rendered as-is (drivers overwhelmingly use text params).
[[nodiscard]] inline std::string pg_param_literal(const std::string& v, bool is_null) {
    if (is_null) return "NULL";
    if (v.empty()) return "''";
    bool numeric = true;
    std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
    bool digit = false, dot = false;
    for (; i < v.size(); ++i) {
        if (v[i] >= '0' && v[i] <= '9') { digit = true; }
        else if (v[i] == '.' && !dot) { dot = true; }
        else { numeric = false; break; }
    }
    if (numeric && digit) return v;  // splice numbers unquoted
    std::string out = "'";
    for (char c : v) { if (c == '\'') out += "''"; else out += c; }
    out += "'";
    return out;
}
// Replace $1,$2,... in `sql` with `lits` (1-based), skipping $N inside a '...' string.
[[nodiscard]] inline std::string pg_substitute_params(const std::string& sql,
                                                      const std::vector<std::string>& lits) {
    std::string out;
    bool in_str = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (c == '\'') { in_str = !in_str; out += c; continue; }
        if (!in_str && c == '$' && i + 1 < sql.size() && sql[i + 1] >= '1' && sql[i + 1] <= '9') {
            std::size_t j = i + 1;
            std::uint32_t n = 0;
            while (j < sql.size() && sql[j] >= '0' && sql[j] <= '9') { n = n * 10 + static_cast<std::uint32_t>(sql[j] - '0'); ++j; }
            out += (n >= 1 && n <= lits.size()) ? lits[n - 1] : "NULL";
            i = j - 1;
            continue;
        }
        out += c;
    }
    return out;
}
inline void pg_parse_complete(std::vector<std::byte>& out) { pg_detail::emit(out, '1', {}); }
inline void pg_bind_complete(std::vector<std::byte>& out) { pg_detail::emit(out, '2', {}); }
inline void pg_close_complete(std::vector<std::byte>& out) { pg_detail::emit(out, '3', {}); }
inline void pg_no_data(std::vector<std::byte>& out) { pg_detail::emit(out, 'n', {}); }
inline void pg_parameter_description(std::vector<std::byte>& out, std::int16_t nparams) {
    std::vector<std::byte> p;
    pg_put_i16(p, nparams);
    for (std::int16_t k = 0; k < nparams; ++k) pg_put_i32(p, 0);  // type OID unknown
    pg_detail::emit(out, 't', p);
}

// Build the backend reply for ONE executed statement (no ReadyForQuery — the caller emits
// exactly one per Query message, after all statements in it).
inline void pg_reply_for_statement(std::vector<std::byte>& out, const std::string& sql,
                                   const sql::ExecResult& r) {
    if (!r.ok) {
        pg_error_response(out, r.error);
        return;
    }
    const bool is_sel = pg_is_select(sql) || !r.rows.empty();
    if (is_sel) {
        pg_row_description(out, pg_cols_of(r));
        for (const sql::ResultRow& row : r.rows) pg_data_row(out, row);
    }
    pg_command_complete(out, pg_command_tag(sql, r, is_sel));
}

// ----------------------------------------------------------------------------------------
// PgSession — the byte-stream state machine. feed(input) buffers bytes, parses every
// COMPLETE PG frontend message available, handles each, and returns the bytes to write
// back. It self-frames PG messages; the transport is a plain byte stream.
// ----------------------------------------------------------------------------------------
// K4.13: text wire value + declared OID -> a typed engine Datum. Unknown/0 OID = TEXT
// (the engine coerces where needed, same as a quoted literal would).
[[nodiscard]] inline sql::Datum pg_typed_datum(std::int32_t oid, const std::string& txt) {
    if (oid == 20 || oid == 23 || oid == 21) {  // int8 / int4 / int2
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(txt.c_str(), &end, 10);
        if (errno == 0 && end != nullptr && *end == 0) return sql::Datum::make_int(v);
    }
    if (oid == 700 || oid == 701) {  // float4 / float8
        double d = 0.0;
        if (sql::parse_double_strict(txt.data(), txt.data() + txt.size(), d))
            return sql::Datum::make_real(d);
    }
    return sql::Datum::make_text(txt);
}

class PgSession {
public:
    using ExecFn = std::function<sql::ExecResult(const std::string&)>;
    // Optional authenticator: (user, password) -> allowed. UNSET (default) = trust (no
    // password requested), preserving the open path. When set, the shim requests a
    // cleartext password on startup and validates it (map user -> RBAC role here).
    using AuthFn = std::function<bool(const std::string& user, const std::string& password)>;
    // K4.13: optional typed-parameter executor (SqlEngine::exec_prepared). When set,
    // Bind keeps the statement text INTACT (with its $N placeholders — one parse-cache
    // entry per statement shape) and carries the parameters as typed Datums; Execute
    // runs through it. When unset, the classic substitute-into-text path is used.
    using ExecPreparedFn =
        std::function<sql::ExecResult(const std::string&, std::vector<sql::Datum>)>;
    explicit PgSession(ExecFn exec, AuthFn auth = {}, ExecPreparedFn exec_prepared = {})
        : exec_(std::move(exec)), auth_(std::move(auth)),
          exec_prepared_(std::move(exec_prepared)) {}

    // W3.5 backpressure: the largest single protocol frame accepted. A frame claiming more
    // (a firehose / oversized query) is rejected with a 54000 error and the connection closed,
    // BEFORE its bytes are accumulated — so a hostile client can't grow the input buffer
    // unbounded. 64 MiB comfortably exceeds any real query / bind payload.
    static constexpr std::int32_t kMaxMessageBytes = 64 * 1024 * 1024;

    [[nodiscard]] bool closed() const noexcept { return closed_; }

    // W3.4: the server assigns this session's cancel key BEFORE the handshake, so accept()
    // can advertise it in BackendKeyData. 0/0 (the default) suppresses BackendKeyData.
    void set_backend_key(std::int32_t pid, std::int32_t secret) noexcept {
        backend_pid_ = pid;
        backend_secret_ = secret;
    }
    // W3.4: true iff this connection was a PG CancelRequest (not a normal session). The
    // server reads the named target key and, if it matches a live session, sets that
    // session's cancel flag. A CancelRequest connection carries no query and is closed.
    [[nodiscard]] bool is_cancel_request() const noexcept { return is_cancel_request_; }
    [[nodiscard]] std::int32_t cancel_target_pid() const noexcept { return cancel_pid_; }
    [[nodiscard]] std::int32_t cancel_target_secret() const noexcept { return cancel_secret_; }

    // Feed raw bytes read off the socket; returns raw bytes to write back (possibly empty).
    [[nodiscard]] std::vector<std::byte> feed(std::span<const std::byte> input) {
        buf_.insert(buf_.end(), input.begin(), input.end());
        std::vector<std::byte> out;
        for (;;) {
            if (!started_ && !awaiting_password_) {
                // The startup phase has NO type byte: [int32 len][int32 code][params...].
                if (buf_.size() < 8) break;
                const std::int32_t len = pg_get_i32(buf_.data());
                // W3.5: reject an over-large startup frame BEFORE accumulating its bytes (a
                // firehose/oversized message must not grow buf_ unbounded).
                if (len > kMaxMessageBytes) {
                    pg_error_response_code(out, "54000",
                                           "startup message too large (backpressure limit)");
                    closed_ = true;
                    break;
                }
                if (len < 8 || buf_.size() < static_cast<std::size_t>(len)) break;
                const std::int32_t code = pg_get_i32(buf_.data() + 4);
                if (code == 80877103) {  // SSLRequest: decline (plaintext shim).
                    out.push_back(static_cast<std::byte>('N'));
                    buf_.erase(buf_.begin(), buf_.begin() + len);
                    continue;  // the client resends a real StartupMessage.
                }
                if (code == 80877102) {  // W3.4 CancelRequest: [len=16][code][pid][secret].
                    if (len >= 16) {
                        cancel_pid_ = pg_get_i32(buf_.data() + 8);
                        cancel_secret_ = pg_get_i32(buf_.data() + 12);
                        is_cancel_request_ = true;
                    }
                    buf_.erase(buf_.begin(), buf_.begin() + len);
                    closed_ = true;  // a cancel connection carries no query; the server acts + closes.
                    break;
                }
                // A real StartupMessage (protocol 3.0 = 196608).
                user_ = startup_user(buf_.data(), static_cast<std::size_t>(len));
                buf_.erase(buf_.begin(), buf_.begin() + len);
                if (auth_) {
                    // Request a cleartext password ('R' + int32 3); validate on PasswordMessage.
                    std::vector<std::byte> p;
                    pg_put_i32(p, 3);
                    pg_detail::emit(out, 'R', p);
                    awaiting_password_ = true;
                } else {
                    accept(out);  // trust: no auth configured.
                }
                continue;
            }
            if (awaiting_password_) {
                // PasswordMessage 'p': [type][int32 len][password\0].
                if (buf_.size() < 5) break;
                const std::int32_t plen = pg_get_i32(buf_.data() + 1);
                if (plen < 4 || buf_.size() < static_cast<std::size_t>(plen) + 1) break;
                std::string pw(reinterpret_cast<const char*>(buf_.data() + 5),
                               plen > 4 ? static_cast<std::size_t>(plen) - 4 - 1 : 0);
                buf_.erase(buf_.begin(), buf_.begin() + plen + 1);
                awaiting_password_ = false;
                if (auth_(user_, pw)) {
                    accept(out);
                } else {
                    pg_error_response_code(out, "28P01", "password authentication failed for user \"" + user_ + "\"");
                    closed_ = true;
                    break;
                }
                continue;
            }
            // Regular phase: [type byte][int32 len][payload].
            if (buf_.size() < 5) break;
            const char type = std::to_integer<char>(buf_[0]);
            const std::int32_t len = pg_get_i32(buf_.data() + 1);
            // W3.5: reject an over-large frame BEFORE accumulating its payload (backpressure).
            if (len > kMaxMessageBytes) {
                pg_error_response_code(out, "54000", "message too large (backpressure limit)");
                closed_ = true;
                break;
            }
            if (len < 4 || buf_.size() < static_cast<std::size_t>(len) + 1) break;
            const std::byte* payload = buf_.data() + 5;
            const std::size_t plen = static_cast<std::size_t>(len) - 4;
            if (type == 'Q') {
                std::string sql(reinterpret_cast<const char*>(payload),
                                plen > 0 ? plen - 1 : 0);  // drop the trailing NUL
                handle_query(out, sql);
            } else if (type == 'X') {  // Terminate
                closed_ = true;
                buf_.erase(buf_.begin(), buf_.begin() + len + 1);
                break;
            } else if (type == 'P') {  // Parse (prepare a statement)
                handle_parse(out, payload, plen);
            } else if (type == 'B') {  // Bind (parameters -> a portal)
                handle_bind(out, payload, plen);
            } else if (type == 'D') {  // Describe (statement or portal)
                handle_describe(out, payload, plen);
            } else if (type == 'E') {  // Execute (run a portal)
                handle_execute(out, payload, plen);
            } else if (type == 'C') {  // Close (a statement or portal)
                handle_close(out, payload, plen);
            } else if (type == 'S') {  // Sync (end of the extended-query cycle)
                poll_notifications(out);
                pg_ready_for_query(out);
            }
            // 'H' Flush and any unknown message: nothing to do (we return `out` already).
            buf_.erase(buf_.begin(), buf_.begin() + len + 1);
        }
        return out;
    }

private:
    // Complete the handshake: AuthenticationOk + a couple of ParameterStatus + ReadyForQuery.
    void accept(std::vector<std::byte>& out) {
        started_ = true;
        pg_auth_ok(out);
        pg_parameter_status(out, "server_version", "14.0 (Lockstep)");
        pg_parameter_status(out, "client_encoding", "UTF8");
        // W3.4: advertise this session's cancel key so the client can later CancelRequest it.
        if (backend_pid_ != 0 || backend_secret_ != 0) {
            std::vector<std::byte> k;
            pg_put_i32(k, backend_pid_);
            pg_put_i32(k, backend_secret_);
            pg_detail::emit(out, 'K', k);  // BackendKeyData
        }
        pg_ready_for_query(out);
    }
    // Extract the "user" parameter from a StartupMessage ([int32 len][int32 code][k\0 v\0 ...]).
    [[nodiscard]] static std::string startup_user(const std::byte* data, std::size_t len) {
        std::size_t pos = 8;  // skip the length + protocol code
        while (pos < len) {
            const std::string key = read_cstring(data, len, pos);
            if (key.empty()) break;  // the trailing NUL ends the parameter list
            const std::string val = read_cstring(data, len, pos);
            if (key == "user") return val;
        }
        return "";
    }

    // K4.5: LISTEN <changefeed> / UNLISTEN <changefeed> — the PG push surface over named
    // changefeeds. PG semantics are per-connection, so this lives in the session, not the
    // SQL engine. On every ReadyForQuery boundary the session checks each listened feed
    // (FETCH cf LIMIT 1 — the cursor does not move) and, when unacked ops exist that it
    // has not yet announced, emits a NotificationResponse 'A' with channel = the feed
    // name and payload = the first unacked _seq. Push-wake + pull-batch: the consumer
    // sleeps on the socket, wakes on 'A', then FETCH/ACK as usual — the exact shape of a
    // long-polling Kafka consumer, atop exactly-once cursors. De-duped per (feed, seq):
    // an unacked-but-announced batch is not re-announced; an ACK re-arms the feed.
    [[nodiscard]] static std::optional<std::string> listen_channel_of(const std::string& s,
                                                                      bool& unlisten) {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
        std::string t = s.substr(b, e - b);
        std::string head;
        for (const char c : t) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) break;
            head += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (head != "listen" && head != "unlisten") return std::nullopt;
        unlisten = head == "unlisten";
        std::string chan = t.substr(head.size());
        b = 0;
        while (b < chan.size() && std::isspace(static_cast<unsigned char>(chan[b])) != 0) ++b;
        chan = chan.substr(b);
        for (const char c : chan) {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') return std::nullopt;
        }
        return chan;
    }
    void poll_notifications(std::vector<std::byte>& out) {
        for (auto& [chan, announced] : listens_) {
            const sql::ExecResult r = exec_("FETCH " + chan + " LIMIT 1");
            if (!r.ok || r.rows.empty()) continue;  // feed gone or fully acked — silent
            const std::int64_t first_unacked = r.rows[0].cells[0].second.i;
            if (first_unacked == announced) continue;  // already announced this batch
            announced = first_unacked;
            std::vector<std::byte> a;
            pg_put_i32(a, backend_pid_);
            pg_put_str(a, chan);
            pg_put_str(a, std::to_string(first_unacked));
            pg_detail::emit(out, 'A', a);  // NotificationResponse
        }
    }

    // A simple-Query string may hold several ';'-separated statements; run each, then emit
    // exactly ONE ReadyForQuery. On the first error, stop (PG aborts the rest of the batch).
    void handle_query(std::vector<std::byte>& out, const std::string& sql) {
        const std::vector<std::string> stmts = split_statements(sql);
        bool any = false;
        for (const std::string& s : stmts) {
            any = true;
            bool unlisten = false;
            if (const auto chan = listen_channel_of(s, unlisten); chan.has_value()) {
                if (unlisten) {
                    listens_.erase(*chan);
                    pg_command_complete(out, "UNLISTEN");
                    continue;
                }
                // LISTEN validates the feed exists (a probe FETCH; the cursor stays put).
                const sql::ExecResult probe = exec_("FETCH " + *chan + " LIMIT 1");
                if (!probe.ok) {
                    pg_error_response(out, probe.error);
                    break;
                }
                listens_.emplace(*chan, -1);
                pg_command_complete(out, "LISTEN");
                continue;
            }
            const sql::ExecResult r = exec_(s);
            pg_reply_for_statement(out, s, r);
            if (!r.ok) break;
        }
        if (!any) {
            // An empty query string: PG replies with EmptyQueryResponse 'I'.
            std::vector<std::byte> empty;
            pg_detail::emit(out, 'I', empty);
        }
        poll_notifications(out);
        pg_ready_for_query(out);
    }

    // ---- EXTENDED protocol (prepared statements) -------------------------------------
    struct Portal {
        std::string sql;                          // substituted text, or (typed path) raw $N text
        std::vector<sql::Datum> params;           // K4.13: typed parameters (typed path only)
        bool typed = false;                       // route through exec_prepared_
        std::optional<sql::ExecResult> cached;    // filled by Describe, consumed by Execute
    };

    [[nodiscard]] static std::string read_cstring(const std::byte* p, std::size_t plen, std::size_t& pos) {
        std::string s;
        while (pos < plen && std::to_integer<char>(p[pos]) != 0) s += std::to_integer<char>(p[pos++]);
        if (pos < plen) ++pos;  // skip the NUL
        return s;
    }

    // Parse 'P': name\0 query\0 int16 nparamtypes int32[]... — store the prepared statement
    // AND its declared parameter-type OIDs (K1: needed to decode a BINARY-format Bind param).
    void handle_parse(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        std::size_t pos = 0;
        const std::string name = read_cstring(p, plen, pos);
        const std::string query = read_cstring(p, plen, pos);
        stmts_[name] = query;
        std::vector<std::int32_t> types;
        if (pos + 2 <= plen) {
            const std::int16_t n = pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < n && pos + 4 <= plen; ++k) {
                types.push_back(pg_get_i32(p + pos));
                pos += 4;
            }
        }
        stmt_types_[name] = std::move(types);
        pg_parse_complete(out);
    }

    // Bind 'B': portal\0 stmt\0 [fmt codes] [params] [result fmts] — substitute -> a portal.
    // K1: parameter format codes are HONORED — a binary param is decoded by its Parse-time
    // type OID (int2/4/8, float4/8, bool, text, pgvector's vector shape); an undecodable
    // binary param and a binary RESULT-format request are clean errors (never garbage).
    void handle_bind(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        std::size_t pos = 0;
        const std::string portal = read_cstring(p, plen, pos);
        const std::string stmt = read_cstring(p, plen, pos);
        if (pos + 2 > plen) return;
        const std::int16_t nfmt = pg_get_i16(p + pos);
        pos += 2;
        std::vector<std::int16_t> fmts;
        for (std::int16_t k = 0; k < nfmt && pos + 2 <= plen; ++k) {
            fmts.push_back(pg_get_i16(p + pos));
            pos += 2;
        }
        const auto fmt_for = [&](std::size_t k) -> std::int16_t {
            if (fmts.empty()) return 0;                       // all-text default
            return fmts.size() == 1 ? fmts[0] : (k < fmts.size() ? fmts[k] : 0);
        };
        const auto tit = stmt_types_.find(stmt);
        const auto oid_for = [&](std::size_t k) -> std::int32_t {
            if (tit == stmt_types_.end() || k >= tit->second.size()) return 0;  // unspecified
            return tit->second[k];
        };
        if (pos + 2 > plen) return;
        const std::int16_t nparams = pg_get_i16(p + pos);
        pos += 2;
        const bool typed = static_cast<bool>(exec_prepared_);
        std::vector<sql::Datum> dparams;
        std::vector<std::string> lits;
        for (std::int16_t k = 0; k < nparams && pos + 4 <= plen; ++k) {
            const std::int32_t vlen = pg_get_i32(p + pos);
            pos += 4;
            if (vlen < 0) {
                if (typed) {
                    dparams.push_back(sql::Datum::make_null(sql::Type::Text));
                } else {
                    lits.push_back(pg_param_literal("", true));
                }
                continue;
            }
            const std::byte* vp = p + pos;
            pos += static_cast<std::size_t>(vlen);
            if (fmt_for(static_cast<std::size_t>(k)) == 1) {  // binary parameter
                const auto txt = pg_decode_binary_param(oid_for(static_cast<std::size_t>(k)), vp,
                                                        static_cast<std::size_t>(vlen));
                if (!txt) {
                    pg_error_response_code(out, "0A000",
                                           "binary parameter format is not supported for this type");
                    return;
                }
                if (typed) {
                    dparams.push_back(pg_typed_datum(oid_for(static_cast<std::size_t>(k)), *txt));
                } else {
                    lits.push_back(pg_param_literal(*txt, false));
                }
            } else {
                const std::string raw(reinterpret_cast<const char*>(vp),
                                      static_cast<std::size_t>(vlen));
                if (typed) {
                    dparams.push_back(pg_typed_datum(oid_for(static_cast<std::size_t>(k)), raw));
                } else {
                    lits.push_back(pg_param_literal(raw, false));
                }
            }
        }
        // Result-format codes: only TEXT results are produced; reject a binary request clean.
        if (pos + 2 <= plen) {
            const std::int16_t nres = pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < nres && pos + 2 <= plen; ++k) {
                if (pg_get_i16(p + pos) == 1) {
                    pg_error_response_code(out, "0A000", "binary result format is not supported");
                    return;
                }
                pos += 2;
            }
        }
        const auto it = stmts_.find(stmt);
        if (typed) {
            portals_[portal] = Portal{it != stmts_.end() ? it->second : std::string(),
                                      std::move(dparams), true, std::nullopt};
        } else {
            const std::string sql =
                (it != stmts_.end()) ? pg_substitute_params(it->second, lits) : std::string();
            portals_[portal] = Portal{sql, {}, false, std::nullopt};
        }
        pg_bind_complete(out);
    }

    // Describe 'D': 'S'+stmt or 'P'+portal. For a portal we execute+cache so the
    // RowDescription is correct AND the following Execute does not re-run the statement.
    void handle_describe(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        if (plen < 1) return;
        const char what = std::to_integer<char>(p[0]);
        std::size_t pos = 1;
        const std::string name = read_cstring(p, plen, pos);
        if (what == 'S') {
            const auto it = stmts_.find(name);
            pg_parameter_description(out, it != stmts_.end() ? count_params(it->second) : 0);
            pg_no_data(out);  // columns are unknown before execution
            return;
        }
        // 'P' portal
        const auto it = portals_.find(name);
        if (it == portals_.end()) { pg_no_data(out); return; }
        it->second.cached = it->second.typed
                                ? exec_prepared_(it->second.sql, it->second.params)
                                : exec_(it->second.sql);
        const sql::ExecResult& r = *it->second.cached;
        if (r.ok && (pg_is_select(it->second.sql) || !r.rows.empty())) {
            pg_row_description(out, pg_cols_of(r));
        } else {
            pg_no_data(out);
        }
    }

    // Execute 'E': portal\0 int32 maxrows — run (or reuse the Describe cache) + reply.
    void handle_execute(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        std::size_t pos = 0;
        const std::string portal = read_cstring(p, plen, pos);
        const auto it = portals_.find(portal);
        if (it == portals_.end()) { pg_error_response(out, "unknown portal"); return; }
        const bool had_describe = it->second.cached.has_value();
        const sql::ExecResult r = had_describe ? *it->second.cached
                                  : it->second.typed
                                      ? exec_prepared_(it->second.sql, it->second.params)
                                      : exec_(it->second.sql);
        it->second.cached.reset();
        if (!r.ok) { pg_error_response(out, r.error); return; }
        const bool is_sel = pg_is_select(it->second.sql) || !r.rows.empty();
        if (!had_describe && is_sel) {  // no prior Describe -> include RowDescription
            pg_row_description(out, pg_cols_of(r));
        }
        for (const sql::ResultRow& row : r.rows) pg_data_row(out, row);
        pg_command_complete(out, pg_command_tag(it->second.sql, r, is_sel));
    }

    // Close 'C': 'S'+stmt or 'P'+portal.
    void handle_close(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        if (plen >= 1) {
            const char what = std::to_integer<char>(p[0]);
            std::size_t pos = 1;
            const std::string name = read_cstring(p, plen, pos);
            if (what == 'S') { stmts_.erase(name); stmt_types_.erase(name); }
            else portals_.erase(name);
        }
        pg_close_complete(out);
    }

    [[nodiscard]] static std::int16_t count_params(const std::string& sql) {
        std::int16_t maxn = 0;
        bool in_str = false;
        for (std::size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '\'') in_str = !in_str;
            else if (!in_str && sql[i] == '$' && i + 1 < sql.size() && sql[i + 1] >= '1' && sql[i + 1] <= '9') {
                std::int16_t n = 0;
                std::size_t j = i + 1;
                while (j < sql.size() && sql[j] >= '0' && sql[j] <= '9') { n = static_cast<std::int16_t>(n * 10 + (sql[j] - '0')); ++j; }
                if (n > maxn) maxn = n;
            }
        }
        return maxn;
    }

    // Split on ';' at the top level (quote-aware — a ';' inside '...' is data).
    [[nodiscard]] static std::vector<std::string> split_statements(const std::string& sql) {
        std::vector<std::string> out;
        std::string cur;
        bool in_str = false;
        for (std::size_t i = 0; i < sql.size(); ++i) {
            const char c = sql[i];
            if (c == '\'') {
                // '' inside a string is an escaped quote (stays in-string).
                if (in_str && i + 1 < sql.size() && sql[i + 1] == '\'') { cur += "''"; ++i; continue; }
                in_str = !in_str;
                cur += c;
            } else if (c == ';' && !in_str) {
                if (!is_blank(cur)) out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!is_blank(cur)) out.push_back(cur);
        return out;
    }
    [[nodiscard]] static bool is_blank(const std::string& s) {
        for (char c : s)
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
        return true;
    }

    ExecFn exec_;
    AuthFn auth_;                  // optional (user,password)->allowed; unset = trust
    ExecPreparedFn exec_prepared_;  // K4.13: typed-parameter path (unset = substitute)
    std::vector<std::byte> buf_;  // unconsumed input bytes
    bool started_ = false;        // startup handshake completed
    bool awaiting_password_ = false;  // sent AuthenticationCleartextPassword, awaiting 'p'
    std::string user_;            // the startup "user" parameter (for auth)
    bool closed_ = false;
    std::map<std::string, std::int64_t> listens_;  // K4.5: feed -> last announced _seq (-1 = none)
    std::map<std::string, std::string> stmts_;  // prepared statements: name -> query
    std::map<std::string, Portal> portals_;     // bound portals: name -> {sql, cached}
    // K1: Parse-time declared parameter-type OIDs (decode key for binary-format Bind params).
    std::map<std::string, std::vector<std::int32_t>> stmt_types_;
    // W3.4 PG CancelRequest: the server assigns this session a (pid, secret) that is sent to
    // the client in BackendKeyData; the client opens a SEPARATE connection and sends a
    // CancelRequest carrying them, which the server matches to this session's cancel flag.
    std::int32_t backend_pid_ = 0;
    std::int32_t backend_secret_ = 0;
    bool is_cancel_request_ = false;   // this connection was a CancelRequest (no normal session)
    std::int32_t cancel_pid_ = 0;      // the target backend pid it named
    std::int32_t cancel_secret_ = 0;   // the target backend secret it named
};

}  // namespace lockstep::query::wire
