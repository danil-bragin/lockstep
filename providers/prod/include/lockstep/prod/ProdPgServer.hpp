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
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
    ProdPgServer(ProdReactor& reactor, std::uint16_t port, ExecFn exec, AuthFn auth = {})
        : reactor_(&reactor), exec_(std::move(exec)), auth_(std::move(auth)) {
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
    }

    ProdPgServer(const ProdPgServer&) = delete;
    ProdPgServer& operator=(const ProdPgServer&) = delete;
    ProdPgServer(ProdPgServer&&) = delete;
    ProdPgServer& operator=(ProdPgServer&&) = delete;

    ~ProdPgServer() {
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
        const std::vector<std::byte> out =
            it->second->feed(std::span<const std::byte>(in.data(), in.size()));
        if (!out.empty()) {
            write_all(fd, out);
        }
        if (it->second->closed()) {
            close_conn(fd);
        }
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
};

}  // namespace lockstep::prod
