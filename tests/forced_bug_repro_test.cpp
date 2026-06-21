// forced_bug_repro_test.cpp — proves the determinism net CATCHES regressions and
// that a failure is BYTE-IDENTICALLY reproducible from its logged seed.
//
// We do NOT plant a bug in the core runtime (core is no-freelance). Instead we
// model the "regression" TEST-SIDE: the ping-pong workload has Variant knobs
// (kDropDelay / kReorderExch / kExtraToken) that reorder/drop/duplicate an
// operation. The point being demonstrated is the master-plan claim:
//
//   "a forced bug is reproducible byte-for-byte from its seed."
//
// For each bug variant we assert:
//   (A) DETECTION: the determinism net flags it — the buggy trace DIFFERS from
//       the correct trace for the same seed (so a regression cannot hide), OR it
//       violates a structural invariant / the predicted (vtime,token).
//   (B) REPRODUCIBILITY: running the SAME (seed, bug) twice yields a
//       BYTE-IDENTICAL trace — the failure replays exactly from its logged seed.
//
// Every run logs its seed.

#include <cstdint>
#include <cstdio>
#include <string>

#include "harness/invariants.hpp"
#include "harness/ping_pong.hpp"

namespace {

using lockstep::verify::check_invariants;
using lockstep::verify::predict_correct;
using lockstep::verify::Prediction;
using lockstep::verify::run_ping_pong;
using lockstep::verify::RunResult;
using lockstep::verify::Variant;

int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FORCED-BUG FAIL [%s:%d]: ", __FILE__,         \
                         __LINE__);                                             \
            std::fprintf(stderr, __VA_ARGS__);                                  \
            std::fprintf(stderr, "\n");                                         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

const char* variant_name(Variant v) {
    switch (v) {
    case Variant::kCorrect:     return "correct";
    case Variant::kDropDelay:   return "drop_delay";
    case Variant::kReorderExch: return "reorder_exchange";
    case Variant::kExtraToken:  return "extra_token";
    }
    return "?";
}

} // namespace

int main() {
    constexpr std::uint64_t kSeed = 0xBADBEEF0FACEFEEDULL;
    constexpr int kExchanges = 5;

    std::printf("[forced_bug] seed=%llu exchanges=%d\n",
                static_cast<unsigned long long>(kSeed), kExchanges);

    // Baseline correct run for the same seed.
    RunResult correct = run_ping_pong(kSeed, kExchanges, Variant::kCorrect);
    Prediction pred = predict_correct(kSeed, kExchanges);

    const Variant bugs[] = {Variant::kDropDelay, Variant::kReorderExch,
                            Variant::kExtraToken};

    for (Variant bug : bugs) {
        // --- (B) REPRODUCIBILITY: same (seed, bug) twice => byte-identical. ---
        RunResult b1 = run_ping_pong(kSeed, kExchanges, bug);
        RunResult b2 = run_ping_pong(kSeed, kExchanges, bug);
        CHECK(b1.trace == b2.trace,
              "[%s] bug NOT byte-identical on rerun from seed=%llu\n--- run1 ---\n%s"
              "\n--- run2 ---\n%s",
              variant_name(bug), static_cast<unsigned long long>(kSeed),
              b1.trace.c_str(), b2.trace.c_str());

        // --- (A) DETECTION: the net must flag the regression. ---
        bool detected = false;
        std::string how;
        // Signal 1: trace differs from correct for the same seed.
        if (b1.trace != correct.trace) {
            detected = true;
            how = "trace-differs-from-correct";
        }
        // Signal 2: violates the hand-prediction (vtime or token drift).
        if (b1.final_vtime != pred.final_vtime || b1.token != pred.token) {
            detected = true;
            how += (how.empty() ? "" : "+") + std::string("prediction-mismatch");
        }
        // Signal 3: structural invariant violation (if any).
        auto inv = check_invariants(b1.trace, kExchanges);
        if (!inv.ok) {
            detected = true;
            how += (how.empty() ? "" : "+") + std::string("invariant:" + inv.why);
        }
        CHECK(detected, "[%s] regression went UNDETECTED by the determinism net",
              variant_name(bug));

        std::printf("[forced_bug] %-16s seed=%llu : reproducible=%s detected_by=%s "
                    "(vtime=%lld token=%lld vs correct vtime=%lld token=%lld)\n",
                    variant_name(bug), static_cast<unsigned long long>(kSeed),
                    (b1.trace == b2.trace) ? "yes" : "NO", how.c_str(),
                    static_cast<long long>(b1.final_vtime),
                    static_cast<long long>(b1.token),
                    static_cast<long long>(correct.final_vtime),
                    static_cast<long long>(correct.token));
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "[forced_bug] FAILED with %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("[forced_bug] OK — every forced regression detected AND "
                "byte-identically reproducible from its seed.\n");
    return 0;
}
