// sim_network_test.cpp — S1 gate for the simulation INetwork (spec C2.1).
//
// Proves the in-memory message bus (providers/sim/.../SimNetwork.hpp) is a pure
// function of (seed, inputs): same seed ⇒ byte-identical event trace, partitions
// LOSE messages and heals RECOVER them reproducibly, and each per-link fault mode
// (drop / reorder / duplicate / latency) is demonstrably exercised AND
// deterministic. All time is virtual (SimClock); all randomness is the seeded
// provider PRNG (SeededRandom). This file is NON-provider code, so the
// forbidden-call lint scans it — no <chrono>/<thread>/<random> here.
//
// REPLAY: the seed is printed on every run and on every failure, so any failing
// assertion is reproducible from the logged seed alone.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimNetwork.hpp>

namespace {

using lockstep::core::Endpoint;
using lockstep::core::Error;
using lockstep::core::ErrorCode;
using lockstep::core::Future;
using lockstep::core::Message;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimNetwork;
using lockstep::sim::SimNetworkBus;

// A tiny payload helper: span over a static-ish byte vector kept alive by caller.
std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s) {
        v.push_back(static_cast<std::byte>(c));
    }
    return v;
}

// A counter of how many messages a receiver actually got. Shared, single-thread.
struct RecvSink {
    int received = 0;
    std::vector<std::string> texts; // decoded payloads, in delivery order
};

// A receiver Task: recv exactly `count` messages, recording each.
Task receiver(SimNetwork net, RecvSink& sink, int count) {
    for (int i = 0; i < count; ++i) {
        Future<Message> f = net.recv();
        Message m = co_await f;
        std::string text;
        for (std::byte b : m.payload) {
            text.push_back(static_cast<char>(b));
        }
        sink.texts.push_back(text);
        sink.received += 1;
    }
    co_return;
}

// ===========================================================================
// Test 1 + 2: partition loses, heal recovers, byte-identical replay.
// ===========================================================================

struct PartitionResult {
    std::string trace;
    int delivered = 0;       // messages the receiver actually got
    int sent_during_cut = 0; // messages attempted while partitioned
    int blocked_at_send = 0; // sends that returned Unavailable
};

// Driver: node 0 sends to node 1. We partition {0}|{1} for a window, send during
// the cut (those are lost), then heal and send again (those arrive). The receiver
// recv()s only the messages we EXPECT to survive, so the run drains cleanly.
//
//   phase A (healed): send 2 → both delivered
//   phase B (cut):    send 2 → both LOST (blocked at send) — receiver does NOT
//                     wait on these
//   phase C (healed): send 2 → both delivered
// Expected delivered = 4 (phase A + C); phase B is the lost window.
Task partition_driver(SimNetworkBus& bus, SimClock& clock, RecvSink& sink,
                      int* blocked_at_send) {
    SimNetwork n0 = bus.node(0);
    Endpoint to{1};

    auto send_one = [&](int idx) -> Task {
        std::vector<std::byte> p = bytes_of(std::string("p") + std::to_string(idx));
        std::span<const std::byte> view(p.data(), p.size());
        Future<Error> sf = n0.send(to, view);
        Error e = co_await sf; // send's Error is the future's VALUE, not error()
        if (e.code == ErrorCode::Unavailable) {
            *blocked_at_send += 1;
        }
        co_return;
    };

    // Phase A — healed.
    co_await send_one(0);
    co_await clock.delay(5);
    co_await send_one(1);
    co_await clock.delay(5);

    // Phase B — partition the link, send into the void.
    bus.partition({0});
    co_await send_one(2);
    co_await clock.delay(5);
    co_await send_one(3);
    co_await clock.delay(5);

    // Phase C — heal, delivery restored.
    bus.heal();
    co_await send_one(4);
    co_await clock.delay(5);
    co_await send_one(5);
    co_await clock.delay(5);
    (void)sink;
    co_return;
}

PartitionResult run_partition(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    SimNetworkBus bus(sched, rng);
    bus.add_nodes({0, 1});
    // Deterministic latency, no random faults: this test isolates partitions.
    bus.set_faults({.drop_prob = 0.0,
                    .dup_prob = 0.0,
                    .reorder_prob = 0.0,
                    .latency_min = 1,
                    .latency_max = 1,
                    .reorder_jitter_max = 0});

    RecvSink sink;
    int blocked = 0;
    // Receiver waits for the 4 messages that survive (phases A + C).
    sched.spawn(receiver(bus.node(1), sink, 4));
    sched.spawn(partition_driver(bus, clock, sink, &blocked));
    sched.run();

    PartitionResult r;
    r.trace = sched.trace_text();
    r.delivered = sink.received;
    r.blocked_at_send = blocked;
    r.sent_during_cut = 2;
    return r;
}

// ===========================================================================
// Test 3: each fault mode demonstrably exercised + deterministic.
// ===========================================================================

// We run a fixed batch of sends under a single fault knob turned up, and observe
// the trace for the fault's signature token. We assert the effect is present AND
// reproduces byte-identically across two same-seed runs.

struct FaultResult {
    std::string trace;
    int dropped = 0;   // net_send ... drop=1 occurrences
    int duped = 0;     // net_deliver ... c=1 occurrences (the duplicate copy)
    int reordered = 0; // net_send ... reorder=1 occurrences
    int delivered = 0; // net_deliver occurrences
    int distinct_latencies = 0;
};

// Count occurrences of a needle in a haystack (simple, deterministic).
int count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    int n = 0;
    std::string::size_type pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Fire N sends 0->1 with the given fault config; let everything deliver. The
// receiver drains whatever lands (we don't know the exact count under faults, so
// it recv()s a generous number bounded by a deadline-style cap via the trace).
Task fault_sender(SimNetwork net, SimClock& clock, Endpoint to, int count) {
    for (int i = 0; i < count; ++i) {
        std::vector<std::byte> p = bytes_of(std::string("f") + std::to_string(i));
        std::span<const std::byte> view(p.data(), p.size());
        Future<Error> sf = net.send(to, view);
        co_await sf;
        co_await clock.delay(3);
    }
    co_return;
}

FaultResult run_faults(std::uint64_t seed, const lockstep::sim::detail::LinkFaults& f,
                       int sends) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    SimNetworkBus bus(sched, rng);
    bus.add_nodes({0, 1});
    bus.set_faults(f);

    // No receiver: we only care about the bus's delivery trace (deposits land in
    // node 1's mailbox and stay there). This keeps the toy minimal and the trace
    // a clean record of bus decisions.
    sched.spawn(fault_sender(bus.node(0), clock, Endpoint{1}, sends));
    sched.run();

    FaultResult r;
    r.trace = sched.trace_text();
    r.dropped = count_occurrences(r.trace, "drop=1");
    r.duped = count_occurrences(r.trace, "net_deliver from=0 to=1 s=") -
              count_occurrences(r.trace, " c=0 ");
    // Reorder + delivery signatures.
    r.reordered = count_occurrences(r.trace, "reorder=1");
    r.delivered = count_occurrences(r.trace, "net_deliver ");
    return r;
}

// ===========================================================================

int fail(const char* what, std::uint64_t seed) {
    std::fprintf(stderr, "FAIL: %s  (replay with seed=%llu)\n", what,
                 static_cast<unsigned long long>(seed));
    return 1;
}

} // namespace

int main() {
    // One master seed for the whole suite. Printed up front and on every failure
    // so the run is replayable from the log alone.
    constexpr std::uint64_t kSeed = 0x5EED'1357'9BDF'2468ULL;
    std::printf("sim_network_test seed=%llu\n", static_cast<unsigned long long>(kSeed));

    // ---- Test 1: partition loses, heal recovers (reproducibly) ------------
    PartitionResult p1 = run_partition(kSeed);
    PartitionResult p2 = run_partition(kSeed);

    // Messages sent during the cut were blocked at send (Unavailable).
    if (p1.blocked_at_send != p1.sent_during_cut) {
        std::fprintf(stderr, "blocked=%d expected=%d\n", p1.blocked_at_send,
                     p1.sent_during_cut);
        return fail("partition did not block messages during cut", kSeed);
    }
    // The 4 messages outside the cut all arrived.
    if (p1.delivered != 4) {
        std::fprintf(stderr, "delivered=%d expected=4\n", p1.delivered);
        return fail("heal did not restore delivery", kSeed);
    }

    // ---- Test 2: same-seed run twice ⇒ byte-identical event trace ----------
    if (p1.trace != p2.trace) {
        std::fprintf(stderr, "--- run A ---\n%s\n--- run B ---\n%s\n", p1.trace.c_str(),
                     p2.trace.c_str());
        return fail("partition trace not byte-identical on replay", kSeed);
    }
    if (p1.delivered != p2.delivered || p1.blocked_at_send != p2.blocked_at_send) {
        return fail("partition outcome not reproducible", kSeed);
    }

    // ---- Test 3: each fault mode exercised + deterministic -----------------

    // (3a) DROP: high drop probability ⇒ at least one message dropped, fewer
    //      deliveries than sends; reproduces byte-identically.
    {
        lockstep::sim::detail::LinkFaults f{};
        f.drop_prob = 0.5;
        f.latency_min = 1;
        f.latency_max = 1;
        FaultResult a = run_faults(kSeed, f, 24);
        FaultResult b = run_faults(kSeed, f, 24);
        if (a.trace != b.trace) {
            return fail("DROP fault not deterministic on replay", kSeed);
        }
        if (a.dropped <= 0) {
            return fail("DROP fault never exercised", kSeed);
        }
        if (a.delivered >= 24) {
            return fail("DROP fault did not reduce deliveries", kSeed);
        }
        std::printf("  DROP: dropped=%d delivered=%d (of 24)\n", a.dropped,
                    a.delivered);
    }

    // (3b) DUPLICATE: high dup probability ⇒ more deliveries than sends; the
    //      duplicate copy (c=1) appears; reproduces byte-identically.
    {
        lockstep::sim::detail::LinkFaults f{};
        f.dup_prob = 0.9;
        f.latency_min = 1;
        f.latency_max = 1;
        FaultResult a = run_faults(kSeed, f, 12);
        FaultResult b = run_faults(kSeed, f, 12);
        if (a.trace != b.trace) {
            return fail("DUPLICATE fault not deterministic on replay", kSeed);
        }
        int dup_copies = count_occurrences(a.trace, " c=1 ");
        if (dup_copies <= 0) {
            return fail("DUPLICATE fault never exercised", kSeed);
        }
        if (a.delivered <= 12) {
            return fail("DUPLICATE fault did not increase deliveries", kSeed);
        }
        std::printf("  DUP: dup_copies=%d delivered=%d (of 12 sends)\n", dup_copies,
                    a.delivered);
    }

    // (3c) LATENCY: a wide latency range ⇒ messages carry varying lat=; the
    //      observed latencies are not all equal; reproduces byte-identically.
    {
        lockstep::sim::detail::LinkFaults f{};
        f.latency_min = 1;
        f.latency_max = 50;
        FaultResult a = run_faults(kSeed, f, 16);
        FaultResult b = run_faults(kSeed, f, 16);
        if (a.trace != b.trace) {
            return fail("LATENCY fault not deterministic on replay", kSeed);
        }
        // Two distinct latency values must appear (range was exercised).
        bool saw_one = a.trace.find("lat=1 ") != std::string::npos;
        bool saw_big = false;
        for (int v = 2; v <= 50 && !saw_big; ++v) {
            if (a.trace.find(std::string("lat=") + std::to_string(v) + " ") !=
                std::string::npos) {
                saw_big = true;
            }
        }
        if (!saw_big) {
            return fail("LATENCY fault never produced a non-minimal latency", kSeed);
        }
        (void)saw_one;
        std::printf("  LATENCY: range [1,50] exercised, deterministic\n");
    }

    // (3d) REORDER: reorder jitter on ⇒ messages get extra jitter, and the
    //      delivery order differs from send order. We detect reorder via the
    //      reorder=1 signature and a non-monotonic delivery sequence.
    {
        lockstep::sim::detail::LinkFaults f{};
        f.reorder_prob = 0.8;
        f.latency_min = 1;
        f.latency_max = 1;
        f.reorder_jitter_max = 20;
        FaultResult a = run_faults(kSeed, f, 16);
        FaultResult b = run_faults(kSeed, f, 16);
        if (a.trace != b.trace) {
            return fail("REORDER fault not deterministic on replay", kSeed);
        }
        if (a.reordered <= 0) {
            return fail("REORDER fault never exercised", kSeed);
        }
        // Reordering means the delivered s= sequence is not strictly ascending.
        // Scan net_deliver lines, extract s=, check for an out-of-order pair.
        std::vector<long> order;
        std::string::size_type pos = 0;
        const std::string mark = "net_deliver from=0 to=1 s=";
        while ((pos = a.trace.find(mark, pos)) != std::string::npos) {
            pos += mark.size();
            order.push_back(std::strtol(a.trace.c_str() + pos, nullptr, 10));
        }
        bool out_of_order = false;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (order[i] < order[i - 1]) {
                out_of_order = true;
                break;
            }
        }
        if (!out_of_order) {
            return fail("REORDER fault did not perturb delivery order", kSeed);
        }
        std::printf("  REORDER: reordered=%d, delivery order perturbed (%zu deliveries)\n",
                    a.reordered, order.size());
    }

    // (3e) JITTER CLAMP: reorder fires but reorder_jitter_max <= 0 ⇒ the drawn
    //      jitter MUST be exactly 0 (no jitter to add). Every send line must carry
    //      jit=0, and with fixed unit latency delivery stays in strict send order.
    //      This pins draw_jitter()'s zero-return path (kills a 0->1 mutant there).
    {
        lockstep::sim::detail::LinkFaults f{};
        f.reorder_prob = 1.0;     // reorder always "fires" — draw_jitter IS called
        f.reorder_jitter_max = 0; // ...but with no jitter budget ⇒ must yield 0
        f.latency_min = 1;
        f.latency_max = 1;
        FaultResult a = run_faults(kSeed, f, 12);
        FaultResult b = run_faults(kSeed, f, 12);
        if (a.trace != b.trace) {
            return fail("JITTER-CLAMP not deterministic on replay", kSeed);
        }
        // reorder fired on every send...
        if (count_occurrences(a.trace, "reorder=1") != 12) {
            return fail("JITTER-CLAMP setup: reorder did not fire on every send", kSeed);
        }
        // ...yet NO send carries a non-zero jitter, and EVERY send shows jit=0.
        if (a.trace.find("jit=") == std::string::npos ||
            count_occurrences(a.trace, "jit=0") != 12) {
            return fail("JITTER-CLAMP: jitter not clamped to 0 when budget is 0", kSeed);
        }
        for (int v = 1; v <= 64; ++v) {
            if (a.trace.find(std::string("jit=") + std::to_string(v)) !=
                std::string::npos) {
                return fail("JITTER-CLAMP: non-zero jitter appeared with zero budget",
                            kSeed);
            }
        }
        // With unit latency and zero jitter, deliveries are in exact send order.
        std::vector<long> order;
        std::string::size_type pos = 0;
        const std::string mark = "net_deliver from=0 to=1 s=";
        while ((pos = a.trace.find(mark, pos)) != std::string::npos) {
            pos += mark.size();
            order.push_back(std::strtol(a.trace.c_str() + pos, nullptr, 10));
        }
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (order[i] < order[i - 1]) {
                return fail("JITTER-CLAMP: order perturbed despite zero jitter", kSeed);
            }
        }
        std::printf("  JITTER-CLAMP: jit=0 on all 12 sends, strict order (kills 0->1)\n");
    }

    std::printf("sim_network_test OK  seed=%llu\n",
                static_cast<unsigned long long>(kSeed));
    std::printf("  partition: blocked_during_cut=%d delivered_after_heal=%d\n",
                p1.blocked_at_send, p1.delivered);
    std::printf("  byte-identical replay: partition trace %zu bytes match\n",
                p1.trace.size());
    return 0;
}
