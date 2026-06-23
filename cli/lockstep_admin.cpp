// lockstep_admin.cpp — Phase 7 S5b-2. The CLUSTER ADMIN CLIENT: a thin CLI that talks
// the lockstepd admin protocol over REAL TCP. It dials a node's admin port, sends ONE
// request, awaits ONE reply, prints the decoded result, and exits — BOUNDED by an
// absolute reactor deadline so a dead/unreachable node can NEVER hang the client.
//
// Three verbs:
//   status --host PORT [--host PORT ...]
//       Query each given admin port's role/term/commit_index/committed-log digest and
//       print one machine-parseable line per node:
//         STATUS port=<P> ok=<0|1> role=<0|1|2> term=<T> commit=<C> log=<v1,v2,...>
//       (role 0=Follower 1=Candidate 2=Leader). A node that does not reply within the
//       deadline prints ok=0 (the orchestrator treats it as down / not-yet-up).
//
//   submit VALUE --host PORT [--host PORT ...]
//       SUBMIT VALUE to the nodes in order; on NotLeader, retry the next host until one
//       ACCEPTS (the LEADER-FIND client). Prints:
//         SUBMIT value=<V> accepted=<0|1> port=<P> term=<T> index=<I> leader_hint=<H>
//       accepted=1 means the leader appended it at <index>; accepted=0 means no host in
//       the list accepted within the per-host deadline (caller retries later).
//
//   bench --count N [--value-bytes B] [--commit-samples S] --host PORT [--host PORT ...]
//       Phase 7 S7 PERFORMANCE BASELINE driver. Over ONE persistent client reactor +
//       ONE persistent connection to the leader's admin port (found via the same
//       leader-find retry as submit), drive a CLOSED-LOOP write load: submit N distinct
//       values back-to-back (one in flight at a time — the admin serve-loop is a single
//       recv->send round-trip), measuring the SERVER submit path (NOT process-spawn
//       overhead — the reactor/connection are reused for all N). Reports:
//         BENCH count=<N> value_bytes=<B> elapsed_ms=<T> submit_tput=<ops/s>
//               submit_p50_us=<..> submit_p99_us=<..>
//               commit_samples=<S> commit_p50_us=<..> commit_p99_us=<..>
//               read_check=<ok|MISMATCH> committed_seen=<C>
//       submit_* = submit->ACCEPT latency (leader appends + replies). commit_* = a
//       SAMPLE of submit->COMMIT latency (poll STATUS until commit_index covers the
//       value's index) measured on S of the submissions — approximate (poll-bounded,
//       so it OVER-counts commit latency by up to one poll interval; stated as a
//       baseline caveat). read_check confirms the committed log contains the submitted
//       values in order (a correctness check under load, not a read benchmark).
// The client speaks the admin wire via the PROVIDER surface (ProdNetwork over the
// ProdReactor), so THIS TU does no raw socket/epoll syscall — exactly like lockstepd.
// It mints its OWN client endpoint (a ProdNetwork node on an ephemeral port) and
// records each target admin port as a PEER it dials.
//
// LINUX-ONLY (epoll/sockets): built only on Linux (if(UNIX AND NOT APPLE)).
// NOT in the providers/ lint-exempt zone: forbidden-lint scans it (no raw syscall — only
// the provider + admin-protocol surfaces). Every coroutine is a FREE FUNCTION over
// stable pointers (no [&] capture Task — avoids stack-use-after-scope).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdConsensusNode.hpp>  // admin protocol encode/decode
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;

// A distinct client endpoint id (well above any node/admin id so it never collides).
constexpr std::uint64_t kClientId = 9'000'000'001ULL;

// Per-request wall guard: how long the client waits for ONE admin round-trip before
// giving up (a down / not-yet-up node yields no reply). Bounded — never an unbounded
// spin. 1.5 s absorbs cross-process connect + reply latency while killing any hang.
constexpr core::Tick kReqWallNs = 1'500'000'000;

std::uint16_t parse_port(const char* s) {
    if (s == nullptr) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == nullptr || *end != '\0' || v == 0 || v > 65535) {
        return 0;
    }
    return static_cast<std::uint16_t>(v);
}

// ---- one STATUS round-trip (free function over stable pointers) ------------
core::Task status_rpc(core::INetwork* cli, core::Endpoint admin, prod::AdminStatus* out,
                      bool* ok, bool* done) {
    const std::vector<std::byte> req = prod::encode_status();
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    *ok = prod::decode_status(rep.payload, *out);
    *done = true;
    co_return;
}

// ---- one SUBMIT round-trip (free function over stable pointers) ------------
struct SubmitOutcome {
    bool replied = false;
    bool accepted = false;
    std::uint64_t term = 0;
    std::uint64_t index = 0;
    std::uint64_t leader_hint = 0;
};

core::Task submit_rpc(core::INetwork* cli, core::Endpoint admin, std::string value,
                      SubmitOutcome* out, bool* done) {
    const std::vector<std::byte> req = prod::encode_submit(value);
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    prod::admin_detail::Reader r{rep.payload.data(), rep.payload.size(), 0, true};
    const auto kind = static_cast<prod::AdminKind>(r.u8());
    out->replied = true;
    if (kind == prod::AdminKind::SubmitOk) {
        out->accepted = true;
        out->term = r.u64();
        out->index = r.u64();
    } else if (kind == prod::AdminKind::NotLeader) {
        out->leader_hint = r.u64();
    }
    *done = true;
    co_return;
}

// A fresh client endpoint dialing one admin port. The admin endpoint id used for
// dialing is synthetic (admin ports map to distinct synthetic peer ids in the client's
// bus); each call recreates the reactor/bus so a prior connection never lingers.
struct Client {
    prod::ProdReactor reactor;
    prod::ProdNetworkBus bus{reactor};
    bool ok = false;
    std::uint64_t admin_peer_id = 0;

    explicit Client(std::uint16_t admin_port) {
        if (!reactor.valid()) {
            return;
        }
        // Synthetic peer id for the target admin endpoint (unique per port).
        admin_peer_id = 8'000'000'000ULL + admin_port;
        if (!bus.add_node(kClientId)) {  // our own ephemeral client listen socket
            return;
        }
        bus.add_peer(admin_peer_id, admin_port);  // record where the daemon's admin lives
        ok = true;
    }

    [[nodiscard]] core::INetwork* net() { return bus.node(kClientId); }
    [[nodiscard]] core::Endpoint admin_ep() const { return core::Endpoint{admin_peer_id}; }
};

// Run STATUS against one admin port; fill `out`. Returns true if a reply decoded.
bool do_status(std::uint16_t port, prod::AdminStatus& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool ok = false;
    bool done = false;
    c.reactor.spawn(status_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && ok;
}

// Run SUBMIT against one admin port; fill `out`. Returns true if a reply was decoded.
bool do_submit(std::uint16_t port, const std::string& value, SubmitOutcome& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool done = false;
    c.reactor.spawn(submit_rpc(c.net(), c.admin_ep(), value, &out, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && out.replied;
}

// ============================================================================
// PERF BASELINE (S7) — closed-loop bench over ONE persistent client + connection.
// ============================================================================
// We REUSE the one `Client` reactor/bus for ALL submits + status polls of a run, so
// the measured time is the SERVER path (connect once, then submit->accept round-trips),
// NOT per-call reactor/process construction. Each round-trip co_awaits send()+recv()
// once (the admin serve-loop's single recv->send), so exactly ONE request is in flight
// — a closed loop of concurrency 1 (a baseline, not a saturation test).

// One submit round-trip on an EXISTING client reactor/connection. Returns true if a
// reply decoded; fills `out`. Bounded by an absolute deadline (never a spin).
bool bench_submit(Client& c, const std::string& value, SubmitOutcome& out) {
    bool done = false;
    out = SubmitOutcome{};
    c.reactor.spawn(submit_rpc(c.net(), c.admin_ep(), value, &out, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && out.replied;
}

// One status round-trip on an EXISTING client reactor/connection.
bool bench_status(Client& c, prod::AdminStatus& out) {
    bool ok = false;
    bool done = false;
    c.reactor.spawn(status_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && ok;
}

// p-th percentile (p in [0,100]) of a latency sample (ns). Caller need not pre-sort;
// we sort a copy. Empty sample -> 0. Nearest-rank (no interpolation) — fine for a
// baseline. Returned in MICROSECONDS.
double percentile_us(std::vector<core::Tick> samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const long rank =
        std::lround((p / 100.0) * static_cast<double>(samples.size() - 1));
    std::size_t idx = rank < 0 ? 0 : static_cast<std::size_t>(rank);
    if (idx >= samples.size()) {
        idx = samples.size() - 1;
    }
    return static_cast<double>(samples[idx]) / 1000.0;
}

struct BenchArgs {
    std::uint64_t count = 1000;
    std::uint64_t value_bytes = 16;
    std::uint64_t commit_samples = 64;  // # of submissions whose submit->commit we time
};

// Find which host is the LEADER right now (leader-find): submit a probe value; whichever
// host ACCEPTS is the leader. Returns its port (and consumes the probe — it commits like
// any value). 0 if none accepted within the retry budget.
std::uint16_t find_leader_port(const std::vector<std::uint16_t>& hosts) {
    for (int attempt = 0; attempt < 40; ++attempt) {  // ~ bounded retries
        for (std::uint16_t port : hosts) {
            Client c(port);
            if (!c.ok) {
                continue;
            }
            SubmitOutcome so;
            if (bench_submit(c, "bench-leader-probe", so) && so.accepted) {
                return port;
            }
        }
    }
    return 0;
}

// Build a distinct value of ~value_bytes bytes carrying the sequence number, so the
// read-check can confirm order. "bench-<seq>-<padding>" padded to value_bytes.
std::string make_value(std::uint64_t seq, std::uint64_t value_bytes) {
    std::string v = "bench-" + std::to_string(seq) + "-";
    if (v.size() < value_bytes) {
        v.append(value_bytes - v.size(), 'x');
    }
    return v;
}

// The closed-loop perf run against the LEADER port. Returns the process exit code.
int cmd_bench(const BenchArgs& ba, const std::vector<std::uint16_t>& hosts) {
    const std::uint16_t leader_port = find_leader_port(hosts);
    if (leader_port == 0) {
        std::printf("BENCH error=no_leader\n");
        std::fflush(stdout);
        return 1;
    }

    // ONE persistent client/connection for the whole measured load.
    Client c(leader_port);
    if (!c.ok) {
        std::printf("BENCH error=client_init\n");
        std::fflush(stdout);
        return 1;
    }

    std::vector<core::Tick> submit_lat;      // submit->accept (ns), per submission
    std::vector<core::Tick> commit_lat;      // submit->commit (ns), a sample
    submit_lat.reserve(ba.count);
    commit_lat.reserve(ba.commit_samples);

    // Sample stride for commit-latency: spread the S samples across the N submissions.
    const std::uint64_t stride =
        ba.commit_samples == 0 ? 0 : (ba.count / (ba.commit_samples + 1) + 1);

    std::uint64_t accepted = 0;
    std::uint64_t last_index = 0;

    const core::Tick t0 = c.reactor.now();
    for (std::uint64_t i = 0; i < ba.count; ++i) {
        const std::string value = make_value(i, ba.value_bytes);
        const core::Tick s0 = c.reactor.now();
        SubmitOutcome so;
        if (!bench_submit(c, value, so) || !so.accepted) {
            // Lost leadership / no reply: stop the measured run (baseline is the
            // steady leader path; a mid-run election would skew it).
            std::printf("BENCH warn=submit_not_accepted_at=%llu\n",
                        static_cast<unsigned long long>(i));
            break;
        }
        const core::Tick s1 = c.reactor.now();
        submit_lat.push_back(s1 - s0);
        ++accepted;
        last_index = so.index;

        // Sampled submit->commit latency: poll STATUS until commit_index covers this
        // value's index. Poll-bounded (over-counts by up to one poll turn) — a baseline.
        if (stride != 0 && commit_lat.size() < ba.commit_samples && (i % stride) == 0) {
            const core::Tick wait_until = c.reactor.now() + kReqWallNs;
            bool committed = false;
            while (c.reactor.now() < wait_until) {
                prod::AdminStatus st;
                if (bench_status(c, st) && st.commit_index >= so.index) {
                    committed = true;
                    break;
                }
            }
            if (committed) {
                commit_lat.push_back(c.reactor.now() - s0);
            }
        }
    }
    const core::Tick t1 = c.reactor.now();
    const double elapsed_ms = static_cast<double>(t1 - t0) / 1'000'000.0;
    const double tput = elapsed_ms > 0.0
                            ? static_cast<double>(accepted) / (elapsed_ms / 1000.0)
                            : 0.0;

    // READ CHECK under load: confirm the leader's committed log contains the submitted
    // values in order (correctness check, not a read benchmark). We wait briefly for
    // commit_index to cover the last accepted index, then verify the prefix.
    bool read_ok = false;
    std::uint64_t committed_seen = 0;
    {
        const core::Tick wait_until = c.reactor.now() + kReqWallNs;
        prod::AdminStatus st;
        while (c.reactor.now() < wait_until) {
            if (bench_status(c, st) && st.commit_index >= last_index) {
                break;
            }
        }
        if (bench_status(c, st)) {
            committed_seen = st.commit_index;
            // Verify our `accepted` values appear IN ORDER as the committed-log slice
            // ending at last_index (the run may NOT start at index 1: a re-run against a
            // persistent daemon appends ABOVE prior values, and the leader-find probe
            // sits below us). Our i-th value landed at log index (last_index-accepted+1+i),
            // i.e. committed[] (1-based) slot (last_index-accepted+i) 0-based. So check the
            // contiguous suffix [last_index-accepted, last_index).
            if (accepted > 0 && last_index >= accepted &&
                st.committed.size() >= last_index) {
                read_ok = true;
                const std::uint64_t base = last_index - accepted;  // 0-based start slot
                for (std::uint64_t i = 0; read_ok && i < accepted; ++i) {
                    if (st.committed[base + i] != make_value(i, ba.value_bytes)) {
                        read_ok = false;
                    }
                }
            }
        }
    }

    std::printf(
        "BENCH count=%llu value_bytes=%llu elapsed_ms=%.3f submit_tput=%.1f "
        "submit_p50_us=%.2f submit_p99_us=%.2f commit_samples=%zu commit_p50_us=%.2f "
        "commit_p99_us=%.2f read_check=%s committed_seen=%llu\n",
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(ba.value_bytes), elapsed_ms, tput,
        percentile_us(submit_lat, 50.0), percentile_us(submit_lat, 99.0),
        commit_lat.size(), percentile_us(commit_lat, 50.0),
        percentile_us(commit_lat, 99.0), read_ok ? "ok" : "MISMATCH",
        static_cast<unsigned long long>(committed_seen));
    std::fflush(stdout);
    return read_ok ? 0 : 1;
}

void print_status(std::uint16_t port, bool ok, const prod::AdminStatus& st) {
    std::printf("STATUS port=%u ok=%d role=%u term=%llu commit=%llu log=", port,
                ok ? 1 : 0, static_cast<unsigned>(st.role),
                static_cast<unsigned long long>(st.term),
                static_cast<unsigned long long>(st.commit_index));
    for (std::size_t i = 0; i < st.committed.size(); ++i) {
        std::printf("%s%s", i == 0 ? "" : ",", st.committed[i].c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

int cmd_status(const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        prod::AdminStatus st;
        const bool ok = do_status(port, st);
        print_status(port, ok, st);
    }
    return 0;
}

// LEADER-FIND submit: try each host in order; the LEADER accepts (accepted=1), a
// follower replies NotLeader (we try the next host). Print the outcome.
int cmd_submit(const std::string& value, const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        SubmitOutcome so;
        const bool replied = do_submit(port, value, so);
        if (replied && so.accepted) {
            std::printf("SUBMIT value=%s accepted=1 port=%u term=%llu index=%llu "
                        "leader_hint=0\n",
                        value.c_str(), port, static_cast<unsigned long long>(so.term),
                        static_cast<unsigned long long>(so.index));
            std::fflush(stdout);
            return 0;
        }
        // NotLeader or no reply: try the next host (the leader-find retry loop).
    }
    std::printf("SUBMIT value=%s accepted=0 port=0 term=0 index=0 leader_hint=0\n",
                value.c_str());
    std::fflush(stdout);
    return 1;  // no host accepted (caller retries later)
}

}  // namespace

std::uint64_t parse_u64_opt(const char* s, std::uint64_t fallback) {
    if (s == nullptr || s[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(v) : fallback;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(
            stderr,
            "usage: lockstep_admin status --host PORT [--host PORT ...]\n"
            "       lockstep_admin submit VALUE --host PORT [--host PORT ...]\n"
            "       lockstep_admin bench --count N [--value-bytes B] "
            "[--commit-samples S] --host PORT [--host PORT ...]\n");
        return 2;
    }

    const std::string verb = argv[1];
    std::vector<std::uint16_t> hosts;
    std::string value;
    BenchArgs ba;
    int i = 2;

    if (verb == "submit") {
        if (argc < 3) {
            std::fprintf(stderr, "lockstep_admin submit: missing VALUE\n");
            return 2;
        }
        value = argv[2];
        i = 3;
    }

    for (; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            const std::uint16_t p = parse_port(argv[i + 1]);
            if (p != 0) {
                hosts.push_back(p);
            }
            ++i;
        } else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            ba.count = parse_u64_opt(argv[i + 1], ba.count);
            ++i;
        } else if (std::strcmp(argv[i], "--value-bytes") == 0 && i + 1 < argc) {
            ba.value_bytes = parse_u64_opt(argv[i + 1], ba.value_bytes);
            ++i;
        } else if (std::strcmp(argv[i], "--commit-samples") == 0 && i + 1 < argc) {
            ba.commit_samples = parse_u64_opt(argv[i + 1], ba.commit_samples);
            ++i;
        }
    }

    if (hosts.empty()) {
        std::fprintf(stderr, "lockstep_admin: no --host PORT given\n");
        return 2;
    }

    if (verb == "status") {
        return cmd_status(hosts);
    }
    if (verb == "submit") {
        return cmd_submit(value, hosts);
    }
    if (verb == "bench") {
        return cmd_bench(ba, hosts);
    }
    std::fprintf(stderr, "lockstep_admin: unknown verb '%s'\n", verb.c_str());
    return 2;
}
