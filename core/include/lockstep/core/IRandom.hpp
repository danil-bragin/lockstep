#pragma once

// IRandom.hpp — the single seeded PRNG surface. This is the ONLY source of
// randomness in the entire system (master-plan cardinal rule 1; spec C1.5).
// Every shuffle, fault-injection coin flip, jitter, and tie-break draws from
// here, so a run is a pure function of (seed, inputs). No <random>: the concrete
// PRNG lives behind the provider in Phase 1.

#include <cstdint>

namespace lockstep::core {

// Pure-virtual PRNG. No state exposed; the seed and internal state live in the
// provider implementation. No ownership.
class IRandom {
public:
    virtual ~IRandom() = default;

    // Returns the next uniformly-distributed 64-bit value and advances the PRNG
    // state. Sequence is fully determined by the seed the provider was
    // constructed with. The primitive on which the helpers below are built.
    [[nodiscard]] virtual std::uint64_t next() noexcept = 0;

    // Returns a uniformly-distributed value in the half-open range [0, bound).
    // `bound == 0` is invalid; providers must document/assert their handling.
    // Must be free of modulo bias. Convenience built on next().
    [[nodiscard]] virtual std::uint64_t uniform(std::uint64_t bound) noexcept = 0;

    // Returns a uniformly-distributed value in the closed range [lo, hi].
    // Requires lo <= hi. Convenience built on next() for bounded picks.
    [[nodiscard]] virtual std::int64_t uniform_range(std::int64_t lo, std::int64_t hi) noexcept = 0;

    // Returns true with the given probability. `p <= 0` yields false, `p >= 1`
    // yields true. The canonical fault-injection / buggify coin flip.
    [[nodiscard]] virtual bool chance(double p) noexcept = 0;
};

} // namespace lockstep::core
