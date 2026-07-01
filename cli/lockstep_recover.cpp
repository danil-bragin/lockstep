// lockstep_recover.cpp — OFFLINE RECOVERY / DIAGNOSTICS CLI (operator toolkit, plan P2).
//
// Read-only inspection of an on-disk Lockstep file (a WAL — lockstepd.wal /
// lockstepd-sql.wal / lockstepd-sql-catalog.wal / consensus.wal — or a logical backup /
// PITR archive). Answers the two questions an operator has after a crash: "is this file
// intact, and if not, WHERE does the good data end?"
//
//   lockstep_recover verify <file>   integrity verdict; exit 0 = clean, 1 = corrupt/torn,
//                                    2 = usage/IO error. For a WAL, a torn tail is NOT a
//                                    hard failure — it prints the recoverable prefix length
//                                    (the engine recovers to that consistent prefix).
//   lockstep_recover dump   <file>   decode + print each WAL record (offset, Seq, op, key/
//                                    value lengths) up to the consistent-prefix boundary.
//
// The file is read through the sanctioned IDisk provider (ProdDisk) — this TU touches no
// raw syscall (forbidden-lint scans cli/). All decode/verify logic is the pure
// storage/Recovery.hpp core, so it is exercised by storage_recovery_test on every host;
// this daemon-side wrapper is built on Linux only (like lockstepd), so the macOS host
// stays green. NOTE: this increment is read-only; the destructive `force-truncate`
// (truncate a WAL to its consistent prefix, guarded by --force) is a later increment.
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/storage/Recovery.hpp>

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Scheduler;
using lockstep::core::Task;
using lockstep::prod::ProdDisk;
namespace st = lockstep::storage;

namespace {
Task read_all(IDisk& d, std::uint64_t len, std::vector<std::byte>& out, Error& res) {
    out.resize(static_cast<std::size_t>(len));
    if (len > 0) {
        res = co_await d.read(0, std::span<std::byte>(out.data(), out.size()));
    } else {
        res = Error{};
    }
    co_return;
}

const char* op_name(std::uint8_t type) {
    switch (type) {
        case 0: return "put";
        case 1: return "del";
        case 2: return "put(vlog)";
        default: return "?";
    }
}

// Read an existing file's bytes through ProdDisk (create=false — never create). Returns
// false (and leaves `image` empty) if the file is absent or unreadable.
bool read_file(const std::string& path, std::vector<std::byte>& image) {
    Scheduler sched;
    ProdDisk disk(sched, path, /*dir_fd=*/-1, /*create=*/false);
    if (!disk.valid()) return false;
    Error rd{lockstep::core::ErrorCode::Unknown, "norun"};
    sched.spawn(read_all(disk, disk.logical_len(), image, rd));
    sched.run();
    return rd.ok();
}

// Verify one known file if present; print its verdict; bump the counters. Absent = skip.
void scrub_one(const std::string& path, int& present, int& corrupt) {
    std::vector<std::byte> image;
    if (!read_file(path, image)) return;  // not present in this data dir — skip.
    ++present;
    const st::VerifyResult vr = st::verify_image(image);
    if (vr.error.ok()) {
        std::printf("  OK       %-40s (%s, %llu bytes)\n", path.c_str(), st::format_name(vr.format),
                    static_cast<unsigned long long>(vr.total_len));
    } else {
        ++corrupt;
        std::printf("  CORRUPT  %-40s (%s) — %s\n", path.c_str(), st::format_name(vr.format),
                    vr.error.detail.empty() ? "integrity check failed" : std::string(vr.error.detail).c_str());
    }
}

int usage() {
    std::printf("usage: lockstep_recover <verify|dump|force-truncate|scrub> <file|data-dir> [--force]\n"
                "  verify          integrity verdict for one file (exit 0 clean / 1 corrupt / 2 error)\n"
                "  dump            print each WAL record up to the consistent-prefix boundary\n"
                "  force-truncate  cut a torn WAL back to its consistent prefix (needs --force)\n"
                "  scrub           verify every known durable file under a data dir (exit 0 all-clean / 1 any-corrupt)\n");
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    // Parse: <cmd> <file> plus an optional --force flag in any position.
    std::string cmd;
    std::string path;
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") {
            force = true;
        } else if (cmd.empty()) {
            cmd = a;
        } else if (path.empty()) {
            path = a;
        } else {
            return usage();
        }
    }
    if (cmd.empty() || path.empty()) return usage();
    if (cmd != "verify" && cmd != "dump" && cmd != "force-truncate" && cmd != "scrub") return usage();

    // scrub <data-dir>: verify every KNOWN durable file (fixed names — no directory
    // listing, so no raw dir IO) under the data dir and its shard_<i> subdirs. Absent
    // files are skipped. Read-only: reports corruption but never truncates (run
    // force-truncate on the named file to recover it).
    if (cmd == "scrub") {
        static const char* kNames[] = {"lockstepd.wal", "lockstepd-sql.wal",
                                       "lockstepd-sql-catalog.wal", "consensus.wal"};
        int present = 0, corrupt = 0;
        std::printf("scrub: %s\n", path.c_str());
        for (const char* n : kNames) scrub_one(path + "/" + n, present, corrupt);
        for (int s = 0; s < 64; ++s) {  // probe shard_<i> subdirs (multi-shard daemon layout)
            const std::string sd = path + "/shard_" + std::to_string(s);
            for (const char* n : kNames) scrub_one(sd + "/" + n, present, corrupt);
        }
        if (present == 0) {
            std::printf("scrub: no known Lockstep files found under '%s'\n", path.c_str());
            return 2;
        }
        std::printf("scrub: %d file(s) checked, %d corrupt\n", present, corrupt);
        return corrupt > 0 ? 1 : 0;
    }

    Scheduler sched;
    ProdDisk disk(sched, path, /*dir_fd=*/-1, /*create=*/false);  // inspect existing file; never create.
    if (!disk.valid()) {
        std::printf("error: cannot open '%s' (errno=%d — file not found?)\n", path.c_str(), disk.open_errno());
        return 2;
    }
    const std::uint64_t len = disk.logical_len();

    std::vector<std::byte> image;
    Error rd{lockstep::core::ErrorCode::Unknown, "recover: read did not run"};
    sched.spawn(read_all(disk, len, image, rd));
    sched.run();
    if (!rd.ok()) {
        std::printf("error: could not read '%s' (%s)\n", path.c_str(),
                    rd.detail.empty() ? "IO error" : std::string(rd.detail).c_str());
        return 2;
    }

    const st::FileFormat fmt = st::detect_format(std::span<const std::byte>(image.data(), image.size()));

    if (cmd == "dump") {
        std::printf("file: %s  (%llu bytes, format=%s)\n", path.c_str(),
                    static_cast<unsigned long long>(len), st::format_name(fmt));
        if (fmt != st::FileFormat::Wal) {
            std::printf("dump: only WAL files carry a per-record log; run `verify` for this format.\n");
            const st::VerifyResult vr = st::verify_image(image);
            return vr.error.ok() ? 0 : 1;
        }
        const st::WalInspection ins = st::inspect_wal(image);
        for (const st::WalRecordInfo& r : ins.records) {
            std::printf("  @%-8llu seq=%-8llu %-9s klen=%-5u vlen=%-8u\n",
                        static_cast<unsigned long long>(r.offset),
                        static_cast<unsigned long long>(r.seq), op_name(r.type), r.klen, r.vlen);
        }
        std::printf("%llu record(s), consistent prefix = %llu / %llu bytes%s\n",
                    static_cast<unsigned long long>(ins.records.size()),
                    static_cast<unsigned long long>(ins.valid_prefix_len),
                    static_cast<unsigned long long>(ins.total_len),
                    ins.clean ? " (clean)" : "  <-- TORN/GARBAGE TAIL");
        return ins.clean ? 0 : 1;
    }

    if (cmd == "force-truncate") {
        if (fmt != st::FileFormat::Wal) {
            std::printf("force-truncate: only applies to WAL files (this is a %s file)\n",
                        st::format_name(fmt));
            return 2;
        }
        const st::WalInspection ins = st::inspect_wal(image);
        if (ins.clean) {
            std::printf("force-truncate: '%s' is already clean (%llu bytes, %llu records) — nothing to do\n",
                        path.c_str(), static_cast<unsigned long long>(ins.total_len),
                        static_cast<unsigned long long>(ins.records.size()));
            return 0;
        }
        const std::uint64_t drop = ins.total_len - ins.valid_prefix_len;
        if (!force) {
            std::printf("force-truncate: '%s' has a torn/garbage tail.\n"
                        "  WOULD keep the %llu-byte consistent prefix (%llu record(s), up to seq %llu)\n"
                        "  WOULD drop %llu tail byte(s) (a torn/uncommitted suffix).\n"
                        "Re-run with --force to apply (DESTRUCTIVE — the dropped bytes are gone).\n",
                        path.c_str(), static_cast<unsigned long long>(ins.valid_prefix_len),
                        static_cast<unsigned long long>(ins.records.size()),
                        static_cast<unsigned long long>(ins.max_seq),
                        static_cast<unsigned long long>(drop));
            return 3;  // refused — needs --force.
        }
        if (!disk.truncate_to(ins.valid_prefix_len)) {
            std::printf("force-truncate: FAILED to truncate '%s'\n", path.c_str());
            return 2;
        }
        std::printf("force-truncate: '%s' truncated to %llu bytes (dropped %llu tail byte(s)); "
                    "the WAL now opens to its consistent prefix (up to seq %llu).\n",
                    path.c_str(), static_cast<unsigned long long>(ins.valid_prefix_len),
                    static_cast<unsigned long long>(drop), static_cast<unsigned long long>(ins.max_seq));
        return 0;
    }

    // verify
    const st::VerifyResult vr = st::verify_image(image);
    std::printf("file: %s  (%llu bytes, format=%s)\n", path.c_str(),
                static_cast<unsigned long long>(len), st::format_name(vr.format));
    if (vr.error.ok()) {
        std::printf("verify: OK — intact\n");
        return 0;
    }
    std::printf("verify: CORRUPT — %s\n",
                vr.error.detail.empty() ? "integrity check failed" : std::string(vr.error.detail).c_str());
    if (vr.format == st::FileFormat::Wal) {
        std::printf("        recoverable consistent prefix = %llu / %llu bytes "
                    "(a force-truncate to this length would recover the WAL)\n",
                    static_cast<unsigned long long>(vr.valid_prefix_len),
                    static_cast<unsigned long long>(vr.total_len));
    }
    return 1;
}
