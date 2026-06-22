#pragma once

// Codec.hpp — Phase 2 batch 2 (stage B). A tiny, hand-rolled, FULLY-SPECIFIED
// wire + WAL codec for the toy replicated KV system. No JSON, no hashing, no
// std::*_distribution, no endianness ambiguity: every field is length-prefixed
// with a fixed little-endian u32, so encode/decode are byte-deterministic and
// a torn/partial frame decodes as a clean failure (return false) rather than
// reading out of bounds. This is what makes the disk torn-write / network
// truncation models surface as a decode failure, not UB.
//
// FRAME (network message): [u8 type][u64 op_id][u8 op_kind][u64 client_ep]
//   [u8 ok][u64 commit_seq][u8 present][str key][str value][str cas_old]
//   [str error]   — where str = [u32 len][len bytes].
// WAL RECORD (durable log entry): [u64 seq][u8 present][str key][str value]
//   [u32 crc32]  — a PER-RECORD integrity trailer (see below).
//
// WAL PER-RECORD INTEGRITY (durability bug fix, spec §4 C-INT/C-DUR, Phase 3
// "no torn corruption survives recovery"). The length-prefix bounds checks alone
// do NOT catch a torn/bit-rotted durable tail that happens to still parse: such a
// frame decodes into a WalRecord with a GARBAGE value, which recovery would apply
// as a committed value NO CLIENT EVER WROTE (fabricated durable value). To make a
// torn/corrupt record a CLEAN decode FAILURE, every WAL record carries a trailing
// 32-bit checksum over the record body ([seq][present][key][value]). On decode we
// recompute it and reject on mismatch — so a torn tail is LOST (not applied) and
// recovery yields a consistent PREFIX of the durable write history.
//
// CHECKSUM ALGORITHM — CRC-32 (ISO 3309 / IEEE 802.3), hand-rolled, no library:
//   * Reflected (LSB-first) bit order, reversed polynomial 0xEDB88320
//     (the bit-reversal of 0x04C11DB7), init = 0xFFFFFFFF, final XOR 0xFFFFFFFF.
//   * Computed byte-by-byte with the standard reflected inner loop. This is the
//     same CRC-32 as zlib/PNG, but implemented inline so there is NO external
//     dependency and the result is fully byte-deterministic on every host.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, any nondeterminism. Pure byte manipulation.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/INetwork.hpp>  // core::Endpoint
#include <lockstep/harness/History.hpp>  // OpKind

namespace lockstep::harness::kv {

using core::Endpoint;

// The kinds of message frames on the wire. Stable, append-only.
enum class FrameType : std::uint8_t {
    ClientRequest = 0,  // client → node: an op to perform
    ClientReply = 1,    // node → client: the op's result
    Replicate = 2,      // leader → backup: a committed record
    ReplicateAck = 3,   // backup → leader: applied (currently unused, reserved)
    ClientTimeout = 4,  // self → client: deadline fired (bounded-termination)
};

// A decoded network frame. Not every field is meaningful for every type; the
// codec round-trips them all so the struct stays flat and branch-free.
struct Frame {
    FrameType type = FrameType::ClientRequest;
    std::uint64_t op_id = 0;
    OpKind op_kind = OpKind::Read;
    Endpoint client_endpoint{};
    bool ok = false;
    std::uint64_t commit_seq = 0;
    bool present = false;
    std::string key;
    std::string value;
    std::string cas_old;
    std::string error;
};

// A durable WAL record (one committed register write).
struct WalRecord {
    std::uint64_t seq = 0;
    bool present = false;
    std::string key;
    std::string value;
};

// ---- primitive encoders (little-endian, fixed width) ----------------------

inline void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

inline void put_str(std::vector<std::byte>& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

// ---- primitive decoders (bounds-checked; return false on short input) -----

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept
        : data_(data) {}

    [[nodiscard]] bool u8(std::uint8_t& v) noexcept {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        v = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& v) noexcept {
        if (pos_ + 4 > data_.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(
                     static_cast<std::uint8_t>(data_[pos_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos_ += 4;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& v) noexcept {
        if (pos_ + 8 > data_.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(
                     static_cast<std::uint8_t>(data_[pos_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos_ += 8;
        return true;
    }
    [[nodiscard]] bool str(std::string& s) {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        if (pos_ + len > data_.size()) {
            return false;
        }
        s.assign(len, '\0');
        for (std::uint32_t i = 0; i < len; ++i) {
            s[i] = static_cast<char>(
                static_cast<std::uint8_t>(data_[pos_ + i]));
        }
        pos_ += len;
        return true;
    }
    [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// ---- WAL per-record integrity checksum (hand-rolled CRC-32) ---------------

// CRC-32 (reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF) over `data`.
// Hand-rolled, branch-deterministic, no table needed (computed inline). Identical
// to zlib/PNG CRC-32, but with no external dependency.
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b));
        for (int k = 0; k < 8; ++k) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- frame codec ----------------------------------------------------------

[[nodiscard]] inline std::vector<std::byte> encode_frame(const Frame& f) {
    std::vector<std::byte> out;
    put_u8(out, static_cast<std::uint8_t>(f.type));
    put_u64(out, f.op_id);
    put_u8(out, static_cast<std::uint8_t>(f.op_kind));
    put_u64(out, f.client_endpoint.id);
    put_u8(out, f.ok ? 1u : 0u);
    put_u64(out, f.commit_seq);
    put_u8(out, f.present ? 1u : 0u);
    put_str(out, f.key);
    put_str(out, f.value);
    put_str(out, f.cas_old);
    put_str(out, f.error);
    return out;
}

[[nodiscard]] inline bool decode_frame(std::span<const std::byte> data,
                                       Frame& f) {
    ByteReader r(data);
    std::uint8_t type = 0;
    std::uint8_t kind = 0;
    std::uint64_t client_ep = 0;
    std::uint8_t ok = 0;
    std::uint8_t present = 0;
    if (!r.u8(type)) {
        return false;
    }
    if (!r.u64(f.op_id)) {
        return false;
    }
    if (!r.u8(kind)) {
        return false;
    }
    if (!r.u64(client_ep)) {
        return false;
    }
    if (!r.u8(ok)) {
        return false;
    }
    if (!r.u64(f.commit_seq)) {
        return false;
    }
    if (!r.u8(present)) {
        return false;
    }
    if (!r.str(f.key) || !r.str(f.value) || !r.str(f.cas_old) ||
        !r.str(f.error)) {
        return false;
    }
    if (type > static_cast<std::uint8_t>(FrameType::ClientTimeout)) {
        return false;
    }
    if (kind > static_cast<std::uint8_t>(OpKind::Cas)) {
        return false;
    }
    f.type = static_cast<FrameType>(type);
    f.op_kind = static_cast<OpKind>(kind);
    f.client_endpoint = Endpoint{client_ep};
    f.ok = ok != 0;
    f.present = present != 0;
    return true;
}

// ---- WAL record codec -----------------------------------------------------

[[nodiscard]] inline std::vector<std::byte> encode_wal_record(
    const std::string& key, const std::string& value, std::uint64_t seq,
    bool present) {
    std::vector<std::byte> out;
    put_u64(out, seq);
    put_u8(out, present ? 1u : 0u);
    put_str(out, key);
    put_str(out, value);
    // Trailing per-record CRC-32 over the record body just written. A torn or
    // bit-rotted body will not match this checksum on decode → clean failure.
    const std::uint32_t crc =
        crc32(std::span<const std::byte>(out.data(), out.size()));
    put_u32(out, crc);
    return out;
}

// Decode one WAL record from the front of `data`. On success sets `consumed` to
// the byte length of the record (body + the 4-byte CRC trailer). Returns false on
// a short/torn tail OR on a CRC MISMATCH (a torn/bit-rotted body that still parses
// within bounds). The caller stops at the first failing record — applying the
// valid prefix and discarding the torn record and everything after it, so a torn
// tail is LOST but NEVER applied as a fabricated committed value. (spec §4 C-INT/
// C-DUR; Phase 3 "no torn corruption survives recovery → consistent PREFIX".)
[[nodiscard]] inline bool decode_wal_record(std::span<const std::byte> data,
                                            WalRecord& rec,
                                            std::size_t& consumed) {
    ByteReader r(data);
    std::uint8_t present = 0;
    if (!r.u64(rec.seq)) {
        return false;
    }
    if (!r.u8(present)) {
        return false;
    }
    if (!r.str(rec.key) || !r.str(rec.value)) {
        return false;
    }
    const std::size_t body_len = r.consumed();  // [seq][present][key][value]
    std::uint32_t stored_crc = 0;
    if (!r.u32(stored_crc)) {
        return false;  // CRC trailer torn off the tail
    }
    // Recompute the CRC over the record body and verify. A mismatch means the
    // body (e.g. a value) was corrupted/torn — reject so recovery stops here.
    const std::uint32_t actual_crc =
        crc32(std::span<const std::byte>(data.data(), body_len));
    if (actual_crc != stored_crc) {
        return false;
    }
    rec.present = present != 0;
    consumed = r.consumed();
    return true;
}

}  // namespace lockstep::harness::kv
