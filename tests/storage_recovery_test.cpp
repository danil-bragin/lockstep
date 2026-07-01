// storage_recovery_test.cpp — offline diagnostics gate (storage/Recovery.hpp), the
// read-only core of the operator recovery toolkit (plan P2). Proves: a real WAL is
// recognised and walked to its consistent-prefix boundary (record summaries match the
// committed ops); a torn / flipped tail is reported with the exact recoverable prefix
// length (never a hard failure — the engine recovers to that prefix); format detection
// across WAL / backup / PITR / unknown; and the integrity verdict for a good vs corrupt
// backup and PITR archive. Pure over byte images — runs everywhere.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/Pitr.hpp>
#include <lockstep/storage/Recovery.hpp>
#include <lockstep/storage/WalEngine.hpp>

using lockstep::core::Error;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::backup_engine_bytes;
using lockstep::storage::detect_format;
using lockstep::storage::Engine;
using lockstep::storage::FileFormat;
using lockstep::storage::inspect_wal;
using lockstep::storage::Snapshot;
using lockstep::storage::verify_image;
using lockstep::storage::WalInspection;
using lockstep::storage::WalEngine;
using lockstep::storage::wal_valid_prefix_len;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
DiskFaultConfig nofault() {
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 0;
    return dc;
}
std::span<const std::byte> sp(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }

Task load(Engine& e) {
    for (int i = 0; i < 8; ++i) (void)co_await e.put("k" + std::to_string(i), "v" + std::to_string(i));
    (void)co_await e.del("k3");           // a tombstone (type 1)
    (void)co_await e.put("k0", "v0-new");  // an update
    (void)co_await e.sync();
    co_return;
}
}  // namespace

int main() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0x5EC0BEEFu);
    const DiskFaultConfig dc = nofault();

    // A real WAL: run the engine, grab its durable disk bytes.
    SimDisk disk(sched, clock, rng, dc);
    WalEngine e(sched, disk);
    sched.spawn(load(e));
    sched.run();
    const std::vector<std::byte> wal = disk.durable_snapshot();
    const std::uint64_t committed = e.last_seq();  // 10 commits (8 puts + del + update)

    // (1) recognise + walk a clean WAL.
    check(detect_format(sp(wal)) == FileFormat::Wal, "WAL recognised by magic");
    const WalInspection ins = inspect_wal(wal);
    check(ins.clean, "clean WAL: valid_prefix_len == total_len");
    check(ins.valid_prefix_len == wal.size(), "clean prefix spans the whole file");
    check(static_cast<std::uint64_t>(ins.records.size()) == committed, "record count == committed op count");
    check(ins.max_seq == committed, "max_seq == last committed Seq");
    // the 9th op (index 8) was the del("k3") -> tombstone type 1.
    check(ins.records.size() >= 9 && ins.records[8].type == 1, "del record decoded as a tombstone (type 1)");
    check(verify_image(wal).error.ok(), "verify: clean WAL is ok");

    // (2) torn tail: append garbage past the last good record.
    {
        std::vector<std::byte> torn = wal;
        for (int i = 0; i < 11; ++i) torn.push_back(std::byte{0xAB});  // partial/garbage record
        const WalInspection t = inspect_wal(torn);
        check(!t.clean, "torn WAL: not clean");
        check(t.valid_prefix_len == wal.size(), "torn WAL: prefix ends exactly at the last good record");
        check(t.records.size() == ins.records.size(), "torn WAL: same good records recovered");
        const auto vr = verify_image(torn);
        check(!vr.error.ok() && vr.valid_prefix_len == wal.size(),
              "verify: torn WAL flagged, recoverable prefix length reported");
        check(wal_valid_prefix_len(torn) == wal.size(), "wal_valid_prefix_len == recoverable boundary");
    }

    // (3) flipped byte inside a record -> the prefix ends BEFORE that record.
    {
        std::vector<std::byte> flip = wal;
        // flip a byte in the middle of the file (inside some record's payload/crc window).
        flip[wal.size() / 2] ^= std::byte{0x80};
        const WalInspection f = inspect_wal(flip);
        check(!f.clean && f.valid_prefix_len < wal.size(), "flipped mid-record byte: prefix truncated before it");
        check(f.valid_prefix_len <= wal.size() / 2 + 21, "prefix ends at/around the damaged record boundary");
    }

    // (4) format detection across the other formats + unknown.
    {
        std::vector<std::byte> bk;
        Snapshot snap; snap.at = committed;
        check(backup_engine_bytes(sched, e, snap, bk).ok(), "made a backup image");
        check(detect_format(sp(bk)) == FileFormat::Backup, "backup recognised");
        check(verify_image(bk).error.ok(), "verify: good backup ok");
        // flip a payload byte -> Corruption.
        bk[lockstep::storage::kBackupHeaderBytes + 1] ^= std::byte{0x10};
        check(!verify_image(bk).error.ok(), "verify: corrupt backup rejected");

        std::vector<std::byte> arc;
        check(lockstep::storage::archive_ops_bytes(sched, e, 0, arc).ok(), "made a PITR archive image");
        check(detect_format(sp(arc)) == FileFormat::PitrArchive, "PITR archive recognised");
        check(verify_image(arc).error.ok(), "verify: good PITR archive ok");
        arc[lockstep::storage::kPitrHeaderBytes + 3] ^= std::byte{0x22};
        check(!verify_image(arc).error.ok(), "verify: corrupt PITR archive rejected");

        std::vector<std::byte> junk(64, std::byte{0x00});
        check(detect_format(sp(junk)) == FileFormat::Unknown, "random bytes -> unknown");
        check(!verify_image(junk).error.ok(), "verify: unknown file rejected");
        std::vector<std::byte> empty;
        check(detect_format(sp(empty)) == FileFormat::Unknown, "empty file -> unknown");
    }

    if (g_fail) { std::printf("storage_recovery_test: FAILED\n"); return 1; }
    std::printf("storage_recovery_test: OK (detect + WAL prefix walk + torn/flip boundary + backup/PITR verdict)\n");
    return 0;
}
