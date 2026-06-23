#pragma once

// ProdRandom.hpp — Phase 7 S2. The PRODUCTION IRandom provider.
//
// V-PROD-CONTRACT: this is the prod twin of sim::SeededRandom and MUST pass the
// SAME universal IRandom conformance suite (tests/provider_conformance/
// ContractConformance.hpp). To make record-replay (V-PROD-REPLAY) work, prod
// reuses the EXACT splitmix64 algorithm + the EXACT bounded-range math of
// sim::SeededRandom, so for any fixed seed the next()/uniform()/uniform_range()/
// chance() sequence is BIT-IDENTICAL sim==prod. That bit-identity is what lets a
// prod incident replay byte-for-byte in sim: recording the seed is sufficient to
// reproduce the whole stream offline.
//
// The ONLY prod-specific freedom is at construction: ProdRandom may consult real
// OS entropy to PICK a seed (the `FromEntropy` tag). Once a seed is fixed, the
// sequence is fully deterministic — real entropy never touches the stream, only
// the initial seed choice. This keeps cardinal rule 1: all nondeterminism is at
// the boundary, and even there it is captured (the chosen seed is recorded).
//
// providers/prod/ is the lint-exempt boundary zone, so std::random_device is
// permitted HERE (and ONLY here) to draw an entropy seed. Everything downstream
// of the seed is the pure splitmix64 engine.

#include <cstdint>
#include <random> // std::random_device — ALLOWED only under providers/ (rule 1)

#include <lockstep/core/IRandom.hpp>

namespace lockstep::prod {

// Tag type selecting the real-entropy seed constructor. Explicit so a caller can
// never accidentally get a nondeterministic seed; they must opt in by name.
struct FromEntropy {};

// Draw a 64-bit seed from the OS entropy source. The ONE nondeterministic act in
// this provider; everything after is a pure function of the returned value. Lives
// behind providers/prod/ so the forbidden-call lint allows std::random_device.
[[nodiscard]] inline std::uint64_t entropy_seed() {
    std::random_device rd;
    // random_device yields unsigned (>=32 bits); compose two draws into 64 bits
    // so the full seed space is reachable regardless of the device word size.
    std::uint64_t hi = static_cast<std::uint64_t>(rd());
    std::uint64_t lo = static_cast<std::uint64_t>(rd());
    return (hi << 32) ^ lo;
}

// Production PRNG. Same engine + same bounded-range math as sim::SeededRandom, so
// a fixed seed yields a sim-identical byte stream (the property the conformance
// suite + the sim==prod cross-check assert).
class ProdRandom final : public core::IRandom {
public:
    // Deterministic construction from an explicit seed. The seed FULLY determines
    // the sequence (identical to sim::SeededRandom(seed)).
    explicit ProdRandom(std::uint64_t seed) noexcept : state_(seed), seed_init_(seed) {}

    // Real-entropy construction: pick a seed from the OS, then run deterministically
    // from it. The chosen seed is observable via seed() so RECORD mode can capture
    // it (that single number replays the whole stream).
    explicit ProdRandom(FromEntropy /*tag*/) : ProdRandom(entropy_seed()) {}

    // The seed this PRNG was constructed with — all that record-replay needs to
    // reproduce the entire sequence offline.
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_init_; }

    // splitmix64 — byte-identical to sim::SeededRandom::next().
    [[nodiscard]] std::uint64_t next() noexcept override {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, bound), Lemire's method, NO modulo bias. bound == 0 returns 0
    // deterministically (matches sim's documented edge — resolves the S1-flagged
    // ambiguity by making prod==sim, so the contract is now testable).
    [[nodiscard]] std::uint64_t uniform(std::uint64_t bound) noexcept override {
        if (bound == 0) {
            return 0; // documented: invalid range yields 0 (no UB, deterministic)
        }
        std::uint64_t x = next();
        std::uint64_t hi = 0;
        std::uint64_t lo = mul128(x, bound, hi);
        if (lo < bound) {
            std::uint64_t threshold = (0ULL - bound) % bound;
            while (lo < threshold) {
                x = next();
                lo = mul128(x, bound, hi);
            }
        }
        return hi;
    }

    // Uniform in the closed range [lo, hi]; empty/degenerate range yields lo.
    [[nodiscard]] std::int64_t uniform_range(std::int64_t lo, std::int64_t hi) noexcept override {
        if (hi <= lo) {
            return lo; // documented: empty/degenerate range yields lo
        }
        std::uint64_t width =
            static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
        std::uint64_t count = width + 1ULL;
        std::uint64_t pick = (count == 0ULL) ? next() : uniform(count);
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + pick);
    }

    // True with probability p. p <= 0 -> false, p >= 1 -> true. 53-bit fixed-point
    // fraction of next(), identical to sim.
    [[nodiscard]] bool chance(double p) noexcept override {
        if (p <= 0.0) {
            return false;
        }
        if (p >= 1.0) {
            return true;
        }
        constexpr std::uint64_t kBits = 53;
        std::uint64_t r = next() >> (64 - kBits);
        double frac = static_cast<double>(r) / static_cast<double>(1ULL << kBits);
        return frac < p;
    }

private:
    // 64x64 -> 128 multiply (low returned, high written to `hi`). Identical to sim.
    static std::uint64_t mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi) noexcept {
#if defined(__SIZEOF_INT128__)
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

    std::uint64_t state_;     // current splitmix64 state
    std::uint64_t seed_init_; // the original seed (for RECORD-mode capture)
};

} // namespace lockstep::prod
