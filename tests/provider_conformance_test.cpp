// provider_conformance_test.cpp — Phase 7 S1 driver. Runs the REUSABLE boundary-
// provider contract conformance suite (tests/provider_conformance/
// ContractConformance.hpp) against the SIM providers, validating BOTH the harness
// (sim already conforms) and the contracts themselves (V-PROD-CONTRACT).
//
// TWO TIERS (the whole point — see ContractConformance.hpp):
//   (A) UNIVERSAL CONTRACT — `conformance::universal::*` checks every impl (sim
//       AND future prod) must pass. Run here against sim factories. A prod driver
//       in S2–S4 builds Prod*Factory and calls the SAME functions; nothing else
//       changes. These assert ONLY the documented happy-path + error contract.
//   (B) SIM-ONLY fault behaviours — torn writes / lying fsync / drop / partition /
//       crash. NOT part of the universal contract; a prod impl neither can nor
//       should reproduce them. Kept in a clearly-labelled section below that runs
//       ONLY against sim and is NEVER handed a prod factory. (The exhaustive (B)
//       coverage lives in sim_disk_test / sim_network_test; here we keep a minimal
//       smoke so the A/B boundary is visible in one place.)
//
// Determinism: the universal suite is run TWICE and the rendered Report must be
// byte-identical (it is a pure function of the threaded seeds). All time is
// virtual (SimClock); all randomness is sim::SeededRandom. This is non-provider
// code → the forbidden-call lint scans it; no <chrono>/<thread>/<random>.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include "provider_conformance/ContractConformance.hpp"

namespace {

namespace core = lockstep::core;
namespace sim = lockstep::sim;
using conformance::Report;

// ===========================================================================
// SIM FACTORIES — the adapters the universal suite is parameterized over. A prod
// driver in S2–S4 provides Prod* analogues with the SAME member shape; the
// `conformance::universal::*` checks are then identical.
// ===========================================================================

// IClock factory: a SimClock bound to the caller's scheduler. The SimClock is
// owned here (kept alive for the whole suite call); we hand back a reference.
struct SimClockFactory {
    std::vector<std::unique_ptr<core::SimClock>> clocks{};
    core::IClock& clock(core::Scheduler& sched) {
        clocks.push_back(std::make_unique<core::SimClock>(sched));
        return *clocks.back();
    }
};

// IRandom factory: a fresh seeded PRNG per call.
struct SeededRandomFactory {
    [[nodiscard]] std::unique_ptr<core::IRandom> make(std::uint64_t seed) {
        return std::make_unique<sim::SeededRandom>(seed);
    }
};

// IDisk factory: an HONEST SimDisk (all fault probs 0 — only latency). The
// universal contract is the no-fault durability promise; faults are tier (B).
struct SimDiskFactory {
    [[nodiscard]] std::unique_ptr<core::IDisk> make(core::Scheduler& sched,
                                                    core::IClock& clock,
                                                    core::IRandom& rng) {
        sim::DiskFaultConfig honest{}; // defaults: 0 fault probs, only latency
        return std::make_unique<sim::SimDisk>(sched, clock, rng, honest);
    }
};

// INetwork factory: an owning bus handle. It bundles the deterministic core
// (Scheduler/SimClock/SeededRandom) the bus needs, built HONEST (no drop/dup/
// reorder/partition). node(id) returns a stable per-node INetwork& (the handles
// are owned here so the reference outlives the suite call). A prod NetworkFactory
// exposes the SAME shape: scheduler(), add_nodes(), node(id)->INetwork&.
struct SimNetworkBusHandle {
    core::Scheduler sched{};
    core::SimClock clock{sched};
    sim::SeededRandom rng;
    sim::SimNetworkBus bus{sched, rng};
    std::vector<std::unique_ptr<sim::SimNetwork>> handles{};

    explicit SimNetworkBusHandle(std::uint64_t seed) : rng(seed) {
        // HONEST link: deterministic unit latency, no drop/dup/reorder.
        bus.set_faults({.drop_prob = 0.0,
                        .dup_prob = 0.0,
                        .reorder_prob = 0.0,
                        .latency_min = 1,
                        .latency_max = 1,
                        .reorder_jitter_max = 0});
    }

    core::Scheduler& scheduler() noexcept { return sched; }
    core::SimClock& sim_clock() noexcept { return clock; }
    void add_nodes(std::initializer_list<std::uint64_t> ids) { bus.add_nodes(ids); }
    core::INetwork& node(std::uint64_t id) {
        handles.push_back(std::make_unique<sim::SimNetwork>(bus.node(id)));
        return *handles.back();
    }
};

struct SimNetworkFactory {
    [[nodiscard]] SimNetworkBusHandle make(std::uint64_t seed) {
        return SimNetworkBusHandle{seed};
    }
};

// ===========================================================================
// Run the WHOLE universal suite against the sim factories into one Report. Pure
// function of the seeds threaded inside the checks ⇒ re-running yields the same
// Report (used for the determinism assertion).
// ===========================================================================
Report run_universal_suite() {
    Report rep;

    SimClockFactory clock_factory;
    SeededRandomFactory random_factory;
    SimDiskFactory disk_factory;
    SimNetworkFactory network_factory;

    // (A) UNIVERSAL CONTRACT — the SAME calls a prod driver makes in S2–S4.
    conformance::universal::check_clock_contract(clock_factory, rep);
    conformance::universal::check_random_contract(random_factory, rep);
    conformance::universal::check_disk_contract(clock_factory, random_factory,
                                                disk_factory, rep);

    // The network check takes an already-built HONEST bus + a pacing clock bound
    // to that bus's scheduler. We construct it here and hand both in — the SAME
    // shape a prod driver uses (build the prod bus, pass it + its clock).
    {
        SimNetworkBusHandle bus = network_factory.make(0x1357'2468'9BDF'ACE0ULL);
        bus.add_nodes({0, 1});
        core::IClock& pacing = bus.sim_clock();
        conformance::universal::check_network_contract_on(bus, &pacing, rep);
    }

    return rep;
}

// Render a Report to a stable string (used for the byte-identical determinism
// check and for the human-facing dashboard).
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

// ===========================================================================
// (B) SIM-ONLY tier — a MINIMAL smoke that the sim's fault injection is real and
// distinct from the universal contract. This NEVER runs against a prod factory;
// a prod provider is not expected (or able) to reproduce torn writes. The full
// (B) battery lives in sim_disk_test.cpp / sim_network_test.cpp. We keep one
// assertion here purely to make the A/B boundary visible in one file.
// ===========================================================================
bool sim_only_torn_write_is_partial(std::uint64_t seed) {
    core::Scheduler sched;
    core::SimClock clock(sched);
    sim::SeededRandom rng(seed);
    sim::DiskFaultConfig cfg;
    cfg.torn_write_prob = 1.0; // FORCE the tear — a sim-only injected fault
    sim::SimDisk disk(sched, clock, rng, cfg);

    struct State {
        std::size_t requested = 0;
        std::size_t landed = 0;
        bool ran = false;
    } st;
    const std::vector<std::byte> p = conformance::payload(32, 0x20);
    st.requested = p.size();

    auto driver = [&](sim::SimDisk& d, State& s) -> core::Task {
        core::Offset off = 0;
        co_await d.append(conformance::view_of(p), off);
        s.landed = d.staged_len();
        s.ran = true;
        co_return;
    };
    sched.spawn(driver(disk, st));
    sched.run();

    // A torn write lands a STRICT partial prefix — the sim-only behaviour a prod
    // disk must NOT exhibit (which is exactly why it is tier B, not universal).
    return st.ran && st.landed >= 1 && st.landed < st.requested;
}

} // namespace

int main() {
    constexpr std::uint64_t kSeed = 0x9E37'79B9'7F4A'7C15ULL;
    std::printf("[provider_conformance_test] seed=%llu (replay any failure with this seed)\n",
                static_cast<unsigned long long>(kSeed));

    // (A) UNIVERSAL CONTRACT vs sim — run, render, tally.
    const Report rep_a = run_universal_suite();
    const std::string rendered_a = render(rep_a);
    std::printf("\n=== (A) UNIVERSAL CONTRACT vs SIM ===\n%s", rendered_a.c_str());
    std::printf("universal: %zu checks, %zu failures\n", rep_a.items.size(),
                rep_a.failures());

    // (A') DETERMINISM — the universal suite is a pure fn of its seeds: re-run and
    // require a byte-identical rendered Report.
    const Report rep_b = run_universal_suite();
    const std::string rendered_b = render(rep_b);
    const bool deterministic = rendered_a == rendered_b;
    std::printf("\n=== (A') DETERMINISM (run twice, byte-identical Report) ===\n%s\n",
                deterministic ? "PASS byte-identical" : "FAIL diverged");
    if (!deterministic) {
        std::fprintf(stderr, "--- run A ---\n%s\n--- run B ---\n%s\n", rendered_a.c_str(),
                     rendered_b.c_str());
    }

    // (B) SIM-ONLY tier smoke — clearly separated; NEVER runs against prod.
    const bool sim_torn = sim_only_torn_write_is_partial(kSeed);
    std::printf("\n=== (B) SIM-ONLY fault behaviours (NOT universal; sim-only) ===\n");
    std::printf("%s sim/torn-write-is-partial (a prod disk must NOT reproduce this)\n",
                sim_torn ? "PASS" : "FAIL");

    const bool all = rep_a.all_pass() && deterministic && sim_torn;
    std::printf("\n[provider_conformance_test] %s (universal_failures=%zu, "
                "deterministic=%d, sim_only=%d)\n",
                all ? "ALL PASS" : "FAILED", rep_a.failures(),
                deterministic ? 1 : 0, sim_torn ? 1 : 0);
    return all ? 0 : 1;
}
