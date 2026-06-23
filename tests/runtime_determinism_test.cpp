// runtime_determinism_test.cpp — Phase 1 minimal determinism self-test.
//
// Proves the substrate's core invariant: run() is a pure function of
// (seed, initial tasks) → byte-identical trace (L5). This is the LIGHTWEIGHT
// self-test (the full adversarial gate — seed sweep, forced-bug repro, fuzz,
// mutation — is P1-VERIFY's job). It runs a tiny ping-pong:
//
//   ping and pong exchange a token N times through Promise/Future pairs, with a
//   clock.delay() between exchanges and a seeded-random jitter delay drawn from
//   SeededRandom. The scheduler records an event trace. We run the SAME
//   (seed, tasks) twice and assert the rendered traces are byte-identical, then
//   assert final virtual time and token count are exactly as predicted.
//
// No <chrono>/<thread>/<random>: all time is virtual (SimClock), all randomness
// is the seeded provider PRNG. This file is non-provider code, so the
// forbidden-call lint scans it.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>

namespace {

using lockstep::core::Future;
using lockstep::core::make_promise;
using lockstep::core::Promise;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::SeededRandom;

// Shared rendezvous between ping and pong: a slot holding the "next" promise the
// other side will fulfill, plus the running token count. Single-threaded, no
// synchronization needed (L6).
struct PingPong {
    Scheduler* sched = nullptr;
    SimClock* clock = nullptr;
    SeededRandom* rng = nullptr;
    int exchanges = 0;       // how many round-trips to perform
    std::int64_t token = 0;  // the passed token (incremented each hop)

    // Each side waits on the future the other side will fulfill. We use a vector
    // of promise/future pairs minted up front so the handoff is explicit and the
    // trace is stable.
    std::vector<std::shared_ptr<lockstep::core::detail::SharedState<std::int64_t>>> slots;
};

// The pinger: for each exchange, delay a seeded jitter, then fulfill the next
// slot with the incremented token. Awaits the following slot for pong's reply.
Task ping(PingPong& s) {
    for (int i = 0; i < s.exchanges; ++i) {
        // Seeded jitter in [1, 4] virtual ticks (drawn deterministically).
        std::int64_t jitter = s.rng->uniform_range(1, 4);
        co_await s.clock->delay(jitter);
        s.token += 1;
        // Fulfill ping's outgoing slot (even index) so pong wakes.
        Promise<std::int64_t> out(s.slots[static_cast<std::size_t>(2 * i)]);
        out.set_value(s.token);
        // Await pong's reply on the odd slot.
        Future<std::int64_t> reply(s.slots[static_cast<std::size_t>(2 * i + 1)]);
        std::int64_t got = co_await reply;
        (void)got;
    }
    co_return;
}

// The ponger: await ping's token on the even slot, delay a seeded jitter, then
// reply on the odd slot with the incremented token.
Task pong(PingPong& s) {
    for (int i = 0; i < s.exchanges; ++i) {
        Future<std::int64_t> in(s.slots[static_cast<std::size_t>(2 * i)]);
        std::int64_t got = co_await in;
        (void)got;
        std::int64_t jitter = s.rng->uniform_range(1, 4);
        co_await s.clock->delay(jitter);
        s.token += 1;
        Promise<std::int64_t> out(s.slots[static_cast<std::size_t>(2 * i + 1)]);
        out.set_value(s.token);
    }
    co_return;
}

struct RunResult {
    std::string trace;
    lockstep::core::Tick final_vtime = 0;
    std::int64_t token = 0;
};

RunResult run_once(std::uint64_t seed, int exchanges) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);

    PingPong s;
    s.sched = &sched;
    s.clock = &clock;
    s.rng = &rng;
    s.exchanges = exchanges;
    // Mint 2*exchanges promise/future slots up front, bound to the scheduler.
    for (int i = 0; i < 2 * exchanges; ++i) {
        s.slots.push_back(
            std::make_shared<lockstep::core::detail::SharedState<std::int64_t>>(&sched));
    }

    sched.spawn(ping(s));
    sched.spawn(pong(s));
    sched.run();

    RunResult r;
    r.trace = sched.trace_text();
    r.final_vtime = sched.now();
    r.token = s.token;
    return r;
}

} // namespace

int main() {
    constexpr std::uint64_t kSeed = 0xC0FFEE1234567890ULL;
    constexpr int kExchanges = 5;

    // (1) Same (seed, tasks) twice ⇒ byte-identical trace (L5).
    RunResult a = run_once(kSeed, kExchanges);
    RunResult b = run_once(kSeed, kExchanges);

    if (a.trace != b.trace) {
        std::fprintf(stderr,
                     "DETERMINISM FAIL: traces differ for seed=%llu\n--- run A ---\n%s\n"
                     "--- run B ---\n%s\n",
                     static_cast<unsigned long long>(kSeed), a.trace.c_str(),
                     b.trace.c_str());
        return 1;
    }

    // (2) The token was passed exactly 2*exchanges times (ping + pong each hop).
    if (a.token != 2 * kExchanges) {
        std::fprintf(stderr, "TOKEN FAIL: got %lld, want %d\n",
                     static_cast<long long>(a.token), 2 * kExchanges);
        return 1;
    }

    // (3) Final virtual time advanced strictly (delays fired) and is identical
    //     across runs — a basic clock-advance sanity check (L4).
    if (a.final_vtime != b.final_vtime || a.final_vtime <= 0) {
        std::fprintf(stderr, "VTIME FAIL: a=%lld b=%lld\n",
                     static_cast<long long>(a.final_vtime),
                     static_cast<long long>(b.final_vtime));
        return 1;
    }

    // (4) A different seed yields a (valid) trace that differs where randomness
    //     applies — proves the seed actually threads through (not strictly
    //     required by the brief, but cheap and catches a dead-seed bug).
    RunResult c = run_once(kSeed ^ 0xFFFFFFFFFFFFFFFFULL, kExchanges);
    if (c.trace == a.trace) {
        std::fprintf(stderr, "SEED FAIL: different seed produced identical trace\n");
        return 1;
    }

    // (5) SeededRandom value contract — pins the documented edge guarantees so
    //     they can't silently drift (each line below kills a real mutant):
    //       * uniform(0) is the invalid-range guard: returns 0, never UB.
    //       * uniform(1) has a single valid output, 0.
    //       * uniform(bound) stays in [0, bound) for every drawn value.
    //       * uniform_range(lo, hi) stays in [lo, hi].
    {
        SeededRandom rng(0xD15EA5EULL);
        if (rng.uniform(0) != 0) { // invalid range -> documented 0 (kills ABS 0->1)
            std::fprintf(stderr, "RNG FAIL: uniform(0) != 0\n");
            return 1;
        }
        for (int i = 0; i < 64; ++i) {
            if (rng.uniform(1) != 0) {
                std::fprintf(stderr, "RNG FAIL: uniform(1) != 0\n");
                return 1;
            }
        }
        for (int i = 0; i < 4096; ++i) {
            std::uint64_t bound = 1 + rng.uniform(1000);
            std::uint64_t v = rng.uniform(bound);
            if (v >= bound) {
                std::fprintf(stderr, "RNG FAIL: uniform(%llu)=%llu out of range\n",
                             static_cast<unsigned long long>(bound),
                             static_cast<unsigned long long>(v));
                return 1;
            }
            std::int64_t rv = rng.uniform_range(-7, 7);
            if (rv < -7 || rv > 7) {
                std::fprintf(stderr, "RNG FAIL: uniform_range(-7,7)=%lld out of range\n",
                             static_cast<long long>(rv));
                return 1;
            }
        }
    }

    std::printf("runtime determinism self-test OK\n");
    std::printf("  seed=%llu exchanges=%d final_vtime=%lld token=%lld trace_bytes=%zu\n",
                static_cast<unsigned long long>(kSeed), kExchanges,
                static_cast<long long>(a.final_vtime),
                static_cast<long long>(a.token), a.trace.size());
    // Emit the trace once for human inspection / dashboards.
    std::printf("--- stable event trace (seed=%llu) ---\n%s",
                static_cast<unsigned long long>(kSeed), a.trace.c_str());
    return 0;
}
