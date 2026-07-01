// consensus_meta_dualcopy_test.cpp — CTRL dual-copy metadata durability (plan P4).
//
// (currentTerm, votedFor) cannot be reconstructed from peers, so a single torn/un-synced
// metadata write lost on recovery would silently REVERT the term/vote — risking a double
// vote in a term (two leaders → split-brain). RaftNodeA::persist_meta now writes TWO
// consecutive CRC'd copies; if the trailing copy is torn on a crash, recovery still finds
// the leading copy with the SAME latest values.
//
// This drives recovery DIRECTLY over a hand-built durable image (using the node's own
// record encoders), so it can chop the trailing copy at a byte boundary the way a torn
// write would. It proves: dual-copy → the metadata update SURVIVES a torn trailing copy;
// a single copy (the old behavior, modelled by a one-copy image) → the update is LOST.
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

using lockstep::consensus::LogEntry;
using lockstep::consensus::NodeConfig;
using lockstep::consensus::NodeDeps;
using lockstep::consensus::raft_a::make_raft_a_factory;
using lockstep::consensus::raft_a::rec_entry;
using lockstep::consensus::raft_a::rec_meta;

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::sim::SimNetworkBus;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
constexpr std::uint64_t kNilVote = UINT64_MAX;

DiskFaultConfig nofault() {
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 0;
    return dc;
}
void append_rec(std::vector<std::byte>& s, const std::vector<std::byte>& r) {
    s.insert(s.end(), r.begin(), r.end());
}
Task seed(IDisk& d, std::vector<std::byte> bytes, Error& res) {
    Offset off = 0;
    res = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (res.ok()) res = co_await d.sync();
    co_return;
}
}  // namespace

int main() {
    // Build a durable stream: initial meta(term 1), one entry, then a metadata UPDATE to
    // term 7 (a vote) — persisted as TWO consecutive copies (what persist_meta now writes).
    std::vector<std::byte> dual;
    append_rec(dual, rec_meta(1, kNilVote));
    append_rec(dual, rec_entry(1, LogEntry{1, "x"}));
    append_rec(dual, rec_meta(7, 2));  // copy 1
    const std::size_t after_copy1 = dual.size();
    append_rec(dual, rec_meta(7, 2));  // copy 2

    // A one-copy stream (the OLD single-write behavior) for contrast.
    std::vector<std::byte> single;
    append_rec(single, rec_meta(1, kNilVote));
    append_rec(single, rec_entry(1, LogEntry{1, "x"}));
    append_rec(single, rec_meta(7, 2));  // the only copy
    const std::size_t single_before_meta = single.size() - (dual.size() - after_copy1);

    // Helper: recover a node over `image`, read currentTerm at the current instant.
    auto term_of = [](const std::vector<std::byte>& image) -> std::uint64_t {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(0x99u);
        SimNetworkBus bus(sched, rng);
        bus.add_node(0);
        auto net = bus.node(0);
        SimDisk disk(sched, clock, rng, nofault());
        Error se{lockstep::core::ErrorCode::Unknown, "norun"};
        sched.spawn(seed(disk, image, se));
        sched.run_until(clock.now());
        NodeDeps deps;
        deps.sched = &sched;
        deps.clock = &clock;
        deps.rng = &rng;
        deps.net = &net;
        deps.disk = &disk;
        NodeConfig nc;
        nc.self_id = 0;
        nc.cluster = {0, 1, 2};
        auto node = make_raft_a_factory()(deps, nc);
        node->start();
        sched.run_until(clock.now());  // recovery runs now; the election deadline is in the future
        return node->current_term();
    };

    // (1) Intact dual copy: term 7 recovered.
    check(term_of(dual) == 7, "intact dual-copy metadata recovers term 7");

    // (2) DUAL COPY, trailing copy TORN: chop the last copy (and part of it). Recovery
    //     still recovers term 7 from the leading copy — the vote/term SURVIVE.
    {
        std::vector<std::byte> torn(dual.begin(), dual.begin() + static_cast<std::ptrdiff_t>(after_copy1 + 6));
        check(term_of(torn) == 7, "dual-copy: a torn trailing copy still recovers term 7 (SURVIVES)");
    }

    // (3) SINGLE copy, torn: the same torn boundary loses the update — recovery falls back
    //     to term 1. This is exactly the silent revert dual-copy prevents.
    {
        std::vector<std::byte> torn(single.begin(),
                                    single.begin() + static_cast<std::ptrdiff_t>(single_before_meta + 6));
        check(term_of(torn) == 1, "single-copy: a torn metadata write LOSES the update (falls back to term 1)");
    }

    if (g_fail) { std::printf("consensus_meta_dualcopy_test: FAILED\n"); return 1; }
    std::printf("consensus_meta_dualcopy_test: OK (dual-copy metadata survives a torn trailing write)\n");
    return 0;
}
