// lockstep_pgd.cpp — a minimal PostgreSQL-wire daemon: a SqlEngine served over a real
// TCP socket via the PG v3 wire shim, so `psql` / any PG driver connects to Lockstep.
// [LINUX ONLY] (epoll + sockets). A demo of drop-in PostgreSQL-client compatibility.
//
//   lockstep_pgd --port P [--data-dir D] [--run-seconds N]
//
// It assembles a SqlEngine over ProdDisk (data + catalog WALs under the data dir), a
// ProdReactor (epoll loop), and a ProdPgServer that pipes each PG connection through the
// query::wire::PgSession -> SqlEngine::exec() path. BOUNDED by an absolute deadline so it
// always terminates. The SQL engine + wire shim are UNCHANGED — this is pure assembly.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdPgServer.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/query/sql/Engine.hpp>

namespace prod = lockstep::prod;
namespace core = lockstep::core;
using lockstep::query::sql::SqlEngine;

namespace {
std::uint64_t parse_u64(const char* s, std::uint64_t def) {
    if (s == nullptr) return def;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(v) : def;
}
}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    std::string data_dir = ".";
    std::uint64_t run_seconds = 30;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--port") == 0) {
            port = static_cast<std::uint16_t>(parse_u64(argv[i + 1], port));
        } else if (std::strcmp(argv[i], "--data-dir") == 0) {
            data_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--run-seconds") == 0) {
            run_seconds = parse_u64(argv[i + 1], run_seconds);
        }
    }
    if (port == 0) {
        std::fprintf(stderr, "usage: lockstep_pgd --port P [--data-dir D] [--run-seconds N]\n");
        return 2;
    }

    // The SQL engine over ProdDisk: two stores (data + catalog) on two schedulers; exec()
    // drives them inline (synchronous), independent of the reactor's epoll loop.
    core::Scheduler d_sched;
    core::Scheduler c_sched;
    prod::ProdDisk d_disk(d_sched, data_dir + "/pgd-data.wal");
    prod::ProdDisk c_disk(c_sched, data_dir + "/pgd-catalog.wal");
    SqlEngine engine(d_sched, d_disk, c_sched, c_disk);
    engine.recover(d_disk.logical_len(), c_disk.logical_len());  // replay durable state (fresh dir -> no-op)

    prod::ProdReactor reactor;
    prod::ProdPgServer pg(reactor, port,
                          [&engine](const std::string& s) { return engine.exec(s); });
    if (!pg.valid()) {
        std::fprintf(stderr, "lockstep_pgd: could not bind port %u\n", static_cast<unsigned>(port));
        return 1;
    }
    std::printf("lockstep_pgd: PostgreSQL-wire on 127.0.0.1:%u (data-dir=%s, run-seconds=%llu)\n",
                static_cast<unsigned>(port), data_dir.c_str(),
                static_cast<unsigned long long>(run_seconds));
    std::fflush(stdout);

    const core::Tick deadline =
        reactor.now() + static_cast<core::Tick>(run_seconds) * 1'000'000'000LL;
    reactor.run_until([] { return false; }, deadline);
    return 0;
}
