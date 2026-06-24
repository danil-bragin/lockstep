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
#include <memory>
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

// ---- one CHEAP commit-index round-trip (S8.3; O(1) on the server, NO log) --
core::Task stat_commit_rpc(core::INetwork* cli, core::Endpoint admin,
                           prod::AdminCommit* out, bool* ok, bool* done) {
    const std::vector<std::byte> req = prod::encode_stat_commit();
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    *ok = prod::decode_stat_commit(rep.payload, *out);
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
//
// CLIENT-ID UNIQUENESS (S8.3): the daemon's admin net learns each inbound connection by
// the connecting node's HELLO id and routes a reply via send(msg.from). If TWO client
// connections to the SAME daemon share a node id, the daemon collapses them to ONE
// connection and a reply for one is sent down the other — the second client never gets
// its reply. So every concurrent client (each load conn AND each commit poller) MUST use
// a DISTINCT id. `client_id` defaults to kClientId for the single-shot verbs; the
// concurrent paths pass a unique id per connection.
struct Client {
    prod::ProdReactor reactor;
    prod::ProdNetworkBus bus{reactor};
    bool ok = false;
    std::uint64_t self_id = 0;
    std::uint64_t admin_peer_id = 0;

    explicit Client(std::uint16_t admin_port, std::uint64_t client_id = kClientId) {
        if (!reactor.valid()) {
            return;
        }
        self_id = client_id;
        // Synthetic peer id for the target admin endpoint (unique per port).
        admin_peer_id = 8'000'000'000ULL + admin_port;
        if (!bus.add_node(self_id)) {  // our own ephemeral client listen socket
            return;
        }
        bus.add_peer(admin_peer_id, admin_port);  // record where the daemon's admin lives
        ok = true;
    }

    [[nodiscard]] core::INetwork* net() { return bus.node(self_id); }
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

// Run the CHEAP commit query against one admin port; fill `out`. True if a reply decoded.
bool do_commit(std::uint16_t port, prod::AdminCommit& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool ok = false;
    bool done = false;
    c.reactor.spawn(stat_commit_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
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

// One CHEAP commit-index round-trip on an EXISTING client reactor/connection (S8.3).
// O(1) on the server (no log serialized), so polling commit progress per submit is cheap
// and does NOT degrade as the durable log grows.
bool bench_commit(Client& c, prod::AdminCommit& out) {
    bool ok = false;
    bool done = false;
    c.reactor.spawn(stat_commit_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
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

// ============================================================================
// PIPELINED / CONCURRENT BENCH (S8.1) — OPEN-LOOP load to find the REAL ceiling.
// ============================================================================
// The S7 `bench` verb is CLOSED-LOOP (concurrency 1: send->recv->send...), so its
// throughput is 1/latency by construction — it measures latency, not the throughput
// ceiling. `pbench` keeps K requests IN FLIGHT on ONE persistent connection (a sliding
// WINDOW: fire K submits without waiting, then for each reply fire one more), and/or
// drives C such connections concurrently on ONE client reactor. Real throughput is the
// total ACCEPTED ops over the wall time of the loop (NOT 1/latency).
//
// WHY THIS REVEALS THE BOTTLENECK: pipelining removes the per-request client<->server
// ROUND-TRIP stall. If throughput RISES with depth, the round-trip (network / commit
// RTT) was the limit. If it stays FLAT, the server's per-op serial work (reactor CPU,
// or fdatasync if accept blocked on sync) is the limit. The admin serve-loop processes
// frames from one connection SERIALLY (recv->handle->send), but the connection BUFFERS
// queued inbound frames (FIFO), so K pipelined submits ARE accepted without K round
// trips. submit->ACCEPT does NOT block on fdatasync (the persist worker syncs in the
// background), so pbench measures the ACCEPT ceiling = reactor/CPU + append + the serve
// loop's per-frame cost.
//
// BOUNDED: finite total N, finite window K, an absolute wall deadline; never a spin.
//
// S8.3 HONEST COMMIT THROUGHPUT (the HEADLINE). accept_tput above is the leader's LOCAL
// APPEND rate: submit() returns at ACCEPT (term,index assigned), BEFORE the entry is
// committed (replicated to a quorum + commit_index advanced). With S8.2b batching, append
// is cheap, so accept_tput inflates FAR past the real commit ceiling (measured: 3-node
// depth-64 accept_tput ~46k/s while only ~1900/4000 had committed when the load
// "finished"). So after the accept load, pbench POLLS the new CHEAP O(1) commit-index
// query (StatCommit — role/term/commit only, no log serialized) on every host until the
// cluster commit_index covers the highest accepted index; commit_tput = accepted /
// (wall until commit covers all accepted). HEADLINE = commit_tput; accept_tput kept as a
// clearly-labeled secondary (leader-append, NOT commit). The cheap poll is O(1) so it
// does NOT degrade with log size — the hot path no longer pays the full-log STATUS cost.

struct PipeArgs {
    std::uint64_t count = 2000;        // total submits across all connections (FINITE)
    std::uint64_t value_bytes = 16;
    std::uint64_t inflight = 16;       // window depth K per connection (>=1)
    std::uint64_t conns = 1;           // concurrent client connections C (>=1)
};

// Per-connection pipeline state (heap-stable; the coroutine holds a raw pointer to it).
struct PipeConn {
    Client* client = nullptr;          // owns reactor/bus (shared reactor via the driver)
    std::uint64_t to_send = 0;         // submits still to fire on THIS connection
    std::uint64_t base_seq = 0;        // first value seq for this conn (distinct values)
    std::uint64_t next_seq = 0;        // next value seq to fire
    std::uint64_t value_bytes = 16;
    std::uint64_t window = 1;          // K
    std::uint64_t accepted = 0;        // replies that were SubmitOk
    std::uint64_t replied = 0;         // total replies seen
    std::uint64_t max_index = 0;       // highest log index ACCEPTED (the commit target)
    bool fault = false;                // a not-accepted / decode failure stops the conn
    bool done = false;
};

// The pipelined driver for ONE connection: keep `window` submits outstanding. Fire the
// first window, then on EACH reply fire the next (sliding window) until `to_send` are all
// fired AND all replies are in. send() and recv() are independent coroutines awaited in
// lockstep order (the connection delivers in send order, the serve loop replies in that
// order, so the j-th recv corresponds to the j-th send). BOUNDED by `to_send`.
//
// EARLY STOP on a NotLeader/garbage reply: if leadership is lost mid-run we STOP firing
// new submits and DRAIN the already-outstanding replies, then finish. This keeps the
// measured throughput honest (it covers the STABLE-leader window, not a long tail of
// doomed NotLeader submits that would dilute the denominator) and bounds the run.
core::Task pipe_run(PipeConn* st) {
    core::INetwork* net = st->client->net();
    const core::Endpoint admin = st->client->admin_ep();
    std::uint64_t outstanding = 0;
    bool stop_sending = false;
    // Prime the window.
    while (outstanding < st->window && st->next_seq < st->base_seq + st->to_send) {
        const std::string v = make_value(st->next_seq++, st->value_bytes);
        const std::vector<std::byte> req = prod::encode_submit(v);
        co_await net->send(admin, {req.data(), req.size()});
        ++outstanding;
    }
    // Drain + refill: one recv per outstanding, fire a replacement if any remain and we
    // have not hit a fault. Loop while ANY reply is still outstanding (covers the early-
    // stop drain) — bounded by the total ever sent.
    while (outstanding > 0) {
        core::Message rep = co_await net->recv();
        --outstanding;
        ++st->replied;
        prod::admin_detail::Reader r{rep.payload.data(), rep.payload.size(), 0, true};
        const auto kind = static_cast<prod::AdminKind>(r.u8());
        if (kind == prod::AdminKind::SubmitOk) {
            ++st->accepted;
            r.u64();  // skip the reply's term field to reach the index
            const std::uint64_t index = r.u64();
            if (r.ok() && index > st->max_index) {
                st->max_index = index;  // highest accepted index = the commit target
            }
        } else {
            st->fault = true;       // NotLeader / garbage: leadership lost — stop firing.
            stop_sending = true;
        }
        if (!stop_sending && st->next_seq < st->base_seq + st->to_send) {
            const std::string v = make_value(st->next_seq++, st->value_bytes);
            const std::vector<std::byte> req = prod::encode_submit(v);
            co_await net->send(admin, {req.data(), req.size()});
            ++outstanding;
        }
    }
    st->done = true;
    co_return;
}

// One persistent client connection per concurrent stream, ALL on ONE shared reactor (so
// C connections are driven concurrently by the single client reactor — true in-flight
// concurrency at the client). Returns the process exit code; prints a PBENCH line.
int cmd_pbench(const PipeArgs& pa, const std::vector<std::uint16_t>& hosts) {
    const std::uint16_t leader_port = find_leader_port(hosts);
    if (leader_port == 0) {
        std::printf("PBENCH error=no_leader\n");
        std::fflush(stdout);
        return 1;
    }
    const std::uint64_t conns = pa.conns == 0 ? 1 : pa.conns;
    const std::uint64_t window = pa.inflight == 0 ? 1 : pa.inflight;

    // Split the total count across the C connections (the LAST takes the remainder).
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::unique_ptr<PipeConn>> states;
    clients.reserve(conns);
    states.reserve(conns);
    for (std::uint64_t ci = 0; ci < conns; ++ci) {
        // DISTINCT id per load connection so the daemon routes each conn's replies back
        // to the right connection (a shared id would collapse them — see Client docs).
        auto cl = std::make_unique<Client>(leader_port, kClientId + ci);
        if (!cl->ok) {
            std::printf("PBENCH error=client_init\n");
            std::fflush(stdout);
            return 1;
        }
        clients.push_back(std::move(cl));
    }

    // Each Client owns its own reactor (epoll fd + bus). To drive C connections
    // concurrently we ROUND-ROBIN pump every reactor a short slice so all C have
    // outstanding work at once (a finite, deadline-bounded loop — never a spin). With
    // C=1 this is a single connection at window depth K (pure pipelining).
    const std::uint64_t per = pa.count / conns;
    const std::uint64_t rem = pa.count % conns;
    std::uint64_t seq = 0;
    for (std::uint64_t ci = 0; ci < conns; ++ci) {
        auto st = std::make_unique<PipeConn>();
        st->client = clients[ci].get();
        st->value_bytes = pa.value_bytes;
        st->window = window;
        st->to_send = per + (ci + 1 == conns ? rem : 0);
        st->base_seq = seq;
        st->next_seq = seq;
        seq += st->to_send;
        clients[ci]->reactor.spawn(pipe_run(st.get()));
        states.push_back(std::move(st));
    }

    // Bound the whole load: a generous absolute wall (scaled by count); if it elapses we
    // stop (a stall is a finding, not a hang). We measure elapsed time from the MAX now()
    // across all reactors — a reactor that finishes early stops being pumped (its clock
    // freezes), so the per-reactor clock is NOT a safe global wall. The max over all live
    // reactors always advances while ANY connection is pumped, so the guard never wedges.
    auto max_now = [&]() -> core::Tick {
        core::Tick m = 0;
        for (std::uint64_t ci = 0; ci < conns; ++ci) {
            const core::Tick n = clients[ci]->reactor.now();
            if (n > m) {
                m = n;
            }
        }
        return m;
    };
    const core::Tick t0 = max_now();
    const core::Tick wall_ns = t0 +
                               static_cast<core::Tick>(pa.count) * 2'000'000 + // ~2ms/op cap
                               5'000'000'000;                                  // +5s floor
    bool all_done = false;
    while (!all_done && max_now() < wall_ns) {
        all_done = true;
        for (std::uint64_t ci = 0; ci < conns; ++ci) {
            if (!states[ci]->done) {
                all_done = false;
                // Pump THIS reactor one short slice so every connection progresses.
                clients[ci]->reactor.run_until([&] { return states[ci]->done; },
                                               clients[ci]->reactor.now() + 1'000'000);
            }
        }
    }
    const core::Tick t1 = max_now();
    const double elapsed_ms = static_cast<double>(t1 - t0) / 1'000'000.0;

    std::uint64_t accepted = 0;
    std::uint64_t replied = 0;
    std::uint64_t commit_target = 0;  // highest accepted log index across all conns
    bool any_fault = false;
    bool any_unfinished = false;
    for (std::uint64_t ci = 0; ci < conns; ++ci) {
        accepted += states[ci]->accepted;
        replied += states[ci]->replied;
        if (states[ci]->max_index > commit_target) {
            commit_target = states[ci]->max_index;
        }
        any_fault = any_fault || states[ci]->fault;
        any_unfinished = any_unfinished || !states[ci]->done;
    }
    // accept_tput = leader-LOCAL-APPEND rate (submit returns at append, BEFORE commit).
    const double accept_tput = elapsed_ms > 0.0
                            ? static_cast<double>(accepted) / (elapsed_ms / 1000.0)
                            : 0.0;

    // ----------------------------------------------------------------------
    // S8.3 HONEST COMMIT THROUGHPUT. accept_tput above measures the leader's local
    // APPEND rate — submit() returns when the entry is appended (term,index assigned),
    // BEFORE it is committed (replicated to a quorum + commit_index advanced). With
    // batching (S8.2b) append is cheap, so accept_tput inflates well past the real
    // COMMIT ceiling. To measure HONEST commit throughput we now POLL the CHEAP O(1)
    // commit-index query (StatCommit) on EVERY host until the cluster's commit_index
    // COVERS the highest accepted index (commit_target). Because the leader only
    // advances commit_index once a QUORUM has acked, the max commit_index across hosts
    // reaching commit_target == quorum-committed. The poll is O(1) per call (no log
    // serialized), so polling per progress-check does NOT degrade as the log grows.
    //
    // commit_tput = accepted / (wall time from the FIRST submit until commit covers all
    // accepted submits). This is the HEADLINE. BOUNDED by an absolute catch-up wall.
    double commit_tput = 0.0;
    double commit_elapsed_ms = 0.0;
    std::uint64_t commit_reached = 0;
    bool commit_covered = false;
    if (commit_target > 0) {
        // A dedicated cheap-poll client per host (separate from the load connections).
        // Each gets a DISTINCT id (well above the load-conn ids) so the daemon does NOT
        // collapse a poller connection with a load connection (see Client docs).
        std::vector<std::unique_ptr<Client>> pollers;
        pollers.reserve(hosts.size());
        std::uint64_t poller_id = kClientId + 1'000'000ULL;
        for (std::uint16_t port : hosts) {
            auto pc = std::make_unique<Client>(port, poller_id++);
            if (pc->ok) {
                pollers.push_back(std::move(pc));
            }
        }
        // Catch-up wall: generous, scaled by count (a stall here is a finding, bounded).
        const core::Tick catchup_wall =
            static_cast<core::Tick>(pa.count) * 4'000'000 + 10'000'000'000;  // ~4ms/op +10s
        core::Tick poll_t0 = 0;
        for (auto& p : pollers) {
            if (p->reactor.now() > poll_t0) {
                poll_t0 = p->reactor.now();
            }
        }
        const core::Tick poll_deadline = poll_t0 + catchup_wall;
        auto poll_max_now = [&]() -> core::Tick {
            core::Tick m = 0;
            for (auto& p : pollers) {
                if (p->reactor.now() > m) {
                    m = p->reactor.now();
                }
            }
            return m;
        };
        while (!commit_covered && poll_max_now() < poll_deadline) {
            commit_reached = 0;
            for (auto& p : pollers) {
                prod::AdminCommit ac;
                if (bench_commit(*p, ac) && ac.commit_index > commit_reached) {
                    commit_reached = ac.commit_index;  // max == the leader's commit_index
                }
            }
            if (commit_reached >= commit_target) {
                commit_covered = true;
                break;
            }
        }
        const core::Tick commit_t1 = poll_max_now();
        // Commit wall measured from the load's t0 (max_now baseline) to commit coverage.
        // The poll reactors are distinct from the load reactors, so we approximate the
        // commit-completion instant by the accept-phase wall + the poll catch-up wall.
        commit_elapsed_ms = elapsed_ms +
            static_cast<double>(commit_t1 - poll_t0) / 1'000'000.0;
        commit_tput = (commit_covered && commit_elapsed_ms > 0.0)
                          ? static_cast<double>(accepted) / (commit_elapsed_ms / 1000.0)
                          : 0.0;
    }

    std::printf(
        "PBENCH count=%llu inflight=%llu conns=%llu value_bytes=%llu "
        "commit_tput=%.1f commit_elapsed_ms=%.3f commit_target=%llu commit_reached=%llu "
        "commit_covered=%d accept_tput=%.1f accept_elapsed_ms=%.3f "
        "accepted=%llu replied=%llu fault=%d unfinished=%d\n",
        static_cast<unsigned long long>(pa.count),
        static_cast<unsigned long long>(window),
        static_cast<unsigned long long>(conns),
        static_cast<unsigned long long>(pa.value_bytes),
        commit_tput, commit_elapsed_ms,
        static_cast<unsigned long long>(commit_target),
        static_cast<unsigned long long>(commit_reached),
        commit_covered ? 1 : 0,
        accept_tput, elapsed_ms,
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(replied), any_fault ? 1 : 0,
        any_unfinished ? 1 : 0);
    std::fflush(stdout);
    return (any_unfinished || accepted == 0) ? 1 : 0;
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

// S8.3 CHEAP commit query — the O(1) counterpart of `status` (role/term/commit ONLY, no
// log digest). Prints one line per host. This is the verb a commit-progress poll uses so
// it never pays the full-log STATUS re-serialization cost.
int cmd_commit(const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        prod::AdminCommit ac;
        const bool ok = do_commit(port, ac);
        std::printf("COMMIT port=%u ok=%d role=%u term=%llu commit=%llu\n", port,
                    ok ? 1 : 0, static_cast<unsigned>(ac.role),
                    static_cast<unsigned long long>(ac.term),
                    static_cast<unsigned long long>(ac.commit_index));
        std::fflush(stdout);
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
            "       lockstep_admin commit --host PORT [--host PORT ...]\n"
            "       lockstep_admin submit VALUE --host PORT [--host PORT ...]\n"
            "       lockstep_admin bench --count N [--value-bytes B] "
            "[--commit-samples S] --host PORT [--host PORT ...]\n"
            "       lockstep_admin pbench --count N [--inflight K] [--conns C] "
            "[--value-bytes B] --host PORT [--host PORT ...]\n");
        return 2;
    }

    const std::string verb = argv[1];
    std::vector<std::uint16_t> hosts;
    std::string value;
    BenchArgs ba;
    PipeArgs pa;
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
        } else if (std::strcmp(argv[i], "--inflight") == 0 && i + 1 < argc) {
            pa.inflight = parse_u64_opt(argv[i + 1], pa.inflight);
            ++i;
        } else if (std::strcmp(argv[i], "--conns") == 0 && i + 1 < argc) {
            pa.conns = parse_u64_opt(argv[i + 1], pa.conns);
            ++i;
        }
    }
    // pbench shares --count + --value-bytes with bench (the --count/--value-bytes flags
    // land in ba; copy them across so pbench honors them).
    pa.count = ba.count;
    pa.value_bytes = ba.value_bytes;

    if (hosts.empty()) {
        std::fprintf(stderr, "lockstep_admin: no --host PORT given\n");
        return 2;
    }

    if (verb == "status") {
        return cmd_status(hosts);
    }
    if (verb == "commit") {
        return cmd_commit(hosts);
    }
    if (verb == "submit") {
        return cmd_submit(value, hosts);
    }
    if (verb == "bench") {
        return cmd_bench(ba, hosts);
    }
    if (verb == "pbench") {
        return cmd_pbench(pa, hosts);
    }
    std::fprintf(stderr, "lockstep_admin: unknown verb '%s'\n", verb.c_str());
    return 2;
}
