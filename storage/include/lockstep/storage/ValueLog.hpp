#pragma once

// ValueLog.hpp — Phase 3 §5 step 6 (C3.6). WiscKey large-value separation: an
// APPEND-ONLY value log that holds the bytes of LARGE values out-of-line, so the
// LSM (WAL record + memtable + SSTable) carries only a small fixed-size POINTER
// instead of the value. The point (master-plan D4): a large value is written
// ONCE to the vlog and never rewritten by compaction — compaction only rewrites
// the tiny pointer — cutting write amplification dramatically for big values.
//
// ----------------------------------------------------------------------------
// WHAT IS SEPARATED (the threshold): a value whose byte length is STRICTLY GREATER
// than the configured threshold (default kDefaultValueThreshold = 256) goes to the
// vlog; a value <= threshold stays INLINE in the LSM (a vlog round-trip for a tiny
// value is pure overhead). Each record carries a flag (inline vs vlog-pointer); a
// tombstone (del) has no value and is never separated.
//
// ----------------------------------------------------------------------------
// VALUE-LOG ON-DISK FORMAT (little-endian; one append-structured IDisk per vlog
// generation, minted via the IDiskFactory like an SSTable backing):
//
//   record = [ u32 magic('LVLG') ][ u32 vlen ][ value bytes ][ u32 crc ]
//   crc = CRC32 over magic+vlen+value (the whole record up to, not incl., the crc).
//
// The POINTER the LSM stores (a fixed 24 bytes; see VlogPtr::encode):
//   [ u64 value_off ][ u32 vlen ][ u32 crc ][ u64 gen ]
//   value_off = offset of the VALUE BYTES (NOT the record start) in the vlog image;
//   vlen      = the value length; crc = the record's crc (so a deref re-verifies
//               the exact bytes a torn/flipped vlog region would fail);
//   gen       = the vlog GENERATION id (an IDiskFactory disk id). A pointer
//               SELF-IDENTIFIES its backing vlog so reads always hit the right
//               disk and compaction can reclaim a generation no live pointer uses.
//
// ----------------------------------------------------------------------------
// CRASH DISCIPLINE (V-NOTORN / V-PREFIX — the crux): a pointer that fails CRC or
// bounds on recovery means the value it references did NOT durably survive, so the
// commit that wrote it is NOT part of the recovered prefix. The vlog append+sync
// shares the SAME durability barrier as the WAL record (engine sync() syncs BOTH).
// On recovery the engine loads the durable vlog image FIRST, then — while replaying
// the WAL prefix — DEREFERENCES every pointer record: a pointer that does not
// resolve (vlog torn/lying-dropped/missing) is treated EXACTLY like a corrupt WAL
// record (stop replay there). So a torn/missing vlog region can NEVER resolve to a
// fabricated value, and a half-durable (WAL pointer durable, vlog value not) commit
// is dropped from the prefix — never surfaced.
//
// FORBIDDEN (storage/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, <random>, raw file IO. All bytes via core::IDisk; the CRC
// is the shared hand-rolled Crc32. A pure function of (seed, ops).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lockstep/storage/Codec.hpp>   // Crc32, put_u32/u64, get_u32/u64
#include <lockstep/storage/Engine.hpp>  // Key, Value, Seq

namespace lockstep::storage {

// The default size threshold: a value STRICTLY LONGER than this is separated to
// the vlog; shorter-or-equal stays inline. Configurable per-engine.
inline constexpr std::size_t kDefaultValueThreshold = 256;

// Framing constants — the on-disk contract.
inline constexpr std::uint32_t kVlogMagic = 0x474C564Cu;  // 'LVLG'
inline constexpr std::size_t kVlogRecordHeaderBytes = 8;  // magic + vlen
inline constexpr std::size_t kVlogCrcBytes = 4;
inline constexpr std::size_t kVlogPtrBytes = 24;  // u64 off + u32 len + u32 crc + u64 gen

// ---------------------------------------------------------------------------
// VlogPtr — the small fixed pointer the LSM stores in place of a large value.
// ---------------------------------------------------------------------------
struct VlogPtr {
    std::uint64_t value_off = 0;  // offset of the VALUE BYTES in the vlog image
    std::uint32_t vlen = 0;       // value length
    std::uint32_t crc = 0;        // the vlog record's CRC (deref re-verifies it)
    std::uint64_t gen = 0;        // the vlog generation (IDiskFactory disk id)

    // Encode to its fixed 24-byte form (stored as a record's "value" bytes).
    [[nodiscard]] std::string encode() const {
        std::vector<std::byte> buf;
        put_u64(buf, value_off);
        put_u32(buf, vlen);
        put_u32(buf, crc);
        put_u64(buf, gen);
        std::string out;
        out.reserve(buf.size());
        for (std::byte b : buf) {
            out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(b)));
        }
        return out;
    }

    // Decode from exactly kVlogPtrBytes bytes; false on a wrong length.
    [[nodiscard]] static bool decode(const std::string& bytes, VlogPtr& out) {
        if (bytes.size() != kVlogPtrBytes) {
            return false;
        }
        const auto* p = reinterpret_cast<const std::byte*>(bytes.data());
        out.value_off = get_u64(p);
        out.vlen = get_u32(p + 8);
        out.crc = get_u32(p + 12);
        out.gen = get_u64(p + 16);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Encode one vlog record for a value. Returns the record bytes AND the pointer
// that addresses the value WITHIN those bytes given the record's base offset in
// the vlog image (base_off = where this record will be appended).
// ---------------------------------------------------------------------------
struct VlogEncoded {
    std::vector<std::byte> bytes;  // the full record to append to the vlog
    VlogPtr ptr;                   // the pointer to store in the LSM
};

[[nodiscard]] inline VlogEncoded encode_vlog_record(const Value& value,
                                                    std::uint64_t base_off,
                                                    std::uint64_t gen) {
    VlogEncoded e;
    std::vector<std::byte>& buf = e.bytes;
    buf.reserve(kVlogRecordHeaderBytes + value.size() + kVlogCrcBytes);
    put_u32(buf, kVlogMagic);
    put_u32(buf, static_cast<std::uint32_t>(value.size()));
    for (char c : value) {
        buf.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    }
    const std::uint32_t crc =
        Crc32::compute(std::span<const std::byte>(buf.data(), buf.size()));
    put_u32(buf, crc);
    // The value bytes start right after the 8-byte header.
    e.ptr.value_off = base_off + kVlogRecordHeaderBytes;
    e.ptr.vlen = static_cast<std::uint32_t>(value.size());
    e.ptr.crc = crc;
    e.ptr.gen = gen;
    return e;
}

// ---------------------------------------------------------------------------
// Dereference a pointer against a durable vlog byte image. Returns the value ONLY
// if the record at the pointer is fully present AND its CRC matches; otherwise
// std::nullopt — the caller treats that as "not durable / torn" (V-NOTORN: never
// fabricate). `image` is the durable vlog bytes (read back from the IDisk).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<Value> vlog_deref(const std::vector<std::byte>& image,
                                                     const VlogPtr& ptr) {
    // The record header sits 8 bytes BEFORE the value bytes.
    if (ptr.value_off < kVlogRecordHeaderBytes) {
        return std::nullopt;
    }
    const std::uint64_t rec_off = ptr.value_off - kVlogRecordHeaderBytes;
    const std::uint64_t rec_end =
        rec_off + kVlogRecordHeaderBytes + ptr.vlen + kVlogCrcBytes;
    if (rec_end > image.size()) {
        return std::nullopt;  // record runs past durable bytes — torn/missing.
    }
    const std::byte* p = image.data() + rec_off;
    if (get_u32(p) != kVlogMagic) {
        return std::nullopt;  // not a record boundary — corrupt.
    }
    const std::uint32_t vlen = get_u32(p + 4);
    if (vlen != ptr.vlen) {
        return std::nullopt;  // header length disagrees with the pointer.
    }
    const std::size_t crc_off =
        static_cast<std::size_t>(kVlogRecordHeaderBytes) + ptr.vlen;
    const std::uint32_t want = get_u32(p + crc_off);
    const std::uint32_t got =
        Crc32::compute(std::span<const std::byte>(p, crc_off));
    if (want != got || want != ptr.crc) {
        return std::nullopt;  // flipped/torn bytes — integrity failure.
    }
    Value v;
    v.assign(reinterpret_cast<const char*>(p + kVlogRecordHeaderBytes), ptr.vlen);
    return v;
}

}  // namespace lockstep::storage
