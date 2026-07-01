// storage_state_hash_test.cpp — CROSS-REPLICA KEYSPACE HASH gate (storage/StateHash.hpp,
// plan P3). Proves the etcd corrupt-check primitive: replicas that applied the same
// committed op-log into an uncorrupted engine hash IDENTICALLY, and a replica whose
// APPLIED state diverged — a real storage bit-flip that truncates its recovered prefix,
// or a dropped op — is DETECTED by find_hash_divergence even though the intended op-log
// is the same. Pure/deterministic over the sim; runs everywhere.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/StateHash.hpp>
#include <lockstep/storage/WalEngine.hpp>

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::Engine;
using lockstep::storage::find_hash_divergence;
using lockstep::storage::keyspace_hash;
using lockstep::storage::KeyspaceHash;
using lockstep::storage::ReplicaHash;
using lockstep::storage::Seq;
using lockstep::storage::WalEngine;

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

// The committed op-log every replica applies (identical intent across replicas). `drop`
// (>=0) skips the op at that index — a materialised-state divergence with the SAME log.
Task apply_log(Engine& e, int drop) {
    for (int i = 0; i < 24; ++i) {
        if (i == drop) continue;
        (void)co_await e.put("k" + std::to_string(i), "val-" + std::to_string(i));
    }
    if (drop != 100) (void)co_await e.del("k7");  // a tombstone in the common log
    (void)co_await e.sync();
    co_return;
}
Task seed(IDisk& d, std::vector<std::byte> bytes, Error& res) {
    Offset off = 0;
    res = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (res.ok()) res = co_await d.sync();
    co_return;
}
Task recover_engine(WalEngine& e, std::size_t len, bool& ok) {
    const Error err = co_await e.recover(len);
    ok = err.ok();
    co_return;
}
}  // namespace

int main() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0xC0FFEE11u);
    const DiskFaultConfig dc = nofault();

    // Three replicas apply the SAME committed op-log into clean engines.
    SimDisk da(sched, clock, rng, dc), db(sched, clock, rng, dc), dc3(sched, clock, rng, dc);
    WalEngine a(sched, da), b(sched, db), c(sched, dc3);
    sched.spawn(apply_log(a, /*drop=*/-1));
    sched.spawn(apply_log(b, /*drop=*/-1));
    sched.spawn(apply_log(c, /*drop=*/-1));
    sched.run();
    const Seq commit = a.last_seq();
    check(b.last_seq() == commit && c.last_seq() == commit, "all replicas at the same commit index");

    // (1) identical applied state -> identical hash -> no divergence.
    const KeyspaceHash ha = keyspace_hash(sched, a, commit);
    const KeyspaceHash hb = keyspace_hash(sched, b, commit);
    const KeyspaceHash hc = keyspace_hash(sched, c, commit);
    check(ha.crc == hb.crc && hb.crc == hc.crc, "clean replicas hash identically");
    check(ha.crc != 0 && ha.payload_len > 0, "hash is non-trivial (keyspace non-empty)");
    {
        std::vector<ReplicaHash> rs = {{"n1", ha}, {"n2", hb}, {"n3", hc}};
        check(!find_hash_divergence(rs).has_value(), "no divergence across clean replicas");
    }

    // determinism: hashing the same replica twice is stable.
    check(keyspace_hash(sched, a, commit).crc == ha.crc, "keyspace_hash is deterministic");

    // (2) REAL BIT-ROT: flip a byte in replica A's durable WAL, recover into a fresh
    //     engine. Its applied keyspace truncates at the corruption (V-PREFIX) -> a
    //     DIFFERENT hash, though the committed op-log intent was identical. Detected.
    {
        std::vector<std::byte> img = da.durable_snapshot();
        check(img.size() > 40, "replica A has a durable WAL");
        img[img.size() / 2] ^= std::byte{0x80};  // bit-rot mid-log
        SimDisk dcorrupt(sched, clock, rng, dc);
        Error se{lockstep::core::ErrorCode::Unknown, "norun"};
        sched.spawn(seed(dcorrupt, img, se));
        sched.run();
        check(se.ok(), "seeded the bit-rotted WAL");
        WalEngine corrupt(sched, dcorrupt);
        bool rec_ok = false;
        sched.spawn(recover_engine(corrupt, img.size(), rec_ok));
        sched.run();
        check(rec_ok, "corrupt replica recovered to a (shorter) consistent prefix");
        const KeyspaceHash hbad = keyspace_hash(sched, corrupt, commit);
        check(hbad.crc != ha.crc, "bit-rotted replica hashes DIFFERENTLY from the clean ones");
        std::vector<ReplicaHash> rs = {{"n1", ha}, {"n2", hb}, {"n3", hbad}};
        const auto d = find_hash_divergence(rs);
        check(d.has_value() && d->other == 2 && d->other_node == "n3",
              "find_hash_divergence flags the bit-rotted replica (n3) — the CORRUPT alarm witness");
    }

    // (3) DROPPED OP: a replica that applied the same log minus one put has a different
    //     materialised keyspace -> detected. (The op-log agreement layer would still
    //     need the storage state hash to catch this class of applied-state divergence.)
    {
        SimDisk dd(sched, clock, rng, dc);
        WalEngine dropped(sched, dd);
        sched.spawn(apply_log(dropped, /*drop=*/5));  // missing k5
        sched.run();
        const KeyspaceHash hdrop = keyspace_hash(sched, dropped, dropped.last_seq());
        check(hdrop.crc != ha.crc, "replica missing one key hashes differently");
        std::vector<ReplicaHash> rs = {{"n1", ha}, {"n2", hb}, {"leader-detects", hdrop}};
        const auto d = find_hash_divergence(rs);
        check(d.has_value() && d->other_node == "leader-detects", "dropped-op divergence detected");
    }

    // (4) a lone replica (nothing to compare) is never a false alarm.
    {
        std::vector<ReplicaHash> one = {{"solo", ha}};
        check(!find_hash_divergence(one).has_value(), "single replica -> no divergence (no peers)");
    }

    if (g_fail) { std::printf("storage_state_hash_test: FAILED\n"); return 1; }
    std::printf("storage_state_hash_test: OK (clean agree + bit-rot detected + dropped-op detected + determinism)\n");
    return 0;
}
