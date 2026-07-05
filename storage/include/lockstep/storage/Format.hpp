#ifndef LOCKSTEP_STORAGE_FORMAT_HPP
#define LOCKSTEP_STORAGE_FORMAT_HPP

// Format.hpp — W2 on-disk / wire FORMAT VERSIONING (central registry).
//
// Every persisted record stream carries a `magic` (a frame/type discriminator) but
// historically no `format_version`, so a future layout change would silently mis-decode
// bytes written by an older build and a mixed-version cluster was undefined. This header
// centralizes the per-stream format-version constants and the append-only STREAM HEADER
// helpers so every format refuses an unknown (future) version UNIFORMLY and loudly.
//
// CONTRACT (until W5.3 adds N/N-1 migration reads):
//   - A reader accepts a record/stream iff its version == the known version for that
//     stream. A HIGHER version means "written by a newer build" -> REFUSE, fail-closed
//     (never mis-decode; recovery stops at it, honoring Seq-contiguity). A LOWER version
//     is reserved for a future migration hook (also refused for now).
//   - Bumps are documented in docs/FORMAT_VERSIONS.md; a bump ships with a fixture canary
//     (tests/fixtures/format/) proving old images are still read (once migration exists)
//     and that a version-bumped image is refused by the current reader (teeth).
//
// See specs/storage-engine.md §7 (W2) for the full inventory and stamping design.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lockstep/storage/Codec.hpp>  // put_u32 / get_u32

namespace lockstep::storage::format {

// ---- per-stream format versions (bump ONE when its layout changes) -------------
inline constexpr std::uint32_t kSstableVersion  = 1;  // SSTable footer  (SSTable.hpp)
inline constexpr std::uint32_t kManifestVersion = 1;  // manifest records (SSTable.hpp)
inline constexpr std::uint32_t kWalStreamVersion  = 1;  // WAL stream header  (WalEngine.hpp)
inline constexpr std::uint32_t kVlogStreamVersion = 1;  // value-log stream   (ValueLog.hpp)
inline constexpr std::uint32_t kRaftStateVersion  = 1;  // raft durable Meta  (RaftNode*.hpp)
inline constexpr std::uint32_t kCatalogVersion    = 1;  // SQL catalog store  (Engine.hpp)
inline constexpr std::uint32_t kWireHelloVersion  = 1;  // internal RPC/admin hello (prod)

// ---- append-only STREAM HEADER (WAL, value-log) --------------------------------
// A one-time header at offset 0 of an append-only file: [u32 magic][u32 version].
// Records follow unchanged (their own magics keep torn-tail detection working).
// Written once at file creation; validated before the record scan begins.
inline constexpr std::size_t kStreamHeaderBytes = 8;

inline void put_stream_header(std::vector<std::byte>& out, std::uint32_t magic,
                              std::uint32_t version) {
    put_u32(out, magic);
    put_u32(out, version);
}

// Validate a stream header at the front of `image`. Returns true iff there are at
// least kStreamHeaderBytes, the magic matches, and the version is exactly `expect`.
// A higher/lower version is refused (fail-closed) — see the CONTRACT above.
[[nodiscard]] inline bool check_stream_header(std::span<const std::byte> image,
                                              std::uint32_t magic, std::uint32_t expect) {
    if (image.size() < kStreamHeaderBytes) {
        return false;
    }
    if (get_u32(image.data()) != magic) {
        return false;
    }
    return get_u32(image.data() + 4) == expect;
}

}  // namespace lockstep::storage::format

#endif  // LOCKSTEP_STORAGE_FORMAT_HPP
