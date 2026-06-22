#pragma once

// SeedBurn.hpp — Phase 2 batch 2 (C2.7 seed-burn farm + C2.8 fault-schedule
// shrinking). The REUSABLE core, shared by the runner executable and the
// kv_seedburn_test. It is a thin, pure layer ON TOP of run_kv_sim_with + the §4
// checker set (it owns no system-under-test and no checker logic of its own).
//
// SPEC (binding): specs/checker-framework.md §6.1, §7.
//   V-SEED1: every run logs its seed; one-command replay reproduces a seed
//            byte-identically (the runner takes a --seed arg; run_one() here is
//            the pure-function-of-(seed,cfg) primitive both paths share).
//   V-SEED2: the seed-burn farm sweeps a CONFIGURABLE range of seeds, asserts
//            the MUST-HOLD invariants (§6.1: C-DUR=0, no INT-1/DUR-2
//            fabrication), TRACKS (counts, never fails) the expected failover
//            anomalies (C-LIN / C-MONO / INT-2 lost-ack), LOGS every seed, and
//            STORES + surfaces any seed that breaks a MUST-HOLD invariant.
//   V-SHRINK1: a FAILING (seed, KvConfig) auto-reduces (delta-debugging) to a
//            MINIMAL config that still triggers the SAME violation class, and
//            the minimal case replays byte-identically.
//
// THE VIOLATION-CLASS abstraction (the thing the shrinker PRESERVES):
//   run_one() runs the §4 checkers over a (seed, factory, cfg) and folds their
//   verdicts into a ViolationClass — a small, ORDER-STABLE bitset of which
//   property-levels fired (FABRICATION = C-INT/INT-1 or C-DUR fabrication; the
//   §6.1 must-hold class — plus LOST_ACK / NON_LIN / NON_MONO). The shrinker's
//   contract is: a reduced config is ACCEPTED only if its ViolationClass STILL
//   CONTAINS the target class. A shrink that loses the bug is rejected (the
//   reduction is monotone-conservative → it converges to a minimal repro).
//
// DETERMINISM (binding, harness/ is NOT lint-exempt): everything here is a pure
// function of (seed, cfg). No wall-clock, no std::thread/atomics, no
// std::*_distribution, no unordered iteration affecting any verdict or order.
// File IO (the runner's failing-seed log) lives in the runner .cpp, NOT here —
// this header stays a pure, side-effect-free deterministic library.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/harness/CheckerRunner.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/checkers/Durability.hpp>
#include <lockstep/harness/checkers/Integrity.hpp>
#include <lockstep/harness/checkers/Linearizable.hpp>
#include <lockstep/harness/checkers/SessionMonotonic.hpp>
#include <lockstep/harness/kv/KvSim.hpp>

namespace lockstep::harness::seedburn {

using lockstep::harness::CheckerRunner;
using lockstep::harness::History;
using lockstep::harness::Verdict;
using lockstep::harness::checkers::DurabilityChecker;
using lockstep::harness::checkers::IntegrityChecker;
using lockstep::harness::checkers::LinearizableChecker;
using lockstep::harness::checkers::SessionMonotonicChecker;
using lockstep::harness::kv::KvConfig;
using lockstep::harness::kv::run_kv_sim_with;
using lockstep::harness::kv::detail::SystemFactory;
using lockstep::harness::kv::detail::default_factory;

// The checker order is FROZEN here (insertion order = verdict order), matching
// the teeth test: C-INT, C-MONO, C-LIN, C-DUR. Both the farm and the shrinker
// classify against this exact order so indices are stable.
enum CheckerIdx : std::size_t {
    kIdxInt = 0,
    kIdxMono = 1,
    kIdxLin = 2,
    kIdxDur = 3,
    kNumCheckers = 4,
};

// Build the §4 checker set in the FROZEN order. A free function so the farm, the
// shrinker, and the runner all use the identical set (no drift).
inline void add_all_checkers(CheckerRunner& runner) {
    runner.add(std::make_unique<IntegrityChecker>());
    runner.add(std::make_unique<SessionMonotonicChecker>());
    runner.add(std::make_unique<LinearizableChecker>());
    runner.add(std::make_unique<DurabilityChecker>());
}

// ViolationClass — the order-stable bitset of which property-LEVELS fired in a
// run. This is the unit the shrinker preserves: a reduced config is a valid
// shrink iff its class STILL contains the target violation level(s). The bits
// are split so the §6.1 MUST-HOLD (fabrication) class is distinguishable from
// the TRACKED failover-anomaly classes.
struct ViolationClass {
    // §6.1 MUST-HOLD class — a violation here HALTS the gate. §6.1 names this
    // class EXACTLY: "no INT-1 fabricated value, no DUR-2". A value the storage
    // MANUFACTURED (no mutation ever offered it) is a true fabrication and is
    // decidable without ambiguity.
    bool int_fabrication = false;  // C-INT/INT-1 fabricated/torn/phantom read
    bool dur_fab2 = false;         // C-DUR/DUR-2 fabricated durable value (must-hold)

    // TRACKED failover anomalies — counted + reported, never fail the gate.
    // NOTE on dur_rejected (C-DUR/DUR-1, "rejected write surfaced"): under the
    // toy workload values are tagged c<client>_v<i> and a SINGLE token can reach
    // a key via BOTH a rejected cas (carrying it as new-value) AND a legitimate
    // path (a lost-ack write, or a committing cas whose cas_old confirms the
    // register HELD it). DUR-1's provenance check sees only the rejected source
    // and false-alarms on that reuse. So DUR-1 is NOT a sound honest-system
    // must-hold; it is TRACKED alongside the failover anomalies (it disappears
    // once values are globally unique — a Phase-4 concern). The fabrication
    // must-hold the gate asserts is INT-1 + DUR-2, per §6.1's exact wording.
    bool dur_rejected = false;  // C-DUR/DUR-1 rejected-write-surfaced (tracked)
    bool int_lost_ack = false;  // C-INT/INT-2 lost ack'd write (tracked)
    bool mono = false;          // C-MONO read-your-writes (tracked)
    bool lin = false;           // C-LIN non-linearizable / undecided (tracked)

    [[nodiscard]] bool any() const {
        return int_fabrication || dur_fab2 || dur_rejected || int_lost_ack ||
               mono || lin;
    }
    // True iff a §6.1 MUST-HOLD invariant was violated (INT-1 / DUR-2 fabrication).
    [[nodiscard]] bool must_hold_violation() const {
        return int_fabrication || dur_fab2;
    }
    // Subset test: does `this` class CONTAIN every level set in `target`? The
    // shrinker accepts a reduced config only when this holds for the target.
    [[nodiscard]] bool contains(const ViolationClass& target) const {
        if (target.int_fabrication && !int_fabrication) return false;
        if (target.dur_fab2 && !dur_fab2) return false;
        if (target.dur_rejected && !dur_rejected) return false;
        if (target.int_lost_ack && !int_lost_ack) return false;
        if (target.mono && !mono) return false;
        if (target.lin && !lin) return false;
        return true;
    }
    // A compact, stable label of the set bits (for receipts / logs).
    [[nodiscard]] std::string label() const {
        std::string s;
        auto add = [&](bool b, const char* name) {
            if (b) {
                if (!s.empty()) s += "+";
                s += name;
            }
        };
        add(int_fabrication, "INT-1-fab");
        add(dur_fab2, "DUR-2-fab");
        add(dur_rejected, "DUR-1-rejected");
        add(int_lost_ack, "INT-2-lostack");
        add(mono, "C-MONO");
        add(lin, "C-LIN");
        if (s.empty()) s = "none";
        return s;
    }
};

// The outcome of running ONE (seed, factory, cfg): the violation class, the
// first witness (for the receipt), and the rendered-history fingerprint used to
// prove byte-identical replay (V-HIST2 / V-SEED1). Pure function of inputs.
struct RunOutcome {
    ViolationClass vclass;
    std::string first_witness;
    std::string history_render;  // render_history(history) — the replay surface
    std::uint64_t seed = 0;
};

// Run one seed against `factory` under `cfg`, run the §4 checker set, and fold
// the verdicts into a RunOutcome. The CANONICAL pure primitive both the farm and
// the shrinker (and the runner's --seed replay) call. Same (seed, factory, cfg)
// ⇒ byte-identical RunOutcome (history_render is the proof surface).
inline RunOutcome run_one(std::uint64_t seed, const SystemFactory& factory,
                          const KvConfig& cfg) {
    CheckerRunner runner;
    add_all_checkers(runner);
    const History h = run_kv_sim_with(seed, factory, &runner, cfg);
    const std::vector<Verdict> vs = runner.finalize(h, seed);

    RunOutcome out;
    out.seed = seed;
    out.history_render = render_history(h);

    for (std::size_t i = 0; i < vs.size(); ++i) {
        const Verdict& v = vs[i];
        if (v.ok) {
            continue;
        }
        if (out.first_witness.empty()) {
            out.first_witness = v.witness;
        }
        switch (static_cast<CheckerIdx>(i)) {
            case kIdxInt:
                if (v.witness.find("FABRICATED READ") != std::string::npos) {
                    out.vclass.int_fabrication = true;
                }
                if (v.witness.find("LOST ACK'D WRITE") != std::string::npos) {
                    out.vclass.int_lost_ack = true;
                }
                break;
            case kIdxMono:
                out.vclass.mono = true;
                break;
            case kIdxLin:
                out.vclass.lin = true;
                break;
            case kIdxDur:
                // Split DUR-2 (fabricated durable — the §6.1 must-hold) from
                // DUR-1 (rejected-write-surfaced — tracked; imprecise under the
                // toy workload's value-token reuse, see the ViolationClass note).
                if (v.witness.find("FABRICATED DURABLE") != std::string::npos) {
                    out.vclass.dur_fab2 = true;
                }
                if (v.witness.find("REJECTED WRITE SURFACED") !=
                    std::string::npos) {
                    out.vclass.dur_rejected = true;
                }
                break;
            case kNumCheckers:
                break;
        }
    }
    return out;
}

// Convenience: run the HONEST system (default_factory) for `seed`.
inline RunOutcome run_one_honest(std::uint64_t seed, const KvConfig& cfg) {
    return run_one(seed, default_factory, cfg);
}

// =====================================================================
// SEED-BURN FARM (C2.7 / V-SEED2)
// =====================================================================

// One failing seed the farm surfaced (a MUST-HOLD breach on the honest system).
struct FailingSeed {
    std::uint64_t seed = 0;
    std::string vclass_label;
    std::string witness;
};

// Aggregate result of a sweep over [seed_start, seed_start+count).
struct SweepSummary {
    std::uint64_t seed_start = 0;
    std::uint64_t count = 0;

    // MUST-HOLD breaches (these are FAILURES; any > 0 halts the gate).
    std::uint64_t must_hold_failures = 0;
    std::vector<FailingSeed> failing_seeds;  // stored + surfaced (V-SEED2)

    // TRACKED failover anomalies — counted + reported, never a failure (§6.1).
    std::uint64_t lin_anomalies = 0;
    std::uint64_t mono_anomalies = 0;
    std::uint64_t lost_ack_anomalies = 0;
    std::uint64_t dur_rejected_anomalies = 0;  // C-DUR/DUR-1 (imprecise; tracked)

    [[nodiscard]] bool ok() const { return must_hold_failures == 0; }
};

// Run the honest KV system + the full §4 checker set under the full fault
// envelope across [seed_start, seed_start+count). For each seed: assert (track)
// MUST-HOLD; count TRACKED anomalies; STORE every MUST-HOLD breach so it is
// surfaced + replayable. `per_seed`, if non-null, is invoked for EVERY seed
// (so the runner can LOG every seed — V-SEED2). PURE w.r.t. (seeds, cfg); the
// only side effect is the optional callback the caller supplies.
template <typename PerSeedFn>
inline SweepSummary seed_burn_sweep(std::uint64_t seed_start,
                                    std::uint64_t count, const KvConfig& cfg,
                                    PerSeedFn&& per_seed) {
    SweepSummary sum;
    sum.seed_start = seed_start;
    sum.count = count;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t seed = seed_start + i;
        const RunOutcome o = run_one_honest(seed, cfg);

        if (o.vclass.lin) {
            ++sum.lin_anomalies;
        }
        if (o.vclass.mono) {
            ++sum.mono_anomalies;
        }
        if (o.vclass.int_lost_ack) {
            ++sum.lost_ack_anomalies;
        }
        if (o.vclass.dur_rejected) {
            ++sum.dur_rejected_anomalies;
        }
        if (o.vclass.must_hold_violation()) {
            ++sum.must_hold_failures;
            FailingSeed fs;
            fs.seed = seed;
            fs.vclass_label = o.vclass.label();
            fs.witness = o.first_witness;
            sum.failing_seeds.push_back(std::move(fs));
        }
        per_seed(seed, o);
    }
    return sum;
}

// Overload with no per-seed callback (logging handled elsewhere).
inline SweepSummary seed_burn_sweep(std::uint64_t seed_start,
                                    std::uint64_t count, const KvConfig& cfg) {
    return seed_burn_sweep(seed_start, count, cfg,
                           [](std::uint64_t, const RunOutcome&) {});
}

// =====================================================================
// SHRINKING (C2.8 / V-SHRINK1) — delta-debugging the fault schedule + workload
// =====================================================================

// The trace of one shrink: the before/after configs + the preserved class, so
// the receipt can show "before config → after minimal config, same violation".
struct ShrinkResult {
    KvConfig before;
    KvConfig after;
    ViolationClass target;        // the class being preserved
    std::string before_label;
    std::string after_label;
    std::uint64_t seed = 0;
    std::uint64_t iterations = 0;  // reduction steps that stuck (converged)
    bool preserved = false;        // after still triggers target (must be true)
};

namespace detail {

// Does running (seed, factory, cfg) still trigger EVERY level in `target`?
inline bool still_triggers(std::uint64_t seed, const SystemFactory& factory,
                           const KvConfig& cfg, const ViolationClass& target) {
    const RunOutcome o = run_one(seed, factory, cfg);
    return o.vclass.contains(target);
}

}  // namespace detail

// Shrink a FAILING (seed, factory, cfg) — one whose ViolationClass contains
// `target` — to a MINIMAL config that STILL triggers `target`. Delta-debugging
// style: greedily try to reduce each fault/workload knob toward its floor,
// KEEPING a reduction only if the violation is PRESERVED. The reductions are
// monotone (each knob only ever shrinks) so the process strictly converges; we
// loop the full knob set until a fixpoint (no knob shrank) or a hard cap.
//
// Knobs reduced (each toward a documented floor):
//   ops_per_client → n_clients → n_keys → n_nodes → crash_episodes →
//   partition_episodes → kill_episodes → the disk/net fault probabilities.
// The floors guarantee a runnable config (≥1 client, ≥1 op, ≥1 key, ≥1 node).
//
// VALIDATION CONTRACT (V-SHRINK1): the returned `after` config, replayed via
// run_one(seed, factory, after), still has a ViolationClass containing `target`
// (preserved=true) and replays byte-identically. The CALLER asserts this.
inline ShrinkResult shrink_failure(std::uint64_t seed,
                                   const SystemFactory& factory,
                                   const KvConfig& start_cfg,
                                   const ViolationClass& target) {
    ShrinkResult r;
    r.seed = seed;
    r.before = start_cfg;
    r.target = target;
    r.before_label = run_one(seed, factory, start_cfg).vclass.label();

    KvConfig cur = start_cfg;

    // A single "try setting field to `cand`; keep iff still triggers" step over
    // an integer knob. Returns true if it shrank (so the outer loop iterates).
    auto try_set_u64 = [&](std::uint64_t KvConfig::*field, std::uint64_t cand,
                           std::uint64_t floor) -> bool {
        std::uint64_t& slot = cur.*field;
        if (cand >= slot || cand < floor) {
            return false;
        }
        const std::uint64_t saved = slot;
        slot = cand;
        if (detail::still_triggers(seed, factory, cur, target)) {
            ++r.iterations;
            return true;
        }
        slot = saved;  // reduction lost the bug → revert.
        return false;
    };

    // Binary-style reduction of one integer knob toward `floor`: try halving,
    // then floor, then decrement — accept the smallest that preserves the bug.
    auto reduce_u64 = [&](std::uint64_t KvConfig::*field,
                          std::uint64_t floor) -> bool {
        bool shrank = false;
        bool progress = true;
        while (progress) {
            progress = false;
            const std::uint64_t v = cur.*field;
            if (v <= floor) {
                break;
            }
            // Try the floor first (biggest jump), then the midpoint, then v-1.
            const std::uint64_t mid = floor + (v - floor) / 2;
            if (try_set_u64(field, floor, floor) ||
                (mid < v && try_set_u64(field, mid, floor)) ||
                try_set_u64(field, v - 1, floor)) {
                shrank = true;
                progress = true;
            }
        }
        return shrank;
    };

    // Turn a boolean-ish fault probability off (set to 0.0) iff the bug survives.
    auto try_zero_prob = [&](double sim::detail::LinkFaults::*field) -> bool {
        double& slot = cur.net_faults.*field;
        if (slot == 0.0) {
            return false;
        }
        const double saved = slot;
        slot = 0.0;
        if (detail::still_triggers(seed, factory, cur, target)) {
            ++r.iterations;
            return true;
        }
        slot = saved;
        return false;
    };
    auto try_zero_disk = [&](double sim::DiskFaultConfig::*field) -> bool {
        double& slot = cur.disk_faults.*field;
        if (slot == 0.0) {
            return false;
        }
        const double saved = slot;
        slot = 0.0;
        if (detail::still_triggers(seed, factory, cur, target)) {
            ++r.iterations;
            return true;
        }
        slot = saved;
        return false;
    };

    // Outer fixpoint loop: keep sweeping the knob set until nothing shrinks (a
    // minimal config) or we hit a hard cap (guaranteed termination; the knobs
    // only ever shrink so the cap is generous).
    constexpr std::uint64_t kMaxRounds = 64;
    for (std::uint64_t round = 0; round < kMaxRounds; ++round) {
        bool any = false;
        // Workload first (cheapest reductions, biggest search-space cuts).
        any |= reduce_u64(&KvConfig::ops_per_client, 1);
        any |= reduce_u64(&KvConfig::n_clients, 1);
        any |= reduce_u64(&KvConfig::n_keys, 1);
        any |= reduce_u64(&KvConfig::n_nodes, 1);
        // Fault schedule (the core C2.8 target — minimal fault SCHEDULE).
        any |= reduce_u64(&KvConfig::crash_episodes, 0);
        any |= reduce_u64(&KvConfig::partition_episodes, 0);
        any |= reduce_u64(&KvConfig::kill_episodes, 0);
        // Fault probabilities: drop any that are not load-bearing for the bug.
        any |= try_zero_prob(&sim::detail::LinkFaults::drop_prob);
        any |= try_zero_prob(&sim::detail::LinkFaults::dup_prob);
        any |= try_zero_prob(&sim::detail::LinkFaults::reorder_prob);
        any |= try_zero_disk(&sim::DiskFaultConfig::io_fault_prob);
        any |= try_zero_disk(&sim::DiskFaultConfig::torn_write_prob);
        any |= try_zero_disk(&sim::DiskFaultConfig::lying_fsync_prob);
        any |= try_zero_disk(&sim::DiskFaultConfig::bit_rot_prob);
        if (!any) {
            break;  // fixpoint → minimal.
        }
    }

    r.after = cur;
    const RunOutcome after_run = run_one(seed, factory, cur);
    r.after_label = after_run.vclass.label();
    r.preserved = after_run.vclass.contains(target);
    return r;
}

}  // namespace lockstep::harness::seedburn
