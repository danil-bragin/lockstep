// consensus_state_hash_test.cpp — the APPLY-SEAM + cross-replica keyspace hash over a
// REAL replicated Raft cluster (plan P3, part 2). A fault-storm run commits KEYED ops
// (encoded via storage/KeyedOp.hpp); each live replica APPLIES its committed prefix into
// an independent WalEngine state machine; then keyspace_hash (storage/StateHash.hpp) is
// cross-checked at a COMMON commit index. This proves: (1) the apply-seam is
// deterministic — replicas that agree on the committed log build byte-identical
// keyspaces; (2) the etcd corrupt-check value-add — a replica whose APPLIED state is
// bit-rotted diverges by hash even though the committed LOG still agrees (so the existing
// log cross-check would pass while this catches it). Plus a KeyedOp round-trip unit.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

#include <lockstep/storage/KeyedOp.hpp>
#include <lockstep/storage/StateHash.hpp>
#include <lockstep/storage/WalEngine.hpp>

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::Index;
using lockstep::consensus::LogEntry;
using lockstep::consensus::NodeSnapshot;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::run_cluster;
using lockstep::consensus::raft_a::make_raft_a_factory;

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::decode_keyed_op;
using lockstep::storage::encode_keyed_op;
using lockstep::storage::find_hash_divergence;
using lockstep::storage::keyspace_hash;
using lockstep::storage::KeyedOp;
using lockstep::storage::KeyspaceHash;
using lockstep::storage::ReplicaHash;
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

// The state machine: apply a committed-log prefix into `e`. Each committed value is a
// keyed op (KeyedOp); an entry that does not decode (a no-op / non-keyed payload) is
// skipped — every replica skips the SAME entries, so the applied keyspace stays a
// deterministic function of the committed prefix.
Task apply_prefix(WalEngine& e, const std::vector<LogEntry>& log, Index upto) {
    for (Index j = 0; j < upto && j < log.size(); ++j) {
        KeyedOp op;
        if (!decode_keyed_op(log[static_cast<std::size_t>(j)].value, op)) continue;
        if (op.del) {
            (void)co_await e.del(op.key);
        } else {
            (void)co_await e.put(op.key, op.value);
        }
    }
    (void)co_await e.sync();
    co_return;
}
Task seed_disk(IDisk& d, std::vector<std::byte> bytes, Error& res) {
    Offset off = 0;
    res = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (res.ok()) res = co_await d.sync();
    co_return;
}
Task recover_only(WalEngine& e, std::size_t len, bool& ok) {
    ok = (co_await e.recover(len)).ok();
    co_return;
}

// A workload that submits KEYED ops over a small key space (so the state machine sees
// real overwrites + deletes — the keyspace is NOT a trivial mirror of the log).
std::string keyed_workload(std::uint64_t /*cid*/, std::uint64_t /*i*/, std::uint64_t op_id) {
    KeyedOp op;
    op.key = "k" + std::to_string(op_id % 6);
    if (op_id % 7 == 0) {
        op.del = true;
    } else {
        op.value = "v" + std::to_string(op_id);
    }
    return encode_keyed_op(op);
}
}  // namespace

int main() {
    // ---- KeyedOp codec round-trip + teeth ------------------------------------
    {
        for (const KeyedOp& op : std::vector<KeyedOp>{
                 {false, "alpha", "one"}, {true, "beta", ""}, {false, "", "empty-key"},
                 {false, std::string("has\0null", 8), std::string("v\0v", 3)}}) {
            KeyedOp back;
            check(decode_keyed_op(encode_keyed_op(op), back), "keyed op decodes");
            check(back.del == op.del && back.key == op.key && back.value == op.value,
                  "keyed op round-trips exactly (incl. embedded NULs)");
        }
        KeyedOp junk;
        check(!decode_keyed_op("x", junk), "too-short value rejected");
        check(!decode_keyed_op(std::string(5, '\0'), junk) == false, "5-byte zero header decodes (klen 0)");
        // a klen that overruns the buffer is rejected.
        check(!decode_keyed_op(std::string("\x00\xff\xff\xff\xff", 5), junk), "overrunning klen rejected");
    }

    Scheduler h_sched;
    SimClock h_clock(h_sched);
    SeededRandom h_rng(0x5A17'0F00u);
    const DiskFaultConfig dc = nofault();

    int seeds_with_prefix = 0, corruption_checks = 0;
    for (std::uint64_t seed = 0xA1; seed <= 0xB4; ++seed) {
        ClusterConfig cfg;              // 3 nodes, full fault envelope (partitions/crashes/lossy bus)
        cfg.value_fn = keyed_workload;  // submit encoded keyed ops
        const ObservedRun run = run_cluster(seed, make_raft_a_factory(), cfg);
        if (run.snapshots.empty()) continue;
        const auto& fin = run.snapshots.back();

        // Live nodes with a committed prefix; the common index they all agree on.
        std::vector<const NodeSnapshot*> live;
        Index common = UINT64_MAX;
        for (const NodeSnapshot& n : fin.nodes) {
            if (!n.live || n.commit_index == 0) continue;
            live.push_back(&n);
            common = std::min(common, n.commit_index);
        }
        if (live.size() < 2 || common == 0 || common == UINT64_MAX) continue;
        ++seeds_with_prefix;

        // Baseline: the committed prefixes agree (what the existing log cross-check sees).
        bool logs_agree = true;
        for (std::size_t k = 1; k < live.size(); ++k)
            for (Index j = 0; j < common; ++j)
                if (live[k]->log[static_cast<std::size_t>(j)].value !=
                    live[0]->log[static_cast<std::size_t>(j)].value)
                    logs_agree = false;
        check(logs_agree, "committed prefixes agree across live replicas (log cross-check baseline)");

        // Apply each live replica's committed prefix into its own engine; hash it.
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed);
        std::vector<std::unique_ptr<SimDisk>> disks;
        std::vector<std::unique_ptr<WalEngine>> engines;
        std::vector<ReplicaHash> hashes;
        for (const NodeSnapshot* n : live) {
            disks.push_back(std::make_unique<SimDisk>(sched, clock, rng, dc));
            engines.push_back(std::make_unique<WalEngine>(sched, *disks.back()));
            sched.spawn(apply_prefix(*engines.back(), n->log, common));
            sched.run();
            hashes.push_back(ReplicaHash{"n" + std::to_string(n->node_id),
                                         keyspace_hash(sched, *engines.back(), engines.back()->last_seq())});
        }

        // (1) clean replicas: applied keyspaces agree -> no divergence.
        check(!find_hash_divergence(hashes).has_value(),
              "clean replicas agree on keyspace hash over REAL replicated commits");

        // (2) corrupt ONE replica's APPLIED store (bit-rot its durable WAL, recover to a
        //     truncated prefix). Its hash diverges though the committed LOG still agrees.
        {
            std::vector<std::byte> img = disks[0]->durable_snapshot();
            if (img.size() > 32) {
                img[img.size() / 2] ^= std::byte{0x80};
                SimDisk cdisk(sched, clock, rng, dc);
                Error se{lockstep::core::ErrorCode::Unknown, "norun"};
                sched.spawn(seed_disk(cdisk, img, se));
                sched.run();
                WalEngine corrupt(sched, cdisk);
                bool rec_ok = false;
                sched.spawn(recover_only(corrupt, img.size(), rec_ok));
                sched.run();
                if (rec_ok) {
                    ++corruption_checks;
                    std::vector<ReplicaHash> bad = hashes;
                    bad[0].hash = keyspace_hash(sched, corrupt, corrupt.last_seq());
                    const auto d = find_hash_divergence(bad);
                    check(d.has_value(),
                          "bit-rotted applied state DETECTED by keyspace hash (log still agrees)");
                }
            }
        }
    }
    check(seeds_with_prefix >= 3, "several fault-storm seeds produced a committed prefix (non-vacuous)");
    check(corruption_checks >= 1, "at least one corruption injection exercised the detector");

    if (g_fail) { std::printf("consensus_state_hash_test: FAILED\n"); return 1; }
    std::printf("consensus_state_hash_test: OK (apply-seam over real Raft + cross-replica hash agree + "
                "bit-rot detected; %d seeds, %d corruption checks)\n",
                seeds_with_prefix, corruption_checks);
    return 0;
}
