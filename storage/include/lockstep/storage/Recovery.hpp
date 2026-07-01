#pragma once
// Recovery.hpp — OFFLINE DIAGNOSTICS for the on-disk Lockstep formats (the read-only
// core of the operator recovery toolkit, plan item P2). Pure functions over a byte
// image: recognise a file by its magic, walk a WAL to its consistent-prefix boundary,
// and give an integrity verdict for the formats that carry a self-contained checksum
// (WAL / logical backup / PITR archive). NOTHING here mutates a byte or touches a
// disk — the CLI wrapper reads the file (through the sanctioned IDisk provider) and
// feeds the bytes in, so the same logic is testable everywhere over crafted images.
//
// This answers the two questions an operator has after a crash: "is this file intact,
// and if not, WHERE does the good data end?" — the detect layer that a force-truncate
// / restore step is later built on. A torn WAL tail is NORMAL and recoverable (the
// engine already recovers to the consistent prefix, V-PREFIX); inspect_wal reports
// exactly that prefix so a recovery tool can truncate to it.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/Codec.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/Pitr.hpp>
#include <lockstep/storage/WalEngine.hpp>

namespace lockstep::storage {

// The recognised on-disk formats (by magic). Unknown = not a Lockstep file (or a
// format this tool does not yet decode, e.g. an SSTable / manifest — increment 2).
enum class FileFormat : std::uint8_t { Unknown, Wal, Backup, PitrArchive };

[[nodiscard]] inline const char* format_name(FileFormat f) noexcept {
    switch (f) {
        case FileFormat::Wal: return "WAL";
        case FileFormat::Backup: return "backup";
        case FileFormat::PitrArchive: return "PITR-archive";
        case FileFormat::Unknown: return "unknown";
    }
    return "unknown";
}

// Recognise a file by its leading magic. WAL frames its magic as a u32 (get_u32),
// while the backup / PITR headers write their magic as literal ASCII — match each the
// way it is written so there is no endianness ambiguity.
[[nodiscard]] inline FileFormat detect_format(std::span<const std::byte> image) {
    if (image.size() >= 4 && get_u32(image.data()) == kWalMagic) return FileFormat::Wal;
    if (image.size() >= kBackupHeaderBytes && detail::backup_magic_ok(image.data())) return FileFormat::Backup;
    if (image.size() >= kPitrHeaderBytes && detail::pitr_magic_ok(image.data())) return FileFormat::PitrArchive;
    return FileFormat::Unknown;
}

// One decoded WAL record's summary (no value bytes — a diagnostic, not a dump of data).
struct WalRecordInfo {
    Seq seq = kNoSeq;
    std::uint8_t type = 0;  // 0 put · 1 del(tombstone) · 2 vlog-pointer put
    std::uint32_t klen = 0;
    std::uint32_t vlen = 0;
    std::size_t offset = 0;  // byte offset of the record within the image
};

// The result of walking a WAL image front-to-back to its consistent-prefix boundary.
struct WalInspection {
    std::vector<WalRecordInfo> records;    // every CRC-valid record in the prefix
    std::size_t valid_prefix_len = 0;      // bytes of the consistent prefix (a truncate target)
    std::size_t total_len = 0;             // the whole image length
    bool clean = false;                    // valid_prefix_len == total_len (no trailing garbage/torn tail)
    Seq max_seq = kNoSeq;                  // the last valid record's Seq (0 if none)
};

// Walk the WAL exactly as recover() does — stop at the FIRST record that fails its CRC,
// decodes short, or claims more bytes than survive (a torn tail). Everything up to that
// boundary is the consistent prefix the engine would recover (V-PREFIX / V-NOTORN); the
// boundary offset is where a force-truncate would cut. Pure, read-only.
[[nodiscard]] inline WalInspection inspect_wal(const std::vector<std::byte>& image) {
    WalInspection ins;
    ins.total_len = image.size();
    std::size_t pos = 0;
    while (pos < image.size()) {
        const DecodeResult dr = try_decode(image, pos);
        if (!dr.ok) break;  // consistent-prefix boundary.
        WalRecordInfo ri;
        ri.seq = dr.record.seq;
        ri.type = dr.record.tombstone ? 1u : (dr.record.vlog ? 2u : 0u);
        ri.klen = static_cast<std::uint32_t>(dr.record.key.size());
        ri.vlen = static_cast<std::uint32_t>(dr.record.value.size());
        ri.offset = pos;
        ins.records.push_back(ri);
        ins.max_seq = dr.record.seq;
        pos += dr.consumed;
    }
    ins.valid_prefix_len = pos;
    ins.clean = (pos == image.size());
    return ins;
}

// The byte length a WAL image would be truncated to for a clean recovery (the
// consistent-prefix boundary). Equal to total_len when the file is already clean.
[[nodiscard]] inline std::size_t wal_valid_prefix_len(const std::vector<std::byte>& image) {
    return inspect_wal(image).valid_prefix_len;
}

// An integrity verdict for a recognised file.
//   * WAL          — ok iff the whole file is a clean consistent prefix; otherwise a
//                    Corruption verdict naming the torn/garbage tail (RECOVERABLE by
//                    truncating to valid_prefix_len — a torn tail is expected after an
//                    unclean crash, never a hard failure).
//   * backup/PITR  — the all-or-nothing validator (magic + version + CRC + framing,
//                    plus Seq-contiguity for a PITR archive). No prefix concept.
//   * unknown      — rejected.
struct VerifyResult {
    core::Error error;                 // ok() == integrity holds
    FileFormat format = FileFormat::Unknown;
    std::size_t valid_prefix_len = 0;  // for a WAL: the recoverable prefix length
    std::size_t total_len = 0;
};

[[nodiscard]] inline VerifyResult verify_image(const std::vector<std::byte>& image) {
    VerifyResult r;
    r.total_len = image.size();
    r.format = detect_format(std::span<const std::byte>(image.data(), image.size()));
    switch (r.format) {
        case FileFormat::Wal: {
            const WalInspection ins = inspect_wal(image);
            r.valid_prefix_len = ins.valid_prefix_len;
            r.error = ins.clean
                          ? core::Error{}
                          : core::Error{core::ErrorCode::Corruption,
                                        "wal: torn/garbage tail after the consistent prefix "
                                        "(recoverable — truncate to valid_prefix_len)"};
            break;
        }
        case FileFormat::Backup:
            r.error = validate_backup_image(std::span<const std::byte>(image.data(), image.size()));
            r.valid_prefix_len = r.error.ok() ? image.size() : 0;
            break;
        case FileFormat::PitrArchive:
            r.error = validate_pitr_archive(std::span<const std::byte>(image.data(), image.size()));
            r.valid_prefix_len = r.error.ok() ? image.size() : 0;
            break;
        case FileFormat::Unknown:
            r.error = core::Error{core::ErrorCode::InvalidArgument,
                                  "unrecognised file (not a Lockstep WAL / backup / PITR archive)"};
            break;
    }
    return r;
}

}  // namespace lockstep::storage
