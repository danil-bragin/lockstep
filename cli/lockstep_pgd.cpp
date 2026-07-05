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
#include <atomic>
#include <chrono>
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
    std::string req_user;
    std::string req_password;
    bool require_auth = false;
    std::int64_t stmt_timeout_ms = 0;  // W3.3: 0 = no statement timeout
    std::int64_t slow_query_ms = 0;    // W3.7: 0 = no slow-query log
    std::uint64_t max_query_mem = 0;   // W3.1: 0 = no per-query memory cap
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--port") == 0) {
            port = static_cast<std::uint16_t>(parse_u64(argv[i + 1], port));
        } else if (std::strcmp(argv[i], "--data-dir") == 0) {
            data_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--max-query-mem") == 0) {
            max_query_mem = parse_u64(argv[i + 1], 0);
        } else if (std::strcmp(argv[i], "--slow-query-ms") == 0) {
            slow_query_ms = static_cast<std::int64_t>(parse_u64(argv[i + 1], 0));
        } else if (std::strcmp(argv[i], "--stmt-timeout-ms") == 0) {
            stmt_timeout_ms = static_cast<std::int64_t>(parse_u64(argv[i + 1], 0));
        } else if (std::strcmp(argv[i], "--run-seconds") == 0) {
            run_seconds = parse_u64(argv[i + 1], run_seconds);
        } else if (std::strcmp(argv[i], "--user") == 0) {
            req_user = argv[i + 1];
            require_auth = true;
        } else if (std::strcmp(argv[i], "--password") == 0) {
            req_password = argv[i + 1];
            require_auth = true;
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
    if (max_query_mem > 0) {  // W3.1: bound per-query memory (materialization + join row set)
        engine.set_max_query_memory(static_cast<std::size_t>(max_query_mem));
    }

    prod::ProdReactor reactor;
    prod::ProdPgServer::AuthFn auth;
    if (require_auth) {
        auth = [req_user, req_password](const std::string& u, const std::string& p) {
            return u == req_user && p == req_password;
        };
    }
    // W3.3: install the cancel flag onto the engine for the duration of a query; the server's
    // watchdog flips it if the query outruns --stmt-timeout-ms, aborting it with "query canceled".
    // W3.7: if --slow-query-ms is set, log any query whose wall-clock exceeds it (observability).
    auto exec_fn = [&engine, slow_query_ms](const std::string& s) {
        if (slow_query_ms <= 0) {
            return engine.exec(s);
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto r = engine.exec(s);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms >= slow_query_ms) {
            std::fprintf(stderr, "lockstep_pgd: slow query %lldms: %s\n",
                         static_cast<long long>(ms), s.c_str());
            std::fflush(stderr);
        }
        return r;
    };
    prod::ProdPgServer pg(reactor, port, exec_fn, auth,
                          [&engine](const std::atomic<bool>* f) { engine.set_cancel_flag(f); },
                          stmt_timeout_ms);
    if (!pg.valid()) {
        std::fprintf(stderr, "lockstep_pgd: could not bind port %u\n", static_cast<unsigned>(port));
        return 1;
    }
    std::printf("lockstep_pgd: PostgreSQL-wire on 127.0.0.1:%u (data-dir=%s, run-seconds=%llu, auth=%s)\n",
                static_cast<unsigned>(port), data_dir.c_str(),
                static_cast<unsigned long long>(run_seconds), require_auth ? "password" : "trust");
    std::fflush(stdout);

    const core::Tick deadline =
        reactor.now() + static_cast<core::Tick>(run_seconds) * 1'000'000'000LL;
    reactor.run_until([] { return false; }, deadline);
    return 0;
}
