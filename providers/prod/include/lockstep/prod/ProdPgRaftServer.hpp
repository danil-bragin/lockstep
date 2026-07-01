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
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lockstep/consensus/ConsensusNode.hpp>  // SubmitResult, Index
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdReactor.hpp>
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

    ProdPgRaftServer(ProdReactor& reactor, std::uint16_t port, SubmitFn submit, ExecFn exec,
                     AppliedFn applied, ResultFn result, LeaderFn is_leader)
        : reactor_(&reactor),
          submit_(std::move(submit)),
          exec_(std::move(exec)),
          applied_(std::move(applied)),
          result_(std::move(result)),
          is_leader_(std::move(is_leader)) {
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
    struct Conn {
        std::vector<std::byte> buf;
        bool started = false;
        // A write awaiting its Raft commit+apply (at most one — PG is request/response).
        bool pending = false;
        consensus::Index pending_index = 0;
        std::string pending_sql;
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
                // StartupMessage -> trust + announce ready (auth is a separate follow-on here).
                pgw::pg_auth_ok(out);
                pgw::pg_parameter_status(out, "server_version", "14.0 (lockstep-raft)");
                pgw::pg_ready_for_query(out);
                c.started = true;
                erase_front(c, static_cast<std::size_t>(len));
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
            if (type == 'Q') {
                // Payload is a single C-string (the SQL). len = 4 + strlen + 1.
                std::string sql;
                if (len > 5) {
                    sql.assign(reinterpret_cast<const char*>(c.buf.data() + 5),
                               static_cast<std::size_t>(len) - 5);
                }
                erase_front(c, total);
                handle_query(fd, c, sql, out);
                continue;
            }
            // Any other message type (extended protocol etc.) — acknowledge as unsupported so
            // the client isn't wedged, then continue.
            erase_front(c, total);
            pgw::pg_error_response(out, "only the simple query protocol is supported here");
            pgw::pg_ready_for_query(out);
        }
        if (!out.empty()) {
            write_all(fd, out);
        }
    }

    void handle_query(int fd, Conn& c, const std::string& sql, std::vector<std::byte>& out) {
        (void)fd;
        if (is_blank(sql)) {
            pgw::pg_detail::emit(out, 'I', {});  // EmptyQueryResponse
            pgw::pg_ready_for_query(out);
            return;
        }
        if (pgw::pg_is_select(sql)) {  // READ — answer from local applied state
            const psql::ExecResult r = exec_(sql);
            if (!r.ok) {
                pgw::pg_error_response_code(out, "42000", r.error);
            } else {
                pgw::pg_row_description(out, pgw::pg_cols_of(r));
                for (const psql::ResultRow& row : r.rows) {
                    pgw::pg_data_row(out, row);
                }
                pgw::pg_command_complete(out, pgw::pg_command_tag(sql, r, true));
            }
            pgw::pg_ready_for_query(out);
            return;
        }
        // WRITE — must go through Raft. Reject on a follower; DEFER on the leader.
        if (!is_leader_()) {
            pgw::pg_error_response_code(out, "25006",
                                        "cannot execute a write on a follower — reconnect to the leader");
            pgw::pg_ready_for_query(out);
            return;
        }
        const consensus::SubmitResult sr = submit_(sql);
        if (!sr.accepted) {
            pgw::pg_error_response_code(out, "25006", "not the leader — reconnect to the leader");
            pgw::pg_ready_for_query(out);
            return;
        }
        c.pending = true;  // reply deferred until applied_() >= sr.index (completion pump)
        c.pending_index = sr.index;
        c.pending_sql = sql;
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
                pgw::pg_ready_for_query(out);
                c.pending = false;
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
    int listen_fd_ = -1;
    std::map<int, std::unique_ptr<Conn>> conns_;
};

}  // namespace lockstep::prod
