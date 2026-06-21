#pragma once

// ping_pong.hpp — P1-VERIFY shared workload + independent prediction harness.
//
// This is VERIFICATION-ONLY code (authored by P1-VERIFY, a different author than
// P1-CORE). It does NOT touch the core runtime; it only DRIVES it via the public
// landed API (Scheduler/SimClock/Future/Promise/Task + sim::SeededRandom) and
// observes the rendered event trace.
//
// It provides:
//   - run_ping_pong(seed, exchanges, variant): runs N token exchanges between two
//     coroutines through Future/Promise pairs, with clock.delay() jitter drawn
//     from the seeded PRNG, and returns the rendered trace + final vtime + token.
//   - an INDEPENDENT prediction of (final_vtime, token_count) computed from the
//     PRNG contract alone (a fresh SeededRandom with the same seed), NOT read back
//     from the run — so assertion (3) compares the run to a hand-derivable value.
//   - a "bug variant" knob used by the forced-bug repro test to perturb the
//     workload TEST-SIDE (reorder/drop an op) without corrupting the core runtime.
//
// Determinism trap (HARD): all randomness is sim::SeededRandom; no <random>,
// std::*_distribution, std::shuffle, std::random_device, std::rand, <chrono>,
// <thread>. The forbidden-call lint scans this file (it is not under providers/).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>

namespace lockstep::verify {

// How the workload may be (test-side) perturbed. kCorrect is the real determin-
// istic ping-pong. The others model a *regression* introduced in the WORKLOAD
// (never in core): the determinism net must still make them reproducible from
// their seed, and must distinguish them from kCorrect.
enum class Variant : std::uint8_t {
    kCorrect,      // the spec-correct ping-pong
    kDropDelay,    // BUG: pong skips its delay() on one exchange (changes vtime)
    kReorderExch,  // BUG: ping draws its jitter in a different order on one hop
    kExtraToken,   // BUG: an extra token increment (changes token count)
};

// The jitter range [lo, hi] drawn per hop. Fixed so the prediction is stable.
inline constexpr std::int64_t kJitterLo = 1;
inline constexpr std::int64_t kJitterHi = 4;

// Shared rendezvous between ping and pong. Single-threaded; no synchronization.
struct PingPongState {
    core::SimClock* clock = nullptr;
    sim::SeededRandom* rng = nullptr;
    int exchanges = 0;
    Variant variant = Variant::kCorrect;
    std::int64_t token = 0;
    // 2*exchanges promise/future slots minted up front, bound to the scheduler.
    std::vector<std::shared_ptr<core::detail::SharedState<std::int64_t>>> slots;
};

// The pinger: each exchange, delay a seeded jitter, then fulfill the even slot
// with the incremented token, then await pong's reply on the odd slot.
inline core::Task ping(PingPongState& s) {
    for (int i = 0; i < s.exchanges; ++i) {
        std::int64_t jitter = s.rng->uniform_range(kJitterLo, kJitterHi);
        // kReorderExch: on the middle hop, draw a SECOND jitter and use it
        // instead — this consumes the PRNG differently => different valid trace.
        if (s.variant == Variant::kReorderExch && i == s.exchanges / 2) {
            jitter = s.rng->uniform_range(kJitterLo, kJitterHi);
        }
        co_await s.clock->delay(jitter);
        s.token += 1;
        if (s.variant == Variant::kExtraToken && i == 0) {
            s.token += 1; // BUG: drift the token count by one.
        }
        core::Promise<std::int64_t> out(s.slots[static_cast<std::size_t>(2 * i)]);
        out.set_value(s.token);
        core::Future<std::int64_t> reply(s.slots[static_cast<std::size_t>(2 * i + 1)]);
        std::int64_t got = co_await reply;
        (void)got;
    }
    co_return;
}

// The ponger: await ping's token (even slot), delay a seeded jitter, reply on
// the odd slot with the incremented token.
inline core::Task pong(PingPongState& s) {
    for (int i = 0; i < s.exchanges; ++i) {
        core::Future<std::int64_t> in(s.slots[static_cast<std::size_t>(2 * i)]);
        std::int64_t got = co_await in;
        (void)got;
        std::int64_t jitter = s.rng->uniform_range(kJitterLo, kJitterHi);
        // kDropDelay: on the middle hop pong SKIPS its delay (still draws the
        // jitter to keep the PRNG stream shape, but does not advance the clock).
        bool drop = (s.variant == Variant::kDropDelay && i == s.exchanges / 2);
        if (!drop) {
            co_await s.clock->delay(jitter);
        }
        s.token += 1;
        core::Promise<std::int64_t> out(s.slots[static_cast<std::size_t>(2 * i + 1)]);
        out.set_value(s.token);
    }
    co_return;
}

struct RunResult {
    std::string trace;
    core::Tick final_vtime = 0;
    std::int64_t token = 0;
    std::size_t trace_bytes = 0;
};

// Run one ping-pong to completion and return its observable result.
inline RunResult run_ping_pong(std::uint64_t seed, int exchanges,
                               Variant variant = Variant::kCorrect) {
    core::Scheduler sched;
    core::SimClock clock(sched);
    sim::SeededRandom rng(seed);

    PingPongState s;
    s.clock = &clock;
    s.rng = &rng;
    s.exchanges = exchanges;
    s.variant = variant;
    for (int i = 0; i < 2 * exchanges; ++i) {
        s.slots.push_back(
            std::make_shared<core::detail::SharedState<std::int64_t>>(&sched));
    }

    sched.spawn(ping(s));
    sched.spawn(pong(s));
    sched.run();

    RunResult r;
    r.trace = sched.trace_text();
    r.final_vtime = sched.now();
    r.token = s.token;
    r.trace_bytes = r.trace.size();
    return r;
}

// ---------------------------------------------------------------------------
// INDEPENDENT prediction of the CORRECT run's (final_vtime, token_count).
//
// Derived from the spec + the PRNG CONTRACT alone, NOT read back from the run:
//   - The runtime serializes ping and pong on a single thread, so the PRNG is
//     drawn in a strict order: ping(hop0), pong(hop0), ping(hop1), pong(hop1),
//     ... i.e. exactly 2*N consecutive uniform_range(kJitterLo,kJitterHi) draws.
//   - Each draw is the number of ticks the clock then advances for that hop
//     (delay(jitter)). Virtual time advances ONLY at timer fire (L4), once per
//     hop, by exactly that hop's jitter. So final_vtime = sum of the 2*N draws.
//   - Each hop increments the token exactly once (ping and pong each once per
//     exchange), so token_count = 2*N.
// We compute the draws with a FRESH SeededRandom(seed) — the same contract the
// workload uses — which is a legitimate hand-derivation, not "equal to itself".
// ---------------------------------------------------------------------------
struct Prediction {
    core::Tick final_vtime = 0;
    std::int64_t token = 0;
    std::vector<std::int64_t> hop_jitters; // the 2*N per-hop advances, in order
};

inline Prediction predict_correct(std::uint64_t seed, int exchanges) {
    Prediction p;
    p.token = 2 * static_cast<std::int64_t>(exchanges);
    sim::SeededRandom rng(seed);
    core::Tick vt = 0;
    for (int i = 0; i < 2 * exchanges; ++i) {
        std::int64_t j = rng.uniform_range(kJitterLo, kJitterHi);
        p.hop_jitters.push_back(j);
        vt += j;
    }
    p.final_vtime = vt;
    return p;
}

} // namespace lockstep::verify
