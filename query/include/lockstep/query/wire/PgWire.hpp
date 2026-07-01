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
#include <functional>
#include <map>
#include <optional>
#include <string>
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

// The `text` type OID (25) — every column is advertised as text/format-0 in this shim.
inline constexpr std::int32_t kPgTextOid = 25;

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
// RowDescription: one field per column name, all typed as text/format-0.
inline void pg_row_description(std::vector<std::byte>& out, const std::vector<std::string>& cols) {
    std::vector<std::byte> p;
    pg_put_i16(p, static_cast<std::int16_t>(cols.size()));
    for (const std::string& name : cols) {
        pg_put_str(p, name);
        pg_put_i32(p, 0);            // table OID (unknown)
        pg_put_i16(p, 0);            // column attribute number
        pg_put_i32(p, kPgTextOid);   // type OID = text
        pg_put_i16(p, -1);           // type size (variable)
        pg_put_i32(p, -1);           // type modifier
        pg_put_i16(p, 0);            // format code: 0 = text
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
// ErrorResponse: minimal field set — Severity, SQLSTATE code, Message — then a NUL.
inline void pg_error_response(std::vector<std::byte>& out, const std::string& msg) {
    std::vector<std::byte> p;
    p.push_back(static_cast<std::byte>('S'));
    pg_put_str(p, "ERROR");
    p.push_back(static_cast<std::byte>('C'));
    pg_put_str(p, "XX000");  // internal_error SQLSTATE (generic)
    p.push_back(static_cast<std::byte>('M'));
    pg_put_str(p, msg.empty() ? "query failed" : msg);
    p.push_back(std::byte{0});  // field terminator
    pg_detail::emit(out, 'E', p);
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
        std::vector<std::string> cols;
        if (!r.rows.empty())
            for (const auto& cell : r.rows.front().cells) cols.push_back(cell.first);
        pg_row_description(out, cols);
        for (const sql::ResultRow& row : r.rows) pg_data_row(out, row);
    }
    pg_command_complete(out, pg_command_tag(sql, r, is_sel));
}

// ----------------------------------------------------------------------------------------
// PgSession — the byte-stream state machine. feed(input) buffers bytes, parses every
// COMPLETE PG frontend message available, handles each, and returns the bytes to write
// back. It self-frames PG messages; the transport is a plain byte stream.
// ----------------------------------------------------------------------------------------
class PgSession {
public:
    using ExecFn = std::function<sql::ExecResult(const std::string&)>;
    explicit PgSession(ExecFn exec) : exec_(std::move(exec)) {}

    [[nodiscard]] bool closed() const noexcept { return closed_; }

    // Feed raw bytes read off the socket; returns raw bytes to write back (possibly empty).
    [[nodiscard]] std::vector<std::byte> feed(std::span<const std::byte> input) {
        buf_.insert(buf_.end(), input.begin(), input.end());
        std::vector<std::byte> out;
        for (;;) {
            if (!started_) {
                // The startup phase has NO type byte: [int32 len][int32 code][params...].
                if (buf_.size() < 8) break;
                const std::int32_t len = pg_get_i32(buf_.data());
                if (len < 8 || buf_.size() < static_cast<std::size_t>(len)) break;
                const std::int32_t code = pg_get_i32(buf_.data() + 4);
                if (code == 80877103) {  // SSLRequest: decline (plaintext shim).
                    out.push_back(static_cast<std::byte>('N'));
                    buf_.erase(buf_.begin(), buf_.begin() + len);
                    continue;  // the client resends a real StartupMessage.
                }
                // A real StartupMessage (protocol 3.0 = 196608): accept, no auth (trust).
                buf_.erase(buf_.begin(), buf_.begin() + len);
                started_ = true;
                pg_auth_ok(out);
                pg_parameter_status(out, "server_version", "14.0 (Lockstep)");
                pg_parameter_status(out, "client_encoding", "UTF8");
                pg_ready_for_query(out);
                continue;
            }
            // Regular phase: [type byte][int32 len][payload].
            if (buf_.size() < 5) break;
            const char type = std::to_integer<char>(buf_[0]);
            const std::int32_t len = pg_get_i32(buf_.data() + 1);
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
                pg_ready_for_query(out);
            }
            // 'H' Flush and any unknown message: nothing to do (we return `out` already).
            buf_.erase(buf_.begin(), buf_.begin() + len + 1);
        }
        return out;
    }

private:
    // A simple-Query string may hold several ';'-separated statements; run each, then emit
    // exactly ONE ReadyForQuery. On the first error, stop (PG aborts the rest of the batch).
    void handle_query(std::vector<std::byte>& out, const std::string& sql) {
        const std::vector<std::string> stmts = split_statements(sql);
        bool any = false;
        for (const std::string& s : stmts) {
            any = true;
            const sql::ExecResult r = exec_(s);
            pg_reply_for_statement(out, s, r);
            if (!r.ok) break;
        }
        if (!any) {
            // An empty query string: PG replies with EmptyQueryResponse 'I'.
            std::vector<std::byte> empty;
            pg_detail::emit(out, 'I', empty);
        }
        pg_ready_for_query(out);
    }

    // ---- EXTENDED protocol (prepared statements) -------------------------------------
    struct Portal {
        std::string sql;                          // the query with $N substituted
        std::optional<sql::ExecResult> cached;    // filled by Describe, consumed by Execute
    };

    [[nodiscard]] static std::string read_cstring(const std::byte* p, std::size_t plen, std::size_t& pos) {
        std::string s;
        while (pos < plen && std::to_integer<char>(p[pos]) != 0) s += std::to_integer<char>(p[pos++]);
        if (pos < plen) ++pos;  // skip the NUL
        return s;
    }

    // Parse 'P': name\0 query\0 int16 nparamtypes int32[]... — store the prepared statement.
    void handle_parse(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        std::size_t pos = 0;
        const std::string name = read_cstring(p, plen, pos);
        const std::string query = read_cstring(p, plen, pos);
        stmts_[name] = query;  // param types ignored: we substitute textually
        pg_parse_complete(out);
    }

    // Bind 'B': portal\0 stmt\0 [fmt codes] [params] [result fmts] — substitute -> a portal.
    void handle_bind(std::vector<std::byte>& out, const std::byte* p, std::size_t plen) {
        std::size_t pos = 0;
        const std::string portal = read_cstring(p, plen, pos);
        const std::string stmt = read_cstring(p, plen, pos);
        if (pos + 2 > plen) return;
        const std::int16_t nfmt = pg_get_i16(p + pos);
        pos += 2 + static_cast<std::size_t>(nfmt < 0 ? 0 : nfmt) * 2;  // skip format codes
        if (pos + 2 > plen) return;
        const std::int16_t nparams = pg_get_i16(p + pos);
        pos += 2;
        std::vector<std::string> lits;
        for (std::int16_t k = 0; k < nparams && pos + 4 <= plen; ++k) {
            const std::int32_t vlen = pg_get_i32(p + pos);
            pos += 4;
            if (vlen < 0) {
                lits.push_back(pg_param_literal("", true));
            } else {
                std::string v(reinterpret_cast<const char*>(p + pos), static_cast<std::size_t>(vlen));
                pos += static_cast<std::size_t>(vlen);
                lits.push_back(pg_param_literal(v, false));
            }
        }
        const auto it = stmts_.find(stmt);
        const std::string sql = (it != stmts_.end()) ? pg_substitute_params(it->second, lits) : std::string();
        portals_[portal] = Portal{sql, std::nullopt};
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
        it->second.cached = exec_(it->second.sql);
        const sql::ExecResult& r = *it->second.cached;
        if (r.ok && (pg_is_select(it->second.sql) || !r.rows.empty())) {
            std::vector<std::string> cols;
            if (!r.rows.empty())
                for (const auto& c : r.rows.front().cells) cols.push_back(c.first);
            pg_row_description(out, cols);
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
        const sql::ExecResult r = had_describe ? *it->second.cached : exec_(it->second.sql);
        it->second.cached.reset();
        if (!r.ok) { pg_error_response(out, r.error); return; }
        const bool is_sel = pg_is_select(it->second.sql) || !r.rows.empty();
        if (!had_describe && is_sel) {  // no prior Describe -> include RowDescription
            std::vector<std::string> cols;
            if (!r.rows.empty())
                for (const auto& c : r.rows.front().cells) cols.push_back(c.first);
            pg_row_description(out, cols);
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
            if (what == 'S') stmts_.erase(name); else portals_.erase(name);
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
    std::vector<std::byte> buf_;  // unconsumed input bytes
    bool started_ = false;        // startup handshake completed
    bool closed_ = false;
    std::map<std::string, std::string> stmts_;  // prepared statements: name -> query
    std::map<std::string, Portal> portals_;     // bound portals: name -> {sql, cached}
};

}  // namespace lockstep::query::wire
