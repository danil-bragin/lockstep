// storage_backup_test.cpp — logical backup / restore gate. A backup taken at a committed Snapshot,
// restored into a FRESH engine, reproduces EXACTLY the snapshot's live state (deleted keys stay
// gone); the same state backs up byte-identically (determinism); and a torn/corrupt or empty backup
// is REJECTED, never partially restored (integrity teeth). See storage/Backup.hpp.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/WalEngine.hpp>

using lockstep::core::Error;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::backup_engine;
using lockstep::storage::Engine;
using lockstep::storage::KeyValue;
using lockstep::storage::Range;
using lockstep::storage::restore_engine;
using lockstep::storage::Seq;
using lockstep::storage::Snapshot;
using lockstep::storage::WalEngine;
using lockstep::core::Task;

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

// Load a deterministic state: 50 keys, then DELETE three (tombstones — must NOT reappear in a
// backup). Returns the committed snapshot.
Task load(Engine& e, Snapshot& snap_out) {
    for (int i = 0; i < 50; ++i) {
        (void)co_await e.put("k" + std::to_string(i), "v" + std::to_string(i));
    }
    (void)co_await e.del("k10");
    (void)co_await e.del("k20");
    (void)co_await e.del("k30");
    (void)co_await e.sync();
    snap_out = co_await e.snapshot();
    co_return;
}

Task scan_all(Engine& e, Snapshot snap, std::vector<KeyValue>& out) {
    Range full;
    full.hi_unbounded = true;
    out = co_await e.scan(full, snap);
    co_return;
}

// Append `bytes` to a disk (used to stage a corrupted backup image for the teeth).
Task write_raw(lockstep::core::IDisk& d, std::vector<std::byte> bytes, Error& result) {
    lockstep::core::Offset off = 0;
    const Error ae = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (!ae.ok()) { result = ae; co_return; }
    result = co_await d.sync();
    co_return;
}
}  // namespace

int main() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0xBAC2BAC2u);
    const DiskFaultConfig dc = nofault();

    SimDisk src_disk(sched, clock, rng, dc);
    WalEngine src(sched, src_disk);
    Snapshot snap;
    sched.spawn(load(src, snap));
    sched.run();

    // (1) backup -> restore into a FRESH engine -> identical live state.
    SimDisk bk(sched, clock, rng, dc);
    check(backup_engine(sched, src, snap, bk).ok(), "backup ok");

    SimDisk dst_disk(sched, clock, rng, dc);
    WalEngine dst(sched, dst_disk);
    check(restore_engine(sched, bk, dst).ok(), "restore ok");

    std::vector<KeyValue> src_kvs;
    std::vector<KeyValue> dst_kvs;
    sched.spawn(scan_all(src, snap, src_kvs));
    sched.spawn(scan_all(dst, Snapshot{dst.last_seq()}, dst_kvs));
    sched.run();
    check(src_kvs == dst_kvs, "round-trip: restored live state == source @ snapshot");
    check(src_kvs.size() == 47, "47 live keys (50 put, 3 deleted)");
    // the deleted keys must be absent on BOTH sides.
    bool del_gone = true;
    for (const KeyValue& kv : dst_kvs)
        if (kv.first == "k10" || kv.first == "k20" || kv.first == "k30") del_gone = false;
    check(del_gone, "deleted keys do not reappear in the restored backup");

    // (2) determinism — the same state backs up to byte-identical bytes.
    SimDisk bk2(sched, clock, rng, dc);
    check(backup_engine(sched, src, snap, bk2).ok(), "second backup ok");
    check(bk.durable_snapshot() == bk2.durable_snapshot(), "two backups of one state are byte-identical");

    // (3) TEETH — an empty disk has no valid header -> rejected.
    SimDisk empty(sched, clock, rng, dc);
    WalEngine d3(sched, dst_disk);  // unused target
    check(!restore_engine(sched, empty, d3).ok(), "restore from an empty disk is rejected (bad magic)");

    // (4) TEETH — a single flipped payload byte -> CRC mismatch -> rejected (no partial restore).
    std::vector<std::byte> img = bk.durable_snapshot();
    check(img.size() > lockstep::storage::kBackupHeaderBytes, "backup image has a payload");
    img[lockstep::storage::kBackupHeaderBytes + 1] ^= std::byte{0x40};  // flip a bit in the payload
    SimDisk corrupt(sched, clock, rng, dc);
    Error wrote{lockstep::core::ErrorCode::Unknown, "norun"};
    sched.spawn(write_raw(corrupt, std::move(img), wrote));
    sched.run();
    check(wrote.ok(), "staged the corrupted image");
    SimDisk c_dst_disk(sched, clock, rng, dc);
    WalEngine c_dst(sched, c_dst_disk);
    const Error ce = restore_engine(sched, corrupt, c_dst);
    check(!ce.ok() && ce.code == lockstep::core::ErrorCode::Corruption,
          "a single flipped byte is caught by the CRC and the restore is rejected");

    if (g_fail) { std::printf("storage_backup_test: FAILED\n"); return 1; }
    std::printf("storage_backup_test: OK (backup/restore round-trip + determinism + integrity teeth)\n");
    return 0;
}
