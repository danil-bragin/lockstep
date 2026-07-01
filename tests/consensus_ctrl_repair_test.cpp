// consensus_ctrl_repair_test.cpp — CTRL protocol-aware recovery of a corrupt COMMITTED
// entry (plan P4, part 2). RaftNodeA recovery, on a mid-log record whose FRAMING is
// intact but whose CRC fails (a bit-flip, not a torn tail), scans forward to learn the
// log EXTENDED past the clean prefix (valid ENTRY records after the hole) and arms a
// REPAIR WATERMARK. While unrepaired the node neither runs for election nor grants a
// vote to a shorter log — so a corrupt node + lagging peers cannot elect a leader that
// DROPPED a committed entry (CTRL's silent-loss disaster). The leader refills the hole.
//
// Proves: a clean image self-elects (watermark 0, no CTRL behavior); a mid-log
// body-corrupt image recovers ONLY the clean prefix, arms the watermark to the highest
// index it HAD, and — even in a 1-node config that would otherwise self-elect — does NOT
// become leader (it waits to be repaired). Directly built durable images; every host.
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
using lockstep::consensus::Role;
using lockstep::consensus::raft_a::rec_entry;
using lockstep::consensus::raft_a::rec_meta;
using RaftNodeA = lockstep::consensus::raft_a::RaftNodeA;

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
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
void app(std::vector<std::byte>& s, const std::vector<std::byte>& r) {
    s.insert(s.end(), r.begin(), r.end());
}
// Free coroutine (never an inline lambda coroutine — that would dangle; ASan lesson).
lockstep::core::Task seed_disk(IDisk& d, std::vector<std::byte> bytes, Error& res) {
    Offset off = 0;
    res = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (res.ok()) res = co_await d.sync();
    co_return;
}

// Recover a node over `image` in a single-node config {0}, run briefly, and report its
// role, retained-log length, and repair watermark.
struct Recovered {
    Role role = Role::Follower;
    std::size_t log_len = 0;
    std::uint64_t watermark = 0;
};
Recovered recover(const std::vector<std::byte>& image) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0x7Au);
    SimNetworkBus bus(sched, rng);
    bus.add_node(0);
    auto net = bus.node(0);
    SimDisk disk(sched, clock, rng, nofault());
    Error se{lockstep::core::ErrorCode::Unknown, "norun"};
    sched.spawn(seed_disk(disk, image, se));
    sched.run_until(clock.now());

    NodeDeps deps;
    deps.sched = &sched;
    deps.clock = &clock;
    deps.rng = &rng;
    deps.net = &net;
    deps.disk = &disk;
    NodeConfig nc;
    nc.self_id = 0;
    nc.cluster = {0};  // a 1-node config: WOULD self-elect immediately if healthy
    nc.election_timeout_min = 5;
    nc.election_timeout_max = 10;
    RaftNodeA node(deps, nc);
    node.start();
    sched.run_until(clock.now() + 200);  // give it ample time to (try to) self-elect

    Recovered r;
    r.role = node.role();
    r.log_len = node.log().size();
    r.watermark = node.repair_watermark();
    return r;
}
}  // namespace

int main() {
    // Five committed entries at term 1 after an initial meta.
    auto build = [](bool corrupt) {
        std::vector<std::byte> s;
        app(s, rec_meta(1, kNilVote));
        app(s, rec_entry(1, LogEntry{1, "a"}));
        app(s, rec_entry(2, LogEntry{1, "b"}));
        const std::size_t e3 = s.size();
        app(s, rec_entry(3, LogEntry{1, "c"}));
        app(s, rec_entry(4, LogEntry{1, "d"}));
        app(s, rec_entry(5, LogEntry{1, "e"}));
        if (corrupt) {
            // Flip a byte INSIDE entry 3's body (past its 8-byte [crc][len] header): the
            // framing (len) stays intact, the CRC now mismatches — a bit-flip, not a torn
            // tail, with valid entries 4,5 still framed after it.
            s[e3 + 8 + 5] ^= std::byte{0x40};
        }
        return s;
    };

    // (1) CLEAN image: the 1-node node self-elects; no CTRL behavior.
    {
        const Recovered r = recover(build(/*corrupt=*/false));
        check(r.watermark == 0, "clean recovery: repair watermark is 0");
        check(r.log_len == 5, "clean recovery: all five entries retained");
        check(r.role == Role::Leader, "clean recovery: the 1-node cluster self-elects");
    }

    // (2) MID-LOG BODY-CORRUPT image: recover only the clean prefix, ARM the watermark to
    //     the highest index we HAD (5), and REFUSE to self-elect until repaired.
    {
        const Recovered r = recover(build(/*corrupt=*/true));
        check(r.log_len == 2, "corrupt recovery: only the clean prefix [1,2] is recovered");
        check(r.watermark == 5, "corrupt recovery: watermark armed to the highest index HAD (5)");
        check(r.role != Role::Leader,
              "corrupt recovery: node does NOT self-elect with an unrepaired hole (CTRL gate)");
    }

    if (g_fail) { std::printf("consensus_ctrl_repair_test: FAILED\n"); return 1; }
    std::printf("consensus_ctrl_repair_test: OK (corrupt committed entry detected; recovery keeps the "
                "clean prefix + arms a repair watermark + refuses to lead until repaired)\n");
    return 0;
}
