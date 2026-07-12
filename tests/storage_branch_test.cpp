// storage_branch_test.cpp — K7: branch fork. THE ORACLE (K7.6 shape): a fork equals
// a full snapshot of the parent at the fork moment; afterwards the two DIVERGE
// freely (each sees its own writes, never the other's); and the parent's
// compaction/flush churn never disturbs the branch's inherited reads (the
// shared-reader refcount rule).
#include <cstdio>
#include <map>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/WalEngine.hpp>

using namespace lockstep::core;
using namespace lockstep::sim;
using namespace lockstep::storage;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
struct Fac final : IDiskFactory {
    Scheduler* s; SimClock* c; SeededRandom* r; DiskFaultConfig d;
    std::map<std::uint64_t, std::unique_ptr<SimDisk>> disks;
    Fac(Scheduler& S, SimClock& C, SeededRandom& R, DiskFaultConfig D) : s(&S), c(&C), r(&R), d(D) {}
    IDisk& disk_for(std::uint64_t id) override {
        auto it = disks.find(id);
        if (it == disks.end()) it = disks.emplace(id, std::make_unique<SimDisk>(*s, *c, *r, d)).first;
        return *it->second;
    }
};
std::map<Key, Value> snap(Scheduler& sched, WalEngine& e) {
    std::map<Key, Value> out;
    (void)e.scan_visit(Range{"", "", true}, e.last_seq(),
                       [&](const Key& k, const Value& v) { out[k] = v; });
    (void)sched;
    return out;
}
}  // namespace

int main() {
    std::printf("=== storage_branch_test (K7 fork) ===\n");
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0x7B7ull);
    DiskFaultConfig dc;
    SimDisk wal(sched, clock, rng, dc), man(sched, clock, rng, dc);
    Fac fac(sched, clock, rng, dc);
    WalEngine parent(sched, wal, man, fac, /*flush_threshold=*/8);
    parent.set_compaction_trigger(2);

    sched.spawn([](WalEngine& e) -> Task {
        for (int i = 0; i < 40; ++i)
            (void)co_await e.put("k" + std::to_string(i), "v" + std::to_string(i));
        (void)co_await e.del("k3");
        (void)co_await e.sync();
        co_return;
    }(parent));
    sched.run();
    check(parent.sstable_count() > 0, "parent flushed (shared readers exist)");
    const std::map<Key, Value> at_fork = snap(sched, parent);

    // FORK — new disks for the branch; O(metadata + memtable).
    SimDisk bwal(sched, clock, rng, dc), bman(sched, clock, rng, dc);
    Fac bfac(sched, clock, rng, dc);
    auto branch = parent.fork_branch(sched, bwal, bman, bfac, /*flush_threshold=*/8);
    check(branch != nullptr, "fork succeeds");

    // (1) ORACLE: the branch == the parent's full state at the fork moment.
    check(snap(sched, *branch) == at_fork, "fork == parent snapshot at the fork point");

    // (2) DIVERGENCE: each side sees its own writes, never the other's.
    sched.spawn([](WalEngine& e) -> Task {
        (void)co_await e.put("branch-only", "B");
        (void)co_await e.put("k5", "branch-v5");
        (void)co_await e.sync();
        co_return;
    }(*branch));
    sched.spawn([](WalEngine& e) -> Task {
        (void)co_await e.put("parent-only", "P");
        (void)co_await e.del("k7");
        (void)co_await e.sync();
        co_return;
    }(parent));
    sched.run();
    const auto bs = snap(sched, *branch);
    const auto ps = snap(sched, parent);
    check(bs.count("branch-only") == 1 && bs.count("parent-only") == 0 &&
              bs.at("k5") == "branch-v5" && bs.count("k7") == 1,
          "branch sees its writes, not the parent's");
    check(ps.count("parent-only") == 1 && ps.count("branch-only") == 0 &&
              ps.at("k5") == "v5" && ps.count("k7") == 0,
          "parent sees its writes, not the branch's");

    // (3) REFCOUNT RULE: parent flush + compaction churn AFTER the fork must not
    // disturb the branch's inherited reads (its shared readers stay alive even as
    // the parent obsoletes its own references).
    sched.spawn([](WalEngine& e) -> Task {
        for (int i = 100; i < 140; ++i)
            (void)co_await e.put("p" + std::to_string(i), "x");
        (void)co_await e.sync();
        co_return;
    }(parent));
    sched.run();
    auto bs2 = snap(sched, *branch);
    bs2.erase("branch-only");
    std::map<Key, Value> want = at_fork;
    want["k5"] = "branch-v5";
    check(bs2 == want, "parent churn (flush+compaction) leaves the branch undisturbed");

    // (4) The branch flushes and compacts INDEPENDENTLY on its own disks.
    sched.spawn([](WalEngine& e) -> Task {
        for (int i = 200; i < 240; ++i)
            (void)co_await e.put("b" + std::to_string(i), "y");
        (void)co_await e.sync();
        co_return;
    }(*branch));
    sched.run();
    check(snap(sched, *branch).count("b210") == 1, "branch flush/compaction on its own disks");
    check(snap(sched, parent).count("b210") == 0, "...invisible to the parent");

    if (g_fail != 0) { std::printf("storage_branch_test: FAILED\n"); return 1; }
    std::printf("storage_branch_test: ALL PASS\n");
    return 0;
}
