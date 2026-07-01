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

int usage() {
    std::printf("usage: lockstep_recover <verify|dump> <file>\n");
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return usage();
    const std::string cmd = argv[1];
    const std::string path = argv[2];
    if (cmd != "verify" && cmd != "dump") return usage();

    Scheduler sched;
    ProdDisk disk(sched, path);
    if (!disk.valid()) {
        std::printf("error: cannot open '%s' (errno=%d)\n", path.c_str(), disk.open_errno());
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
