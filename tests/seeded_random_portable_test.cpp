// seeded_random_portable_test.cpp — pins the PORTABLE 64x64->128 multiply in
// SeededRandom (the #else schoolbook path). On every real target the host has
// __int128, so that branch is dead code and the mutation gate flagged its
// arithmetic (e.g. SeededRandom.hpp `hi = hh + (lh>>32) + (hl>>32) + (cross>>32)`)
// as an un-killed mutant. This test compiles SeededRandom with
// LOCKSTEP_FORCE_PORTABLE_MUL128 defined (which selects the #else branch even on
// a host that HAS __int128) and cross-checks the portable result against the
// __int128 reference for many inputs incl. edges. A single sign/op error in the
// portable high-word computation -> mismatch -> this test fails (mutant killed).
//
// NOTE: the macro is defined ONLY in this TU, before the include, so every other
// build keeps the fast __int128 path.

#define LOCKSTEP_FORCE_PORTABLE_MUL128
#include <lockstep/sim/SeededRandom.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using lockstep::sim::SeededRandom;

// Reference 64x64->128 via the compiler's native 128-bit type (available on the
// host running the tests). This is the oracle the portable path must match.
void ref_mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi,
                std::uint64_t& lo) noexcept {
    unsigned __int128 prod = static_cast<unsigned __int128>(a) * b;
    hi = static_cast<std::uint64_t>(prod >> 64);
    lo = static_cast<std::uint64_t>(prod);
}

// A tiny self-contained input generator (xorshift64) so the test vectors do NOT
// depend on the code under test.
std::uint64_t next_input(std::uint64_t& s) noexcept {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

} // namespace

int main() {
    int failures = 0;
    std::uint64_t checked = 0;

    // 1) Edge cases that stress the high word + the cross-limb carry.
    const std::uint64_t kMax = ~0ULL;
    const std::uint64_t edges[] = {
        0ULL, 1ULL, 2ULL, 0xFFFFFFFFULL, 0x1'0000'0000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL, 0xDEADBEEFCAFEBABEULL, kMax - 1, 0xFFFFFFFF00000000ULL,
    };
    for (std::uint64_t a : edges) {
        for (std::uint64_t b : edges) {
            std::uint64_t rhi = 0, rlo = 0;
            ref_mul128(a, b, rhi, rlo);
            std::uint64_t phi = 0;
            std::uint64_t plo = SeededRandom::mul128(a, b, phi);  // portable (forced)
            ++checked;
            if (phi != rhi || plo != rlo) {
                ++failures;
                std::fprintf(stderr,
                             "PORTABLE MUL128 MISMATCH a=%llu b=%llu : portable(hi=%llu lo=%llu)"
                             " != ref(hi=%llu lo=%llu)\n",
                             (unsigned long long)a, (unsigned long long)b,
                             (unsigned long long)phi, (unsigned long long)plo,
                             (unsigned long long)rhi, (unsigned long long)rlo);
            }
        }
    }

    // 2) A large pseudo-random sweep — the high word is exercised across the full
    //    range, so any single corrupted +/>> in the schoolbook carry is caught.
    std::uint64_t sa = 0x123456789ABCDEFULL, sb = 0xFEDCBA987654321ULL;
    for (int i = 0; i < 200000; ++i) {
        std::uint64_t a = next_input(sa), b = next_input(sb);
        std::uint64_t rhi = 0, rlo = 0;
        ref_mul128(a, b, rhi, rlo);
        std::uint64_t phi = 0;
        std::uint64_t plo = SeededRandom::mul128(a, b, phi);
        ++checked;
        if (phi != rhi || plo != rlo) {
            ++failures;
            if (failures <= 5) {
                std::fprintf(stderr,
                             "PORTABLE MUL128 MISMATCH a=%llu b=%llu : portable(hi=%llu lo=%llu)"
                             " != ref(hi=%llu lo=%llu)\n",
                             (unsigned long long)a, (unsigned long long)b,
                             (unsigned long long)phi, (unsigned long long)plo,
                             (unsigned long long)rhi, (unsigned long long)rlo);
            }
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "seeded_random_portable_test: %d FAILURE(S) of %llu checks\n",
                     failures, (unsigned long long)checked);
        return 1;
    }
    std::printf("seeded_random_portable_test OK — portable mul128 == __int128 reference"
                " over %llu inputs (edges + 200k random)\n",
                (unsigned long long)checked);
    return 0;
}
