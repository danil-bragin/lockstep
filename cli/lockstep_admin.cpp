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
//   submit VALUE [--no-await] --host PORT [--host PORT ...]
//       SUBMIT VALUE to the nodes in order; on NotLeader, retry the next host until one
//       ACCEPTS (the LEADER-FIND client). S8.4 DURABLE-BY-DEFAULT: success now means the
//       write is COMMITTED (durable on a quorum), not merely accepted. After a host
//       accepts {term,index}, poll that node until commit_index covers the index AND the
//       entry there is STILL our value (a stale leader's accepted-but-uncommitted entry
//       can be overwritten / lost — accept != commit), THEN report durable=1. Prints:
//         SUBMIT value=<V> accepted=<0|1> durable=<0|1> port=<P> term=<T> index=<I> ...
//       durable=1 means COMMITTED (exit 0); durable=0 means accepted but NOT confirmed
//       committed within the bounded deadline (exit nonzero — caller retries). --no-await
//       reverts to accept-only (omits durable=; the load harness's accept measurement);
//       the DEFAULT awaits commit so a client is told "done" only when the write is
//       durable. The poll is BOUNDED by an absolute deadline — never an unbounded spin.
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

// S8.4 DURABLE SUBMIT — how long (absolute, BOUNDED) the durable `submit` waits for the
// accepted entry to COMMIT (commit_index covers its index AND the value there is still
// ours) before reporting NOT-durable (the caller retries). accept != commit: a leader
// that crashes before a quorum replicates an accepted-but-UNcommitted entry LEGITIMATELY
// loses it (Raft only promises COMMITTED entries survive). So a durable submit reports
// success ONLY once the entry is COMMITTED — never merely accepted. 5 s comfortably
// covers replication+commit on a healthy quorum yet kills any hang.
constexpr core::Tick kDurableWallNs = 5'000'000'000;

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

// ---- one METRICS scrape round-trip (Phase 10 OBSERVABILITY) ----------------
// Sends the METRICS request, awaits the reply, decodes the Prometheus text blob.
core::Task metrics_rpc(core::INetwork* cli, core::Endpoint admin, std::string* out,
                       bool* ok, bool* done) {
    const std::vector<std::byte> req = prod::encode_metrics();
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    *ok = prod::decode_metrics(rep.payload, *out);
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

// Run a METRICS scrape against one admin port; fill `out` with the Prometheus text.
// Returns true if a reply decoded. (Phase 10 OBSERVABILITY.)
bool do_metrics(std::uint16_t port, std::string& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool ok = false;
    bool done = false;
    c.reactor.spawn(metrics_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
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

// ============================================================================
// Phase 9 S9.1 — MULTI-SHARD KEY-ROUTED LOAD (`mbench`). The throughput-scaling
// proof. Given M shard admin ports (--host P0 --host P1 ...), generate `count`
// DISTINCT keys, route each by key-hash to a shard (shard = hash(key) % M), and
// pipeline each shard's submits to THAT shard's port on its OWN client reactor +
// connection (one connection per shard, all pumped round-robin on this client). Each
// shard is an independent single-node Raft group, so there is NO leader-find and NO
// cross-shard coordination — a key deterministically lands on one shard.
//
// AGGREGATE COMMIT THROUGHPUT (the HEADLINE): drive the accept load across all M
// shards, then POLL each shard's CHEAP O(1) commit query until its commit_index covers
// its highest accepted index. agg_commit_tput = total accepted across all shards / wall
// time from first submit until EVERY shard's commit covers its accepted load. This is
// the aggregate that should rise ~linearly with M up to the core count.
//
// BOUNDED: finite total count, finite per-shard window, an absolute wall + catch-up
// deadline; never a spin. Clean: each shard's Client owns its reactor (RAII).

// FNV-1a hash of a key string -> 64-bit. Deterministic key->shard routing.
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

// Build the key for the n-th submit (distinct per submit). The shard is hash(key)%M.
std::string make_key(std::uint64_t seq) { return "key-" + std::to_string(seq); }

// Per-shard load state for mbench: the shard's port, its connection, and the submits
// routed to it (seqs assigned by key-hash). Reuses PipeConn for the pipelining engine.
int cmd_mbench(const PipeArgs& pa, const std::vector<std::uint16_t>& hosts) {
    const std::uint64_t m = hosts.size();
    if (m == 0) {
        std::printf("MBENCH error=no_hosts\n");
        std::fflush(stdout);
        return 1;
    }
    const std::uint64_t window = pa.inflight == 0 ? 1 : pa.inflight;

    // Route each key to a shard by hash; assign that submit's seq to the shard's stream.
    // We keep a per-shard list of seqs so values stay distinct AND map to the shard the
    // key hashes to (the key-routing the client performs).
    std::vector<std::vector<std::uint64_t>> shard_seqs(m);
    for (std::uint64_t i = 0; i < pa.count; ++i) {
        const std::uint64_t shard = fnv1a(make_key(i)) % m;
        shard_seqs[shard].push_back(i);
    }

    // One persistent client connection per shard, each on its OWN reactor (so M shards
    // are driven concurrently by this client). DISTINCT client id per shard so no daemon
    // collapses two connections (see Client docs).
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<std::unique_ptr<PipeConn>> states;
    clients.reserve(m);
    states.reserve(m);
    for (std::uint64_t s = 0; s < m; ++s) {
        auto cl = std::make_unique<Client>(hosts[s], kClientId + s);
        if (!cl->ok) {
            std::printf("MBENCH error=client_init shard=%llu\n",
                        static_cast<unsigned long long>(s));
            std::fflush(stdout);
            return 1;
        }
        clients.push_back(std::move(cl));
    }
    for (std::uint64_t s = 0; s < m; ++s) {
        auto st = std::make_unique<PipeConn>();
        st->client = clients[s].get();
        st->value_bytes = pa.value_bytes;
        st->window = window;
        st->to_send = shard_seqs[s].size();
        // Values carry the global seq so each is distinct; base_seq/next_seq index into
        // a CONTIGUOUS local block here, but we need the routed seqs, so we pre-build the
        // values and drive via base_seq=0..to_send with a remapped value table is complex.
        // Simpler: give each shard a contiguous private seq block (base = s*count) — still
        // distinct values, still one-per-shard pipelined. The KEY-ROUTING above decided
        // HOW MANY land on each shard (the load distribution); the exact value text need
        // only be distinct. So set base_seq to a per-shard offset.
        st->base_seq = s * (pa.count + 1);
        st->next_seq = st->base_seq;
        states.push_back(std::move(st));
        clients[s]->reactor.spawn(pipe_run(states[s].get()));
    }

    auto max_now = [&]() -> core::Tick {
        core::Tick mx = 0;
        for (std::uint64_t s = 0; s < m; ++s) {
            const core::Tick n = clients[s]->reactor.now();
            if (n > mx) {
                mx = n;
            }
        }
        return mx;
    };
    const core::Tick t0 = max_now();
    const core::Tick wall_ns =
        t0 + static_cast<core::Tick>(pa.count) * 2'000'000 + 5'000'000'000;
    bool all_done = false;
    while (!all_done && max_now() < wall_ns) {
        all_done = true;
        for (std::uint64_t s = 0; s < m; ++s) {
            if (!states[s]->done) {
                all_done = false;
                clients[s]->reactor.run_until([&] { return states[s]->done; },
                                              clients[s]->reactor.now() + 1'000'000);
            }
        }
    }
    const core::Tick t1 = max_now();
    const double accept_elapsed_ms = static_cast<double>(t1 - t0) / 1'000'000.0;

    std::uint64_t accepted = 0;
    std::uint64_t replied = 0;
    bool any_fault = false;
    bool any_unfinished = false;
    std::vector<std::uint64_t> shard_target(m, 0);
    for (std::uint64_t s = 0; s < m; ++s) {
        accepted += states[s]->accepted;
        replied += states[s]->replied;
        shard_target[s] = states[s]->max_index;
        any_fault = any_fault || states[s]->fault;
        any_unfinished = any_unfinished || !states[s]->done;
    }
    const double accept_tput =
        accept_elapsed_ms > 0.0
            ? static_cast<double>(accepted) / (accept_elapsed_ms / 1000.0)
            : 0.0;

    // AGGREGATE COMMIT: poll each shard's CHEAP O(1) commit query until its commit_index
    // covers its own accepted target. The aggregate commit wall = accept wall + the
    // catch-up wall (commit lags accept; same approximation as pbench).
    std::vector<std::unique_ptr<Client>> pollers;
    pollers.reserve(m);
    std::uint64_t poller_id = kClientId + 1'000'000ULL;
    for (std::uint64_t s = 0; s < m; ++s) {
        pollers.push_back(std::make_unique<Client>(hosts[s], poller_id++));
    }
    const core::Tick catchup_wall =
        static_cast<core::Tick>(pa.count) * 4'000'000 + 10'000'000'000;
    auto poll_max_now = [&]() -> core::Tick {
        core::Tick mx = 0;
        for (auto& p : pollers) {
            if (p->ok && p->reactor.now() > mx) {
                mx = p->reactor.now();
            }
        }
        return mx;
    };
    const core::Tick poll_t0 = poll_max_now();
    const core::Tick poll_deadline = poll_t0 + catchup_wall;
    std::uint64_t shards_covered = 0;
    bool all_covered = false;
    while (!all_covered && poll_max_now() < poll_deadline) {
        shards_covered = 0;
        for (std::uint64_t s = 0; s < m; ++s) {
            if (!pollers[s]->ok) {
                continue;
            }
            if (shard_target[s] == 0) {  // no accepted load on this shard -> trivially done
                ++shards_covered;
                continue;
            }
            prod::AdminCommit ac;
            if (bench_commit(*pollers[s], ac) && ac.commit_index >= shard_target[s]) {
                ++shards_covered;
            }
        }
        if (shards_covered == m) {
            all_covered = true;
        }
    }
    const core::Tick poll_t1 = poll_max_now();
    const double commit_elapsed_ms =
        accept_elapsed_ms + static_cast<double>(poll_t1 - poll_t0) / 1'000'000.0;
    const double agg_commit_tput =
        (all_covered && commit_elapsed_ms > 0.0)
            ? static_cast<double>(accepted) / (commit_elapsed_ms / 1000.0)
            : 0.0;

    std::printf(
        "MBENCH shards=%llu count=%llu inflight=%llu value_bytes=%llu "
        "agg_commit_tput=%.1f commit_elapsed_ms=%.3f agg_accept_tput=%.1f "
        "accept_elapsed_ms=%.3f accepted=%llu replied=%llu shards_covered=%llu "
        "all_covered=%d fault=%d unfinished=%d\n",
        static_cast<unsigned long long>(m),
        static_cast<unsigned long long>(pa.count),
        static_cast<unsigned long long>(window),
        static_cast<unsigned long long>(pa.value_bytes), agg_commit_tput,
        commit_elapsed_ms, accept_tput, accept_elapsed_ms,
        static_cast<unsigned long long>(accepted),
        static_cast<unsigned long long>(replied),
        static_cast<unsigned long long>(shards_covered), all_covered ? 1 : 0,
        any_fault ? 1 : 0, any_unfinished ? 1 : 0);
    std::fflush(stdout);
    return (any_unfinished || !all_covered || accepted == 0) ? 1 : 0;
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

// Phase 10 OBSERVABILITY — scrape each host's METRICS and print the Prometheus text
// exposition verbatim to stdout (a scrape target a Prometheus server / a curl can read).
// On a host that does not reply / does not support METRICS, prints a comment line so the
// output stays valid Prometheus (comments begin with '#').
int cmd_metrics(const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        std::string text;
        const bool ok = do_metrics(port, text);
        if (ok) {
            std::fputs(text.c_str(), stdout);
        } else {
            std::printf("# scrape_failed host=%u (node down or METRICS unsupported)\n",
                        static_cast<unsigned>(port));
        }
        std::fflush(stdout);
    }
    return 0;
}

// S8.4 DURABLE CONFIRMATION — given an accepted {value, index}, decide whether it is
// COMMITTED on `port`: poll STATUS until commit_index >= index AND the entry at that
// 1-based index is STILL `value`. The committed-log digest STATUS returns is the FULL
// durable log; index `idx` lives at committed[idx-1]. Three terminating outcomes (all
// BOUNDED by the absolute `deadline` — never a spin):
//   * COMMITTED: commit_index covers idx AND committed[idx-1] == value  -> durable.
//   * OVERWRITTEN: commit_index covers idx but committed[idx-1] != value -> a NEW leader
//     overwrote this stale-leader entry (spec conflict rule). NOT durable; caller retries
//     with a fresh submit (a different index). We stop polling immediately (it can never
//     become ours again at this index).
//   * TIMEOUT: deadline elapsed before commit_index reached idx -> NOT durable (the entry
//     may yet commit, but we will NOT report success on an un-confirmed write).
// Returns true ONLY for COMMITTED. `port` is the ACCEPTING node (the leader that took the
// entry); its own commit_index advancing to idx means a QUORUM replicated+acked it.
enum class DurableResult : std::uint8_t { Committed, Overwritten, Timeout };

DurableResult confirm_durable(std::uint16_t port, const std::string& value,
                              std::uint64_t idx, core::Tick deadline_ns) {
    Client c(port);
    if (!c.ok) {
        return DurableResult::Timeout;
    }
    while (c.reactor.now() < deadline_ns) {
        prod::AdminStatus st;
        bool ok = false;
        bool done = false;
        c.reactor.spawn(status_rpc(c.net(), c.admin_ep(), &st, &ok, &done));
        c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
        if (done && ok && st.commit_index >= idx) {
            // commit_index covers our index: the entry there is FINAL (committed entries
            // never change). Is it still ours?
            if (idx >= 1 && idx <= st.committed.size() && st.committed[idx - 1] == value) {
                return DurableResult::Committed;
            }
            return DurableResult::Overwritten;  // a new leader overwrote our slot.
        }
        // not yet committed to our index: poll again (bounded by `deadline_ns`).
    }
    return DurableResult::Timeout;
}

// LEADER-FIND submit: try each host in order; the LEADER accepts (accepted=1), a
// follower replies NotLeader (we try the next host).
//
// S8.4 DURABLE-BY-DEFAULT: a successful response now means the write is COMMITTED (durable
// on a quorum), not merely accepted (appended by a possibly-stale leader). After a host
// accepts {term,index}, we POLL that node (confirm_durable) until commit_index covers the
// index AND the entry there is STILL our value — only THEN print durable=1. On
// timeout/overwrite/leader-change we print durable=0 (caller retries with a fresh submit).
// `await_durable=false` (--no-await) reverts to the OLD accept-only behaviour (durable
// omitted) for the load harness's accept measurement — but the DEFAULT awaits commit.
int cmd_submit(const std::string& value, const std::vector<std::uint16_t>& hosts,
               bool await_durable) {
    for (std::uint16_t port : hosts) {
        SubmitOutcome so;
        const bool replied = do_submit(port, value, so);
        if (replied && so.accepted) {
            if (!await_durable) {
                // accept-only mode: report the append (NOT a durability claim).
                std::printf("SUBMIT value=%s accepted=1 port=%u term=%llu index=%llu "
                            "leader_hint=0\n",
                            value.c_str(), port,
                            static_cast<unsigned long long>(so.term),
                            static_cast<unsigned long long>(so.index));
                std::fflush(stdout);
                return 0;
            }
            // Durable-by-default: confirm the accepted entry COMMITTED (commit_index
            // covers it AND the value is still ours) before reporting success.
            Client probe(port);  // a stable now() baseline for the absolute deadline.
            const core::Tick deadline = (probe.ok ? probe.reactor.now() : 0) + kDurableWallNs;
            const DurableResult dr = confirm_durable(port, value, so.index, deadline);
            const bool durable = (dr == DurableResult::Committed);
            std::printf("SUBMIT value=%s accepted=1 durable=%d port=%u term=%llu "
                        "index=%llu leader_hint=0\n",
                        value.c_str(), durable ? 1 : 0, port,
                        static_cast<unsigned long long>(so.term),
                        static_cast<unsigned long long>(so.index));
            std::fflush(stdout);
            return durable ? 0 : 1;  // not-durable -> nonzero (caller retries).
        }
        // NotLeader or no reply: try the next host (the leader-find retry loop).
    }
    std::printf("SUBMIT value=%s accepted=0 durable=0 port=0 term=0 index=0 "
                "leader_hint=0\n",
                value.c_str());
    std::fflush(stdout);
    return 1;  // no host accepted (caller retries later)
}

// ============================================================================
// Phase 9 S9.4 — REPLICATED-SHARD LEADER-ROUTED CLIENT (`rsubmit` / `rstatus`).
// ============================================================================
// Each shard is an N-node Raft group spread across N PROCESSES; a shard's LEADER can be
// on ANY of the N processes. So routing a key is two steps: (1) key -> shard s (hash%M),
// (2) leader-FIND among shard s's N admin ports (reuse the NotLeader-retry from `submit`,
// scoped to ONE shard's N ports). The client computes each shard's N admin ports from the
// SAME deterministic port scheme the daemon uses, so it needs only --shards M --procs N
// --base-port B (no explicit host list).
//
//   admin_port(proc p in 1..N, shard s in 0..M-1) = base + (p-1)*(2*M) + M + s
//
// This mirrors prod::shard_detail::repl_admin_port — kept in sync by construction (same
// arithmetic). rsubmit is DURABLE-by-default (await commit on the accepting leader, same
// as submit). rstatus prints every replica's STATUS grouped by shard (the HA test reads
// it to confirm each shard's group has a live leader + agreeing committed logs).

struct ReplTopo {
    std::uint64_t shards = 0;   // M
    std::uint64_t procs = 0;    // N
    std::uint16_t base = 0;     // global base port
};

std::uint16_t repl_admin_port(const ReplTopo& t, std::uint64_t proc_1based,
                              std::uint64_t shard) {
    const std::uint64_t stride = 2 * t.shards;
    return static_cast<std::uint16_t>(t.base + (proc_1based - 1) * stride + t.shards +
                                      shard);
}

// The N admin ports of shard s's replica group (one per process).
std::vector<std::uint16_t> shard_admin_ports(const ReplTopo& t, std::uint64_t shard) {
    std::vector<std::uint16_t> ports;
    ports.reserve(t.procs);
    for (std::uint64_t p = 1; p <= t.procs; ++p) {
        ports.push_back(repl_admin_port(t, p, shard));
    }
    return ports;
}

// rsubmit: route a key to its shard, then DURABLE leader-find submit among that shard's N
// admin ports. Prints one SUBMIT line; exit 0 only if the write COMMITTED (durable).
// Bounded leader-find retry (an election in flight resolves within the retry budget).
int cmd_rsubmit(const std::string& key, const std::string& value, const ReplTopo& t) {
    const std::uint64_t shard = fnv1a(key) % t.shards;
    const std::vector<std::uint16_t> ports = shard_admin_ports(t, shard);
    std::printf("RROUTE key=%s shard=%llu ports=", key.c_str(),
                static_cast<unsigned long long>(shard));
    for (std::size_t i = 0; i < ports.size(); ++i) {
        std::printf("%s%u", i == 0 ? "" : ",", ports[i]);
    }
    std::printf("\n");
    std::fflush(stdout);
    // Leader-find with bounded retry across the shard's N ports (handles an election in
    // flight). Reuses do_submit (the NotLeader-retry round-trip) + confirm_durable.
    for (int attempt = 0; attempt < 40; ++attempt) {
        for (std::uint16_t port : ports) {
            SubmitOutcome so;
            const bool replied = do_submit(port, value, so);
            if (replied && so.accepted) {
                Client probe(port);
                const core::Tick deadline =
                    (probe.ok ? probe.reactor.now() : 0) + kDurableWallNs;
                const DurableResult dr =
                    confirm_durable(port, value, so.index, deadline);
                const bool durable = (dr == DurableResult::Committed);
                std::printf("SUBMIT key=%s value=%s accepted=1 durable=%d shard=%llu "
                            "port=%u term=%llu index=%llu\n",
                            key.c_str(), value.c_str(), durable ? 1 : 0,
                            static_cast<unsigned long long>(shard), port,
                            static_cast<unsigned long long>(so.term),
                            static_cast<unsigned long long>(so.index));
                std::fflush(stdout);
                if (durable) {
                    return 0;
                }
                // not durable (overwrite/timeout): retry the leader-find loop.
            }
            // NotLeader / no reply: try the next process's replica.
        }
    }
    std::printf("SUBMIT key=%s value=%s accepted=0 durable=0 shard=%llu port=0\n",
                key.c_str(), value.c_str(),
                static_cast<unsigned long long>(shard));
    std::fflush(stdout);
    return 1;
}

// rstatus: print every replica's STATUS, grouped by shard. One RSTATUS line per (shard,
// proc) so the HA test can see each shard's group (who is leader, committed-log digest).
int cmd_rstatus(const ReplTopo& t) {
    for (std::uint64_t s = 0; s < t.shards; ++s) {
        for (std::uint64_t p = 1; p <= t.procs; ++p) {
            const std::uint16_t port = repl_admin_port(t, p, s);
            prod::AdminStatus st;
            const bool ok = do_status(port, st);
            std::printf("RSTATUS shard=%llu proc=%llu port=%u ok=%d role=%u term=%llu "
                        "commit=%llu log=",
                        static_cast<unsigned long long>(s),
                        static_cast<unsigned long long>(p), port, ok ? 1 : 0,
                        static_cast<unsigned>(st.role),
                        static_cast<unsigned long long>(st.term),
                        static_cast<unsigned long long>(st.commit_index));
            for (std::size_t i = 0; i < st.committed.size(); ++i) {
                std::printf("%s%s", i == 0 ? "" : ",", st.committed[i].c_str());
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    return 0;
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
            "       lockstep_admin metrics --host PORT [--host PORT ...] "
            "(scrape Prometheus metrics)\n"
            "       lockstep_admin submit VALUE [--no-await] --host PORT "
            "[--host PORT ...]\n"
            "       lockstep_admin bench --count N [--value-bytes B] "
            "[--commit-samples S] --host PORT [--host PORT ...]\n"
            "       lockstep_admin pbench --count N [--inflight K] [--conns C] "
            "[--value-bytes B] --host PORT [--host PORT ...]\n"
            "       lockstep_admin mbench --count N [--inflight K] [--value-bytes B] "
            "--host SHARD0_PORT --host SHARD1_PORT ... (multi-shard key-routed load)\n"
            "       lockstep_admin rsubmit KEY VALUE --shards M --procs N --base-port B "
            "(replicated-shard: route key->shard, leader-find among the shard's N "
            "replicas, durable submit)\n"
            "       lockstep_admin rstatus --shards M --procs N --base-port B "
            "(replicated-shard: every replica's STATUS grouped by shard)\n");
        return 2;
    }

    const std::string verb = argv[1];
    std::vector<std::uint16_t> hosts;
    std::string value;
    std::string key;          // S9.4 rsubmit routing key
    ReplTopo topo;            // S9.4 replicated-shard topology (--shards/--procs/--base-port)
    BenchArgs ba;
    PipeArgs pa;
    bool await_durable = true;  // S8.4: submit is DURABLE (await commit) by default.
    int i = 2;

    if (verb == "submit") {
        if (argc < 3) {
            std::fprintf(stderr, "lockstep_admin submit: missing VALUE\n");
            return 2;
        }
        value = argv[2];
        i = 3;
    } else if (verb == "rsubmit") {
        if (argc < 4) {
            std::fprintf(stderr, "lockstep_admin rsubmit: missing KEY VALUE\n");
            return 2;
        }
        key = argv[2];
        value = argv[3];
        i = 4;
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
        } else if (std::strcmp(argv[i], "--no-await") == 0) {
            // Accept-only submit (load-harness accept measurement); DEFAULT is durable.
            await_durable = false;
        } else if (std::strcmp(argv[i], "--shards") == 0 && i + 1 < argc) {
            topo.shards = parse_u64_opt(argv[i + 1], topo.shards);
            ++i;
        } else if (std::strcmp(argv[i], "--procs") == 0 && i + 1 < argc) {
            topo.procs = parse_u64_opt(argv[i + 1], topo.procs);
            ++i;
        } else if (std::strcmp(argv[i], "--base-port") == 0 && i + 1 < argc) {
            topo.base = parse_port(argv[i + 1]);
            ++i;
        }
    }

    // Phase 9 S9.4 replicated-shard verbs: routed by topology, not an explicit --host list.
    if (verb == "rsubmit" || verb == "rstatus") {
        if (topo.shards == 0 || topo.procs == 0 || topo.base == 0) {
            std::fprintf(stderr,
                         "lockstep_admin %s: requires --shards M --procs N --base-port B\n",
                         verb.c_str());
            return 2;
        }
        if (verb == "rsubmit") {
            return cmd_rsubmit(key, value, topo);
        }
        return cmd_rstatus(topo);
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
    if (verb == "metrics") {
        return cmd_metrics(hosts);
    }
    if (verb == "submit") {
        return cmd_submit(value, hosts, await_durable);
    }
    if (verb == "bench") {
        return cmd_bench(ba, hosts);
    }
    if (verb == "pbench") {
        return cmd_pbench(pa, hosts);
    }
    if (verb == "mbench") {
        return cmd_mbench(pa, hosts);
    }
    std::fprintf(stderr, "lockstep_admin: unknown verb '%s'\n", verb.c_str());
    return 2;
}
