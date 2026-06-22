#pragma once

// Codec.hpp — Phase 3. Shared, hand-rolled, deterministic serialisation
// primitives used by every on-disk format in the storage engine (WAL records,
// SSTable blocks/index/bloom/footer, manifest records): a table-built CRC32 and
// little-endian fixed-width integer encode/decode helpers. Factored out of
// WalEngine.hpp so the WAL, the SSTable, and the manifest all share ONE byte-
// exact integrity check + framing (the batch-2 lesson: per-record/per-block CRC
// in exactly one place). NOT library calls (storage/ is not lint-exempt; this
// must be deterministic + byte-identical across platforms).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lockstep::storage {

// ---------------------------------------------------------------------------
// Hand-rolled CRC32 (IEEE 802.3 polynomial 0xEDB88320, reflected). The table is
// built once (function-local static, single-threaded deterministic init) so the
// integrity check is byte-identical across platforms — the batch-2 per-record
// CRC fix, reused at WAL-record / SSTable-block / manifest-record granularity.
// ---------------------------------------------------------------------------
class Crc32 {
public:
    [[nodiscard]] static std::uint32_t compute(std::span<const std::byte> data) noexcept {
        const std::uint32_t* tbl = table();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::byte b : data) {
            const std::uint8_t idx =
                static_cast<std::uint8_t>(crc) ^ std::to_integer<std::uint8_t>(b);
            crc = (crc >> 8) ^ tbl[idx];
        }
        return crc ^ 0xFFFFFFFFu;
    }

private:
    [[nodiscard]] static const std::uint32_t* table() noexcept {
        static const std::array<std::uint32_t, 256> kTable = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                t[i] = c;
            }
            return t;
        }();
        return kTable.data();
    }
};

// ---- little-endian fixed-width encode helpers (deterministic, no <bit>) -----
inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
}
inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
[[nodiscard]] inline std::uint32_t get_u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24);
}
[[nodiscard]] inline std::uint64_t get_u64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

}  // namespace lockstep::storage
