// lockstep_kvbench.cpp — load driver for the keyed-KV wire::Server (lockstepd --wire-server).
//
// Drives the REAL keyed transactional path (Connection.put / Connection.get over the wire
// stub) — the read+write surface the consensus admin path (write-only value append) cannot
// represent. Closes the bench's read-heavy / read-mix vectors vs etcd/tikv keyed get/put.
//
// One client process: own ProdReactor + ProdNetwork, dials the wire-server (--host
// [HOST:]PORT, cross-machine capable), runs a LOAD phase (populate the keyspace so reads
// hit data) then a measured RUN phase of `count` ops at `inflight` concurrency (K worker
// coroutines on the one reactor → K ops in flight). Reports committed throughput + nearest-
// rank p50/p99 (real wall-clock from the reactor's ProdClock). Bounded by --run-seconds.
//
// LINUX-ONLY (prod providers are __linux__; built under if(UNIX AND NOT APPLE)).
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
#include <lockstep/query/Driver.hpp>
#include <lockstep/query/wire/ClientStub.hpp>

namespace {
namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace query = lockstep::query;

constexpr std::uint64_t kServerId = 1;
constexpr std::uint64_t kClientId = 2;

enum class Mix { Write, Read, RW5050 };

struct Shared {
    query::Connection* conn = nullptr;
    prod::ProdReactor* reactor = nullptr;
    std::uint64_t count = 0;
    std::uint64_t keyspace = 1000;
    std::size_t value_bytes = 16;
    Mix mix = Mix::Write;
    std::uint64_t next = 0;       // next op index to claim
    std::uint64_t committed = 0;  // ops confirmed (write committed / read ok)
    std::uint64_t active = 0;     // live workers
    bool done = false;
    std::vector<core::Tick> lat;  // per-op latency (ns)
    std::string val;
};

std::string key_for(std::uint64_t i, std::uint64_t keyspace) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "k%010llu",
                  static_cast<unsigned long long>(i % keyspace));
    return std::string(buf);
}

// LOAD worker: claim key indices from `next` and put them (PIPELINED — K loaders run
// concurrently on the reactor, else a sequential inflight-1 load at ~fsync latency is far
// too slow to populate a large keyspace within the deadline).
core::Task load_worker(Shared* s) {
    for (;;) {
        const std::uint64_t i = s->next++;
        if (i >= s->keyspace) {
            break;
        }
        query::WriteOutcome w;
        co_await s->conn->put(key_for(i, s->keyspace), s->val, w);
    }
    if (--s->active == 0) {
        s->done = true;
    }
    co_return;
}

// One worker: claim op indices until the count is exhausted; per op, put or get by the mix,
// record commit + latency. Free function over stable pointers (no dangling-lambda frame).
core::Task worker(Shared* s) {
    for (;;) {
        const std::uint64_t i = s->next++;
        if (i >= s->count) {
            break;
        }
        const std::string k = key_for(i, s->keyspace);
        const core::Tick t0 = s->reactor->now();
        bool ok = false;
        const bool do_read =
            (s->mix == Mix::Read) || (s->mix == Mix::RW5050 && (i & 1ULL) == 0ULL);
        if (do_read) {
            query::ReadOutcome r;
            co_await s->conn->get(k, r);
            ok = r.ok;
        } else {
            query::WriteOutcome w;
            co_await s->conn->put(k, s->val, w);
            ok = w.committed;
        }
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
    std::uint64_t inflight = 64;
    std::uint64_t keyspace = 10000;
    std::size_t value_bytes = 16;
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
        } else if (std::strcmp(k, "--value-bytes") == 0) {
            value_bytes = static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
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
        std::printf("KVBENCH error=no_host\n");
        return 2;
    }
    if (inflight == 0) {
        inflight = 1;
    }

    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::printf("KVBENCH error=reactor\n");
        return 1;
    }
    prod::ProdNetworkBus bus(reactor);
    if (!bus.add_node(kClientId)) {
        std::printf("KVBENCH error=client_bind\n");
        return 1;
    }
    bus.add_peer(kServerId, host, port);  // where the wire-server listens (cross-machine OK)
    core::INetwork* net = bus.node(kClientId);
    // REAL-TIME retry config: ClientStub's default {1, 48, 16} are VIRTUAL sim ticks; under
    // ProdClock a Tick is a NANOSECOND, so 48 ns would time out before any real network RTT.
    // Size the grid for real loopback/LAN latency: poll every 50 µs, 200 ms per attempt.
    // PATIENT config: a per-attempt deadline far above any real RTT, with only ONE retry.
    // A short deadline + many retries causes a retry STORM under load (a slow reply triggers
    // a duplicate send → more load → slower → more retries → collapse). Waiting patiently
    // (3 s) keeps the in-flight window honest; the dedup table makes the rare retry safe.
    query::wire::ClientConfig ccfg;
    ccfg.poll_grain = 50'000;              // 50 µs — fine grid keeps read latency low
    ccfg.attempt_deadline = 1'000'000'000; // 1 s — above write fsync RTT so no retry storm
    ccfg.max_attempts = 2;
    query::wire::ClientStub stub(*net, reactor.clock(), core::Endpoint{kServerId}, ccfg);
    query::Connection conn(stub);
    reactor.spawn(stub.pump(1'000'000'000));

    Shared s;
    s.conn = &conn;
    s.reactor = &reactor;
    s.count = count;
    s.keyspace = keyspace == 0 ? 1 : keyspace;
    s.value_bytes = value_bytes;
    s.mix = mix;
    s.val.assign(value_bytes, 'x');

    const core::Tick wall = static_cast<core::Tick>(run_seconds) * 1'000'000'000LL;

    // LOAD phase (populate keyspace; not measured) — PIPELINED with `inflight` loaders.
    s.next = 0;
    s.active = inflight;
    s.done = false;
    for (std::uint64_t w = 0; w < inflight; ++w) {
        reactor.spawn(load_worker(&s));
    }
    reactor.run_until([&] { return s.done; }, reactor.now() + wall);

    // RUN phase (measured): K workers, K ops in flight on the one reactor.
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
        "KVBENCH workload=%s count=%llu inflight=%llu value_bytes=%zu keyspace=%llu "
        "committed=%llu tput=%.1f elapsed_ms=%.3f p50_us=%.2f p99_us=%.2f finished=%d\n",
        wl, static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(inflight), value_bytes,
        static_cast<unsigned long long>(s.keyspace),
        static_cast<unsigned long long>(s.committed), tput, elapsed_ms,
        pct_us(s.lat, 50.0), pct_us(s.lat, 99.0), finished ? 1 : 0);
    std::fflush(stdout);
    return (s.committed == count && finished) ? 0 : 1;
}
