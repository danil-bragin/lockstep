// lockstep_pgd_lite.cpp — a PORTABLE (POSIX sockets, no epoll) PG-wire daemon for
// driver-compatibility work on any host. Thread-per-connection; a global mutex
// serializes engine access (the engine is single-threaded by design). NOT the prod
// server (that is lockstep_pgd on epoll) — this exists so the SQLAlchemy/psycopg/
// alembic compatibility grind can run on the dev host.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/query/wire/PgWire.hpp>

using lockstep::query::sql::SqlEngine;
namespace pw = lockstep::query::wire;

int main(int argc, char** argv) {
    int port = 5433;
    std::string data_dir = ".";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) port = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--data-dir") == 0) data_dir = argv[i + 1];
    }
    lockstep::core::Scheduler d_sched, c_sched;
    lockstep::prod::ProdDisk d_disk(d_sched, data_dir + "/pgdlite-data.wal");
    lockstep::prod::ProdDisk c_disk(c_sched, data_dir + "/pgdlite-catalog.wal");
    SqlEngine engine(d_sched, d_disk, c_sched, c_disk);
    engine.recover(d_disk.logical_len(), c_disk.logical_len());
    engine.set_trace_enabled(false);
    std::mutex engine_mu;

    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(lfd, 16) != 0) {
        std::perror("bind/listen");
        return 1;
    }
    std::fprintf(stderr, "lockstep_pgd_lite: listening on 127.0.0.1:%d\n", port);
    for (;;) {
        const int fd = accept(lfd, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread([fd, &engine, &engine_mu] {
            pw::PgSession sess(
                [&engine, &engine_mu](const std::string& s) {
                    std::lock_guard<std::mutex> lk(engine_mu);
                    return engine.exec(s);
                },
                {},
                [&engine, &engine_mu](const std::string& s,
                                      std::vector<lockstep::query::sql::Datum> ps) {
                    std::lock_guard<std::mutex> lk(engine_mu);
                    return engine.exec_prepared(s, std::move(ps));
                });
            std::vector<std::byte> buf(1 << 16);
            while (!sess.closed()) {
                const ssize_t n = read(fd, buf.data(), buf.size());
                if (n <= 0) break;
                std::string in_t, out_t;
                for (ssize_t k = 0; k < n;) {  // debug: frontend message types
                    in_t += static_cast<char>(std::to_integer<unsigned char>(buf[static_cast<std::size_t>(k)]));
                    in_t += ' ';
                    if (k + 5 > n) break;
                    std::int32_t l;
                    std::memcpy(&l, buf.data() + k + 1, 4);
                    l = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(l)));
                    k += 1 + l;
                }
                const std::vector<std::byte> out =
                    sess.feed(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
                for (std::size_t k = 0; k < out.size();) {
                    out_t += static_cast<char>(std::to_integer<unsigned char>(out[k]));
                    out_t += ' ';
                    std::int32_t l;
                    std::memcpy(&l, out.data() + k + 1, 4);
                    l = static_cast<std::int32_t>(ntohl(static_cast<std::uint32_t>(l)));
                    k += 1 + static_cast<std::size_t>(l);
                }
                std::fprintf(stderr, "IN[%s] OUT[%s]\n", in_t.c_str(), out_t.c_str());
                std::size_t off = 0;
                while (off < out.size()) {
                    const ssize_t w = write(fd, out.data() + off, out.size() - off);
                    if (w <= 0) break;
                    off += static_cast<std::size_t>(w);
                }
            }
            if (sess.mid_txn()) {  // a dead connection must not wedge the shared engine
                std::lock_guard<std::mutex> lk(engine_mu);
                (void)engine.exec("ROLLBACK");
            }
            close(fd);
        }).detach();
    }
}
