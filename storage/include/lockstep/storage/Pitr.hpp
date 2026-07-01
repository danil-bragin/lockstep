#pragma once
// Pitr.hpp — POINT-IN-TIME RECOVERY (PITR) for the Lockstep storage engine.
//
// Where Backup.hpp captures the LIVE VALUES at ONE committed Snapshot (a logical
// point-in-time image), PITR captures the committed OP-LOG so a restore can rebuild
// the database state as-of ANY commit Seq — the defence against LOGICAL corruption
// (a bad DELETE, an operator mistake, an app bug that wrote valid-but-wrong bytes):
// restore to the Seq just BEFORE the incident and the damage is undone.
//
//   archive_ops(sched, src, from_seq, out) — export the committed op-log (seq >=
//     from_seq) from a WalEngine into a self-contained, CRC-protected archive on
//     `out`. Vlog pointers are derefed to inline values (portable). The exported
//     run MUST be a gap-free [from_seq .. tip] (WalEngine::export_ops enforces it),
//     so the archive is a complete, replayable prefix of history.
//   restore_pitr(sched, in, target_seq, dst) — CRC-verify the whole archive BEFORE
//     applying anything, then REPLAY the ops with (original) seq <= target_seq, in
//     Seq order, into a FRESH engine `dst`. `dst` ends holding EXACTLY the live
//     state the source had as-of target_seq. target_seq is expressed in the
//     ORIGINAL commit-Seq space carried in each record, so it is stable regardless
//     of how `dst` renumbers its own commits on replay.
//
// STREAM LAYOUT (little-endian; same Crc32 as the WAL/SSTable/backup codecs):
//   header (44 bytes): magic "LWA1"(4) · version u32 · from_seq u64 · to_seq u64 ·
//                      count u64 · payload_len u64 · payload_crc u32
//   payload          : repeated record { seq u64 · type u8 (0 put | 1 del) ·
//                      klen u32 · vlen u32 · key[klen] · value[vlen] }, Seq-ASCENDING,
//                      strictly contiguous (seq_i+1 == seq_{i+1}); count records.
//
// INVARIANTS:
//   V-PITR-PREFIX : a validated archive is a gap-free, ascending run [from_seq..to_seq];
//                   restore@target yields the live state after applying every committed
//                   op with seq <= target and NONE with seq > target (a consistent prefix).
//   V-PITR-DET    : the same op-log archives to BYTE-IDENTICAL bytes (deterministic scan +
//                   the shared CRC32), and the same (archive, target) restores to the same state.
//   V-NOTORN      : a torn / flipped / truncated / out-of-order archive is REJECTED whole —
//                   NEVER partially restored (CRC + framing + Seq-contiguity checked before apply).
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
#include <lockstep/storage/WalEngine.hpp>

namespace lockstep::storage {

inline constexpr std::size_t kPitrHeaderBytes = 44;
inline constexpr std::uint32_t kPitrVersion = 1;
inline constexpr std::size_t kPitrRecordHeaderBytes = 17;  // seq u64 + type u8 + klen u32 + vlen u32

namespace detail {
[[nodiscard]] inline bool pitr_magic_ok(const std::byte* p) noexcept {
    return std::to_integer<char>(p[0]) == 'L' && std::to_integer<char>(p[1]) == 'W' &&
           std::to_integer<char>(p[2]) == 'A' && std::to_integer<char>(p[3]) == '1';
}

// Serialise an already-exported op-log into the self-contained archive image
// (header + payload). Pure — the SAME bytes whether written to a disk or kept in
// memory (V-PITR-DET). `ops` MUST be ascending, gap-free (export_ops guarantees it).
inline void build_archive_image(const std::vector<ExportedOp>& ops, std::vector<std::byte>& image) {
    std::vector<std::byte> payload;
    for (const ExportedOp& op : ops) {
        put_u64(payload, op.seq);
        payload.push_back(static_cast<std::byte>(op.tombstone ? 1u : 0u));
        put_u32(payload, static_cast<std::uint32_t>(op.key.size()));
        put_u32(payload, static_cast<std::uint32_t>(op.tombstone ? 0u : op.value.size()));
        for (char c : op.key) payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        if (!op.tombstone) {
            for (char c : op.value) payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    }
    const std::uint64_t from_seq = ops.empty() ? 0 : ops.front().seq;
    const std::uint64_t to_seq = ops.empty() ? 0 : ops.back().seq;
    const std::uint32_t crc = Crc32::compute(payload);

    image.clear();
    for (char c : std::string_view("LWA1")) image.push_back(static_cast<std::byte>(c));
    put_u32(image, kPitrVersion);
    put_u64(image, from_seq);
    put_u64(image, to_seq);
    put_u64(image, static_cast<std::uint64_t>(ops.size()));
    put_u64(image, static_cast<std::uint64_t>(payload.size()));
    put_u32(image, crc);
    image.insert(image.end(), payload.begin(), payload.end());
}

// FULLY validate an in-memory archive image WITHOUT applying it: magic, version,
// payload CRC, record framing, AND the Seq-contiguity contract (ascending, gap-free,
// front==from_seq, back==to_seq, exactly `count` records). A CRC-valid but
// out-of-order / gapped / miscounted payload is ALSO rejected (V-NOTORN / V-PITR-PREFIX).
inline core::Error validate_archive_image(std::span<const std::byte> image) {
    if (image.size() < kPitrHeaderBytes || !pitr_magic_ok(image.data())) {
        return core::Error{core::ErrorCode::Corruption, "pitr: bad magic"};
    }
    if (get_u32(image.data() + 4) != kPitrVersion) {
        return core::Error{core::ErrorCode::InvalidArgument, "pitr: unsupported version"};
    }
    const std::uint64_t from_seq = get_u64(image.data() + 8);
    const std::uint64_t to_seq = get_u64(image.data() + 16);
    const std::uint64_t count = get_u64(image.data() + 24);
    const std::uint64_t payload_len = get_u64(image.data() + 32);
    const std::uint32_t want_crc = get_u32(image.data() + 40);
    if (static_cast<std::uint64_t>(kPitrHeaderBytes) + payload_len > image.size()) {
        return core::Error{core::ErrorCode::Corruption, "pitr: truncated image"};
    }
    std::span<const std::byte> payload = image.subspan(kPitrHeaderBytes, static_cast<std::size_t>(payload_len));
    if (Crc32::compute(payload) != want_crc) {
        return core::Error{core::ErrorCode::Corruption, "pitr: CRC mismatch (torn/corrupt archive)"};
    }
    std::size_t pos = 0;
    std::uint64_t seen = 0;
    std::uint64_t prev = 0;
    while (pos < payload.size()) {
        if (pos + kPitrRecordHeaderBytes > payload.size())
            return core::Error{core::ErrorCode::Corruption, "pitr: truncated record header"};
        const std::uint64_t seq = get_u64(payload.data() + pos);
        const std::uint8_t type = std::to_integer<std::uint8_t>(payload[pos + 8]);
        const std::uint32_t klen = get_u32(payload.data() + pos + 9);
        const std::uint32_t vlen = get_u32(payload.data() + pos + 13);
        if (type > 1) return core::Error{core::ErrorCode::Corruption, "pitr: bad record type"};
        pos += kPitrRecordHeaderBytes;
        if (pos + static_cast<std::size_t>(klen) + vlen > payload.size())
            return core::Error{core::ErrorCode::Corruption, "pitr: truncated record"};
        pos += static_cast<std::size_t>(klen) + vlen;
        // Seq-contiguity: strictly ascending by exactly 1 from `from_seq`.
        const std::uint64_t want = (seen == 0) ? from_seq : prev + 1;
        if (seq != want) return core::Error{core::ErrorCode::Corruption, "pitr: non-contiguous op Seq"};
        prev = seq;
        ++seen;
    }
    if (seen != count) return core::Error{core::ErrorCode::Corruption, "pitr: record count mismatch"};
    if (seen != 0 && prev != to_seq) return core::Error{core::ErrorCode::Corruption, "pitr: to_seq mismatch"};
    return core::Error{};
}

// CRC-verify + Seq-validate an archive image, then REPLAY every op with (original)
// seq <= target_seq into `dst`, in Seq order (put / del). A target beyond the
// archive's to_seq is REJECTED (we cannot reproduce a state we do not hold); a
// target below from_seq replays nothing (the empty prefix). Same all-or-nothing
// integrity contract as the backup path — a corrupt archive leaves `dst` untouched.
inline core::Task apply_archive_image(std::span<const std::byte> image, Seq target_seq, Engine& dst,
                                      core::Error& result) {
    if (const core::Error e = validate_archive_image(image); !e.ok()) {
        result = e;
        co_return;
    }
    const std::uint64_t to_seq = get_u64(image.data() + 16);
    const std::uint64_t payload_len = get_u64(image.data() + 32);
    if (static_cast<std::uint64_t>(target_seq) > to_seq) {
        result = core::Error{core::ErrorCode::InvalidArgument, "pitr: target beyond archive"};
        co_return;
    }
    std::span<const std::byte> payload = image.subspan(kPitrHeaderBytes, static_cast<std::size_t>(payload_len));
    std::size_t pos = 0;
    while (pos < payload.size()) {
        const std::uint64_t seq = get_u64(payload.data() + pos);
        const std::uint8_t type = std::to_integer<std::uint8_t>(payload[pos + 8]);
        const std::uint32_t klen = get_u32(payload.data() + pos + 9);
        const std::uint32_t vlen = get_u32(payload.data() + pos + 13);
        pos += kPitrRecordHeaderBytes;
        std::string key(reinterpret_cast<const char*>(payload.data() + pos), klen);
        pos += klen;
        std::string val(reinterpret_cast<const char*>(payload.data() + pos), vlen);
        pos += vlen;
        if (seq > static_cast<std::uint64_t>(target_seq)) {
            break;  // ascending contiguous ⇒ everything after is also > target.
        }
        if (type == 1) {
            (void)co_await dst.del(std::move(key));
        } else {
            (void)co_await dst.put(std::move(key), std::move(val));
        }
    }
    result = co_await dst.sync();
    co_return;
}

inline core::Task archive_task(WalEngine& src, Seq from_seq, core::IDisk& out, core::Error& result) {
    std::vector<ExportedOp> ops;
    const core::Error ee = co_await src.export_ops(from_seq, ops);
    if (!ee.ok()) { result = ee; co_return; }
    std::vector<std::byte> image;
    build_archive_image(ops, image);
    core::Offset off = 0;
    const core::Error ae = co_await out.append(std::span<const std::byte>(image.data(), image.size()), off);
    if (!ae.ok()) { result = ae; co_return; }
    result = co_await out.sync();
    co_return;
}

inline core::Task archive_bytes_task(WalEngine& src, Seq from_seq, std::vector<std::byte>& out,
                                     core::Error& result) {
    std::vector<ExportedOp> ops;
    const core::Error ee = co_await src.export_ops(from_seq, ops);
    if (!ee.ok()) { result = ee; co_return; }
    build_archive_image(ops, out);
    co_return;
}

inline core::Task restore_task(core::IDisk& in, Seq target_seq, Engine& dst, core::Error& result) {
    std::array<std::byte, kPitrHeaderBytes> hdr{};
    if (const core::Error e = co_await in.read(0, std::span<std::byte>(hdr.data(), hdr.size())); !e.ok()) {
        result = e;
        co_return;
    }
    if (!pitr_magic_ok(hdr.data())) { result = core::Error{core::ErrorCode::Corruption, "pitr: bad magic"}; co_return; }
    if (get_u32(hdr.data() + 4) != kPitrVersion) {
        result = core::Error{core::ErrorCode::InvalidArgument, "pitr: unsupported version"};
        co_return;
    }
    const std::uint64_t payload_len = get_u64(hdr.data() + 32);
    std::vector<std::byte> image(static_cast<std::size_t>(kPitrHeaderBytes) + static_cast<std::size_t>(payload_len));
    std::copy(hdr.begin(), hdr.end(), image.begin());
    if (payload_len > 0) {
        if (const core::Error e = co_await in.read(static_cast<core::Offset>(kPitrHeaderBytes),
                                                   std::span<std::byte>(image.data() + kPitrHeaderBytes,
                                                                        static_cast<std::size_t>(payload_len)));
            !e.ok()) {
            result = e;
            co_return;
        }
    }
    co_await apply_archive_image(std::span<const std::byte>(image.data(), image.size()), target_seq, dst, result);
    co_return;
}
}  // namespace detail

// Write a CRC-protected PITR archive of `src`'s committed op-log (seq >= from_seq)
// to `out`. Drives export + write on `sched` (engine + disk must share it). A flush
// that ate part of the log below from_seq is refused (honest, no silent gap).
[[nodiscard]] inline core::Error archive_ops(core::Scheduler& sched, WalEngine& src, Seq from_seq,
                                             core::IDisk& out) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "pitr archive: did not run"};
    sched.spawn(detail::archive_task(src, from_seq, out, result));
    sched.run();
    return result;
}

// Restore `dst` (a FRESH engine) to the live state `src` had as-of target_seq, from
// the archive on `in`. CRC + framing + Seq-contiguity are verified before ANY apply,
// so a corrupt archive leaves `dst` untouched (V-NOTORN).
[[nodiscard]] inline core::Error restore_pitr(core::Scheduler& sched, core::IDisk& in, Seq target_seq,
                                              Engine& dst) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "pitr restore: did not run"};
    sched.spawn(detail::restore_task(in, target_seq, dst, result));
    sched.run();
    return result;
}

// In-memory variants: produce / consume the SAME self-contained image without an
// intermediary IDisk (parity with backup_engine_bytes / restore_engine_bytes).
[[nodiscard]] inline core::Error archive_ops_bytes(core::Scheduler& sched, WalEngine& src, Seq from_seq,
                                                   std::vector<std::byte>& out) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "pitr archive: did not run"};
    sched.spawn(detail::archive_bytes_task(src, from_seq, out, result));
    sched.run();
    return result;
}

// FULLY validate an in-memory archive image (magic + version + CRC + framing +
// Seq-contiguity) WITHOUT applying it. Pure (no scheduler).
[[nodiscard]] inline core::Error validate_pitr_archive(std::span<const std::byte> image) {
    return detail::validate_archive_image(image);
}

[[nodiscard]] inline core::Error restore_pitr_bytes(core::Scheduler& sched, std::span<const std::byte> image,
                                                    Seq target_seq, Engine& dst) {
    core::Error result = core::Error{core::ErrorCode::Unknown, "pitr restore: did not run"};
    sched.spawn(detail::apply_archive_image(image, target_seq, dst, result));
    sched.run();
    return result;
}

}  // namespace lockstep::storage
