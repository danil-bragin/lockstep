#pragma once

// Uint256.hpp — a portable 256-bit UNSIGNED integer (crypto-scale amounts, e.g. Ethereum uint256)
// for the SQL engine's UINT256 logical type. There is no native 256-bit integer, so this is a
// little-endian array of four uint64 limbs with schoolbook add / sub / mul (checked for overflow) +
// decimal parse / render. The STORAGE form is the 32-byte BIG-ENDIAN encoding, which is
// order-preserving under a plain lexicographic byte compare (unsigned) — so the existing TEXT key /
// value / zone / index codecs order UINT256 values correctly with NO change. Everything here is a
// pure, deterministic, platform-independent function.

#include <array>
#include <cstdint>
#include <string>

namespace lockstep::query::sql {

struct u256 {
    std::array<std::uint64_t, 4> w{0, 0, 0, 0};  // w[0] = least significant limb
};

// Three-way compare (unsigned).
[[nodiscard]] inline int u256_cmp(const u256& a, const u256& b) {
    for (int i = 3; i >= 0; --i) {
        if (a.w[static_cast<std::size_t>(i)] < b.w[static_cast<std::size_t>(i)]) return -1;
        if (a.w[static_cast<std::size_t>(i)] > b.w[static_cast<std::size_t>(i)]) return 1;
    }
    return 0;
}

// r = a + b; returns true on overflow (carry out of the top limb).
[[nodiscard]] inline bool u256_add(const u256& a, const u256& b, u256& r) {
    unsigned __int128 carry = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned __int128 s =
            static_cast<unsigned __int128>(a.w[i]) + b.w[i] + carry;
        r.w[i] = static_cast<std::uint64_t>(s);
        carry = s >> 64;
    }
    return carry != 0;
}

// r = a - b; returns true on underflow (a < b, i.e. a negative result, which UINT256 cannot hold).
[[nodiscard]] inline bool u256_sub(const u256& a, const u256& b, u256& r) {
    unsigned __int128 borrow = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned __int128 d =
            static_cast<unsigned __int128>(a.w[i]) - b.w[i] - borrow;
        r.w[i] = static_cast<std::uint64_t>(d);
        borrow = (d >> 64) & 1;
    }
    return borrow != 0;
}

// r = a * b; returns true on overflow (the product does not fit in 256 bits).
[[nodiscard]] inline bool u256_mul(const u256& a, const u256& b, u256& r) {
    std::uint64_t prod[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 4; ++i) {
        unsigned __int128 carry = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const unsigned __int128 p = static_cast<unsigned __int128>(a.w[i]) * b.w[j] +
                                        prod[i + j] + carry;
            prod[i + j] = static_cast<std::uint64_t>(p);
            carry = p >> 64;
        }
        prod[i + 4] += static_cast<std::uint64_t>(carry);  // i+4 <= 7, no further carry possible
    }
    bool overflow = false;
    for (std::size_t k = 4; k < 8; ++k)
        if (prod[k] != 0) overflow = true;
    for (std::size_t k = 0; k < 4; ++k) r.w[k] = prod[k];
    return overflow;
}

// r = a / b, rem = a % b (long division by repeated shift-subtract). Returns false if b == 0.
[[nodiscard]] inline bool u256_divmod(const u256& a, const u256& b, u256& q, u256& rem) {
    if (u256_cmp(b, u256{}) == 0) return false;
    q = u256{};
    rem = u256{};
    for (int bit = 255; bit >= 0; --bit) {
        // rem <<= 1
        unsigned __int128 carry = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const unsigned __int128 v = (static_cast<unsigned __int128>(rem.w[i]) << 1) | carry;
            rem.w[i] = static_cast<std::uint64_t>(v);
            carry = v >> 64;
        }
        // rem |= bit of a
        const std::size_t limb = static_cast<std::size_t>(bit) / 64;
        const unsigned shift = static_cast<unsigned>(bit) % 64;
        rem.w[0] |= (a.w[limb] >> shift) & 1ULL;
        if (u256_cmp(rem, b) >= 0) {
            u256 t;
            (void)u256_sub(rem, b, t);
            rem = t;
            q.w[limb] |= (1ULL << shift);
        }
    }
    return true;
}

// Parse an unsigned decimal string into a u256. Returns false on a non-digit or on overflow.
[[nodiscard]] inline bool u256_from_dec(const std::string& in, u256& out) {
    std::size_t p = 0;
    while (p < in.size() && (in[p] == ' ' || in[p] == '\t')) ++p;
    if (p < in.size() && in[p] == '+') ++p;
    if (p >= in.size()) return false;
    u256 v{};
    const u256 ten{{10, 0, 0, 0}};
    bool any = false;
    for (; p < in.size(); ++p) {
        const char c = in[p];
        if (c < '0' || c > '9') return false;
        u256 m;
        if (u256_mul(v, ten, m)) return false;
        u256 d{{static_cast<std::uint64_t>(c - '0'), 0, 0, 0}};
        if (u256_add(m, d, v)) return false;
        any = true;
    }
    if (!any) return false;
    out = v;
    return true;
}

// Render a u256 as an unsigned decimal string (no leading zeros; "0" for zero).
[[nodiscard]] inline std::string u256_to_dec(const u256& v) {
    if (u256_cmp(v, u256{}) == 0) return "0";
    std::string digits;
    u256 cur = v;
    const u256 ten{{10, 0, 0, 0}};
    while (u256_cmp(cur, u256{}) != 0) {
        u256 q, rem;
        (void)u256_divmod(cur, ten, q, rem);
        digits.push_back(static_cast<char>('0' + rem.w[0]));
        cur = q;
    }
    return {digits.rbegin(), digits.rend()};
}

// The 32-byte BIG-ENDIAN storage encoding (order-preserving under unsigned lexicographic compare).
[[nodiscard]] inline std::string u256_encode(const u256& v) {
    std::string s(32, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint64_t limb = v.w[i];                  // limb i covers bytes [least .. ]
        const std::size_t base = (3 - i) * 8;               // big-endian: high limb first
        for (int b = 0; b < 8; ++b)
            s[base + static_cast<std::size_t>(b)] =
                static_cast<char>(static_cast<unsigned char>(limb >> (8 * (7 - b))));
    }
    return s;
}
[[nodiscard]] inline u256 u256_decode(const std::string& s) {
    u256 v{};
    if (s.size() != 32) return v;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t base = (3 - i) * 8;
        std::uint64_t limb = 0;
        for (int b = 0; b < 8; ++b)
            limb = (limb << 8) | static_cast<unsigned char>(s[base + static_cast<std::size_t>(b)]);
        v.w[i] = limb;
    }
    return v;
}

[[nodiscard]] inline u256 u256_from_u64(std::uint64_t x) { return u256{{x, 0, 0, 0}}; }

}  // namespace lockstep::query::sql
