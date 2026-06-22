#pragma once

// Buggify.hpp — Phase 2 batch 2 (stage B, C2.4). The buggify primitive:
// compile-in injection points, ACTIVE ONLY IN SIM, seed-driven, that force the
// system down rare branches it would otherwise almost never take (extra
// retries, delayed delivery, slow/alloc-failure-ish paths). This is the
// FoundationDB "Buggify" idea: the system carries a few `buggify(...)` call
// sites that, when sim mode is on, flip a seeded coin and push the code into a
// corner case — so the deterministic seed-burn explores far more of the state
// space than an unperturbed run ever would.
//
// DESIGN INVARIANTS (binding):
//   - PURE FUNCTION OF (seed): every buggify decision is a draw from the single
//     seeded IRandom (SeededRandom, providers/sim), in the order the call sites
//     execute. Same seed ⇒ same buggify schedule ⇒ byte-identical run. NO
//     wall-clock, NO std::*_distribution, NO ambient randomness.
//   - ACTIVE ONLY IN SIM: a global enable flag gates every site. When disabled
//     (the default, and the only state a hypothetical prod build would use)
//     every buggify() returns false and the system runs its honest fast path.
//   - PASSIVE-ish: buggify perturbs the SYSTEM-UNDER-TEST's control flow (that
//     is its whole job), but never the History recorder or the checkers. It is
//     a property of the system, not of the observation.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, any nondeterminism. The ONLY randomness is the injected
// core::IRandom.

#include <lockstep/core/IRandom.hpp>

namespace lockstep::harness::kv {

// A small, explicitly-enumerated set of buggify INTENTS. Naming each site keeps
// the injected behaviours auditable (which corner is being forced) and lets the
// probabilities be tuned per-intent without magic numbers scattered in code.
enum class BuggifyKind {
    // Force one extra round of the leader's replicate-and-wait loop even though
    // it already had enough acks — exercises the slow/duplicate-replication path.
    ExtraReplicaWait,
    // Force a client/router to retry an op it could have completed in one shot —
    // exercises the retry + dedup path (writes are exactly-once if ack'd).
    ExtraClientRetry,
    // Add an extra processing delay before a node handles a request — widens the
    // window for interleavings / partitions to land mid-operation.
    SlowPath,
    // Force a node to re-read its durable log on a path that would normally trust
    // its in-memory cache — exercises the recover-from-disk read path.
    ColdRead,
};

// Buggify — the seeded injection oracle. Holds a non-owning IRandom pointer (the
// SAME single PRNG the providers draw faults from, so all nondeterminism shares
// one seed) and a global enable flag. One Buggify is threaded through the system
// in a run.
class Buggify {
public:
    // Construct bound to the run's seeded PRNG. `enabled` is the sim gate: true
    // in the deterministic sim (the only mode this harness runs), false would be
    // a prod build that takes no injected corner.
    Buggify(core::IRandom& rng, bool enabled) noexcept
        : rng_(&rng), enabled_(enabled) {}

    // Should this call site take its rare branch? Draws a seeded coin at the
    // site's per-kind probability. Returns false immediately (no draw) when
    // disabled, so a prod build is byte-for-byte the honest fast path AND does
    // not perturb the PRNG stream. Pure function of (seed, call order).
    [[nodiscard]] bool fire(BuggifyKind kind) {
        if (!enabled_) {
            return false;
        }
        return rng_->chance(probability(kind));
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

private:
    // Per-intent probabilities. Deliberately LOW: buggify forces RARE branches,
    // it must not dominate the run (the system still has to make progress and
    // reach quiescence). Constants, not distributions — fully reproducible.
    [[nodiscard]] static double probability(BuggifyKind kind) noexcept {
        switch (kind) {
            case BuggifyKind::ExtraReplicaWait:
                return 0.10;
            case BuggifyKind::ExtraClientRetry:
                return 0.10;
            case BuggifyKind::SlowPath:
                return 0.15;
            case BuggifyKind::ColdRead:
                return 0.10;
        }
        return 0.0;
    }

    core::IRandom* rng_;
    bool enabled_;
};

}  // namespace lockstep::harness::kv
