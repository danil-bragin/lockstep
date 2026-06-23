// prod_provider_test.cpp — Phase 7 S2 driver. Runs the REUSABLE S1 universal
// conformance suite (tests/provider_conformance/ContractConformance.hpp) against
// the PROD providers (ProdRandom + ProdClock), proves sim==prod byte-identity of
// the PRNG stream, and proves the record-replay TRACE round-trips byte-identical
// (V-PROD-REPLAY).
//
// REUSES the S1 check functions VERBATIM — `conformance::universal::*` are called
// here through Prod*Factory adapters with the SAME member shape as the S1 sim
// factories; not a single check is forked. What runs against prod NOW vs what is
// DEFERRED:
//   * ProdRandom: ALL tier-A IRandom checks (determinism, different-seed, range,
//     no-modulo-bias, chance) run now and must pass.
//   * ProdClock: only the now()-monotonic check runs now. delay()/timer-firing
//     needs the S4 epoll reactor, so the full clock contract is DEFERRED to S4 —
//     we run a dedicated now()-monotonic check here, not check_clock_contract
//     (which awaits delay() the prod clock cannot satisfy until S4).
//
// This is NON-provider code (a test) → the forbidden-call lint scans it; it must
// touch NO <chrono>/<random>/<thread>. All real time/entropy stays inside
// providers/prod/. The prod seeds here are fixed literals (deterministic test).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdClock.hpp>
#include <lockstep/prod/ProdRandom.hpp>
#include <lockstep/prod/ReplayTrace.hpp>
#include <lockstep/sim/SeededRandom.hpp>

#include "provider_conformance/ContractConformance.hpp"

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace sim = lockstep::sim;
using conformance::Report;

// ===========================================================================
// PROD FACTORIES — same member shape as the S1 sim factories, so the IDENTICAL
// universal checks run against prod.
// ===========================================================================

// IRandom factory: a fresh ProdRandom per call (deterministic seed path).
struct ProdRandomFactory {
    [[nodiscard]] std::unique_ptr<core::IRandom> make(std::uint64_t seed) {
        return std::make_unique<prod::ProdRandom>(seed);
    }
};

// IClock factory: a ProdClock. The universal clock check binds a clock to a
// scheduler; ProdClock ignores the scheduler (real time, S4 reactor pending) but
// the member shape matches SimClockFactory so the factory plugs in identically.
struct ProdClockFactory {
    std::vector<std::unique_ptr<prod::ProdClock>> clocks{};
    core::IClock& clock(core::Scheduler& /*sched*/) {
        clocks.push_back(std::make_unique<prod::ProdClock>());
        return *clocks.back();
    }
};

std::string render(const Report& rep) {
    std::string out;
    for (const Report::Item& it : rep.items) {
        out += (it.pass ? "PASS " : "FAIL ") + it.name;
        if (!it.pass && !it.detail.empty()) {
            out += "  -- " + it.detail;
        }
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// (1) ProdRandom — full tier-A IRandom contract (reuses S1 verbatim).
// ---------------------------------------------------------------------------
Report run_prod_random_contract() {
    Report rep;
    ProdRandomFactory factory;
    conformance::universal::check_random_contract(factory, rep);
    return rep;
}

// ---------------------------------------------------------------------------
// (2) ProdClock — ONLY the now()-monotonic slice of the universal clock contract
// runs now (delay/timers deferred to S4). We exercise now() across many samples
// and assert it never decreases — the SAME property check_clock_contract asserts
// for sim, just without the delay() awaits the prod clock can't satisfy yet.
// ---------------------------------------------------------------------------
bool prod_clock_now_monotonic() {
    prod::ProdClock clock;
    core::Tick last = clock.now();
    for (int i = 0; i < 10000; ++i) {
        core::Tick t = clock.now();
        if (t < last) {
            return false;
        }
        last = t;
    }
    return true;
}

// ---------------------------------------------------------------------------
// (3) sim==prod byte-identity: for several seeds the ProdRandom and SeededRandom
// streams (next + uniform + uniform_range + chance) are IDENTICAL. This is what
// makes record-replay sound — replaying a prod seed in sim reproduces the stream.
// ---------------------------------------------------------------------------
bool sim_eq_prod_for_seed(std::uint64_t seed) {
    sim::SeededRandom s(seed);
    prod::ProdRandom p(seed);
    for (int i = 0; i < 1000; ++i) {
        if (s.next() != p.next()) {
            return false;
        }
    }
    // Also cross-check the derived helpers stay in lockstep on a fresh pair.
    sim::SeededRandom s2(seed);
    prod::ProdRandom p2(seed);
    for (int i = 0; i < 1000; ++i) {
        if (s2.uniform(7) != p2.uniform(7)) {
            return false;
        }
        if (s2.uniform_range(-1000, 1000) != p2.uniform_range(-1000, 1000)) {
            return false;
        }
        if (s2.chance(0.3) != p2.chance(0.3)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// (4) RECORD -> REPLAY byte-identical (V-PROD-REPLAY proof). Script a prod
// Clock+Random session through the RECORDING wrappers, capture the observations,
// then REPLAY the trace and assert the replayed observations are byte-identical.
// ---------------------------------------------------------------------------
struct ReplayProof {
    bool ok = false;
    std::size_t records = 0;
    std::string recorded_obs;
    std::string replayed_obs;
    std::string trace_render;
};

// Run a scripted session against a (clock, random) pair, returning a stable
// string of every boundary observation in order. The SAME script is run twice:
// once over the recording prod providers, once over the replay providers.
template <class Clock, class Random>
std::string run_session(Clock& clock, Random& rng) {
    std::string obs;
    // Interleave now() reads and random draws — the order is part of the trace's
    // meaning for the Clock (each now() is a sample in arm order).
    for (int round = 0; round < 5; ++round) {
        obs += "now=" + std::to_string(clock.now()) + " ";
        obs += "next=" + std::to_string(rng.next()) + " ";
        obs += "u7=" + std::to_string(rng.uniform(7)) + " ";
        obs += "now=" + std::to_string(clock.now()) + " ";
        obs += "rng_range=" + std::to_string(rng.uniform_range(-50, 50)) + "\n";
    }
    return obs;
}

ReplayProof run_record_replay() {
    ReplayProof proof;

    constexpr std::uint64_t kSeed = 0xC0FF'EE12'3456'789AULL;

    // --- RECORD: scripted prod session through the recording wrappers ----------
    prod::ReplayTrace trace;
    prod::ProdClock prod_clock;
    prod::ProdRandom prod_rng(kSeed);
    prod::RecordingClock rec_clock(prod_clock, trace);
    prod::RecordingRandom rec_rng(prod_rng, prod_rng.seed(), trace);
    proof.recorded_obs = run_session(rec_clock, rec_rng);
    proof.trace_render = trace.render();
    proof.records = trace.size();

    // --- REPLAY: same script over the replay providers fed from the trace ------
    // ReplayRandom rebuilds the stream from the recorded seed via the sim engine
    // (byte-identical to prod); ReplayClock replays the recorded now() samples.
    ProdRandomFactory rfac; // any splitmix64 factory works — algo is shared
    prod::ReplayClock replay_clock(trace);
    prod::ReplayRandom<ProdRandomFactory> replay_rng(trace, rfac);
    proof.replayed_obs = run_session(replay_clock, replay_rng);

    proof.ok = proof.recorded_obs == proof.replayed_obs;
    return proof;
}

} // namespace

int main() {
    std::printf("[prod_provider_test] Phase 7 S2 — prod IRandom/IClock + record-replay\n");

    bool all = true;

    // (1) ProdRandom — full tier-A contract (S1 checks verbatim).
    const Report rr = run_prod_random_contract();
    std::printf("\n=== (1) ProdRandom UNIVERSAL CONTRACT (tier-A, S1 verbatim) ===\n%s",
                render(rr).c_str());
    std::printf("prod random: %zu checks, %zu failures\n", rr.items.size(), rr.failures());
    all = all && rr.all_pass();

    // (2) ProdClock — now()-monotonic (the only clock check that runs pre-S4).
    const bool clock_mono = prod_clock_now_monotonic();
    std::printf("\n=== (2) ProdClock now()-monotonic (delay/timers DEFERRED to S4) ===\n");
    std::printf("%s clock/now-monotonic\n", clock_mono ? "PASS" : "FAIL");
    all = all && clock_mono;

    // (3) sim==prod byte-identical PRNG stream for several seeds.
    const std::uint64_t seeds[] = {0ULL, 1ULL, 0xA5A5'1234'DEAD'BEEFULL,
                                   0x9E37'79B9'7F4A'7C15ULL, 0xFFFF'FFFF'FFFF'FFFFULL};
    bool sim_eq_prod = true;
    std::printf("\n=== (3) sim==prod PRNG byte-identity (record-replay soundness) ===\n");
    for (std::uint64_t s : seeds) {
        const bool eq = sim_eq_prod_for_seed(s);
        sim_eq_prod = sim_eq_prod && eq;
        std::printf("%s seed=%llu sim==prod sequence\n", eq ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(s));
    }
    all = all && sim_eq_prod;

    // (4) RECORD -> REPLAY byte-identical.
    const ReplayProof proof = run_record_replay();
    std::printf("\n=== (4) RECORD -> REPLAY byte-identical (V-PROD-REPLAY) ===\n");
    std::printf("trace (%zu records):\n%s", proof.records, proof.trace_render.c_str());
    std::printf("%s record-replay observations byte-identical\n",
                proof.ok ? "PASS" : "FAIL");
    if (!proof.ok) {
        std::fprintf(stderr, "--- recorded ---\n%s\n--- replayed ---\n%s\n",
                     proof.recorded_obs.c_str(), proof.replayed_obs.c_str());
    }
    all = all && proof.ok;

    std::printf("\n[prod_provider_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
