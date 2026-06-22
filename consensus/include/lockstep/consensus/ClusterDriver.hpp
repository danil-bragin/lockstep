#pragma once

// ClusterDriver.hpp — Phase 4 Stage M. THE CLUSTER DRIVER.
//
// Spins up N ConsensusNode replicas (default 3, configurable 5) — built behind
// the seam factory (ConsensusNode.hpp) — on ONE shared SimNetworkBus, each with
// its OWN SimDisk, all on one deterministic Scheduler. It drives:
//   * a SEEDED CLIENT WORKLOAD: clients submit unique values to the believed
//     leader, then the driver watches the cluster to learn if/when each value
//     COMMITS (it derives commit from the observables, exactly as
//     specs/Consensus.tla's AdvanceCommitIndex governs commitIndex — never from
//     the node "telling" it committed).
//   * a SEEDED FAULT SCHEDULE: partition/heal, crash/restart, and the bus's
//     message reorder / drop / dup faults — all from the SAME IRandom.
//   * SNAPSHOTTING: at every observed step it COPIES out every node's
//     role/current_term/log/commit_index (V-RKV1: deep copy, no live span held),
//     producing the ObservedRun the conformance checkers judge.
//
// PURE FUNCTION OF (seed): one SeededRandom drives the workload, the fault
// schedule, the bus faults, and the per-node election jitter. Same seed ⇒
// byte-identical ObservedRun AND byte-identical scheduler trace. Every run logs
// its seed; replay is byte-identical (the gate proves it with an external diff).
//
// BOUNDED: a fixed submit budget; a bounded number of fault episodes; a hard
// virtual-time horizon on each "settle" wait. A REAL Raft node runs a perpetual
// heartbeat timer, so the scheduler NEVER quiesces (run() would spin forever
// advancing virtual time). Instead the driver computes a virtual-time DEADLINE
// (the workload + fault + settle horizon below) and drives with
// Scheduler::run_until(deadline): the cluster is observed/snapshotted up to the
// deadline, then the driver STOPS (heartbeats still pending is fine) — no
// livelock; inherits the CTest TIMEOUT 90 ceiling. Pure fn of (seed): the
// deadline is a deterministic fn of cfg, identical across replays.
//
// FORBIDDEN here (consensus/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting output, any nondeterminism.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>

namespace lockstep::consensus {

using core::Scheduler;
using core::SimClock;
using core::Task;
using core::Tick;

// Run configuration. Defaults are a small, FAST, fault-heavy scenario that still
// reaches quiescence well under the 90s CTest ceiling. Every knob is
// deterministic; nothing here reads a clock.
struct ClusterConfig {
    std::uint64_t n_nodes = 3;     // 3 (default) or 5 (spec instance Server = 3)
    std::uint64_t n_clients = 2;   // client sessions issuing submits
    std::uint64_t submits_per_client = 8;  // bounded submit budget per client

    // Per-node timing (passed into NodeConfig). Election timeout is RANDOMIZED in
    // [min,max] from IRandom to break symmetric split votes (a leader must emerge).
    Tick election_timeout_min = 15;
    Tick election_timeout_max = 30;
    Tick heartbeat_interval = 5;
    Tick request_deadline = 400;

    // How long the driver lets the cluster settle (advance virtual time) after a
    // submit / a fault, before snapshotting + moving on. Bounded.
    Tick settle_ticks = 60;
    Tick client_gap_min = 5;
    Tick client_gap_max = 20;

    // Network fault envelope (applied bus-wide). Reorder/drop/dup exercise the
    // spec's lossy-bus assumptions; a leader must still make progress.
    sim::detail::LinkFaults net_faults{
        /*drop_prob=*/0.05, /*dup_prob=*/0.05, /*reorder_prob=*/0.15,
        /*latency_min=*/1, /*latency_max=*/6, /*reorder_jitter_max=*/8};
    sim::DiskFaultConfig disk_faults{
        /*latency_min=*/1, /*latency_max=*/4, /*io_fault_prob=*/0.0,
        /*torn_write_prob=*/0.0, /*lying_fsync_prob=*/0.0, /*bit_rot_prob=*/0.0};

    // Fault schedule: how many partition + crash episodes across the run.
    std::uint64_t partition_episodes = 2;
    std::uint64_t crash_episodes = 2;
    bool full_envelope = true;  // false ⇒ pristine bus + honest disk (calm baseline)
};

namespace detail {

// One run's owned state. Held in one struct so the coroutines can keep a stable
// pointer to it for the whole run (the scheduler outlives them). Disks are heap-
// owned (one per node) so their addresses are stable across the run.
struct ClusterRunState {
    explicit ClusterRunState(std::uint64_t seed)
        : rng(seed), clock(sched), bus(sched, rng) {}

    Scheduler sched;
    sim::SeededRandom rng;
    SimClock clock;
    sim::SimNetworkBus bus;

    std::vector<std::unique_ptr<sim::SimDisk>> disks;
    std::vector<sim::SimNetwork> nets;  // per-node INetwork handles (value type)
    std::vector<std::unique_ptr<ConsensusNode>> nodes;
    std::vector<bool> live;             // mirror of crash/restart state (by index)

    ObservedRun run;
    std::uint64_t next_op_id = 1;
    std::uint64_t step = 0;
};

// Snapshot every node's observables into one ClusterSnapshot (deep copy; V-RKV1).
inline void snapshot(ClusterRunState* st) {
    ClusterSnapshot snap;
    snap.vt = st->clock.now();
    snap.step = st->step++;
    for (std::size_t i = 0; i < st->nodes.size(); ++i) {
        NodeSnapshot ns;
        ns.node_id = st->nodes[i]->id();
        ns.live = st->live[i];
        if (ns.live) {
            ns.role = st->nodes[i]->role();
            ns.term = st->nodes[i]->current_term();
            ns.commit_index = st->nodes[i]->commit_index();
            const std::span<const LogEntry> lg = st->nodes[i]->log();
            ns.log.assign(lg.begin(), lg.end());  // deep copy out
        } else {
            // A crashed node serves nothing; record an empty/idle snapshot so the
            // checkers skip it (ElectionSafety/LogMatching only read live nodes).
            ns.role = Role::Follower;
            ns.term = 0;
            ns.commit_index = 0;
        }
        snap.nodes.push_back(std::move(ns));
    }
    st->run.snapshots.push_back(std::move(snap));
}

// Advance virtual time by `ticks` while snapshotting at sub-steps so the
// conformance checkers see the cluster's evolution at fine granularity. The
// scheduler is run to quiescence after each delay, then a snapshot is taken.
inline Task settle_and_snapshot(ClusterRunState* st, Tick ticks, Tick stride) {
    Tick elapsed = 0;
    while (elapsed < ticks) {
        Tick step = stride < (ticks - elapsed) ? stride : (ticks - elapsed);
        if (step > 0) {
            co_await st->clock.delay(step);
        }
        elapsed += step;
        snapshot(st);
    }
    co_return;
}

// Find the believed leader index right now (lowest-id live Leader); UINT64_MAX if
// none. The driver submits to whatever node currently believes it is leader; the
// node's submit() enforces the spec ClientRequest guard (only a leader accepts).
inline std::uint64_t believed_leader(ClusterRunState* st) {
    for (std::size_t i = 0; i < st->nodes.size(); ++i) {
        if (st->live[i] && st->nodes[i]->role() == Role::Leader) {
            return i;
        }
    }
    return UINT64_MAX;
}

// Wait (bounded) for a Leader to emerge, snapshotting as we go. Returns its index
// or UINT64_MAX if none emerged within the budget.
inline Task await_leader(ClusterRunState* st, const ClusterConfig* cfg,
                         Tick budget, std::uint64_t* out_leader) {
    Tick waited = 0;
    const Tick stride = cfg->election_timeout_max;
    *out_leader = believed_leader(st);
    while (*out_leader == UINT64_MAX && waited < budget) {
        co_await st->clock.delay(stride);
        waited += stride;
        snapshot(st);
        *out_leader = believed_leader(st);
    }
    co_return;
}

// One client session: submit unique values to the believed leader and watch the
// cluster commit them. BOUNDED → terminates. Records each as a SubmitObservation.
inline Task client_driver(ClusterRunState* st, const ClusterConfig* cfg,
                          std::uint64_t client_id) {
    for (std::uint64_t i = 0; i < cfg->submits_per_client; ++i) {
        Tick gap = st->rng.uniform_range(cfg->client_gap_min, cfg->client_gap_max);
        if (gap > 0) {
            co_await st->clock.delay(gap);
        }

        // Ensure a leader exists (elections may be in progress after a fault).
        std::uint64_t li = UINT64_MAX;
        {
            Task t = await_leader(st, cfg, cfg->election_timeout_max * 4, &li);
            co_await std::move(t);
        }

        SubmitObservation obs;
        obs.op_id = st->next_op_id++;
        obs.client_id = client_id;
        obs.value = "c" + std::to_string(client_id) + "_v" + std::to_string(i) +
                    "_op" + std::to_string(obs.op_id);
        obs.invoke_vt = st->clock.now();

        if (li != UINT64_MAX) {
            SubmitResult r = st->nodes[li]->submit(obs.value);
            obs.accepted = r.accepted;
            obs.term = r.term;
            obs.index = r.index;
        }

        // Let the cluster replicate + commit, snapshotting as we go. We learn
        // commitment by OBSERVING: the leader's (and a quorum's) commit_index
        // reaching obs.index with the entry there still holding obs.value.
        if (obs.accepted) {
            Tick waited = 0;
            const Tick stride = cfg->heartbeat_interval * 2;
            while (waited < cfg->request_deadline && !obs.committed) {
                if (stride > 0) {
                    co_await st->clock.delay(stride);
                }
                waited += stride;
                snapshot(st);
                // Committed iff SOME live node has commit_index >= obs.index AND
                // the entry it holds at obs.index has our value (a different value
                // means a stale-leader entry was overwritten — not our commit).
                for (std::size_t n = 0; n < st->nodes.size(); ++n) {
                    if (!st->live[n]) {
                        continue;
                    }
                    if (st->nodes[n]->commit_index() >= obs.index) {
                        const std::span<const LogEntry> lg = st->nodes[n]->log();
                        if (obs.index <= lg.size() &&
                            lg[obs.index - 1].value == obs.value) {
                            obs.committed = true;
                            break;
                        }
                    }
                }
            }
        }
        obs.return_vt = st->clock.now();
        st->run.submits.push_back(std::move(obs));
    }
    co_return;
}

// The fault driver: a seeded schedule of partition/heal + crash/restart, on
// virtual time, from the SAME PRNG. Bounded episodes; ALWAYS heals + restarts at
// the end so the cluster can quiesce (no permanently-wedged run).
inline Task fault_driver(ClusterRunState* st, const ClusterConfig* cfg) {
    const std::uint64_t n = cfg->n_nodes;
    std::uint64_t parts_left = cfg->partition_episodes;
    std::uint64_t crashes_left = cfg->crash_episodes;
    std::vector<std::uint64_t> crashed;  // indices currently crashed

    const std::uint64_t episodes = (parts_left + crashes_left) * 2 + 2;
    for (std::uint64_t ep = 0; ep < episodes; ++ep) {
        Tick step = st->rng.uniform_range(cfg->election_timeout_max,
                                          cfg->election_timeout_max * 3);
        co_await st->clock.delay(step);
        snapshot(st);

        // Recover an outstanding crash roughly half the time (transient faults).
        if (!crashed.empty() && st->rng.chance(0.6)) {
            std::uint64_t idx = st->rng.uniform(crashed.size());
            std::uint64_t node = crashed[idx];
            st->disks[node]->recover();
            st->nodes[node]->restart();
            st->live[node] = true;
            crashed.erase(crashed.begin() + static_cast<std::ptrdiff_t>(idx));
            snapshot(st);
            continue;
        }
        st->bus.heal();  // clear any standing partition before a new episode

        std::uint64_t choice = st->rng.uniform(2);
        if (choice == 0 && crashes_left > 0 && n >= 3) {
            // Crash a node, but keep a MAJORITY live (so the cluster can still
            // elect + commit — a permanently-no-quorum run would just time out).
            std::uint64_t live_ct =
                n - static_cast<std::uint64_t>(crashed.size());
            if (live_ct > (n / 2) + 1) {
                std::uint64_t node = st->rng.uniform(n);
                bool already = false;
                for (std::uint64_t c : crashed) {
                    if (c == node) {
                        already = true;
                    }
                }
                if (!already && st->live[node]) {
                    st->nodes[node]->crash();
                    st->disks[node]->crash();
                    st->live[node] = false;
                    crashed.push_back(node);
                    --crashes_left;
                    snapshot(st);
                }
            }
        } else if (choice == 1 && parts_left > 0 && n >= 2) {
            // Partition a seeded non-trivial minority to one side (the majority
            // side keeps quorum so progress continues; heals next episode).
            std::vector<std::uint64_t> side_a;
            for (std::uint64_t i = 0; i < n; ++i) {
                if (st->rng.chance(0.4)) {
                    side_a.push_back(i);
                }
            }
            if (!side_a.empty() && side_a.size() < n) {
                st->bus.partition(std::move(side_a));
                --parts_left;
                snapshot(st);
            }
        }
    }

    // Final cleanup: heal + restart everything so the cluster quiesces cleanly.
    st->bus.heal();
    for (std::uint64_t node : crashed) {
        st->disks[node]->recover();
        st->nodes[node]->restart();
        st->live[node] = true;
    }
    snapshot(st);
    co_return;
}

// Reconstruct the FINAL committed log: the longest committed prefix any node ever
// witnessed, each slot's (term,value) the first committed entry seen there. (The
// safety checkers prove this is consistent; this is the cross-check payload.)
inline void finalize_committed_log(ClusterRunState* st) {
    std::vector<LogEntry> seq;
    std::vector<bool> known;
    for (const ClusterSnapshot& snap : st->run.snapshots) {
        for (const NodeSnapshot& nd : snap.nodes) {
            for (Index i = 1; i <= nd.commit_index && i <= nd.log.size(); ++i) {
                if (i > seq.size()) {
                    seq.resize(i);
                    known.resize(i, false);
                }
                if (!known[i - 1]) {
                    seq[i - 1] = nd.log[i - 1];
                    known[i - 1] = true;
                }
            }
        }
    }
    st->run.committed_log = std::move(seq);
}

// Compute the virtual-time DEADLINE the driver runs to. A deterministic upper
// bound (pure fn of cfg) on how long the longest concurrent coroutine chain can
// take, PLUS a settle margin — so every client session, the fault schedule, and
// the final settle all have room to complete within [0, deadline]. The driver
// then STOPS (a real node's perpetual heartbeat timer stays pending past here;
// that is fine — we have already snapshotted the whole evolution).
inline Tick compute_deadline(const ClusterConfig* cfg) {
    // Worst-case per-submit chain: a client gap, then waiting for a leader to
    // (re)emerge after a fault, then the full replication/commit deadline. The
    // await_leader budget inside client_driver is election_timeout_max * 4.
    const Tick per_submit = cfg->client_gap_max +
                            cfg->election_timeout_max * 4 + cfg->request_deadline;
    const Tick client_chain = static_cast<Tick>(cfg->submits_per_client) * per_submit;

    // Worst-case fault schedule: `episodes` steps, each up to election_timeout_max
    // * 3 of delay (mirrors fault_driver's episode count + max step).
    const std::uint64_t episodes =
        (cfg->partition_episodes + cfg->crash_episodes) * 2 + 2;
    const Tick fault_chain =
        static_cast<Tick>(episodes) * (cfg->election_timeout_max * 3);

    // The final settle coroutine advances settle_ticks * 4.
    const Tick settle = cfg->settle_ticks * 4;

    // The chains run CONCURRENTLY (client + fault + settle on one scheduler); the
    // horizon is the longest of them, plus a generous settle margin so any in-
    // flight election/replication that started near the end still completes.
    Tick horizon = client_chain;
    if (fault_chain > horizon) {
        horizon = fault_chain;
    }
    if (settle > horizon) {
        horizon = settle;
    }
    return horizon + settle + cfg->election_timeout_max * 4;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// PUBLIC: build + drive one cluster for `seed` behind `factory`, return the
// ObservedRun. PURE FUNCTION OF (seed, cfg, factory): same inputs ⇒ byte-identical
// ObservedRun. The factory selects the implementation (impl A / impl B / teeth).
// ---------------------------------------------------------------------------
[[nodiscard]] inline ObservedRun run_cluster(std::uint64_t seed,
                                             const ConsensusNodeFactory& factory,
                                             const ClusterConfig& cfg = {}) {
    auto stp = std::make_unique<detail::ClusterRunState>(seed);
    detail::ClusterRunState* st = stp.get();
    st->run.seed = seed;
    st->run.n_nodes = cfg.n_nodes;

    // Apply the bus fault envelope before any traffic.
    if (cfg.full_envelope) {
        st->bus.set_faults(cfg.net_faults);
    }

    // The cluster membership view (all node ids 0..N-1, sorted).
    std::vector<std::uint64_t> cluster;
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        cluster.push_back(i);
    }

    // Build N nodes behind the seam factory: one SimDisk + one INetwork handle
    // each, all sharing the bus + scheduler + clock + rng. Disks are heap-owned so
    // their addresses are stable for the run (nodes hold IDisk* refs).
    const sim::DiskFaultConfig disk_cfg =
        cfg.full_envelope ? cfg.disk_faults : sim::DiskFaultConfig{};
    st->disks.reserve(cfg.n_nodes);
    st->nets.reserve(cfg.n_nodes);
    st->nodes.reserve(cfg.n_nodes);
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        st->disks.push_back(std::make_unique<sim::SimDisk>(st->sched, st->clock,
                                                           st->rng, disk_cfg));
        st->nets.push_back(st->bus.node(i));
        st->live.push_back(true);
    }
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        NodeDeps deps;
        deps.sched = &st->sched;
        deps.clock = &st->clock;
        deps.rng = &st->rng;
        deps.net = &st->nets[i];
        deps.disk = st->disks[i].get();

        NodeConfig nc;
        nc.self_id = i;
        nc.cluster = cluster;
        nc.election_timeout_min = cfg.election_timeout_min;
        nc.election_timeout_max = cfg.election_timeout_max;
        nc.heartbeat_interval = cfg.heartbeat_interval;
        nc.request_deadline = cfg.request_deadline;

        st->nodes.push_back(factory(deps, nc));
    }

    // Start every node (arm election timers, serve RPC). Snapshot the initial
    // state (all Followers, term 0, empty logs — vacuously safe).
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        st->nodes[i]->start();
    }
    detail::snapshot(st);

    // Spawn the client drivers + the fault driver, then run to quiescence.
    for (std::uint64_t c = 0; c < cfg.n_clients; ++c) {
        st->sched.spawn(detail::client_driver(st, &cfg, c));
    }
    st->sched.spawn(detail::fault_driver(st, &cfg));

    // A final settle coroutine: after the workload + faults, advance time so any
    // in-flight election/replication completes, snapshotting throughout.
    st->sched.spawn(detail::settle_and_snapshot(
        st, cfg.settle_ticks * 4, cfg.heartbeat_interval * 2));

    // Drive to a virtual-time DEADLINE (not quiescence): a real node's perpetual
    // heartbeat timer never lets run() return, so we run_until() the computed
    // workload+fault+settle horizon, then STOP (later heartbeats stay pending).
    // Deterministic: the deadline is a pure fn of cfg, so replays are identical.
    const Tick deadline = detail::compute_deadline(&cfg);
    st->sched.run_until(deadline);

    // Final snapshot + reconstruct the committed log (cross-check payload).
    detail::snapshot(st);
    detail::finalize_committed_log(st);

    return std::move(st->run);
}

}  // namespace lockstep::consensus
