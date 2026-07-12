#pragma once
// ProdPgServer.hpp — a raw-TCP PostgreSQL-wire listener on the prod epoll reactor, so a
// real PG client (psql / any driver) connects to Lockstep over a real socket. PG frames
// its own messages over a plain byte STREAM (not our length-prefixed frames), so this is
// a raw socket loop — NOT ProdNetwork — that pipes each connection's bytes through a
// query::wire::PgSession (the pure protocol/translation) and writes the reply back.
//
// LINUX-ONLY (epoll + sockets). Demo-grade: level-triggered epoll (a half-drained socket
// re-wakes), small replies written inline. providers/prod is the lint-exempt boundary —
// raw syscalls live here, never in core/query/txn.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/wire/PgWire.hpp>

namespace lockstep::prod {

class ProdPgServer {
public:
    using ExecFn = query::wire::PgSession::ExecFn;
    using AuthFn = query::wire::PgSession::AuthFn;

    // Bind + listen on 127.0.0.1:`port` and register the accept handler on `reactor`.
    // `exec` runs one SQL statement (typically SqlEngine::exec). `auth` (optional) gates
    // each connection with a cleartext password; unset = trust. valid() reports bind ok.
    // W3.3: `cancel_setter` installs a cancel flag onto the SQL engine for the duration of a
    // query (typically [&](f){ engine.set_cancel_flag(f); }); `stmt_timeout_ms` (>0) arms a
    // watchdog thread that flips the flag when a query outruns the deadline, so a long query
    // aborts with "query canceled" even though the reactor is single-threaded and blocked in
    // exec. Both default off (unchanged behavior).
    using CancelSetter = std::function<void(const std::atomic<bool>*)>;
    ProdPgServer(ProdReactor& reactor, std::uint16_t port, ExecFn exec, AuthFn auth = {},
                 CancelSetter cancel_setter = {}, std::int64_t stmt_timeout_ms = 0,
                 std::size_t max_connections = 0)
        : reactor_(&reactor),
          exec_(std::move(exec)),
          auth_(std::move(auth)),
          cancel_setter_(std::move(cancel_setter)),
          stmt_timeout_ms_(stmt_timeout_ms),
          max_conns_(max_connections) {
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
        // W3.3: arm the statement-timeout watchdog thread (real thread — this is prod).
        if (cancel_setter_ && stmt_timeout_ms_ > 0) {
            watchdog_ = std::thread([this] { watchdog_loop(); });
        }
    }

    ProdPgServer(const ProdPgServer&) = delete;
    ProdPgServer& operator=(const ProdPgServer&) = delete;
    ProdPgServer(ProdPgServer&&) = delete;
    ProdPgServer& operator=(ProdPgServer&&) = delete;

    ~ProdPgServer() {
        stop_.store(true);
        if (watchdog_.joinable()) {
            watchdog_.join();
        }
        for (auto& [fd, sess] : conns_) {
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
    static void set_nonblock(int fd) {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    void accept_ready() {
        for (;;) {
            const int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0) {
                break;  // EAGAIN / no more pending connections.
            }
            // W3.6 admission control: cap the number of live connections. Beyond it, close the
            // new fd immediately (the client sees a refused connection) rather than let idle
            // connections grow the server's memory / fd use unbounded. 0 = unlimited.
            if (max_conns_ > 0 && conns_.size() >= max_conns_) {
                ::close(c);
                continue;
            }
            set_nonblock(c);
            conns_.emplace(c, std::make_unique<query::wire::PgSession>(exec_, auth_));
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
        std::vector<std::byte> in(static_cast<std::size_t>(n));
        std::memcpy(in.data(), buf, static_cast<std::size_t>(n));
        // W3.3: for the duration of feed() (which runs the query synchronously on this reactor
        // thread), point the engine at cur_cancel_ and arm the deadline. The watchdog thread
        // flips cur_cancel_ if the query outruns it; the engine polls it and aborts.
        const bool guarded = static_cast<bool>(cancel_setter_) && stmt_timeout_ms_ > 0;
        if (guarded) {
            cur_cancel_.store(false);
            cur_deadline_ns_.store(now_ns() + stmt_timeout_ms_ * 1'000'000);
            cancel_setter_(&cur_cancel_);
        }
        const std::vector<std::byte> out =
            it->second->feed(std::span<const std::byte>(in.data(), in.size()));
        if (guarded) {
            cur_deadline_ns_.store(0);
            cancel_setter_(nullptr);
        }
        if (!out.empty()) {
            write_all(fd, out);
        }
        if (it->second->closed()) {
            close_conn(fd);
        }
        // K4.14: MID-IDLE PUSH — this session's traffic may have committed writes some
        // OTHER session is LISTENing for. Sweep the rest and push their pending
        // notifications now, instead of making them wait for their own next query.
        // (LISTEN-less sessions return empty instantly; de-dup keeps this quiet.)
        for (auto& [ofd, osess] : conns_) {
            if (ofd == fd || !osess) continue;
            const std::vector<std::byte> push = osess->pump_notifications();
            if (!push.empty()) write_all(ofd, push);
        }
    }

    // W3.3: the statement-timeout watchdog. Sleeps in short ticks; when a query is active
    // (cur_deadline_ns_ != 0) and has outrun its deadline, sets cur_cancel_ so the engine's
    // next poll aborts. A separate thread so it fires even while the reactor is blocked in exec.
    void watchdog_loop() {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const std::int64_t dl = cur_deadline_ns_.load();
            if (dl != 0 && now_ns() > dl) {
                cur_cancel_.store(true);
            }
        }
    }

    [[nodiscard]] static std::int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static void write_all(int fd, const std::vector<std::byte>& bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (w <= 0) {
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;  // socket buffer full; retry (demo-grade blocking drain).
                }
                break;  // peer closed / error.
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
    ExecFn exec_;
    AuthFn auth_;
    int listen_fd_ = -1;
    std::map<int, std::unique_ptr<query::wire::PgSession>> conns_;
    // W3.3 statement-timeout state (shared with the watchdog thread).
    CancelSetter cancel_setter_;
    std::int64_t stmt_timeout_ms_ = 0;
    std::atomic<bool> cur_cancel_{false};        // engine points here during a query
    std::atomic<std::int64_t> cur_deadline_ns_{0};  // 0 = no active guarded query
    std::atomic<bool> stop_{false};
    std::thread watchdog_;
    std::size_t max_conns_ = 0;  // W3.6: max live connections (0 = unlimited)
};

}  // namespace lockstep::prod
