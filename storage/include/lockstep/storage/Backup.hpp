#pragma once
// Backup.hpp — LOGICAL point-in-time backup / restore for any storage::Engine.
//
//   backup_engine(sched, src, snap, out)  — scan the FULL live keyspace as-of a committed Snapshot
//     and write a self-contained, CRC-protected stream to the `out` IDisk.
//   restore_engine(sched, in, dst)        — read that stream (CRC-VERIFIED before anything is
//     applied — a torn/flipped/truncated backup is REJECTED, never partially restored) and replay
//     it into a fresh `dst` Engine, so `dst` ends holding EXACTLY the backed-up snapshot's live
//     state (scan(dst) == scan(src as-of snap)).
//
// PORTABLE: the stream is (key,value) records over the Engine interface — independent of the
// SSTable / WAL / manifest on-disk format, so a backup taken from one engine restores into any
// Engine (incl. a fresh one on a different machine). POINT-IN-TIME: it captures the LIVE values at
// one Snapshot (not the MVCC version history) — the standard logical-backup contract.
//
// STREAM LAYOUT (little-endian; CRC32 over the payload, same Crc32 as the WAL/SSTable codecs):
//   header (28 bytes): magic "LSB1"(4) · version u32 · snap_seq u64 · payload_len u64 · payload_crc u32
//   payload          : repeated record { klen u32 · vlen u32 · key[klen] · value[vlen] }, key-ascending
//
// Determinism: scan is key-ascending and a pure function of (range, seq), so the SAME committed
// state backs up to a BYTE-IDENTICAL stream (V-DET); the integrity check is the same deterministic
// CRC32 used elsewhere.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/storage/Codec.hpp>
#include <lockstep/storage/Engine.hpp>

namespace lockstep::storage {

inline constexpr std::size_t kBackupHeaderBytes = 28;
inline constexpr std::uint32_t kBackupVersion = 1;

namespace detail {
[[nodiscard]] inline bool backup_magic_ok(const std::byte* p) noexcept {
    return std::to_integer<char>(p[0]) == 'L' && std::to_integer<char>(p[1]) == 'S' &&
           std::to_integer<char>(p[2]) == 'B' && std::to_integer<char>(p[3]) == '1';
}

// Scan the full live keyspace as-of `snap` and build the self-contained CRC-protected image
// (header + payload) in `image` — the SAME bytes whether the image is written to a disk or kept
// in memory (so a disk backup and an in-memory backup of one state are byte-identical, V-DET).
inline core::Task build_backup_image(Engine& src, Snapshot snap, std::vector<std::byte>& image) {
    Range full;
    full.hi_unbounded = true;
    const std::vector<KeyValue> kvs = co_await src.scan(full, snap);

    std::vector<std::byte> payload;
    for (const KeyValue& kv : kvs) {
        put_u32(payload, static_cast<std::uint32_t>(kv.first.size()));
        put_u32(payload, static_cast<std::uint32_t>(kv.second.size()));
        for (char c : kv.first) payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        for (char c : kv.second) payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    const std::uint32_t crc = Crc32::compute(payload);

    image.clear();
    for (char c : std::string_view("LSB1")) image.push_back(static_cast<std::byte>(c));
    put_u32(image, kBackupVersion);
    put_u64(image, snap.at);
    put_u64(image, static_cast<std::uint64_t>(payload.size()));
    put_u32(image, crc);
    image.insert(image.end(), payload.begin(), payload.end());
    co_return;
}

// FULLY validate a complete in-memory backup image WITHOUT applying it: magic, version, the payload
// CRC, AND the record framing (so a CRC-valid but structurally malformed payload is also rejected).
// Pure (no scheduler / no Engine) — lets a caller verify EVERY section before applying ANY, so a
// multi-section restore stays all-or-nothing (no partial state from a later corrupt section).
inline core::Error validate_image(std::span<const std::byte> image) {
    if (image.size() < kBackupHeaderBytes || !backup_magic_ok(image.data())) {
        return core::Error{core::ErrorCode::Corruption, "backup: bad magic"};
    }
    if (get_u32(image.data() + 4) != kBackupVersion) {
        return core::Error{core::ErrorCode::InvalidArgument, "backup: unsupported version"};
    }
    const std::uint64_t payload_len = get_u64(image.data() + 16);
    const std::uint32_t want_crc = get_u32(image.data() + 24);
    if (static_cast<std::uint64_t>(kBackupHeaderBytes) + payload_len > image.size()) {
        return core::Error{core::ErrorCode::Corruption, "backup: truncated image"};
    }
    std::span<const std::byte> payload = image.subspan(kBackupHeaderBytes, static_cast<std::size_t>(payload_len));
    if (Crc32::compute(payload) != want_crc) {
        return core::Error{core::ErrorCode::Corruption, "backup: CRC mismatch (torn/corrupt backup)"};
    }
    std::size_t pos = 0;
    while (pos < payload.size()) {
        if (pos + 8 > payload.size()) return core::Error{core::ErrorCode::Corruption, "backup: truncated record header"};
        const std::uint32_t klen = get_u32(payload.data() + pos);
        const std::uint32_t vlen = get_u32(payload.data() + pos + 4);
        pos += 8;
        if (pos + static_cast<std::size_t>(klen) + vlen > payload.size())
            return core::Error{core::ErrorCode::Corruption, "backup: truncated record"};
        pos += static_cast<std::size_t>(klen) + vlen;
    }
    return core::Error{};
}

// CRC-VERIFY a complete in-memory backup image and replay it into `dst`. Identical integrity
// contract to restore_task (bad magic / version / CRC / truncation → rejected, no partial restore).
inline core::Task apply_backup_image(std::span<const std::byte> image, Engine& dst, core::Error& result) {
    if (const core::Error e = validate_image(image); !e.ok()) {
        result = e;
        co_return;
    }
    const std::uint64_t payload_len = get_u64(image.data() + 16);
    std::span<const std::byte> payload = image.subspan(kBackupHeaderBytes, static_cast<std::size_t>(payload_len));
    std::size_t pos = 0;
    while (pos < payload.size()) {
        const std::uint32_t klen = get_u32(payload.data() + pos);
        const std::uint32_t vlen = get_u32(payload.data() + pos + 4);
        pos += 8;
        std::string key(reinterpret_cast<const char*>(payload.data() + pos), klen);
        pos += klen;
        std::string val(reinterpret_cast<const char*>(payload.data() + pos), vlen);
        pos += vlen;
        (void)co_await dst.put(std::move(key), std::move(val));
    }
    result = co_await dst.sync();
    co_return;
}

inline core::Task backup_task(Engine& src, Snapshot snap, core::IDisk& out, core::Error& result) {
    std::vector<std::byte> image;
    co_await build_backup_image(src, snap, image);
    core::Offset off = 0;
    const core::Error ae = co_await out.append(std::span<const std::byte>(image.data(), image.size()), off);
    if (!ae.ok()) { result = ae; co_return; }
    result = co_await out.sync();
    co_return;
}

inline core::Task backup_bytes_task(Engine& src, Snapshot snap, std::vector<std::byte>& out) {
    co_await build_backup_image(src, snap, out);
    co_return;
}

inline core::Task restore_task(core::IDisk& in, Engine& dst, core::Error& result) {
    // Read the fixed header, then the exact payload it advertises. CRC the payload BEFORE applying
    // anything — a torn/short/flipped backup is rejected with NO partial restore.
    std::array<std::byte, kBackupHeaderBytes> hdr{};
    if (const core::Error e = co_await in.read(0, std::span<std::byte>(hdr.data(), hdr.size())); !e.ok()) {
        result = e;
        co_return;
    }
    if (!backup_magic_ok(hdr.data())) { result = core::Error{core::ErrorCode::Corruption, "backup: bad magic"}; co_return; }
    if (get_u32(hdr.data() + 4) != kBackupVersion) {
        result = core::Error{core::ErrorCode::InvalidArgument, "backup: unsupported version"};
        co_return;
    }
    const std::uint64_t payload_len = get_u64(hdr.data() + 16);
    const std::uint32_t want_crc = get_u32(hdr.data() + 24);

    std::vector<std::byte> payload(static_cast<std::size_t>(payload_len));
    if (payload_len > 0) {
        if (const core::Error e = co_await in.read(static_cast<core::Offset>(kBackupHeaderBytes),
                                                   std::span<std::byte>(payload.data(), payload.size()));
            !e.ok()) {
            result = e;
            co_return;
        }
    }
    if (Crc32::compute(payload) != want_crc) {
        result = core::Error{core::ErrorCode::Corruption, "backup: CRC mismatch (torn/corrupt backup)"};
        co_return;
    }
    // Decode + replay. Bounds-checked: a malformed length field aborts before any out-of-range read.
    std::size_t pos = 0;
    while (pos < payload.size()) {
        if (pos + 8 > payload.size()) { result = core::Error{core::ErrorCode::Corruption, "backup: truncated record header"}; co_return; }
        const std::uint32_t klen = get_u32(payload.data() + pos);
        const std::uint32_t vlen = get_u32(payload.data() + pos + 4);
        pos += 8;
        if (pos + klen + vlen > payload.size()) { result = core::Error{core::ErrorCode::Corruption, "backup: truncated record"}; co_return; }
        std::string key(reinterpret_cast<const char*>(payload.data() + pos), klen);
        pos += klen;
        std::string val(reinterpret_cast<const char*>(payload.data() + pos), vlen);
        pos += vlen;
        (void)co_await dst.put(std::move(key), std::move(val));
    }
    result = co_await dst.sync();
    co_return;
}
}  // namespace detail

// Write a CRC-protected logical backup of `src` (as-of `snap`) to `out`. Drives the scan + write
// on `sched` (the engine + disks must share it). Returns ok on success; a disk fault propagates.
[[nodiscard]] inline core::Error backup_engine(core::Scheduler& sched, Engine& src, Snapshot snap,
                                               core::IDisk& out) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "backup: did not run"};
    sched.spawn(detail::backup_task(src, snap, out, result));
    sched.run();
    return result;
}

// Restore a backup from `in` into the (fresh) Engine `dst`. CRC-verifies the whole payload before
// applying anything — a corrupt backup leaves `dst` untouched and returns an error (V-NOTORN).
[[nodiscard]] inline core::Error restore_engine(core::Scheduler& sched, core::IDisk& in, Engine& dst) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "restore: did not run"};
    sched.spawn(detail::restore_task(in, dst, result));
    sched.run();
    return result;
}

// In-memory variants: produce / consume the SAME self-contained image as the disk path, without an
// intermediary IDisk. Used to back up two stores that live on DIFFERENT schedulers into one stream
// (each store backs up to bytes on its OWN scheduler; an outer layer concatenates + writes once).
[[nodiscard]] inline core::Error backup_engine_bytes(core::Scheduler& sched, Engine& src, Snapshot snap,
                                                     std::vector<std::byte>& out) {
    sched.spawn(detail::backup_bytes_task(src, snap, out));
    sched.run();
    return core::Error{};
}

// FULLY validate an in-memory backup image (magic + version + CRC + record framing) WITHOUT
// applying it — used to verify every section of a multi-section stream before any is applied, so a
// restore stays all-or-nothing. Pure (no scheduler).
[[nodiscard]] inline core::Error validate_backup_image(std::span<const std::byte> image) {
    return detail::validate_image(image);
}

// CRC-verify a complete in-memory image and replay it into `dst` (same integrity contract as
// restore_engine — corrupt image leaves `dst` untouched, V-NOTORN).
[[nodiscard]] inline core::Error restore_engine_bytes(core::Scheduler& sched, std::span<const std::byte> image,
                                                      Engine& dst) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "restore: did not run"};
    sched.spawn(detail::apply_backup_image(image, dst, result));
    sched.run();
    return result;
}

}  // namespace lockstep::storage
