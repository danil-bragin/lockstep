#pragma once
// ProdPgRaftServer.hpp — a PostgreSQL-wire listener where SQL WRITES go THROUGH RAFT, so a
// real PG client (psql / any driver) talks to the REPLICATED, highly-available SQL database
// directly (not via the admin CLI). This is the last mile of SQL-over-Raft.
//
// THE ASYNC PROBLEM + THE SHAPE OF THE FIX. A read (SELECT) is answered locally from the
// applied SqlEngine — synchronous. A write (INSERT/UPDATE/DELETE/DDL) must be committed by
// Raft (replicated to a quorum) BEFORE the client's CommandComplete, and that commit needs
// the reactor to keep running (replicate + collect acks) — it cannot happen inside one
// synchronous request handler. So the write path is DEFERRED: on a write we submit the SQL
// to the local node and record a PENDING reply; a periodic completion pump watches the
// node's applied index and, once it reaches the submit index (the write is committed AND
// applied here), sends the CommandComplete built from the applied result. PG is strictly
// request/response, so a connection has AT MOST ONE pending write at a time — the client is
// blocked waiting for our reply, so we simply stop processing its input until the pump
// completes the write.
//
// LEADER ROUTING: a write submitted to a follower is rejected (not leader) — we reply an
// error and the client reconnects to the leader (psql-level; a driver with multi-host
// failover does this automatically). Reads are served by any replica from local applied
// state. SIMPLE query protocol only (Query 'Q'); the extended/prepared path is a follow-on.
//
// LINUX-ONLY (epoll + sockets). providers/prod is the lint-exempt boundary. Demo-grade:
// level-triggered epoll, small replies written inline.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lockstep/consensus/ConsensusNode.hpp>  // SubmitResult, Index
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdScram.hpp>  // SCRAM-SHA-256 (LOCKSTEP_TLS-gated)
#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/wire/PgWire.hpp>

namespace lockstep::prod {

namespace consensus = ::lockstep::consensus;
namespace pgw = ::lockstep::query::wire;
namespace psql = ::lockstep::query::sql;

class ProdPgRaftServer {
public:
    using SubmitFn = std::function<consensus::SubmitResult(const std::string&)>;  // write -> Raft
    using ExecFn = std::function<psql::ExecResult(const std::string&)>;           // read  -> local
    using AppliedFn = std::function<consensus::Index()>;                          // node applied idx
    using ResultFn = std::function<std::optional<psql::ExecResult>(consensus::Index)>;  // stash pop
    using LeaderFn = std::function<bool()>;                                       // is this node leader
    // AUTH + RBAC: validate (user, password) and return the access level:
    //   -1 = reject (bad credentials), 0 = read-only (SELECT), 1 = read-write.
    // Unset = trust (no password prompt, read-write) — the demo/loopback default.
    using AuthFn = std::function<int(const std::string& user, const std::string& password)>;
    // SCRAM lookup: a user -> {cleartext password, access level} the server uses to derive
    // the SCRAM keys (the password is never sent over the wire). Set = prefer SCRAM-SHA-256.
    using ScramFn = std::function<std::optional<std::pair<std::string, int>>(const std::string& user)>;

    ProdPgRaftServer(ProdReactor& reactor, std::uint16_t port, SubmitFn submit, ExecFn exec,
                     AppliedFn applied, ResultFn result, LeaderFn is_leader, AuthFn auth = {},
                     ScramFn scram = {})
        : reactor_(&reactor),
          submit_(std::move(submit)),
          exec_(std::move(exec)),
          applied_(std::move(applied)),
          result_(std::move(result)),
          is_leader_(std::move(is_leader)),
          auth_(std::move(auth)),
          scram_(std::move(scram)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        set_nonblock(listen_fd_);
        reactor_->add_fd(listen_fd_, EPOLLIN, [this](std::uint32_t) { accept_ready(); });
        reactor_->spawn(complete_pump(this));
    }

    ProdPgRaftServer(const ProdPgRaftServer&) = delete;
    ProdPgRaftServer& operator=(const ProdPgRaftServer&) = delete;
    ProdPgRaftServer(ProdPgRaftServer&&) = delete;
    ProdPgRaftServer& operator=(ProdPgRaftServer&&) = delete;

    ~ProdPgRaftServer() {
        for (auto& [fd, c] : conns_) {
            reactor_->remove_fd(fd);
            ::close(fd);
        }
        if (listen_fd_ >= 0) {
            reactor_->remove_fd(listen_fd_);
            ::close(listen_fd_);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return listen_fd_ >= 0; }

private:
    struct Portal {
        std::string sql;                            // the query with $N params substituted
        std::optional<psql::ExecResult> cached;     // filled by Describe, consumed by Execute
    };
    struct Conn {
        std::vector<std::byte> buf;
        bool started = false;
        bool awaiting_password = false;  // sent AuthenticationCleartextPassword, awaiting 'p'
        std::string user;
        int role = 1;  // 0 = read-only, 1 = read-write (default when no auth is configured)
        int sasl_state = 0;  // 0 none, 1 awaiting SASLInitialResponse, 2 awaiting SASLResponse
        int sasl_role = 1;   // access level to grant if the SCRAM proof verifies
#ifdef LOCKSTEP_TLS
        std::unique_ptr<ScramServer> scram;
#endif
        // A write awaiting its Raft commit+apply (at most one — PG is request/response).
        bool pending = false;
        consensus::Index pending_index = 0;
        std::string pending_sql;
        bool pending_ready = false;  // simple query -> pump appends ReadyForQuery; extended -> Sync does
        // EXTENDED protocol (prepared statements): name -> SQL, portal -> bound query.
        std::map<std::string, std::string> stmts;
        std::map<std::string, Portal> portals;
        // K1: Parse-time declared param-type OIDs (decode key for binary-format Bind params).
        std::map<std::string, std::vector<std::int32_t>> stmt_types;
    };

    static void set_nonblock(int fd) {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    void accept_ready() {
        for (;;) {
            const int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0) {
                break;
            }
            set_nonblock(c);
            conns_.emplace(c, std::make_unique<Conn>());
            reactor_->add_fd(c, EPOLLIN, [this, c](std::uint32_t) { conn_ready(c); });
        }
    }

    void conn_ready(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) {
            return;
        }
        char buf[8192];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_conn(fd);
            }
            return;
        }
        Conn& c = *it->second;
        const std::size_t base = c.buf.size();
        c.buf.resize(base + static_cast<std::size_t>(n));
        std::memcpy(c.buf.data() + base, buf, static_cast<std::size_t>(n));
        process(fd);
    }

    // Drain as many complete PG messages from a connection's buffer as possible. Stops when
    // a write goes pending (waiting for its Raft commit) or the buffer holds a partial frame.
    void process(int fd) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) {
            return;
        }
        Conn& c = *it->second;
        std::vector<std::byte> out;
        for (;;) {
            if (c.pending) {
                break;  // blocked until the completion pump replies to the in-flight write
            }
            if (c.awaiting_password || c.sasl_state != 0) {
                // A 'p' message: cleartext PasswordMessage OR a SASL (SCRAM) response.
                if (c.buf.size() < 5) break;
                const char ptype = static_cast<char>(c.buf[0]);
                const std::int32_t plen = pgw::pg_get_i32(c.buf.data() + 1);
                if (plen < 4 || c.buf.size() < 1 + static_cast<std::size_t>(plen)) break;
                if (ptype != 'p') {  // protocol error — expected a password / SASL frame
                    write_all(fd, out);
                    close_conn(fd);
                    return;
                }
                const std::byte* pp = c.buf.data() + 5;
                const std::size_t pl = static_cast<std::size_t>(plen) - 4;
#ifdef LOCKSTEP_TLS
                if (c.sasl_state == 1) {  // SASLInitialResponse
                    sasl_initial(c, pp, pl, out);
                    erase_front(c, 1 + static_cast<std::size_t>(plen));
                    continue;
                }
                if (c.sasl_state == 2) {  // SASLResponse (client-final)
                    const bool alive = sasl_final(fd, c, pp, pl, out);
                    if (!alive) return;  // rejected + connection closed
                    erase_front(c, 1 + static_cast<std::size_t>(plen));
                    continue;
                }
#endif
                std::string pw;  // cleartext PasswordMessage
                if (pl > 0) pw.assign(reinterpret_cast<const char*>(pp), pl - 1);
                erase_front(c, 1 + static_cast<std::size_t>(plen));
                const int lvl = auth_ ? auth_(c.user, pw) : 1;
                if (lvl < 0) {
                    pgw::pg_error_response_code(
                        out, "28P01", "password authentication failed for user \"" + c.user + "\"");
                    write_all(fd, out);
                    close_conn(fd);
                    return;
                }
                c.role = lvl;
                accept(out);
                c.started = true;
                c.awaiting_password = false;
                continue;
            }
            if (!c.started) {
                if (c.buf.size() < 4) break;
                const std::int32_t len = pgw::pg_get_i32(c.buf.data());
                if (len < 8 || c.buf.size() < static_cast<std::size_t>(len)) break;
                const std::int32_t code = pgw::pg_get_i32(c.buf.data() + 4);
                if (len == 8 && code == 80877103) {  // SSLRequest -> decline, stay plaintext
                    out.push_back(std::byte{static_cast<unsigned char>('N')});
                    erase_front(c, static_cast<std::size_t>(len));
                    continue;
                }
                c.user = extract_user(c.buf.data(), static_cast<std::size_t>(len));
                erase_front(c, static_cast<std::size_t>(len));
#ifdef LOCKSTEP_TLS
                if (scram_) {  // AuthenticationSASL — offer SCRAM-SHA-256 (password never sent)
                    std::vector<std::byte> p;
                    pgw::pg_put_i32(p, 10);
                    for (const char* m = "SCRAM-SHA-256"; *m != 0; ++m) {
                        p.push_back(std::byte{static_cast<unsigned char>(*m)});
                    }
                    p.push_back(std::byte{0});  // terminate the mechanism name
                    p.push_back(std::byte{0});  // terminate the mechanism list
                    pgw::pg_detail::emit(out, 'R', p);
                    c.sasl_state = 1;
                    continue;
                }
#endif
                if (auth_) {  // AuthenticationCleartextPassword (protect with TLS on the wire)
                    std::vector<std::byte> ch;
                    pgw::pg_put_i32(ch, 3);
                    pgw::pg_detail::emit(out, 'R', ch);
                    c.awaiting_password = true;
                    continue;
                }
                accept(out);  // trust (no auth configured)
                c.started = true;
                continue;
            }
            if (c.buf.size() < 5) break;
            const char type = static_cast<char>(c.buf[0]);
            const std::int32_t len = pgw::pg_get_i32(c.buf.data() + 1);
            if (len < 4) {  // malformed frame length
                write_all(fd, out);
                close_conn(fd);
                return;
            }
            const std::size_t total = 1 + static_cast<std::size_t>(len);
            if (c.buf.size() < total) break;
            if (type == 'X') {  // Terminate
                write_all(fd, out);
                close_conn(fd);
                return;
            }
            // Copy the payload (bytes after the 5-byte header) before erasing the frame.
            const std::size_t plen = static_cast<std::size_t>(len) - 4;
            std::vector<std::byte> payload(c.buf.begin() + 5, c.buf.begin() + static_cast<std::ptrdiff_t>(total));
            erase_front(c, total);
            const std::byte* p = payload.data();
            switch (type) {
                case 'Q': {  // simple query — one C-string
                    std::string sql;
                    if (!payload.empty()) {
                        sql.assign(reinterpret_cast<const char*>(p), plen ? plen - 1 : 0);
                    }
                    handle_query(c, sql, out);
                    break;
                }
                case 'P':  // Parse
                    ext_parse(c, p, plen, out);
                    break;
                case 'B':  // Bind
                    ext_bind(c, p, plen, out);
                    break;
                case 'D':  // Describe
                    ext_describe(c, p, plen, out);
                    break;
                case 'E':  // Execute
                    ext_execute(c, p, plen, out);
                    break;
                case 'C':  // Close
                    ext_close(c, p, plen, out);
                    break;
                case 'S':  // Sync — end of an extended-protocol batch
                    pgw::pg_ready_for_query(out);
                    break;
                case 'H':  // Flush — results are flushed at the end of process() anyway
                    break;
                default:
                    pgw::pg_error_response(out, "unsupported message type");
                    pgw::pg_ready_for_query(out);
                    break;
            }
        }
        if (!out.empty()) {
            write_all(fd, out);
        }
    }

    // A statement that produces rows is a READ (served from local applied state); everything
    // else is a WRITE (routed through Raft). VALUES/TABLE/EXPLAIN/SHOW count as reads.
    static bool is_write(const std::string& sql) { return !pgw::pg_is_select(sql); }

    void handle_query(Conn& c, const std::string& sql, std::vector<std::byte>& out) {
        if (is_blank(sql)) {
            pgw::pg_detail::emit(out, 'I', {});  // EmptyQueryResponse
            pgw::pg_ready_for_query(out);
            return;
        }
        if (is_write(sql)) {
            defer_write(c, sql, /*add_ready=*/true, out);
        } else {
            reply_read(sql, std::nullopt, /*include_row_desc=*/true, /*add_ready=*/true, out);
        }
    }

    // Answer a READ from the local applied SqlEngine. `precomputed` reuses a Describe cache
    // (extended protocol) so Execute doesn't re-run the statement.
    void reply_read(const std::string& sql, std::optional<psql::ExecResult> precomputed,
                    bool include_row_desc, bool add_ready, std::vector<std::byte>& out) {
        const psql::ExecResult r = precomputed ? *precomputed : exec_(sql);
        if (!r.ok) {
            pgw::pg_error_response_code(out, "42000", r.error);
        } else {
            const bool is_sel = pgw::pg_is_select(sql) || !r.rows.empty();
            if (include_row_desc) pgw::pg_row_description(out, pgw::pg_cols_of(r));
            for (const psql::ResultRow& row : r.rows) pgw::pg_data_row(out, row);
            pgw::pg_command_complete(out, pgw::pg_command_tag(sql, r, is_sel));
        }
        if (add_ready) pgw::pg_ready_for_query(out);
    }

    // Route a WRITE through Raft: reject on a follower (leader routing), DEFER on the leader
    // until the commit lands. `add_ready` = whether the completion pump should append
    // ReadyForQuery (true for simple query; false for extended, where Sync does it).
    void defer_write(Conn& c, const std::string& sql, bool add_ready, std::vector<std::byte>& out) {
        if (c.role < 1) {  // RBAC: a read-only user may not write
            pgw::pg_error_response_code(out, "42501",
                                        "permission denied: user \"" + c.user + "\" is read-only");
            if (add_ready) pgw::pg_ready_for_query(out);
            return;
        }
        if (!is_leader_()) {
            pgw::pg_error_response_code(out, "25006",
                                        "cannot execute a write on a follower — reconnect to the leader");
            if (add_ready) pgw::pg_ready_for_query(out);
            return;
        }
        const consensus::SubmitResult sr = submit_(sql);
        if (!sr.accepted) {
            pgw::pg_error_response_code(out, "25006", "not the leader — reconnect to the leader");
            if (add_ready) pgw::pg_ready_for_query(out);
            return;
        }
        c.pending = true;  // reply deferred until applied_() >= sr.index (completion pump)
        c.pending_index = sr.index;
        c.pending_sql = sql;
        c.pending_ready = add_ready;
    }

    // Send the post-authentication handshake: AuthenticationOk + a ParameterStatus + Ready.
    void accept(std::vector<std::byte>& out) {
        pgw::pg_auth_ok(out);
        pgw::pg_parameter_status(out, "server_version", "14.0 (lockstep-raft)");
        pgw::pg_ready_for_query(out);
    }

    // Extract the "user" parameter from a StartupMessage: [int32 len][int32 version][k\0v\0...\0].
    static std::string extract_user(const std::byte* data, std::size_t len) {
        std::size_t pos = 8;  // skip the length + protocol-version words
        while (pos < len) {
            const std::string key = rd_cstr(data, len, pos);
            if (key.empty()) break;
            const std::string val = rd_cstr(data, len, pos);
            if (key == "user") return val;
        }
        return "";
    }

#ifdef LOCKSTEP_TLS
    // SASLInitialResponse: [mechanism\0][int32 ir-len][client-first-message]. Start the SCRAM
    // exchange with the server-known password and reply AuthenticationSASLContinue.
    void sasl_initial(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        std::size_t pos = 0;
        const std::string mech = rd_cstr(p, plen, pos);
        std::string client_first;
        if (pos + 4 <= plen) {
            const std::int32_t irlen = pgw::pg_get_i32(p + pos);
            pos += 4;
            if (irlen > 0 && pos + static_cast<std::size_t>(irlen) <= plen) {
                client_first.assign(reinterpret_cast<const char*>(p + pos),
                                    static_cast<std::size_t>(irlen));
            }
        }
        if (mech != "SCRAM-SHA-256") {
            pgw::pg_error_response_code(out, "28P01", "unsupported SASL mechanism");
            c.sasl_state = 0;
            return;
        }
        // Strip the GS2 header ("n,," etc.) -> client-first-message-bare (after the 2nd comma).
        const std::size_t c1 = client_first.find(',');
        const std::size_t c2 = c1 == std::string::npos ? std::string::npos : client_first.find(',', c1 + 1);
        const std::string bare =
            c2 == std::string::npos ? client_first : client_first.substr(c2 + 1);
        std::string password;
        const auto looked = scram_(c.user);
        if (looked) {
            password = looked->first;
            c.sasl_role = looked->second;
        } else {
            c.sasl_role = -1;  // unknown user — run the exchange but reject at the end
        }
        c.scram = std::make_unique<ScramServer>();
        if (!c.scram->begin(bare, password)) {
            pgw::pg_error_response_code(out, "28P01", "SCRAM initialization failed");
            c.sasl_state = 0;
            return;
        }
        std::vector<std::byte> pl;
        pgw::pg_put_i32(pl, 11);  // AuthenticationSASLContinue
        const std::string& sf = c.scram->server_first();
        for (const char ch : sf) pl.push_back(std::byte{static_cast<unsigned char>(ch)});
        pgw::pg_detail::emit(out, 'R', pl);
        c.sasl_state = 2;
    }

    // SASLResponse: the client-final-message. Verify the proof + reply AuthenticationSASLFinal
    // then AuthenticationOk. Returns false (connection closed) on a failed proof / unknown user.
    bool sasl_final(int fd, Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        const std::string client_final(reinterpret_cast<const char*>(p), plen);
        if (!c.scram || c.sasl_role < 0 || !c.scram->finish(client_final)) {
            pgw::pg_error_response_code(out, "28P01",
                                        "SCRAM authentication failed for user \"" + c.user + "\"");
            write_all(fd, out);
            close_conn(fd);
            return false;
        }
        std::vector<std::byte> pl;
        pgw::pg_put_i32(pl, 12);  // AuthenticationSASLFinal
        const std::string& sfin = c.scram->server_final();
        for (const char ch : sfin) pl.push_back(std::byte{static_cast<unsigned char>(ch)});
        pgw::pg_detail::emit(out, 'R', pl);
        c.role = c.sasl_role;
        accept(out);  // AuthenticationOk + ParameterStatus + ReadyForQuery
        c.started = true;
        c.sasl_state = 0;
        c.scram.reset();
        return true;
    }
#endif  // LOCKSTEP_TLS

    // ---- EXTENDED protocol (prepared statements) over the Raft path ------------------------
    static std::string rd_cstr(const std::byte* p, std::size_t plen, std::size_t& pos) {
        std::string s;
        while (pos < plen && std::to_integer<char>(p[pos]) != 0) s += std::to_integer<char>(p[pos++]);
        if (pos < plen) ++pos;  // skip the NUL
        return s;
    }

    void ext_parse(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        std::size_t pos = 0;
        const std::string name = rd_cstr(p, plen, pos);
        const std::string query = rd_cstr(p, plen, pos);
        c.stmts[name] = query;
        // K1: capture the declared param-type OIDs (the binary-Bind decode key).
        std::vector<std::int32_t> types;
        if (pos + 2 <= plen) {
            const std::int16_t n = pgw::pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < n && pos + 4 <= plen; ++k) {
                types.push_back(pgw::pg_get_i32(p + pos));
                pos += 4;
            }
        }
        c.stmt_types[name] = std::move(types);
        pgw::pg_parse_complete(out);
    }

    void ext_bind(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        std::size_t pos = 0;
        const std::string portal = rd_cstr(p, plen, pos);
        const std::string stmt = rd_cstr(p, plen, pos);
        // K1: parameter format codes are HONORED (binary decoded by the Parse-time OID);
        // a binary RESULT-format request is a clean error (only text results are produced).
        std::vector<std::int16_t> fmts;
        if (pos + 2 <= plen) {
            const std::int16_t nfmt = pgw::pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < nfmt && pos + 2 <= plen; ++k) {
                fmts.push_back(pgw::pg_get_i16(p + pos));
                pos += 2;
            }
        }
        const auto fmt_for = [&](std::size_t k) -> std::int16_t {
            if (fmts.empty()) return 0;
            return fmts.size() == 1 ? fmts[0] : (k < fmts.size() ? fmts[k] : 0);
        };
        const auto tit = c.stmt_types.find(stmt);
        const auto oid_for = [&](std::size_t k) -> std::int32_t {
            if (tit == c.stmt_types.end() || k >= tit->second.size()) return 0;
            return tit->second[k];
        };
        std::vector<std::string> lits;
        if (pos + 2 <= plen) {
            const std::int16_t nparams = pgw::pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < nparams && pos + 4 <= plen; ++k) {
                const std::int32_t vlen = pgw::pg_get_i32(p + pos);
                pos += 4;
                if (vlen < 0) {
                    lits.push_back(pgw::pg_param_literal("", true));
                    continue;
                }
                const std::byte* vp = p + pos;
                pos += static_cast<std::size_t>(vlen);
                if (fmt_for(static_cast<std::size_t>(k)) == 1) {
                    const auto txt = pgw::pg_decode_binary_param(
                        oid_for(static_cast<std::size_t>(k)), vp, static_cast<std::size_t>(vlen));
                    if (!txt) {
                        pgw::pg_error_response_code(
                            out, "0A000", "binary parameter format is not supported for this type");
                        return;
                    }
                    lits.push_back(pgw::pg_param_literal(*txt, false));
                } else {
                    lits.push_back(pgw::pg_param_literal(
                        std::string(reinterpret_cast<const char*>(vp), static_cast<std::size_t>(vlen)),
                        false));
                }
            }
        }
        if (pos + 2 <= plen) {  // result-format codes: reject binary clean
            const std::int16_t nres = pgw::pg_get_i16(p + pos);
            pos += 2;
            for (std::int16_t k = 0; k < nres && pos + 2 <= plen; ++k) {
                if (pgw::pg_get_i16(p + pos) == 1) {
                    pgw::pg_error_response_code(out, "0A000", "binary result format is not supported");
                    return;
                }
                pos += 2;
            }
        }
        const auto it = c.stmts.find(stmt);
        const std::string sql =
            (it != c.stmts.end()) ? pgw::pg_substitute_params(it->second, lits) : std::string();
        c.portals[portal] = Portal{sql, std::nullopt};
        pgw::pg_bind_complete(out);
    }

    void ext_describe(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        if (plen < 1) return;
        const char what = std::to_integer<char>(p[0]);
        std::size_t pos = 1;
        const std::string name = rd_cstr(p, plen, pos);
        if (what == 'S') {
            const auto it = c.stmts.find(name);
            pgw::pg_parameter_description(out, it != c.stmts.end() ? count_params(it->second) : 0);
            pgw::pg_no_data(out);  // columns unknown before execution
            return;
        }
        const auto it = c.portals.find(name);
        if (it == c.portals.end()) {
            pgw::pg_no_data(out);
            return;
        }
        // A WRITE describes as NoData and is NOT exec'd here (that would bypass Raft). A READ
        // is exec'd locally + cached so the following Execute reuses it.
        if (is_write(it->second.sql)) {
            pgw::pg_no_data(out);
            return;
        }
        it->second.cached = exec_(it->second.sql);
        const psql::ExecResult& r = *it->second.cached;
        if (r.ok && (pgw::pg_is_select(it->second.sql) || !r.rows.empty())) {
            pgw::pg_row_description(out, pgw::pg_cols_of(r));
        } else {
            pgw::pg_no_data(out);
        }
    }

    void ext_execute(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        std::size_t pos = 0;
        const std::string portal = rd_cstr(p, plen, pos);
        const auto it = c.portals.find(portal);
        if (it == c.portals.end()) {
            pgw::pg_error_response(out, "unknown portal");
            return;
        }
        const std::string sql = it->second.sql;
        if (is_write(sql)) {
            it->second.cached.reset();
            defer_write(c, sql, /*add_ready=*/false, out);  // Sync sends ReadyForQuery
            return;
        }
        const bool had_describe = it->second.cached.has_value();
        const std::optional<psql::ExecResult> pre = it->second.cached;
        it->second.cached.reset();
        reply_read(sql, pre, /*include_row_desc=*/!had_describe, /*add_ready=*/false, out);
    }

    void ext_close(Conn& c, const std::byte* p, std::size_t plen, std::vector<std::byte>& out) {
        if (plen >= 1) {
            const char what = std::to_integer<char>(p[0]);
            std::size_t pos = 1;
            const std::string name = rd_cstr(p, plen, pos);
            if (what == 'S') {
                c.stmts.erase(name);
            } else {
                c.portals.erase(name);
            }
        }
        pgw::pg_close_complete(out);
    }

    static std::int16_t count_params(const std::string& sql) {
        std::int16_t maxn = 0;
        bool in_str = false;
        for (std::size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '\'') {
                in_str = !in_str;
            } else if (!in_str && sql[i] == '$' && i + 1 < sql.size() && sql[i + 1] >= '1' &&
                       sql[i + 1] <= '9') {
                std::int16_t n = 0;
                std::size_t j = i + 1;
                while (j < sql.size() && sql[j] >= '0' && sql[j] <= '9') {
                    n = static_cast<std::int16_t>(n * 10 + (sql[j] - '0'));
                    ++j;
                }
                if (n > maxn) maxn = n;
            }
        }
        return maxn;
    }

    // Periodic pump: complete any connection whose pending write has been applied locally.
    static core::Task complete_pump(ProdPgRaftServer* self) {
        for (;;) {
            co_await self->reactor_->clock().delay(2'000'000);  // 2ms
            const consensus::Index a = self->applied_();
            // Collect ready fds first (completing may re-enter process()/erase the conn).
            std::vector<int> ready;
            for (auto& [fd, c] : self->conns_) {
                if (c->pending && c->pending_index <= a) {
                    ready.push_back(fd);
                }
            }
            for (const int fd : ready) {
                auto it = self->conns_.find(fd);
                if (it == self->conns_.end()) continue;
                Conn& c = *it->second;
                if (!c.pending || c.pending_index > a) continue;
                std::vector<std::byte> out;
                const std::optional<psql::ExecResult> r = self->result_(c.pending_index);
                if (r && r->ok) {
                    pgw::pg_command_complete(out, pgw::pg_command_tag(c.pending_sql, *r, false));
                } else if (r) {
                    pgw::pg_error_response_code(out, "42000", r->error);
                } else {
                    pgw::pg_error_response(out, "write committed but its result was unavailable");
                }
                if (c.pending_ready) {
                    pgw::pg_ready_for_query(out);  // simple query — extended defers to Sync
                }
                c.pending = false;
                c.pending_ready = false;
                c.pending_sql.clear();
                c.pending_index = 0;
                self->write_all(fd, out);
                // A follow-up query may already be buffered; resume draining it.
                self->process(fd);
            }
        }
    }

    static bool is_blank(const std::string& s) {
        for (const char ch : s) {
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != ';' && ch != '\0') {
                return false;
            }
        }
        return true;
    }

    static void erase_front(Conn& c, std::size_t n) {
        c.buf.erase(c.buf.begin(), c.buf.begin() + static_cast<std::ptrdiff_t>(n));
    }

    void write_all(int fd, const std::vector<std::byte>& bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (w <= 0) {
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                break;
            }
            off += static_cast<std::size_t>(w);
        }
    }

    void close_conn(int fd) {
        reactor_->remove_fd(fd);
        ::close(fd);
        conns_.erase(fd);
    }

    ProdReactor* reactor_;
    SubmitFn submit_;
    ExecFn exec_;
    AppliedFn applied_;
    ResultFn result_;
    LeaderFn is_leader_;
    AuthFn auth_;
    ScramFn scram_;
    int listen_fd_ = -1;
    std::map<int, std::unique_ptr<Conn>> conns_;
};

}  // namespace lockstep::prod
