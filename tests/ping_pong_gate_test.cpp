// ping_pong_gate_test.cpp — THE Phase 1 GATE (independent, P1-VERIFY author).
//
// Spec: lockstep-phase-specs-all.md, Phase 1 "Gate — deterministic ping-pong".
// Two coroutines exchange a token N times through Futures, with clock.delay()
// between exchanges and randomness via SeededRandom. Asserts ALL THREE spec
// conditions — and asserts EXACTLY the determinism CONTRACT (not stronger: we
// never pin a specific trace literal; we assert same-seed-identical,
// diff-seed-valid-different, and the HAND-PREDICTED vtime/token).
//
// (1) same seed            => byte-identical Trace::render() output.
// (2) different seed       => a VALID but DIFFERENT trace (difference asserted;
//                             validity = predicted token + structural invariants).
// (3) final vtime + token  => EXACTLY the hand-computed expectation (independent
//                             prediction from N + the PRNG draw schedule).
//
// Every run logs its seed (so a failing seed replays). No <chrono>/<thread>/
// <random>: virtual time + seeded PRNG only.

#include <cstdint>
#include <cstdio>
#include <string>

#include "harness/invariants.hpp"
#include "harness/ping_pong.hpp"

namespace {

using lockstep::verify::predict_correct;
using lockstep::verify::Prediction;
using lockstep::verify::run_ping_pong;
using lockstep::verify::RunResult;
using lockstep::verify::Variant;

int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "GATE FAIL [%s:%d]: ", __FILE__, __LINE__);    \
            std::fprintf(stderr, __VA_ARGS__);                                  \
            std::fprintf(stderr, "\n");                                         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

} // namespace

int main() {
    constexpr std::uint64_t kSeed = 0xC0FFEE1234567890ULL;
    constexpr std::uint64_t kSeedB = 0x0123456789ABCDEFULL;
    constexpr int kExchanges = 5;

    std::printf("[ping_pong_gate] seedA=%llu seedB=%llu exchanges=%d\n",
                static_cast<unsigned long long>(kSeed),
                static_cast<unsigned long long>(kSeedB), kExchanges);

    // ---- (1) same seed => byte-identical trace ----------------------------
    RunResult a1 = run_ping_pong(kSeed, kExchanges, Variant::kCorrect);
    RunResult a2 = run_ping_pong(kSeed, kExchanges, Variant::kCorrect);
    CHECK(a1.trace == a2.trace,
          "same seed produced DIFFERENT traces (seed=%llu)\n--- A1 ---\n%s\n--- A2 ---\n%s",
          static_cast<unsigned long long>(kSeed), a1.trace.c_str(), a2.trace.c_str());

    // ---- (3) final vtime + token EXACTLY as hand-predicted ----------------
    // Independent prediction from N + the PRNG draw schedule (fresh PRNG):
    //   token  = 2*N (each of N exchanges increments once on ping, once on pong).
    //   vtime  = sum of the 2*N consecutive uniform_range(1,4) draws (one clock
    //            advance per hop, by exactly that hop's jitter; L4).
    Prediction pred = predict_correct(kSeed, kExchanges);
    CHECK(a1.token == pred.token, "token: run=%lld predicted=%lld",
          static_cast<long long>(a1.token), static_cast<long long>(pred.token));
    CHECK(a1.final_vtime == pred.final_vtime, "vtime: run=%lld predicted=%lld",
          static_cast<long long>(a1.final_vtime),
          static_cast<long long>(pred.final_vtime));
    // And the prediction is a real derivation: token must be 2*N, vtime > 0.
    CHECK(pred.token == 2 * kExchanges, "prediction self-check token=%lld",
          static_cast<long long>(pred.token));
    CHECK(pred.final_vtime > 0, "prediction self-check vtime=%lld",
          static_cast<long long>(pred.final_vtime));

    // Cross-check: the per-hop timer-arm deltas in the trace equal the predicted
    // jitter schedule (proves the clock advanced by exactly the drawn amounts and
    // the prediction is anchored to the actual run, not coincidence).
    std::vector<std::int64_t> arms =
        lockstep::verify::clock_advance_deltas(a1.trace);
    CHECK(arms == pred.hop_jitters,
          "clock-advance deltas do not match predicted jitter schedule");

    // Structural invariants on the correct run.
    auto inv = lockstep::verify::check_invariants(a1.trace, kExchanges);
    CHECK(inv.ok, "structural invariants on correct run: %s", inv.why.c_str());
    CHECK(inv.spawns == 2, "spawns=%d (want 2)", inv.spawns);
    CHECK(inv.task_done == 2, "task_done=%d (want 2)", inv.task_done);
    CHECK(inv.timer_arm == 2 * kExchanges, "timer_arm=%d (want %d)", inv.timer_arm,
          2 * kExchanges);
    CHECK(inv.timer_fire == 2 * kExchanges, "timer_fire=%d (want %d)",
          inv.timer_fire, 2 * kExchanges);

    // ---- (2) different seed => valid-but-different trace ------------------
    RunResult b = run_ping_pong(kSeedB, kExchanges, Variant::kCorrect);
    // DIFFERENT: randomness applies, so the byte stream must differ.
    CHECK(b.trace != a1.trace,
          "different seed produced IDENTICAL trace (dead seed leak)");
    // STILL VALID: same token count + structural invariants hold; vtime matches
    // ITS OWN independent prediction.
    Prediction predB = predict_correct(kSeedB, kExchanges);
    CHECK(b.token == predB.token, "diff-seed token: run=%lld predicted=%lld",
          static_cast<long long>(b.token), static_cast<long long>(predB.token));
    CHECK(b.final_vtime == predB.final_vtime,
          "diff-seed vtime: run=%lld predicted=%lld",
          static_cast<long long>(b.final_vtime),
          static_cast<long long>(predB.final_vtime));
    auto invB = lockstep::verify::check_invariants(b.trace, kExchanges);
    CHECK(invB.ok, "structural invariants on diff-seed run: %s", invB.why.c_str());
    // Token count identical across seeds (token count is seed-INDEPENDENT; only
    // the timing/trace differs where randomness applies).
    CHECK(a1.token == b.token, "token count differs across seeds (%lld vs %lld)",
          static_cast<long long>(a1.token), static_cast<long long>(b.token));

    if (g_failures != 0) {
        std::fprintf(stderr, "[ping_pong_gate] FAILED with %d check failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("[ping_pong_gate] OK — (1) same-seed byte-identical, "
                "(2) diff-seed valid+different, (3) vtime=%lld token=%lld matched "
                "hand-prediction.\n",
                static_cast<long long>(a1.final_vtime),
                static_cast<long long>(a1.token));
    return 0;
}
