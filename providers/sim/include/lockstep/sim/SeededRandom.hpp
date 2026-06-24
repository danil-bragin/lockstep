#pragma once

// SeededRandom.hpp — C1.5. The single seeded PRNG implementing IRandom.
//
// This lives under providers/sim/ (the lint-exempt boundary zone). It is the
// ONLY source of randomness in the whole system (cardinal rule 1 / L6). Every
// shuffle, fault coin-flip, jitter, and tie-break draws from here, so a run is a
// pure function of (seed, inputs).
//
// PORTABILITY (HARD determinism trap): std::uniform_int_distribution and friends
// are implementation-defined, so they produce DIFFERENT byte streams across
// libstdc++ / libc++ / MSVC and would break byte-identical replay. We therefore
//   (a) use a hand-rolled, fully-specified engine — splitmix64 — and
//   (b) compute integer ranges ourselves (Lemire's nearly-divisionless method
//       with rejection for zero bias).
// NO <random>, NO std::*_distribution anywhere in this file.
//
// splitmix64 is the reference SplitMix64 generator (Steele/Lea/Vigna): a 64-bit
// state advanced by the golden-ratio increment 0x9E3779B97F4A7C15, finalized by
// the MurmurHash3-style avalanche. Fully specified, stable, byte-identical
// across platforms with 64-bit two's-complement integers (guaranteed by C++).

#include <cstdint>

#include <lockstep/core/IRandom.hpp>

namespace lockstep::sim {

class SeededRandom final : public core::IRandom {
public:
    // Construct from a 64-bit seed. The seed fully determines the sequence.
    explicit SeededRandom(std::uint64_t seed) noexcept : state_(seed), seed_init_(seed) {}

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_init_; }

    // Next 64-bit value (splitmix64). Advances state by the golden-ratio
    // increment, then applies the avalanche finalizer. Fully specified.
    [[nodiscard]] std::uint64_t next() noexcept override {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, bound) with NO modulo bias, via Lemire's method. bound == 0
    // is invalid (would be an empty range); we return 0 deterministically rather
    // than divide by zero. Uses 128-bit multiply (__uint128_t) when available;
    // otherwise a portable 64x64->128 high-word multiply.
    [[nodiscard]] std::uint64_t uniform(std::uint64_t bound) noexcept override {
        if (bound == 0) {
            return 0; // documented: invalid range yields 0 (no UB, deterministic)
        }
        // Lemire: draw x, multiply by bound to get a 128-bit product; the low 64
        // bits compared against a rejection threshold remove bias.
        std::uint64_t x = next();
        std::uint64_t hi = 0;
        std::uint64_t lo = mul128(x, bound, hi);
        if (lo < bound) {
            // Rejection zone: threshold = (-bound) % bound, computed without bias.
            std::uint64_t threshold = (0ULL - bound) % bound;
            while (lo < threshold) {
                x = next();
                lo = mul128(x, bound, hi);
            }
        }
        return hi;
    }

    // Uniform in the closed range [lo, hi]. Requires lo <= hi. Computes the span
    // as an unsigned width to avoid signed-overflow UB across the full int64
    // range, draws uniform over that width, then offsets back.
    [[nodiscard]] std::int64_t uniform_range(std::int64_t lo, std::int64_t hi) noexcept override {
        if (hi <= lo) {
            return lo; // documented: empty/degenerate range yields lo
        }
        // width = hi - lo, computed in unsigned to dodge signed overflow.
        std::uint64_t width =
            static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
        // We want [lo, hi] inclusive, so the count of values is width + 1. If
        // width == UINT64_MAX the +1 wraps to 0 meaning "full 64-bit range".
        std::uint64_t count = width + 1ULL;
        std::uint64_t pick = (count == 0ULL) ? next() : uniform(count);
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + pick);
    }

    // True with probability p. p <= 0 -> false, p >= 1 -> true. Maps p onto a
    // 53-bit fixed-point fraction of next() (the mantissa width of double) so the
    // comparison is deterministic and free of std::*_distribution.
    [[nodiscard]] bool chance(double p) noexcept override {
        if (p <= 0.0) {
            return false;
        }
        if (p >= 1.0) {
            return true;
        }
        // Draw a uniform fraction in [0,1) from the top 53 bits of next(), then
        // compare to p. 53 bits is the exact mantissa of double, so this is the
        // canonical bias-free unit-interval draw without any distribution type.
        constexpr std::uint64_t kBits = 53;
        std::uint64_t r = next() >> (64 - kBits);
        double frac = static_cast<double>(r) / static_cast<double>(1ULL << kBits);
        return frac < p;
    }

private:
    // 64x64 -> 128 multiply. Returns the low 64 bits; writes the high 64 to `hi`.
    // Uses __uint128_t when the compiler provides it (clang/gcc on this host),
    // else a portable schoolbook 32-bit-limb multiply.
    //
    // Public (a pure stateless helper, no state touched) so the portable
    // schoolbook path can be conformance-tested directly even on a host that has
    // __int128: a test TU defining LOCKSTEP_FORCE_PORTABLE_MUL128 before including
    // this header forces the #else branch and cross-checks it against the
    // __int128 reference. Without that macro every real build keeps the fast path.
public:
    static std::uint64_t mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi) noexcept {
#if defined(__SIZEOF_INT128__) && !defined(LOCKSTEP_FORCE_PORTABLE_MUL128)
        unsigned __int128 prod = static_cast<unsigned __int128>(a) * b;
        hi = static_cast<std::uint64_t>(prod >> 64);
        return static_cast<std::uint64_t>(prod);
#else
        std::uint64_t a_lo = a & 0xFFFFFFFFULL, a_hi = a >> 32;
        std::uint64_t b_lo = b & 0xFFFFFFFFULL, b_hi = b >> 32;
        std::uint64_t ll = a_lo * b_lo;
        std::uint64_t lh = a_lo * b_hi;
        std::uint64_t hl = a_hi * b_lo;
        std::uint64_t hh = a_hi * b_hi;
        std::uint64_t cross = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);
        hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
        return (cross << 32) | (ll & 0xFFFFFFFFULL);
#endif
    }

private:
    std::uint64_t state_;     // current splitmix64 state
    std::uint64_t seed_init_; // the original seed (for logging/replay)
};

} // namespace lockstep::sim
