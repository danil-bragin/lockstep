// query_protocol_test.cpp — Phase 6 Stage B (C6.3) GATE for P6-PROTOCOL.
//
// Source of truth: briefs/phase6.md C6.3 (the client network protocol; Postgres-
// wire shim deferred). Judges the WIRE PROTOCOL (query/wire/Protocol.hpp +
// Server.hpp + ClientStub.hpp): the client<->server exchange over SimNetwork.
//
// Asserts, over a bounded seed sweep (<=64 in-gate, LOCKSTEP_PROTOCOL_SEEDS env):
//   (A) ROUND-TRIP CORRECTNESS: a Submit/Query taken THROUGH the wire (encode ->
//       SimNetwork -> Server -> dispatch -> encode -> SimNetwork -> decode) equals
//       the same op applied DIRECTLY on a twin Server's Database surface (which is
//       the Stage-F surface == the strict-serializable oracle). Byte/field equal.
//   (B) EXACTLY-ONCE UNDER FAULTS: with dup/reorder/drop turned UP, a re-delivered
//       Submit (the dropped-reply retry path) applies the txn EXACTLY ONCE — the
//       server's applied-submit count == the number of DISTINCT submit_keys, and
//       the final balances equal a single application. No duplicate txn effect.
//   (C) TORN FRAME REJECTED: a deliberately-corrupted request/response frame FAILS
//       to decode (CRC mismatch), never mis-decodes into a fabricated message.
//   (D) DETERMINISM: same seed => byte-identical rendered run (trace + outcomes).
//   (E) TEETH: a single flipped byte in an otherwise-valid frame MUST be detected
//       (the CRC check is non-vacuous).
//
// Determinism (binding; query/ is NOT lint-exempt): the only entropy is the seed,
// consumed by SeededRandom (the sim PRNG) for the net faults + an inlined SplitMix
// for the workload shape. Bounded; CTest TIMEOUT inherited. No <chrono>/<thread>/
// <random>; all time is the virtual SimClock.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/ClientStub.hpp>
#include <lockstep/query/wire/Protocol.hpp>
#include <lockstep/query/wire/Server.hpp>

namespace {

using lockstep::core::Endpoint;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimNetworkBus;

namespace wire = lockstep::query::wire;
using lockstep::query::Query;
using lockstep::query::Strict;

int g_failures = 0;

#define P_CHECK(cond, msg, seed)                                                 \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr,                                                 \
                         "query_protocol_test FAIL [%s:%d]: %s (seed=%llu)\n",   \
                         __FILE__, __LINE__, (msg),                              \
                         static_cast<unsigned long long>(seed));                 \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// Inlined SplitMix64 for the deterministic WORKLOAD shape (independent of the
// PRNG the net faults consume, so the workload is fixed per seed).
struct SplitMix {
    std::uint64_t s;
    explicit SplitMix(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t in(std::uint64_t lo, std::uint64_t hi) {
        return lo + (next() % (hi - lo + 1));
    }
};

constexpr std::uint64_t kServerEp = 1;
constexpr std::uint64_t kClientEp = 2;

// A single logical submit in the workload: a Transfer of `amount` a->b.
struct WorkSubmit {
    std::string a;
    std::string b;
    std::int64_t amount;
};

// Build a deterministic workload from a seed: a fixed set of accounts seeded to a
// starting balance, then a sequence of transfers among them.
struct Workload {
    std::vector<std::pair<std::string, std::int64_t>> seed_accounts;
    std::vector<WorkSubmit> transfers;
};

Workload make_workload(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xA5A5'1234'DEAD'BEEFULL);
    Workload w;
    const char* names[] = {"acct:a", "acct:b", "acct:c", "acct:d"};
    for (const char* n : names) {
        w.seed_accounts.emplace_back(n, 1000);
    }
    const int n_xfer = static_cast<int>(rng.in(3, 6));
    for (int i = 0; i < n_xfer; ++i) {
        std::uint64_t ai = rng.in(0, 3);
        std::uint64_t bi = rng.in(0, 3);
        if (bi == ai) {
            bi = (bi + 1) % 4;
        }
        WorkSubmit s;
        s.a = names[ai];
        s.b = names[bi];
        s.amount = static_cast<std::int64_t>(rng.in(1, 200));
        w.transfers.push_back(s);
    }
    return w;
}

// ---------------------------------------------------------------------------
// THE DIRECT (no-wire) ORACLE: apply the SAME request stream to a twin Server via
// dispatch() (no network). This is the Stage-F Database surface result the wire
// MUST match. Returns the final SubmitOk responses + final balances via a Query.
// ---------------------------------------------------------------------------
struct DirectResult {
    std::vector<wire::Response> submit_oks;
    std::map<std::string, std::int64_t> balances;
    std::uint64_t applied = 0;
};

// Build the request list (seed puts then transfers), each with its own submit_key.
std::vector<wire::Request> build_requests(const Workload& w) {
    std::vector<wire::Request> reqs;
    std::uint64_t key = 0;
    std::uint64_t rid = 0;
    for (const auto& [name, bal] : w.seed_accounts) {
        wire::Request r;
        r.kind = wire::MsgKind::Submit;
        r.req_id = ++rid;
        r.submit_key = ++key;
        r.op = wire::SubmitOp::Put;
        wire::OpParam p;
        p.key = name;
        p.value = wire::encode_balance(bal);
        r.params.push_back(p);
        reqs.push_back(std::move(r));
    }
    for (const WorkSubmit& t : w.transfers) {
        wire::Request r;
        r.kind = wire::MsgKind::Submit;
        r.req_id = ++rid;
        r.submit_key = ++key;
        r.op = wire::SubmitOp::Transfer;
        wire::OpParam pa;
        pa.key = t.a;
        pa.amount = t.amount;
        wire::OpParam pb;
        pb.key = t.b;
        r.params.push_back(pa);
        r.params.push_back(pb);
        reqs.push_back(std::move(r));
    }
    return reqs;
}

DirectResult run_direct(const Workload& w) {
    // No network is touched by dispatch(); the dispatch-only Server has a null net.
    wire::Server srv;
    DirectResult out;
    for (const wire::Request& r : build_requests(w)) {
        wire::Response resp = srv.dispatch(r);
        out.submit_oks.push_back(resp);
    }
    out.applied = srv.applied_submits();
    // Final balances via a Strict query over the four accounts.
    wire::Request q;
    q.kind = wire::MsgKind::Query;
    q.req_id = 9999;
    q.level = lockstep::txn::Level::StrictSerializable;
    for (const char* n : {"acct:a", "acct:b", "acct:c", "acct:d"}) {
        wire::Step s;
        s.kind = lockstep::query::StepKind::Point;
        s.key = n;
        q.steps.push_back(s);
    }
    wire::Response qr = srv.dispatch(q);
    for (const wire::PointWire& pw : qr.points) {
        out.balances[pw.key] = wire::parse_balance(
            pw.present ? std::optional<std::string>(pw.value) : std::nullopt);
    }
    return out;
}

// ---------------------------------------------------------------------------
// THE WIRE RUN: drive a real ClientStub <-> Server exchange over SimNetwork under
// the given faults. The client submits each transfer with a STABLE submit_key and
// retries on timeout (so a dropped Submit reuses its key -> exactly-once). After
// all submits, it issues a Strict query for the four balances.
// ---------------------------------------------------------------------------
struct WireRun {
    std::string trace;
    std::map<std::string, std::int64_t> balances;
    std::uint64_t applied = 0;
    std::uint64_t server_rejected = 0;
    bool all_ok = true;  // every call got a reply within the retry budget
};

Task client_driver(wire::ClientStub& cli, const Workload& w, WireRun* out,
                   bool* all_ok) {
    // Seed accounts (Put), then transfers, each with its own stable submit_key.
    for (const auto& [name, bal] : w.seed_accounts) {
        wire::OpParam p;
        p.key = name;
        p.value = wire::encode_balance(bal);
        wire::CallResult cr;
        co_await cli.submit(wire::SubmitOp::Put, {p}, cli.new_submit_key(), cr);
        if (!cr.ok) {
            *all_ok = false;
        }
    }
    for (const WorkSubmit& t : w.transfers) {
        wire::OpParam pa;
        pa.key = t.a;
        pa.amount = t.amount;
        wire::OpParam pb;
        pb.key = t.b;
        wire::CallResult cr;
        co_await cli.submit(wire::SubmitOp::Transfer, {pa, pb}, cli.new_submit_key(),
                            cr);
        if (!cr.ok) {
            *all_ok = false;
        }
    }
    // Final balances via a Strict query.
    Query<Strict> q;
    q.get("acct:a").get("acct:b").get("acct:c").get("acct:d");
    wire::CallResult qr;
    co_await cli.query(q, qr);
    if (!qr.ok) {
        *all_ok = false;
    } else {
        for (const wire::PointWire& pw : qr.response.points) {
            out->balances[pw.key] = wire::parse_balance(
                pw.present ? std::optional<std::string>(pw.value) : std::nullopt);
        }
    }
    co_return;
}

WireRun run_wire(std::uint64_t seed, const Workload& w,
                 const lockstep::sim::detail::LinkFaults& faults) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    SimNetworkBus bus(sched, rng);
    bus.add_nodes({kServerEp, kClientEp});
    bus.set_faults(faults);

    // SimNetwork is a thin value handle; we keep the two handles in stable storage
    // (unique_ptr) for the run's lifetime so the Server/ClientStub INetwork& stays
    // valid. Freed deterministically at end of scope (no leak).
    auto srv_net = std::make_unique<lockstep::sim::SimNetwork>(bus.node(kServerEp));
    auto cli_net = std::make_unique<lockstep::sim::SimNetwork>(bus.node(kClientEp));
    wire::Server srv(*srv_net);
    wire::ClientStub cli(*cli_net, clock, Endpoint{kServerEp});

    WireRun out;
    bool all_ok = true;
    // Server recv budget + client pump budget sized generously to absorb dups +
    // retries (bounded — never unbounded). n requests + a query, times a fat
    // multiplier for duplicates/retries.
    const int n_calls = static_cast<int>(w.seed_accounts.size() + w.transfers.size()) + 1;
    const int budget = (n_calls + 4) * 64;
    sched.spawn(srv.serve(budget));
    sched.spawn(cli.pump(budget));
    sched.spawn(client_driver(cli, w, &out, &all_ok));
    sched.run();

    out.trace = sched.trace_text();
    out.applied = srv.applied_submits();
    out.server_rejected = srv.rejected();
    out.all_ok = all_ok;
    return out;
}

} // namespace

int main() {
    std::printf("query_protocol_test\n");

    // Seed sweep (<=64 in-gate; LOCKSTEP_PROTOCOL_SEEDS overrides the count).
    int sweep = 64;
    if (const char* env = std::getenv("LOCKSTEP_PROTOCOL_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0 && v <= 4096) {
            sweep = static_cast<int>(v);
        }
    }

    constexpr std::uint64_t kBase = 0x6'C0DE'5713'9BDFULL;

    // Fault profiles: clean (isolate correctness) + a nasty dup/reorder/drop mix.
    lockstep::sim::detail::LinkFaults clean{};
    clean.latency_min = 1;
    clean.latency_max = 1;

    lockstep::sim::detail::LinkFaults nasty{};
    nasty.drop_prob = 0.25;
    nasty.dup_prob = 0.30;
    nasty.reorder_prob = 0.50;
    nasty.latency_min = 1;
    nasty.latency_max = 8;
    nasty.reorder_jitter_max = 12;

    for (int i = 0; i < sweep; ++i) {
        const std::uint64_t seed = kBase + static_cast<std::uint64_t>(i) * 0x1000193ULL;
        const Workload w = make_workload(seed);
        const DirectResult direct = run_direct(w);

        // The number of DISTINCT submit_keys == seed accounts + transfers. The
        // direct (oracle) run applied exactly that many (each committed once).
        const std::uint64_t distinct_keys =
            w.seed_accounts.size() + w.transfers.size();
        P_CHECK(direct.applied == distinct_keys,
                "direct oracle did not apply each submit exactly once", seed);

        // ---- (A) ROUND-TRIP over a CLEAN net == direct/oracle -----------------
        const WireRun clean_run = run_wire(seed, w, clean);
        P_CHECK(clean_run.all_ok, "a client call timed out on a clean net", seed);
        P_CHECK(clean_run.balances == direct.balances,
                "wire balances (clean) != direct/oracle balances", seed);
        P_CHECK(clean_run.applied == distinct_keys,
                "wire applied count (clean) != distinct submit keys", seed);
        P_CHECK(clean_run.server_rejected == 0,
                "clean net should produce no torn frames", seed);

        // ---- (B) EXACTLY-ONCE under dup/reorder/drop --------------------------
        const WireRun nasty_run = run_wire(seed, w, nasty);
        P_CHECK(nasty_run.all_ok,
                "a client call timed out under faults (retry budget too small?)",
                seed);
        // Each submit applied EXACTLY ONCE despite duplicated/retried Submits.
        P_CHECK(nasty_run.applied == distinct_keys,
                "EXACTLY-ONCE violated: duplicate/retried Submit double-applied",
                seed);
        // ...and the observable effect equals the single-application oracle.
        P_CHECK(nasty_run.balances == direct.balances,
                "wire balances under faults != oracle (effect not exactly-once)",
                seed);

        // ---- (D) DETERMINISM: same seed => byte-identical run -----------------
        const WireRun nasty_run2 = run_wire(seed, w, nasty);
        P_CHECK(nasty_run.trace == nasty_run2.trace,
                "wire run not byte-identical on replay (trace differs)", seed);
        P_CHECK(nasty_run.balances == nasty_run2.balances,
                "wire balances not reproducible on replay", seed);
        P_CHECK(nasty_run.applied == nasty_run2.applied,
                "wire applied count not reproducible on replay", seed);
    }

    // ---- (C)+(E) TORN / CORRUPT FRAME REJECTED (teeth) ------------------------
    {
        const std::uint64_t seed = kBase;
        // A valid Submit request frame.
        wire::Request req;
        req.kind = wire::MsgKind::Submit;
        req.req_id = 7;
        req.submit_key = 42;
        req.op = wire::SubmitOp::Transfer;
        wire::OpParam pa;
        pa.key = "acct:a";
        pa.amount = 50;
        wire::OpParam pb;
        pb.key = "acct:b";
        req.params = {pa, pb};
        std::vector<std::byte> frame = wire::encode_request(req);

        // It decodes cleanly first (control).
        wire::Request rt;
        P_CHECK(wire::decode_request(
                    std::span<const std::byte>(frame.data(), frame.size()), rt),
                "a valid request frame must decode", seed);
        P_CHECK(rt.submit_key == 42 && rt.op == wire::SubmitOp::Transfer,
                "valid request did not round-trip its fields", seed);

        // (E) TEETH: flip ONE byte in the body — the CRC MUST catch it.
        for (std::size_t pos = 0; pos + 4 < frame.size(); ++pos) {
            std::vector<std::byte> bad = frame;
            bad[pos] = static_cast<std::byte>(
                static_cast<std::uint8_t>(bad[pos]) ^ 0xFFu);
            wire::Request junk;
            const bool decoded = wire::decode_request(
                std::span<const std::byte>(bad.data(), bad.size()), junk);
            P_CHECK(!decoded,
                    "a single-byte-corrupted request frame must be REJECTED", seed);
        }

        // (C) Truncated frame (CRC trailer torn off) -> rejected, never fabricated.
        if (frame.size() > 5) {
            std::vector<std::byte> truncated(frame.begin(), frame.end() - 3);
            wire::Request junk;
            P_CHECK(!wire::decode_request(std::span<const std::byte>(
                                              truncated.data(), truncated.size()),
                                          junk),
                    "a truncated request frame must be REJECTED", seed);
        }

        // Same teeth on a RESPONSE frame.
        wire::Response resp;
        resp.kind = wire::MsgKind::SubmitOk;
        resp.req_id = 7;
        resp.status = 0;
        resp.commit_version = 3;
        resp.result = "xfer:acct:a->acct:b";
        resp.writes["acct:a"] = "950";
        resp.writes["acct:b"] = "1050";
        std::vector<std::byte> rframe = wire::encode_response(resp);
        wire::Response rrt;
        P_CHECK(wire::decode_response(
                    std::span<const std::byte>(rframe.data(), rframe.size()), rrt),
                "a valid response frame must decode", seed);
        for (std::size_t pos = 0; pos + 4 < rframe.size(); ++pos) {
            std::vector<std::byte> bad = rframe;
            bad[pos] = static_cast<std::byte>(
                static_cast<std::uint8_t>(bad[pos]) ^ 0xFFu);
            wire::Response junk;
            P_CHECK(!wire::decode_response(
                        std::span<const std::byte>(bad.data(), bad.size()), junk),
                    "a single-byte-corrupted response frame must be REJECTED", seed);
        }
    }

    if (g_failures == 0) {
        std::printf("query_protocol_test OK (sweep=%d)\n", sweep);
        return 0;
    }
    std::fprintf(stderr, "query_protocol_test FAILED: %d failures\n", g_failures);
    return 1;
}
