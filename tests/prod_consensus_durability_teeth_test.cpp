// prod_consensus_durability_teeth_test.cpp — Phase 9 S9.2 DURABILITY TEETH.
//
// THE BUG (surfaced + confirmed by S9.2 async io_uring fdatasync): a 1-node
// (peerless) Raft cluster's self-commit fast path used to advance commit_index at
// persist-ENQUEUE time — right after RaftNode::submit() handed the entry to the FIFO
// persist worker — BEFORE that entry's fdatasync completed. So a lone leader could
// mark an entry COMMITTED while it was still page-cache-only; an ABRUPT crash
// (SIGKILL / power loss) in the enqueue->fsync window would lose a COMMITTED entry,
// violating "commit implies durable". Synchronous fdatasync masked the sub-ms gap;
// the async io_uring fsync widened + exposed it. N>=2 is durable via QUORUM acks (a
// follower acks only after IT persists), so this is N=1-ONLY.
//
// THE FIX (both impls, N=1-specific): the lone-leader self-commit now fires at the
// persist worker's POST-SYNC point — once sync() COMPLETES for the just-persisted
// prefix — not at submit-enqueue. commit_index advances only after the entry is
// durable on the leader's own disk.
//
// THESE TEETH prove commit-FOLLOWS-fsync on the REAL prod IO stack. We build a REAL
// 1-node RaftNodeA (impl A, make_raft_a_factory) on a REAL ProdReactor epoll loop +
// real ProdClock/ProdRandom, over a StallSyncDisk that WRAPS a real ProdDisk: append/
// read pass straight through to the real fd, but sync() is HELD — its completion
// Future is parked until the test explicitly RELEASES it. That makes the enqueue->
// fsync window — the exact window the bug lives in, and which async io_uring opens for
// real — DETERMINISTIC and observable:
//
//   1. drive to Leader, submit("alpha"); pump the reactor so the persist worker
//      APPENDS the entry to the real WAL fd and PARKS on the (stalled) sync().
//   2. ASSERT commit_index() == 0 — the entry is appended but NOT yet fsync'd, so with
//      the fix it is NOT committed. (BEFORE the fix, commit_index would already be 1
//      here — a COMMITTED-but-un-fsynced entry. An abrupt crash in THIS window would
//      lose a committed entry. The assertion is the teeth: it FAILS pre-fix.)
//   3. RELEASE the stalled sync (the entry is now durable on the real platter), pump.
//   4. ASSERT commit_index() == 1 — commit FOLLOWS fsync. No committed entry can ever
//      be lost to an abrupt crash, because nothing is committed until it is durable.
//
// We run this BOTH with the real async io_uring fsync ENGAGED (the path that exposed
// the bug; --security-opt seccomp=unconfined) AND on the synchronous-fdatasync
// fallback (seccomp-blocked ring) — the stall wrapper sits ABOVE the disk so the teeth
// hold identically on either underlying barrier.
//
// LINUX-ONLY (ProdReactor/ProdDisk are __linux__): tests/CMakeLists.txt guards with
// if(UNIX AND NOT APPLE). The macOS host never builds it. NON-provider code (a test) →
// the forbidden-call lint scans it: it does NO raw syscall of its own (all real disk/
// epoll IO stays inside providers/prod/). ASAN-safe: every coroutine is a FREE FUNCTION
// over stable pointers, never an inline [&] lambda Task. BOUNDED by an absolute reactor
// deadline so a missing CQE / parked sync can NEVER hang the loop.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if defined(__linux__)

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>

#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdRandom.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdScratchDir.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace consensus = lockstep::consensus;

int g_failures = 0;
void check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

// Absolute wall guard (ns). The 1-node election window is tiny; this generous bound
// absorbs scheduler/sanitizer jitter while still killing any true hang (a never-
// resolved sync would otherwise park the persist worker forever — but we RELEASE it,
// and the deadline is the backstop).
constexpr core::Tick kWallNs = 2'000'000'000;  // 2 s

// ----------------------------------------------------------------------------
// StallSyncDisk — an IDisk that wraps a real ProdDisk. append()/read() pass straight
// through (the bytes really hit the WAL fd). sync() is INTERCEPTED: instead of issuing
// the barrier immediately, it mints a Promise from the reactor sink, parks it, and only
// COMPLETES it (by delegating to the real ProdDisk::sync()) when the test calls
// release_sync(). This makes the enqueue->fsync window deterministic so we can observe
// commit_index across it. One sync is in flight at a time (the single FIFO persist
// worker), so a single parked promise suffices; we still queue defensively.
// ----------------------------------------------------------------------------
class StallSyncDisk final : public core::IDisk {
public:
    StallSyncDisk(prod::ProdReactor& reactor, prod::ProdDisk& real) noexcept
        : reactor_(&reactor), real_(&real) {}

    [[nodiscard]] core::Future<core::Error>
    append(std::span<const std::byte> data, core::Offset& out_offset) override {
        return real_->append(data, out_offset);  // pass-through: real bytes to real fd
    }
    [[nodiscard]] core::Future<core::Error>
    read(core::Offset at, std::span<std::byte> into) override {
        return real_->read(at, into);  // pass-through
    }

    // INTERCEPTED durability barrier. Park a promise; do NOT issue the real fsync until
    // release_sync(). The awaiting persist worker stays suspended (so commit_index
    // cannot advance for the un-synced entry) until then.
    [[nodiscard]] core::Future<core::Error> sync() override {
        ++sync_requests_;
        core::Promise<core::Error> p = core::make_promise<core::Error>(reactor_);
        core::Future<core::Error> f = p.get_future();
        parked_.push_back(std::move(p));
        return f;
    }

    // How many sync barriers are currently parked (requested but not yet released).
    [[nodiscard]] std::size_t parked() const noexcept { return parked_.size(); }
    [[nodiscard]] std::uint64_t sync_requests() const noexcept { return sync_requests_; }

    // Release every parked sync: issue the REAL fdatasync now (the entries become
    // durable on the platter) and resolve each parked promise with its real result.
    // Resolving SCHEDULES the awaiting persist worker (L1) — it resumes on the next
    // reactor pump and the post-sync N=1 self-commit hook fires.
    void release_sync() {
        std::vector<core::Promise<core::Error>> to_fire;
        to_fire.swap(parked_);
        for (auto& p : to_fire) {
            // Drive the REAL barrier synchronously here (the real ProdDisk::sync may
            // itself be async via io_uring — we co_await-free it by pumping below).
            // Simplest + honest: issue the real sync and pump the reactor until it
            // resolves, then propagate its result. Since the real sync's own promise
            // is minted from the SAME reactor, a bounded pump resolves it.
            core::Error result = drive_real_sync();
            p.set_value(result);
        }
    }

private:
    // Issue the real ProdDisk::sync() and pump the reactor until that barrier resolves,
    // returning its Error. Bounded by the wall guard. This actually fdatasyncs the WAL.
    core::Error drive_real_sync() {
        auto out = std::make_shared<core::Error>();
        auto done = std::make_shared<bool>(false);
        reactor_->spawn(await_real_sync(real_, out, done));
        reactor_->run_until([done] { return *done; },
                            reactor_->clock().now() + kWallNs);
        return *out;
    }

    // Free-function coroutine (ASAN-safe: stable shared_ptr captures, no [&] Task).
    static core::Task await_real_sync(prod::ProdDisk* real,
                                      std::shared_ptr<core::Error> out,
                                      std::shared_ptr<bool> done) {
        *out = co_await real->sync();
        *done = true;
        co_return;
    }

    prod::ProdReactor* reactor_;
    prod::ProdDisk* real_;
    std::vector<core::Promise<core::Error>> parked_;
    std::uint64_t sync_requests_ = 0;
};

// ----------------------------------------------------------------------------
// One 1-node RaftNodeA over the real reactor + StallSyncDisk(real ProdDisk). No peers:
// quorum()==1, so the lone leader self-commits via the persist worker's post-sync hook.
// ----------------------------------------------------------------------------
struct Node1 {
    prod::ProdReactor reactor;
    prod::ProdNetworkBus bus{reactor};
    prod::ProdRandom rng;
    std::unique_ptr<prod::ProdDisk> real_disk;
    std::unique_ptr<StallSyncDisk> stall;
    std::unique_ptr<consensus::ConsensusNode> node;
    bool built = false;

    Node1(const std::string& wal_path, std::uint64_t seed) : rng(seed) {
        if (!reactor.valid()) {
            return;
        }
        if (!bus.add_node(1)) {  // the lone node's real INetwork endpoint
            return;
        }
        reactor.arm_uring();  // engage io_uring async fsync if available (else sync fallback)
        real_disk = std::make_unique<prod::ProdDisk>(reactor, wal_path);
        if (!real_disk->valid()) {
            return;
        }
        stall = std::make_unique<StallSyncDisk>(reactor, *real_disk);

        consensus::NodeDeps deps;
        deps.sched = &reactor;
        deps.clock = &reactor.clock();
        deps.rng = &rng;
        deps.net = bus.node(1);  // real endpoint (a 1-node cluster issues no peer RPC,
                                 // but recv_loop awaits net_->recv(), so it must be live)
        deps.disk = stall.get();

        consensus::NodeConfig nc;
        nc.self_id = 1;
        nc.cluster = std::vector<std::uint64_t>{1};  // ONE node: quorum()==1
        nc.election_timeout_min = 5'000'000;          // 5 ms (ns ticks)
        nc.election_timeout_max = 10'000'000;
        nc.heartbeat_interval = 2'000'000;

        node = consensus::raft_a::make_raft_a_factory()(deps, nc);
        built = node != nullptr;
    }
};

bool drive_until_leader(Node1& n) {
    return n.reactor.run_until(
        [&n] { return n.node->role() == consensus::Role::Leader; },
        n.reactor.clock().now() + kWallNs);
}

// Run ONE teeth scenario over a fresh WAL. Returns true on PASS.
bool run_teeth(const std::string& wal_path) {
    Node1 n(wal_path, 0xD00D9F2);
    if (!n.built) {
        std::fprintf(stderr, "FAIL: could not build 1-node RaftNodeA over prod stack\n");
        return false;
    }
    const bool ring = n.reactor.uring_available();
    std::printf("  [io_uring async fsync %s]\n",
                ring ? "ENGAGED (--seccomp=unconfined)" : "UNAVAILABLE → sync fdatasync fallback");

    n.node->start();
    const bool leader = drive_until_leader(n);
    check(leader, "1-node cluster self-elects Leader (quorum=1)");
    if (!leader) {
        return false;
    }
    check(n.node->commit_index() == 0, "pre-submit: commit_index == 0");

    // SUBMIT one value. submit() appends the entry to the in-memory log + ENQUEUES it
    // to the persist worker (which appends to the real WAL fd, then requests sync()).
    const consensus::SubmitResult r = n.node->submit("alpha");
    check(r.accepted && r.index == 1, "submit accepted at index 1 (lone leader)");

    // Pump until the persist worker has APPENDED the entry and PARKED on the stalled
    // sync (sync requested, not yet released). The entry's bytes are on the WAL fd but
    // NOT fdatasync'd. Stop as soon as a sync is parked (or the wall guard trips).
    n.reactor.run_until([&n] { return n.stall->parked() >= 1; },
                        n.reactor.clock().now() + kWallNs);
    check(n.stall->parked() >= 1, "persist worker enqueued + parked on the (stalled) fsync");

    // ===== THE TEETH ===== Pump a generous slice MORE: with the bug, the submit-time
    // self-commit would already have advanced commit_index to 1 here (entry committed
    // while only page-cache-resident — an abrupt crash in THIS window loses a committed
    // entry). With the fix, commit_index is STILL 0: nothing commits until the entry is
    // durable (the post-sync hook has not run because sync is parked).
    n.reactor.run_with_deadline(n.reactor.clock().now() + 50'000'000);  // 50 ms drain
    const consensus::Index ci_before_sync = n.node->commit_index();
    check(ci_before_sync == 0,
          "TEETH: entry APPENDED but NOT fsync'd ⇒ commit_index == 0 "
          "(un-durable entry is NOT committed; pre-fix this was 1 — a committed-yet-"
          "un-fsynced entry an abrupt crash would lose)");

    // The entry IS in the durable-intent log (appended), proving the gap is real: the
    // value exists on the fd, it just is not committed until fsync. (Sanity, not teeth.)
    check(n.node->log().size() == 1, "the entry IS in the log (appended), awaiting fsync");

    // ===== RELEASE the fsync ===== The real fdatasync now makes the entry durable on
    // the platter; the parked promise resolves; the persist worker resumes and the
    // post-sync N=1 self-commit hook fires.
    n.stall->release_sync();
    n.reactor.run_until([&n] { return n.node->commit_index() >= 1; },
                        n.reactor.clock().now() + kWallNs);
    const consensus::Index ci_after_sync = n.node->commit_index();
    check(ci_after_sync == 1,
          "TEETH: after fsync COMPLETES ⇒ commit_index == 1 "
          "(commit FOLLOWS fsync; no committed entry can be lost to an abrupt crash)");

    // The committed entry holds the submitted value (end-to-end).
    const std::span<const consensus::LogEntry> lg = n.node->log();
    const bool val_ok = lg.size() >= 1 && lg[0].value == "alpha";
    check(val_ok, "the committed entry holds the submitted value \"alpha\"");

    return g_failures == 0;
}

}  // namespace

int main() {
    std::printf("[prod_consensus_durability_teeth_test] Phase 9 S9.2 — N=1 "
                "commit-FOLLOWS-fsync durability teeth (real prod IO stack)\n\n");

    prod::ProdScratchDir scratch("lockstep_durability_teeth");
    if (!scratch.ok()) {
        std::fprintf(stderr, "FAILED to make scratch dir\n");
        return 1;
    }
    const std::string wal = scratch.path() + "/consensus.wal";

    const bool ok = run_teeth(wal);

    std::printf("\n[prod_consensus_durability_teeth_test] %s (%d failure%s)\n",
                ok ? "ALL TEETH PASSED" : "TEETH FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return ok ? 0 : 1;
}

#else  // !__linux__

int main() {
    std::printf("[prod_consensus_durability_teeth_test] skipped (Linux-only: "
                "ProdReactor/ProdDisk require __linux__)\n");
    return 0;
}

#endif
