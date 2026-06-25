// lockstep_sqlbench.cpp — SQL-over-the-wire load driver for lockstepd --wire-server.
//
// Sends SQL statement strings over the wire (ClientStub.sql → wire::Server → sql::Engine →
// query::Database), the real SQL path Postgres/Cockroach are compared on. SETUP: CREATE TABLE
// + pipelined INSERT load; RUN: measured SELECT (read) / UPDATE (write) / 50-50 single
// statements at K concurrency. Reports committed throughput + p50/p99. Cross-machine via
// --host HOST:PORT. LINUX-ONLY (prod providers).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/query/wire/ClientStub.hpp>

namespace {
namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace query = lockstep::query;
namespace wire = lockstep::query::wire;

constexpr std::uint64_t kServerId = 1;
constexpr std::uint64_t kClientId = 2;

enum class Mix : std::uint8_t { Write, Read, RW5050 };

struct Shared {
    wire::ClientStub* stub = nullptr;
    prod::ProdReactor* reactor = nullptr;
    std::uint64_t count = 0;
    std::uint64_t keyspace = 1000;
    Mix mix = Mix::Write;
    std::uint64_t next = 0;
    std::uint64_t committed = 0;
    std::uint64_t active = 0;
    bool done = false;
    std::vector<core::Tick> lat;
};

// CREATE TABLE (free function over stable pointers — no dangling-lambda coroutine frame).
core::Task create_table_program(wire::ClientStub* st, bool* done) {
    wire::CallResult c;
    co_await st->sql("CREATE TABLE kv (id INT, v TEXT, PRIMARY KEY (id))", c);
    *done = true;
    co_return;
}

// SETUP loader: INSERT keyspace rows (pipelined K loaders).
core::Task load_worker(Shared* s) {
    for (;;) {
        const std::uint64_t i = s->next++;
        if (i >= s->keyspace) {
            break;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "INSERT INTO kv (id, v) VALUES (%llu, 'val%llu')",
                      static_cast<unsigned long long>(i),
                      static_cast<unsigned long long>(i));
        wire::CallResult cr;
        co_await s->stub->sql(std::string(buf), cr);
    }
    if (--s->active == 0) {
        s->done = true;
    }
    co_return;
}

core::Task worker(Shared* s) {
    for (;;) {
        const std::uint64_t i = s->next++;
        if (i >= s->count) {
            break;
        }
        const std::uint64_t id = i % s->keyspace;
        const bool do_read =
            (s->mix == Mix::Read) || (s->mix == Mix::RW5050 && (i & 1ULL) == 0ULL);
        char buf[128];
        if (do_read) {
            std::snprintf(buf, sizeof(buf), "SELECT id, v FROM kv WHERE id = %llu",
                          static_cast<unsigned long long>(id));
        } else {
            std::snprintf(buf, sizeof(buf),
                          "UPDATE kv SET v = 'upd%llu' WHERE id = %llu",
                          static_cast<unsigned long long>(i),
                          static_cast<unsigned long long>(id));
        }
        const core::Tick t0 = s->reactor->now();
        wire::CallResult cr;
        co_await s->stub->sql(std::string(buf), cr);
        const bool ok = cr.ok && cr.response.kind == wire::MsgKind::SqlResult &&
                        cr.response.sql_ok;
        if (ok) {
            ++s->committed;
        }
        s->lat.push_back(s->reactor->now() - t0);
    }
    if (--s->active == 0) {
        s->done = true;
    }
    co_return;
}

double pct_us(std::vector<core::Tick>& v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>((p / 100.0) * static_cast<double>(v.size()));
    if (idx >= v.size()) {
        idx = v.size() - 1;
    }
    return static_cast<double>(v[idx]) / 1000.0;
}

std::uint16_t parse_port(const char* s) {
    char* e = nullptr;
    const unsigned long long v = std::strtoull(s, &e, 10);
    return (e != nullptr && *e == '\0' && v > 0 && v <= 65535)
               ? static_cast<std::uint16_t>(v)
               : 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint64_t count = 20000;
    std::uint64_t inflight = 32;
    std::uint64_t keyspace = 10000;
    std::uint64_t run_seconds = 60;
    Mix mix = Mix::Write;

    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = argv[i];
        const char* v = argv[i + 1];
        if (std::strcmp(k, "--host") == 0) {
            const char* colon = std::strrchr(v, ':');
            if (colon != nullptr && colon != v) {
                host.assign(v, colon);
                port = parse_port(colon + 1);
            } else {
                port = parse_port(v);
            }
        } else if (std::strcmp(k, "--count") == 0) {
            count = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(k, "--inflight") == 0) {
            inflight = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(k, "--keyspace") == 0) {
            keyspace = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(k, "--run-seconds") == 0) {
            run_seconds = std::strtoull(v, nullptr, 10);
        } else if (std::strcmp(k, "--workload") == 0) {
            if (std::strcmp(v, "read") == 0) {
                mix = Mix::Read;
            } else if (std::strcmp(v, "rw5050") == 0) {
                mix = Mix::RW5050;
            } else {
                mix = Mix::Write;
            }
        }
    }
    if (port == 0) {
        std::printf("SQLBENCH error=no_host\n");
        return 2;
    }
    if (inflight == 0) {
        inflight = 1;
    }

    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::printf("SQLBENCH error=reactor\n");
        return 1;
    }
    prod::ProdNetworkBus bus(reactor);
    if (!bus.add_node(kClientId)) {
        std::printf("SQLBENCH error=client_bind\n");
        return 1;
    }
    bus.add_peer(kServerId, host, port);
    core::INetwork* net = bus.node(kClientId);
    wire::ClientConfig ccfg;
    ccfg.poll_grain = 50'000;
    ccfg.attempt_deadline = 1'000'000'000;
    ccfg.max_attempts = 2;
    wire::ClientStub stub(*net, reactor.clock(), core::Endpoint{kServerId}, ccfg);
    reactor.spawn(stub.pump(1'000'000'000));

    Shared s;
    s.stub = &stub;
    s.reactor = &reactor;
    s.count = count;
    s.keyspace = keyspace == 0 ? 1 : keyspace;
    s.mix = mix;

    const core::Tick wall = static_cast<core::Tick>(run_seconds) * 1'000'000'000LL;

    // SETUP: CREATE TABLE (single statement), then pipelined INSERT load.
    {
        bool setup_done = false;
        reactor.spawn(create_table_program(&stub, &setup_done));
        reactor.run_until([&] { return setup_done; }, reactor.now() + wall);
    }
    s.next = 0;
    s.active = inflight;
    s.done = false;
    for (std::uint64_t w = 0; w < inflight; ++w) {
        reactor.spawn(load_worker(&s));
    }
    reactor.run_until([&] { return s.done; }, reactor.now() + wall);

    // RUN (measured).
    s.next = 0;
    s.committed = 0;
    s.active = inflight;
    s.done = false;
    s.lat.clear();
    s.lat.reserve(static_cast<std::size_t>(count));
    const core::Tick t_start = reactor.now();
    for (std::uint64_t w = 0; w < inflight; ++w) {
        reactor.spawn(worker(&s));
    }
    const bool finished = reactor.run_until([&] { return s.done; }, t_start + wall);
    const core::Tick elapsed = reactor.now() - t_start;

    const double elapsed_ms = static_cast<double>(elapsed) / 1'000'000.0;
    const double tput = elapsed > 0
                            ? static_cast<double>(s.committed) /
                                  (static_cast<double>(elapsed) / 1'000'000'000.0)
                            : 0.0;
    const char* wl = mix == Mix::Read ? "read" : (mix == Mix::RW5050 ? "rw5050" : "write");
    std::printf(
        "SQLBENCH workload=%s count=%llu inflight=%llu keyspace=%llu committed=%llu "
        "tput=%.1f elapsed_ms=%.3f p50_us=%.2f p99_us=%.2f finished=%d\n",
        wl, static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(inflight),
        static_cast<unsigned long long>(s.keyspace),
        static_cast<unsigned long long>(s.committed), tput, elapsed_ms,
        pct_us(s.lat, 50.0), pct_us(s.lat, 99.0), finished ? 1 : 0);
    std::fflush(stdout);
    return (s.committed == count && finished) ? 0 : 1;
}
