// prod_network_test.cpp — Phase 7 S4b driver. Proves the PRODUCTION INetwork
// provider (prod::ProdNetwork — real TCP over loopback, registered on the S4a
// ProdReactor's epoll loop) satisfies the frozen INetwork contract on REAL sockets:
//
//   (1) tier-A INetwork UNIVERSAL CONTRACT — the S1 checks, with ASSERTIONS
//       IDENTICAL to conformance::universal::check_network_contract_on, but driven
//       on the ProdReactor (epoll) instead of the sim Scheduler, with TWO nodes on
//       loopback in one process: local() stable; send accepted on a live link; recv
//       delivers the right from+payload; per-link delivery in SEND ORDER (a TCP
//       single-connection guarantee — resolving the S1 per-link-order flag #3 for
//       the TCP-scoped prod Network). V-PROD-CONTRACT.
//   (2) INTEGRATION: two nodes exchange a sequence of messages; then a connection
//       DROP (close one side) + RECONNECT, and messages flow again. Asserts no
//       corruption, in-order per connection, reconnect recovers. Bounded (small
//       counts + an absolute reactor deadline so a lost frame can NEVER hang).
//   (3) NETWORK RECORD -> REPLAY byte-identical (V-PROD-REPLAY): a RecordingNetwork
//       logs each recv'd Message; a ReplayNetwork reproduces those recvs in order;
//       the observed recv stream is byte-identical across record and replay.
//
// LINUX-ONLY: this TU is built only on Linux (tests/CMakeLists.txt guards it with
// if(UNIX AND NOT APPLE)); ProdNetwork/ProdReactor are #ifdef __linux__. The macOS
// host never sees it and stays green.
//
// NON-provider code (a test) -> the forbidden-call lint scans it. It touches NO
// socket/epoll syscall of its own: all real network plumbing stays inside
// providers/prod/. The test uses ONLY core::INetwork + the reactor's run loop.

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ReplayTrace.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;

// Small payload helper (mirrors conformance::payload): n bytes from a base.
std::vector<std::byte> payload(std::size_t n, std::uint8_t base) {
    std::vector<std::byte> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back(static_cast<std::byte>(base + static_cast<std::uint8_t>(i)));
    }
    return v;
}
std::span<const std::byte> view_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

// 50 ms absolute reactor deadline guard: every networked run TERMINATES even if a
// frame is lost / a socket half-opens. In practice loopback completes in << 1 ms.
constexpr core::Tick kWallNs = 50'000'000;

// ===========================================================================
// (1) UNIVERSAL INetwork CONTRACT — assertions identical to S1, on prod TCP.
// ===========================================================================
struct ConfResult {
    bool local_stable = false;
    bool send_accepted = false;
    bool recv_count_ok = false;
    bool from_and_payload_ok = false;
    bool per_link_order = false;
};

struct ConfState {
    bool sender_done = false;
    bool recv_done = false;
    std::vector<core::Error> send_errs{};
    std::vector<std::string> got_texts{};
    std::vector<std::uint64_t> got_from{};
};

constexpr int kMsgs = 8;

core::Task conf_sender(core::INetwork* net, core::Endpoint to, ConfState* s) {
    for (int i = 0; i < kMsgs; ++i) {
        std::vector<std::byte> p = payload(4, static_cast<std::uint8_t>('A' + i));
        core::Error e = co_await net->send(to, view_of(p));
        s->send_errs.push_back(e);
    }
    s->sender_done = true;
    co_return;
}

core::Task conf_receiver(core::INetwork* net, ConfState* s) {
    for (int i = 0; i < kMsgs; ++i) {
        core::Message m = co_await net->recv();
        std::string text;
        for (std::byte byte : m.payload) {
            text.push_back(static_cast<char>(byte));
        }
        s->got_texts.push_back(text);
        s->got_from.push_back(m.from.id);
    }
    s->recv_done = true;
    co_return;
}

ConfResult run_conformance() {
    ConfResult r;
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        return r;
    }
    prod::ProdNetworkBus bus(reactor);
    bus.add_node(0);
    bus.add_node(1);
    core::INetwork* n0 = bus.node(0);
    core::INetwork* n1 = bus.node(1);

    // local() stable (identical to S1's network/local-stable).
    {
        const core::Endpoint a = n0->local();
        const core::Endpoint b = n0->local();
        r.local_stable = (a == b) && a.id == 0;
    }

    ConfState st;
    reactor.spawn(conf_receiver(n1, &st));
    reactor.spawn(conf_sender(n0, core::Endpoint{1}, &st));
    reactor.run_until([&] { return st.sender_done && st.recv_done; },
                      reactor.now() + kWallNs);

    // send accepted on a live link (identical to S1).
    bool all_accepted = st.send_errs.size() == static_cast<std::size_t>(kMsgs);
    for (const core::Error& e : st.send_errs) {
        if (e) {
            all_accepted = false;
        }
    }
    r.send_accepted = st.sender_done && all_accepted;

    // recv delivers kMsgs (identical to S1).
    r.recv_count_ok =
        st.recv_done && st.got_texts.size() == static_cast<std::size_t>(kMsgs);

    // from is the sender (0) and payloads are the sent bytes (identical to S1).
    bool from_ok = true;
    bool payload_ok = st.got_texts.size() == static_cast<std::size_t>(kMsgs);
    for (std::uint64_t f : st.got_from) {
        if (f != 0) {
            from_ok = false;
        }
    }
    for (std::size_t i = 0; i < st.got_texts.size(); ++i) {
        std::string want(4, '\0');
        want[0] = static_cast<char>('A' + static_cast<int>(i));
        want[1] = static_cast<char>('A' + static_cast<int>(i) + 1);
        want[2] = static_cast<char>('A' + static_cast<int>(i) + 2);
        want[3] = static_cast<char>('A' + static_cast<int>(i) + 3);
        if (st.got_texts[i] != want) {
            payload_ok = false;
        }
    }
    r.from_and_payload_ok = from_ok && payload_ok;

    // per-link order: first byte ascends 'A','B','C',... (TCP single-conn order).
    bool ordered = st.got_texts.size() == static_cast<std::size_t>(kMsgs);
    for (std::size_t i = 0; i < st.got_texts.size(); ++i) {
        if (st.got_texts[i].empty() ||
            st.got_texts[i][0] != static_cast<char>('A' + static_cast<int>(i))) {
            ordered = false;
        }
    }
    r.per_link_order = ordered;
    return r;
}

// ===========================================================================
// (2) INTEGRATION — exchange, DROP, RECONNECT, exchange again.
// ===========================================================================
struct IntegResult {
    bool phase1_ok = false;  // first batch delivered, in order, correct bytes
    bool phase2_ok = false;  // after drop+reconnect, second batch delivered in order
    bool no_corruption = true;
    int phase1_recv = 0;
    int phase2_recv = 0;
};

// Receiver that drains a fixed count, appending (from,firstbyte) order witnesses.
struct DrainState {
    int want = 0;
    int got = 0;
    bool done = false;
    bool order_ok = true;
    bool bytes_ok = true;
    std::uint8_t expect_first = 0; // first byte of msg #got should ascend from here
    std::uint64_t want_from = 0;
};

core::Task drainer(core::INetwork* net, DrainState* s) {
    for (int i = 0; i < s->want; ++i) {
        core::Message m = co_await net->recv();
        if (m.from.id != s->want_from) {
            s->order_ok = false;
        }
        if (m.payload.empty() ||
            std::to_integer<unsigned char>(m.payload[0]) !=
                static_cast<unsigned char>(s->expect_first + i)) {
            s->order_ok = false;
        }
        // bytes_ok: payload is a 4-byte ascending run from its first byte.
        if (m.payload.size() != 4) {
            s->bytes_ok = false;
        } else {
            for (std::size_t k = 0; k < 4; ++k) {
                if (std::to_integer<unsigned char>(m.payload[k]) !=
                    static_cast<unsigned char>(s->expect_first + i + static_cast<int>(k))) {
                    s->bytes_ok = false;
                }
            }
        }
        ++s->got;
    }
    s->done = true;
    co_return;
}

struct PusherState {
    bool done = false;
};
core::Task pusher(core::INetwork* net, core::Endpoint to, std::uint8_t base, int count,
                  PusherState* s) {
    for (int i = 0; i < count; ++i) {
        std::vector<std::byte> p = payload(4, static_cast<std::uint8_t>(base + i));
        co_await net->send(to, view_of(p));
    }
    s->done = true;
    co_return;
}

IntegResult run_integration() {
    IntegResult r;
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        return r;
    }
    prod::ProdNetworkBus bus(reactor);
    bus.add_node(10);
    bus.add_node(20);
    prod::ProdNetwork* a = bus.node(10);
    prod::ProdNetwork* b = bus.node(20);

    // --- phase 1: a -> b, 5 messages 'A'.. in order ---
    {
        constexpr int kN = 5;
        DrainState ds;
        ds.want = kN;
        ds.want_from = 10;
        ds.expect_first = 'A';
        PusherState ps;
        reactor.spawn(drainer(b, &ds));
        reactor.spawn(pusher(a, core::Endpoint{20}, 'A', kN, &ps));
        reactor.run_until([&] { return ds.done && ps.done; }, reactor.now() + kWallNs);
        r.phase1_recv = ds.got;
        r.phase1_ok = ds.done && ds.got == kN && ds.order_ok && ds.bytes_ok;
        r.no_corruption = r.no_corruption && ds.bytes_ok;
    }

    // --- DROP: forcibly close A's connection to B (and B's accepted side) ---
    // Done through the provider API only: drop_all_connections() is a test hook on
    // the node that tears down its live sockets, simulating a peer reset. The next
    // send() re-establishes a fresh connection (RECONNECT).
    a->drop_all_connections();
    b->drop_all_connections();
    // Pump briefly so the FIN/close is processed by the reactor (bounded).
    reactor.run_with_deadline(reactor.now() + 5'000'000);

    // --- phase 2: a -> b again, 4 messages 'a'.. in order (reconnect recovers) ---
    {
        constexpr int kN = 4;
        DrainState ds;
        ds.want = kN;
        ds.want_from = 10;
        ds.expect_first = 'a';
        PusherState ps;
        reactor.spawn(drainer(b, &ds));
        reactor.spawn(pusher(a, core::Endpoint{20}, 'a', kN, &ps));
        reactor.run_until([&] { return ds.done && ps.done; }, reactor.now() + kWallNs);
        r.phase2_recv = ds.got;
        r.phase2_ok = ds.done && ds.got == kN && ds.order_ok && ds.bytes_ok;
        r.no_corruption = r.no_corruption && ds.bytes_ok;
    }
    return r;
}

// ===========================================================================
// (3) NETWORK RECORD -> REPLAY byte-identical (V-PROD-REPLAY).
// ===========================================================================
struct ReplayProof {
    bool ok = false;
    std::size_t records = 0;
    std::string recorded_obs;
    std::string replayed_obs;
    std::string trace_render;
};

// Render a recv observation stream (from + hex bytes) deterministically.
std::string obs_line(const core::Message& m) {
    std::string out = "recv from=" + std::to_string(m.from.id) + " bytes=";
    out += prod::bytes_to_hex(m.payload);
    out += "\n";
    return out;
}

struct CaptureState {
    int want = 0;
    int got = 0;
    bool done = false;
    std::string out;
};

core::Task capture(core::INetwork* net, CaptureState* s) {
    for (int i = 0; i < s->want; ++i) {
        core::Message m = co_await net->recv();
        s->out += obs_line(m);
        ++s->got;
    }
    s->done = true;
    co_return;
}

ReplayProof run_record_replay() {
    ReplayProof pr;
    constexpr int kN = 6;
    prod::ReplayTrace trace;

    // --- RECORD: real TCP, node 0 -> node 1; node 1 recvs through a
    // RecordingNetwork that logs each Message into the trace. ---
    {
        prod::ProdReactor reactor;
        if (!reactor.valid()) {
            return pr;
        }
        prod::ProdNetworkBus bus(reactor);
        bus.add_node(0);
        bus.add_node(1);
        core::INetwork* n0 = bus.node(0);
        core::INetwork* n1 = bus.node(1);

        prod::RecordingNetwork rec(*n1, reactor, reactor, trace);
        CaptureState cs;
        cs.want = kN;
        PusherState ps;
        reactor.spawn(capture(&rec, &cs));
        reactor.spawn(pusher(n0, core::Endpoint{1}, 'M', kN, &ps));
        reactor.run_until([&] { return cs.done && ps.done; }, reactor.now() + kWallNs);
        pr.recorded_obs = cs.out;
        pr.trace_render = trace.render();
        pr.records = trace.size();
    }

    // --- REPLAY: same capture script over a ReplayNetwork fed from the trace,
    // driven on a plain sim Scheduler (pure, no sockets). ---
    {
        core::Scheduler sched;
        prod::ReplayNetwork replay(trace, sched, core::Endpoint{1});
        CaptureState cs;
        cs.want = kN;
        sched.spawn(capture(&replay, &cs));
        sched.run();
        pr.replayed_obs = cs.out;
    }

    pr.ok = pr.recorded_obs == pr.replayed_obs && pr.records == static_cast<std::size_t>(kN);
    return pr;
}

} // namespace

int main() {
    std::printf("[prod_network_test] Phase 7 S4b — PROD NETWORK (real TCP on the "
                "epoll reactor); core runs UNCHANGED\n\n");
    bool all = true;

    // (1) Universal INetwork contract (S1 assertions, on prod TCP).
    const ConfResult c = run_conformance();
    std::printf("=== (1) ProdNetwork UNIVERSAL INetwork CONTRACT (tier-A, S1 assertions) ===\n");
    std::printf("%s network/local-stable\n", c.local_stable ? "PASS" : "FAIL");
    std::printf("%s network/send-accepted-on-live-link\n", c.send_accepted ? "PASS" : "FAIL");
    std::printf("%s network/recv-delivers-message\n", c.recv_count_ok ? "PASS" : "FAIL");
    std::printf("%s network/recv-from-and-payload-correct\n",
                c.from_and_payload_ok ? "PASS" : "FAIL");
    std::printf("%s network/per-link-delivery-order (TCP ordered — resolves S1 flag #3)\n",
                c.per_link_order ? "PASS" : "FAIL");
    all = all && c.local_stable && c.send_accepted && c.recv_count_ok &&
          c.from_and_payload_ok && c.per_link_order;

    // (2) Integration: exchange + drop + reconnect + exchange.
    const IntegResult ir = run_integration();
    std::printf("\n=== (2) INTEGRATION: exchange -> DROP -> RECONNECT -> exchange ===\n");
    std::printf("%s net/phase1-exchange-in-order (got %d/5)\n",
                ir.phase1_ok ? "PASS" : "FAIL", ir.phase1_recv);
    std::printf("%s net/reconnect-recovers-phase2-in-order (got %d/4)\n",
                ir.phase2_ok ? "PASS" : "FAIL", ir.phase2_recv);
    std::printf("%s net/no-corruption-across-drop-reconnect\n",
                ir.no_corruption ? "PASS" : "FAIL");
    all = all && ir.phase1_ok && ir.phase2_ok && ir.no_corruption;

    // (3) Network record -> replay byte-identical.
    const ReplayProof rp = run_record_replay();
    std::printf("\n=== (3) NETWORK RECORD -> REPLAY byte-identical (V-PROD-REPLAY) ===\n");
    std::printf("network trace (%zu records):\n%s", rp.records, rp.trace_render.c_str());
    std::printf("%s network record-replay observations byte-identical\n",
                rp.ok ? "PASS" : "FAIL");
    if (!rp.ok) {
        std::fprintf(stderr, "--- recorded ---\n%s\n--- replayed ---\n%s\n",
                     rp.recorded_obs.c_str(), rp.replayed_obs.c_str());
    }
    all = all && rp.ok;

    std::printf("\n[prod_network_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
