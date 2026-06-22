#pragma once

// KvSim.hpp — Phase 2 batch 2 (stage B). The REUSABLE run harness: it wires up
// N nodes + the sim providers (SimNetwork + SimDisk) under the FULL fault
// envelope (DECISION-A), generates a deterministic seeded client workload
// (C2.5), drives node lifecycle + faults from the SAME seed (C2.3 + §1), feeds
// every client op through the HistoryRecorder (faithful invoke/return virtual
// times + real results), runs the scheduler to quiescence, and returns the
// recorded History.
//
// THE INTEGRATION POINT (for Stage C checkers + the teeth fixture):
//
//   History run_kv_sim(std::uint64_t seed, const KvConfig& cfg = {});
//   History run_kv_sim_checked(std::uint64_t seed, CheckerRunner& runner,
//                              const KvConfig& cfg = {});   // online observe
//
// Both build the SAME system + workload deterministically. Stage C reuses these
// to run THIS (honest) system and a buggy variant; the run is PLUGGABLE — the
// system is constructed behind the IKvSystem interface (see make_system_) so a
// later agent can swap the implementation WITHOUT rewriting this driver.
//
// INVARIANTS (binding):
//   * PURE FUNCTION OF (seed): one SeededRandom drives the workload, the
//     lifecycle/fault schedule, the providers, and buggify. Same seed ⇒
//     byte-identical History AND byte-identical scheduler trace.
//   * BOUNDED: the workload issues a fixed op budget; every client op has a
//     bounded deadline; the run reaches quiescence (run() returns) → no
//     livelock. Inherits the CTest TIMEOUT 90 ceiling.
//   * RECORDER FED FAITHFULLY: on_invoke at submit (clock.now()), on_return at
//     ack/error (clock.now()), real result/error. Recording is PASSIVE.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting order, any nondeterminism.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/harness/CheckerRunner.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/kv/Buggify.hpp>
#include <lockstep/harness/kv/KvSystem.hpp>
#include <lockstep/harness/kv/ReplicatedKvSystem.hpp>

#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

namespace lockstep::harness::kv {

using core::Scheduler;
using core::SimClock;
using core::Task;
using core::Tick;

// Run configuration. Defaults are a small, FAST, fault-heavy scenario that
// still reaches quiescence well under the 90s CTest ceiling. A test dials these
// per-scenario. All knobs are deterministic; nothing here reads a clock.
struct KvConfig {
    std::uint64_t n_nodes = 3;     // replicated nodes (spec ≈3-5)
    std::uint64_t n_clients = 3;   // concurrent client sessions
    std::uint64_t n_keys = 4;      // small keyspace → contention on registers
    std::uint64_t ops_per_client = 20;  // bounded op budget per client

    // Workload op-kind weights (out of 100). Read-heavy with real write/cas mix.
    std::uint32_t read_weight = 45;
    std::uint32_t write_weight = 40;
    std::uint32_t cas_weight = 15;

    // Gap between a client's successive ops (virtual ticks, seeded jitter range).
    Tick client_gap_min = 2;
    Tick client_gap_max = 10;

    // Fault envelope (DECISION-A: FULL). Network knobs are applied bus-wide.
    bool full_envelope = true;
    sim::detail::LinkFaults net_faults{
        /*drop_prob=*/0.05, /*dup_prob=*/0.05, /*reorder_prob=*/0.15,
        /*latency_min=*/1, /*latency_max=*/6, /*reorder_jitter_max=*/8};
    sim::DiskFaultConfig disk_faults{
        /*latency_min=*/1, /*latency_max=*/4, /*io_fault_prob=*/0.03,
        /*torn_write_prob=*/0.03, /*lying_fsync_prob=*/0.03,
        /*bit_rot_prob=*/0.0};

    // Node lifecycle (C2.3): how many crash/recover and partition episodes to
    // schedule across the run, and the window of virtual time they fall in.
    std::uint64_t crash_episodes = 3;
    std::uint64_t partition_episodes = 2;
    std::uint64_t kill_episodes = 0;  // permanent removals (off by default)
    Tick chaos_step_min = 15;
    Tick chaos_step_max = 40;

    // Buggify (C2.4): force rare branches in sim. ON by default for the harness.
    bool buggify = true;
};

namespace detail {

// A single run's owned state. Kept in one struct so the coroutines can hold a
// stable pointer to it for the whole run (the scheduler outlives them).
struct RunState {
    explicit RunState(std::uint64_t seed)
        : rng(seed), clock(sched), bus(sched, rng) {}

    Scheduler sched;
    sim::SeededRandom rng;
    SimClock clock;
    sim::SimNetworkBus bus;
    HistoryRecorder recorder;
    CheckerRunner* runner = nullptr;  // optional online observer (Stage C)
    std::unique_ptr<IKvSystem> system;
    std::uint64_t live_workers = 0;   // client drivers still running
};

// Pick a weighted op kind from the seeded PRNG. Deterministic; no distribution.
inline OpKind pick_kind(core::IRandom& rng, const KvConfig& cfg) {
    const std::uint32_t total =
        cfg.read_weight + cfg.write_weight + cfg.cas_weight;
    const std::uint64_t r = rng.uniform(total == 0 ? 1 : total);
    if (r < cfg.read_weight) {
        return OpKind::Read;
    }
    if (r < cfg.read_weight + cfg.write_weight) {
        return OpKind::Write;
    }
    return OpKind::Cas;
}

// One client session driver. Issues cfg.ops_per_client ops, each recorded
// faithfully through the recorder: on_invoke at submit-time clock.now(),
// on_return at completion clock.now() with the real result/error. Spaces ops by
// a seeded gap so clients interleave. BOUNDED → terminates.
inline Task client_driver(RunState* st, const KvConfig* cfg,
                          std::uint64_t client_id) {
    for (std::uint64_t i = 0; i < cfg->ops_per_client; ++i) {
        // Seeded inter-op gap (lets faults land between a client's ops).
        Tick gap = st->rng.uniform_range(cfg->client_gap_min, cfg->client_gap_max);
        if (gap > 0) {
            co_await st->clock.delay(gap);
        }

        // Build the request: a key in [0, n_keys) and (for write/cas) a value
        // tagged with client+seq so the checker can attribute writes uniquely.
        OpKind kind = pick_kind(st->rng, *cfg);
        std::uint64_t k = st->rng.uniform(cfg->n_keys == 0 ? 1 : cfg->n_keys);
        std::string key = "k" + std::to_string(k);
        std::string value;
        std::string cas_old;
        if (kind == OpKind::Write || kind == OpKind::Cas) {
            value = "c" + std::to_string(client_id) + "_v" + std::to_string(i);
        }
        if (kind == OpKind::Cas) {
            // Compare against either ∅ or a recently-plausible value; seeded.
            // Half the time guess ∅ (often succeeds early), half a self value.
            if (st->rng.chance(0.5)) {
                cas_old = "";
            } else {
                cas_old = "c" + std::to_string(client_id) + "_v" +
                          std::to_string(i == 0 ? 0 : i - 1);
            }
        }

        KvRequest req;
        req.kind = kind;
        req.key = key;
        req.value = value;
        req.cas_old = cas_old;

        // RECORD invoke (V-HIST1): stamp invoke_vt = now, passively.
        const std::uint64_t op_id = st->recorder.on_invoke(
            client_id, kind, key, value, cas_old, st->clock.now());

        // Submit + await the real result.
        KvResult res = co_await st->system->submit(client_id, req);

        // RECORD return (V-HIST1): stamp return_vt = now, real ok/result/error.
        st->recorder.on_return(op_id, res.ok, res.result, res.error,
                               st->clock.now());

        // Online observe (Stage C): fan the just-returned op to the checkers.
        if (st->runner != nullptr) {
            // The recorder's last completed op is this one; surface it.
            const History& h = st->recorder.history();
            // Find this op_id (small history; deterministic linear scan).
            for (const Op& op : h) {
                if (op.op_id == op_id) {
                    st->runner->observe(op);
                    break;
                }
            }
        }
    }
    st->live_workers -= 1;
    co_return;
}

// The chaos driver (node lifecycle C2.3 + network partition §1). Drives a
// seeded schedule of crash/recover, partition/heal, and optional kill episodes
// on virtual time, from the SAME PRNG. Every fault is a pure function of (seed).
// It runs for a bounded number of episodes then stops — it never wedges the run
// (a crashed node is always eventually recovered unless explicitly killed).
inline Task chaos_driver(RunState* st, const KvConfig* cfg) {
    const std::uint64_t n = cfg->n_nodes;
    std::uint64_t crashes_left = cfg->crash_episodes;
    std::uint64_t parts_left = cfg->partition_episodes;
    std::uint64_t kills_left = cfg->kill_episodes;

    // A small set of "currently crashed" nodes we must bring back so the
    // cluster does not permanently lose its leader (keeps the run live).
    std::vector<std::uint64_t> crashed;

    std::uint64_t total_episodes = crashes_left + parts_left + kills_left;
    for (std::uint64_t ep = 0; ep < total_episodes * 2 + 4; ++ep) {
        Tick step =
            st->rng.uniform_range(cfg->chaos_step_min, cfg->chaos_step_max);
        co_await st->clock.delay(step);

        // First, heal/recover anything outstanding roughly half the time so
        // faults are transient (the system gets a chance to make progress).
        if (!crashed.empty() && st->rng.chance(0.6)) {
            std::uint64_t idx = st->rng.uniform(crashed.size());
            std::uint64_t node = crashed[idx];
            st->system->recover_node(node);
            crashed.erase(crashed.begin() + static_cast<std::ptrdiff_t>(idx));
            continue;
        }
        st->bus.heal();  // clear any standing partition before a new episode

        // Choose an episode kind among the remaining budgets, seeded.
        std::uint64_t choice = st->rng.uniform(3);
        if (choice == 0 && crashes_left > 0 && n >= 2) {
            // Crash a node that is not already down. Avoid crashing the LAST
            // live node (keep at least one up so clients can eventually ack).
            std::uint64_t live = n - static_cast<std::uint64_t>(crashed.size());
            if (live >= 2) {
                std::uint64_t node = st->rng.uniform(n);
                bool already = false;
                for (std::uint64_t c : crashed) {
                    if (c == node) {
                        already = true;
                    }
                }
                if (!already) {
                    st->system->crash_node(node);
                    crashed.push_back(node);
                    --crashes_left;
                }
            }
        } else if (choice == 1 && parts_left > 0 && n >= 2) {
            // Partition a seeded non-trivial subset to one side.
            std::vector<std::uint64_t> side_a;
            for (std::uint64_t i = 0; i < n; ++i) {
                if (st->rng.chance(0.5)) {
                    side_a.push_back(i);
                }
            }
            if (!side_a.empty() && side_a.size() < n) {
                st->bus.partition(std::move(side_a));
                --parts_left;
            }
        } else if (choice == 2 && kills_left > 0 && n >= 3) {
            std::uint64_t live = n - static_cast<std::uint64_t>(crashed.size());
            if (live >= 3) {
                std::uint64_t node = st->rng.uniform(n);
                st->system->kill_node(node);
                --kills_left;
            }
        }
    }

    // Final cleanup: heal the network and recover every still-crashed node so
    // the system can quiesce cleanly (any in-flight client retries can ack).
    st->bus.heal();
    for (std::uint64_t node : crashed) {
        st->system->recover_node(node);
    }
    co_return;
}

// Build the system behind the IKvSystem interface. The factory indirection is
// the PLUGGABILITY seam: a later agent supplies a different make_system to swap
// in a known-buggy variant behind this same driver.
using SystemFactory = std::function<std::unique_ptr<IKvSystem>(
    Scheduler&, SimClock&, core::IRandom&, sim::SimNetworkBus&, Buggify&,
    const KvConfig&)>;

inline std::unique_ptr<IKvSystem> default_factory(
    Scheduler& sched, SimClock& clock, core::IRandom& rng,
    sim::SimNetworkBus& bus, Buggify& buggify, const KvConfig& cfg) {
    auto sys = std::make_unique<ReplicatedKvSystem>(
        sched, clock, rng, bus, buggify, cfg.n_nodes, cfg.disk_faults);
    sys->start();
    return sys;
}

// Core run routine shared by the public entry points. Returns the recorded
// History. If `runner` is non-null, ops are observed online (Stage C surface).
inline History run_kv_sim_impl(std::uint64_t seed, const KvConfig& cfg,
                               CheckerRunner* runner,
                               const SystemFactory& factory) {
    detail::RunState st(seed);
    st.runner = runner;

    // Apply the FULL fault envelope (DECISION-A) bus-wide before any traffic.
    if (cfg.full_envelope) {
        st.bus.set_faults(cfg.net_faults);
    }

    Buggify buggify(st.rng, cfg.buggify);

    // Construct the system behind the interface (pluggable seam).
    st.system = factory(st.sched, st.clock, st.rng, st.bus, buggify, cfg);

    // Spawn the client drivers (the workload) + the chaos driver.
    st.live_workers = cfg.n_clients;
    for (std::uint64_t c = 0; c < cfg.n_clients; ++c) {
        st.sched.spawn(detail::client_driver(&st, &cfg, c));
    }
    st.sched.spawn(detail::chaos_driver(&st, &cfg));

    // Drive to quiescence: the scheduler returns when no work and no timers
    // remain. The workload is bounded and every op has a deadline, so this
    // terminates (no livelock). Server loops end when their nodes are
    // crashed/killed at run end (chaos_driver recovers, then traffic drains;
    // any node still recv-parked at the end simply has no more deliveries, so
    // the scheduler's timer/ready queues empty and run() returns).
    st.sched.run();

    // Return the faithful, totally-ordered History (by return_vt, seq).
    return st.recorder.history();
}

}  // namespace detail

// ---- PUBLIC RUN API (Stage C + teeth integration point) -------------------

// Run the toy replicated KV system under the full fault envelope for `seed` and
// return the recorded History. Pure function of (seed, cfg): same seed ⇒
// byte-identical History.
[[nodiscard]] inline History run_kv_sim(std::uint64_t seed,
                                        const KvConfig& cfg = {}) {
    return detail::run_kv_sim_impl(seed, cfg, nullptr, detail::default_factory);
}

// Same, but fans each completed op to `runner` online (V-CHK3) as well as
// returning the History for the after-the-run checks. Stage C uses this to run
// online + offline checkers against the system.
[[nodiscard]] inline History run_kv_sim_checked(std::uint64_t seed,
                                                CheckerRunner& runner,
                                                const KvConfig& cfg = {}) {
    return detail::run_kv_sim_impl(seed, cfg, &runner, detail::default_factory);
}

// Pluggability seam: run with a CUSTOM system factory (e.g. a known-buggy KV
// variant for the harness-has-teeth gate) behind the SAME workload + lifecycle
// + fault driver. The teeth fixture agent supplies its own factory here.
[[nodiscard]] inline History run_kv_sim_with(std::uint64_t seed,
                                             const detail::SystemFactory& factory,
                                             CheckerRunner* runner = nullptr,
                                             const KvConfig& cfg = {}) {
    return detail::run_kv_sim_impl(seed, cfg, runner, factory);
}

}  // namespace lockstep::harness::kv
