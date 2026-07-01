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
#include <functional>
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

    // C4.2 MEMBERSHIP CHANGE (Membership.tla). `n_nodes` is the UNIVERSE of all
    // servers that may ever participate (== Server in the spec). `init_config_size`
    // is how many of them the cluster STARTS with (configs[1] = {0 .. size-1}); the
    // remaining ids are add/remove candidates. EQUAL to n_nodes (the default) ⇒
    // fixed membership (init_config = the whole universe), byte-identical to the
    // pre-membership driver. `membership_changes` single-server add/remove episodes
    // are driven mid-run by the leader (commit-before-next-paced) when > 0.
    std::uint64_t init_config_size = 0;   // 0 ⇒ = n_nodes (fixed membership)
    std::uint64_t membership_changes = 0;  // single-server changes to drive (0 = none)

    // OPTIONAL submit-value generator. When UNSET (the default), the client driver
    // builds its usual unique string "c<cid>_v<i>_op<opid>" — every existing run is
    // BYTE-IDENTICAL. A test may set this to submit STRUCTURED payloads (e.g.
    // encoded keyed ops) so the committed log applies to a real state machine; the
    // value is still opaque to consensus (the linearizability tracking compares
    // whatever bytes were submitted). Must be a pure function of its args (no clock).
    std::function<std::string(std::uint64_t client_id, std::uint64_t i, std::uint64_t op_id)> value_fn;
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
            // C4.3 snapshot measurement (introspection; not a safety observable).
            ns.snapshot_index = st->nodes[i]->snapshot_index();
            ns.physical_log_size = st->nodes[i]->physical_log_size();
            ns.snapshots_taken = st->nodes[i]->snapshots_taken();
            ns.snapshots_installed = st->nodes[i]->snapshots_installed();
            // C4.2 membership measurement (introspection; not a safety observable).
            ns.config = st->nodes[i]->current_config();
            ns.config_index = st->nodes[i]->config_index();
            ns.config_committed_index = st->nodes[i]->config_committed_index();
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
        obs.value = cfg->value_fn
                        ? cfg->value_fn(client_id, i, obs.op_id)
                        : ("c" + std::to_string(client_id) + "_v" + std::to_string(i) +
                           "_op" + std::to_string(obs.op_id));
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

// C4.2 MEMBERSHIP DRIVER (Membership.tla). Drives a bounded sequence of SINGLE-
// server add/remove changes through the believed leader, COMMIT-BEFORE-NEXT paced:
// each change is proposed only once the previous one is Settled (config_committed
// caught up to config_index on the leader). Bounded episodes + a per-episode
// virtual-time budget ⇒ terminates. The leader's propose_config_change enforces
// the single-server rule + Settled gate; the driver just picks targets. The first
// change ADDS the first non-init server; later changes alternate remove/add of a
// non-leader server so the config keeps moving (each step delta 1).
inline Task membership_driver(ClusterRunState* st, const ClusterConfig* cfg) {
    if (cfg->membership_changes == 0) {
        co_return;
    }
    const std::uint64_t universe = cfg->n_nodes;
    const std::uint64_t init_n =
        (cfg->init_config_size == 0 || cfg->init_config_size > universe)
            ? universe
            : cfg->init_config_size;
    // The candidate to add first = the first server OUTSIDE the init config.
    std::uint64_t add_target = init_n < universe ? init_n : universe - 1;
    bool last_was_add = false;
    std::uint64_t done = 0;

    for (std::uint64_t ep = 0; ep < cfg->membership_changes; ++ep) {
        // Let the cluster run + (re)elect before each change.
        std::uint64_t li = UINT64_MAX;
        {
            Task t = await_leader(st, cfg, cfg->election_timeout_max * 4, &li);
            co_await std::move(t);
        }
        if (li == UINT64_MAX) {
            // No leader this window (a fault is in flight) — wait + retry next ep.
            co_await st->clock.delay(cfg->election_timeout_max);
            snapshot(st);
            continue;
        }
        ConsensusNode* leader = st->nodes[li].get();
        // Decide the single-server change: alternate ADD (the candidate) then REMOVE
        // (a non-leader member of the current config), keeping delta == 1 each step.
        bool add;
        std::uint64_t target;
        if (!last_was_add) {
            add = true;
            target = add_target;
        } else {
            add = false;
            // Remove a member that is NOT the leader and NOT the just-added one we
            // want to keep churning — pick the lowest such id in the current config.
            const std::vector<std::uint64_t> curc = leader->current_config();
            target = UINT64_MAX;
            for (std::uint64_t id : curc) {
                if (id != leader->id() && id != add_target) {
                    target = id;
                    break;
                }
            }
            if (target == UINT64_MAX) {
                // Nothing safe to remove this round; try an add of the candidate
                // instead (if absent) so the sweep still exercises a change.
                add = true;
                target = add_target;
            }
        }
        const bool proposed = leader->propose_config_change(target, add);
        snapshot(st);
        if (proposed) {
            last_was_add = add;
            ++done;
        }
        // commit-before-next: wait (bounded) until the leader reports the change
        // Settled (config_committed_index caught up to config_index), snapshotting.
        Tick waited = 0;
        const Tick stride = cfg->heartbeat_interval * 2 > 0
                                ? cfg->heartbeat_interval * 2
                                : 1;
        const Tick budget = cfg->election_timeout_max * 8 + cfg->request_deadline;
        while (waited < budget) {
            co_await st->clock.delay(stride);
            waited += stride;
            snapshot(st);
            const std::uint64_t cur = believed_leader(st);
            if (cur != UINT64_MAX) {
                ConsensusNode* l = st->nodes[cur].get();
                if (l->config_committed_index() == l->config_index()) {
                    break;  // Settled — the next change may start
                }
            }
        }
    }
    (void)done;
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

    // C4.2: worst-case membership chain. Each change waits for a leader
    // (election_timeout_max*4) then for the change to Settle
    // (election_timeout_max*8 + request_deadline). Pure fn of cfg.
    const Tick per_change = cfg->election_timeout_max * 12 + cfg->request_deadline;
    const Tick membership_chain =
        static_cast<Tick>(cfg->membership_changes) * per_change;

    // The chains run CONCURRENTLY (client + fault + membership + settle on one
    // scheduler); the horizon is the longest of them, plus a generous settle margin
    // so any in-flight election/replication that started near the end still completes.
    Tick horizon = client_chain;
    if (fault_chain > horizon) {
        horizon = fault_chain;
    }
    if (membership_chain > horizon) {
        horizon = membership_chain;
    }
    if (settle > horizon) {
        horizon = settle;
    }
    return horizon + settle + cfg->election_timeout_max * 4;
}

// DEFENSE-IN-DEPTH step backstop bound (root-cause-independent). A generous, fully
// DETERMINISTIC upper bound on how many scheduler resume-steps one healthy run can
// legitimately take, so that a real run NEVER trips it but a zero-virtual-time
// message storm (a bug that keeps the ready queue non-empty without advancing the
// clock — e.g. an InstallSnapshot↔AppendEntries ping-pong) is cut off as a
// DETECTABLE failure instead of spinning the host forever.
//
// Bound = (virtual-time horizon) * (per-tick step budget). Each virtual tick a
// correct run does O(nodes^2) RPC hops plus per-node coroutine resumptions; we use
// a fat constant per (tick * node^2) so headroom is enormous (legitimate runs use a
// tiny fraction) while an unbounded same-tick storm — which would do MANY times this
// many steps at a SINGLE tick — is caught. Pure fn of cfg ⇒ identical across replays.
inline std::uint64_t compute_step_cap(const ClusterConfig* cfg) {
    const std::uint64_t horizon = static_cast<std::uint64_t>(compute_deadline(cfg));
    const std::uint64_t nodes = cfg->n_nodes == 0 ? 1 : cfg->n_nodes;
    // Per-tick budget: ~256 resume-steps per node-pair per virtual tick (huge
    // headroom over the handful a real tick uses), plus a flat floor so tiny
    // horizons still allow startup/election churn.
    const std::uint64_t per_tick = 256ULL * nodes * nodes;
    return horizon * per_tick + 1'000'000ULL;
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

    // The cluster membership view (all node ids 0..N-1, sorted) — the UNIVERSE of
    // servers (== Server). For C4.2 the cluster STARTS in init_config (the first
    // `init_config_size` ids); the rest are add/remove candidates.
    std::vector<std::uint64_t> cluster;
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        cluster.push_back(i);
    }
    std::vector<std::uint64_t> init_config;
    const std::uint64_t init_n =
        (cfg.init_config_size == 0 || cfg.init_config_size > cfg.n_nodes)
            ? cfg.n_nodes
            : cfg.init_config_size;
    for (std::uint64_t i = 0; i < init_n; ++i) {
        init_config.push_back(i);
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
        // C4.2: init_config == cluster ⇒ fixed membership (empty also means that in
        // the impls, but pass it explicitly so the membership sweep starts in the
        // subset). When init_n == n_nodes this is the whole universe (no change).
        if (init_n < cfg.n_nodes) {
            nc.init_config = init_config;
        }
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

    // C4.2: drive single-server membership changes mid-run (no-op when
    // membership_changes == 0 — the coroutine returns immediately).
    st->sched.spawn(detail::membership_driver(st, &cfg));

    // A final settle coroutine: after the workload + faults, advance time so any
    // in-flight election/replication completes, snapshotting throughout.
    st->sched.spawn(detail::settle_and_snapshot(
        st, cfg.settle_ticks * 4, cfg.heartbeat_interval * 2));

    // Drive to a virtual-time DEADLINE (not quiescence): a real node's perpetual
    // heartbeat timer never lets run() return, so we run_until() the computed
    // workload+fault+settle horizon, then STOP (later heartbeats stay pending).
    // Deterministic: the deadline is a pure fn of cfg, so replays are identical.
    const Tick deadline = detail::compute_deadline(&cfg);
    // DEFENSE-IN-DEPTH: bound the resume-steps for this window. With a correct impl
    // the deadline is reached with steps to spare and finished==true. A zero-virtual-
    // time storm (the InstallSnapshot livelock class) trips the cap → finished==false
    // → progress_stalled, which a test surfaces as a LOUD failure (host never hangs).
    const std::uint64_t step_cap = detail::compute_step_cap(&cfg);
    const bool finished = st->sched.run_until(deadline, step_cap);
    st->run.progress_stalled = !finished;

    // Final snapshot + reconstruct the committed log (cross-check payload).
    detail::snapshot(st);
    detail::finalize_committed_log(st);

    return std::move(st->run);
}

}  // namespace lockstep::consensus
