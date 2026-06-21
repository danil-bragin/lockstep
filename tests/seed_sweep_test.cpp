// seed_sweep_test.cpp — multi-seed determinism sweep (P1-VERIFY).
//
// Iterates many seeds; for EACH seed asserts: run-twice-same-seed =>
// byte-identical Trace::render(), AND the run matches its OWN independent
// (vtime, token) prediction, AND structural invariants hold. Surfaces and PRINTS
// any failing seed so it replays. Fast enough for the local gate (default a few
// thousand seeds); CI can crank it up via env LOCKSTEP_SWEEP_SEEDS.
//
// The seed STREAM is itself driven by sim::SeededRandom (no <random>): we draw
// each trial seed from a meta-PRNG seeded by LOCKSTEP_SWEEP_BASE (default fixed),
// so the whole sweep is reproducible and logs its base + every failing seed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "harness/invariants.hpp"
#include "harness/ping_pong.hpp"

#include <lockstep/sim/SeededRandom.hpp>

namespace {

using lockstep::verify::check_invariants;
using lockstep::verify::predict_correct;
using lockstep::verify::Prediction;
using lockstep::verify::run_ping_pong;
using lockstep::verify::RunResult;
using lockstep::verify::Variant;

std::uint64_t env_u64(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return static_cast<std::uint64_t>(std::strtoull(v, nullptr, 0));
}

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return static_cast<int>(std::strtol(v, nullptr, 0));
}

} // namespace

int main() {
    // Defaults tuned to run fast locally; CI overrides via env.
    const int kSeeds = env_int("LOCKSTEP_SWEEP_SEEDS", 4000);
    const std::uint64_t kBase = env_u64("LOCKSTEP_SWEEP_BASE", 0xA5A5A5A500000001ULL);
    // Vary exchange count across a small set so the sweep covers different
    // workload sizes (all still pure functions of their seed).
    const int kExchangeChoices[] = {1, 3, 5, 8, 13};

    std::printf("[seed_sweep] seeds=%d base=%llu\n", kSeeds,
                static_cast<unsigned long long>(kBase));

    lockstep::sim::SeededRandom meta(kBase);
    int passed = 0;
    int failed = 0;

    for (int t = 0; t < kSeeds; ++t) {
        std::uint64_t seed = meta.next();
        int exchanges = kExchangeChoices[static_cast<std::size_t>(
            meta.uniform(sizeof(kExchangeChoices) / sizeof(int)))];

        RunResult r1 = run_ping_pong(seed, exchanges, Variant::kCorrect);
        RunResult r2 = run_ping_pong(seed, exchanges, Variant::kCorrect);

        bool ok = true;
        std::string why;

        if (r1.trace != r2.trace) {
            ok = false;
            why = "run-twice traces differ (non-deterministic)";
        }
        if (ok) {
            Prediction pred = predict_correct(seed, exchanges);
            if (r1.token != pred.token) {
                ok = false;
                why = "token != predicted (" + std::to_string(r1.token) + " vs " +
                      std::to_string(pred.token) + ")";
            } else if (r1.final_vtime != pred.final_vtime) {
                ok = false;
                why = "vtime != predicted (" + std::to_string(r1.final_vtime) +
                      " vs " + std::to_string(pred.final_vtime) + ")";
            }
        }
        if (ok) {
            auto inv = check_invariants(r1.trace, exchanges);
            if (!inv.ok) {
                ok = false;
                why = "invariant: " + inv.why;
            }
        }

        if (ok) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr,
                         "[seed_sweep] FAIL seed=%llu exchanges=%d : %s\n"
                         "  REPLAY: LOCKSTEP_SWEEP_BASE=%llu (or run_ping_pong(%llu, %d))\n",
                         static_cast<unsigned long long>(seed), exchanges, why.c_str(),
                         static_cast<unsigned long long>(kBase),
                         static_cast<unsigned long long>(seed), exchanges);
            if (failed >= 10) {
                std::fprintf(stderr, "[seed_sweep] ... stopping after 10 failures\n");
                break;
            }
        }
    }

    std::printf("[seed_sweep] done: passed=%d failed=%d / %d\n", passed, failed,
                kSeeds);
    if (failed != 0) {
        std::fprintf(stderr, "[seed_sweep] FAILED: %d seed(s) non-deterministic\n",
                     failed);
        return 1;
    }
    std::printf("[seed_sweep] OK — all %d seeds byte-identical on rerun + matched "
                "prediction + invariants.\n",
                passed);
    return 0;
}
