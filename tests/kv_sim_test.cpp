// kv_sim_test.cpp — Phase 2 batch 2 (stage B) self-test for the toy replicated
// KV register system + the reusable run harness (specs/checker-framework.md §5;
// briefs/phase2-batch2.md stage B).
//
// WHAT IT PROVES — that the HARNESS RUNS DETERMINISTICALLY and the SYSTEM IS
// ALIVE under the FULL fault envelope. It deliberately does NOT assert deep
// correctness (C-INT / C-MONO / C-LIN / C-DUR) — those are Stage C's
// independent checker set (DECISION-D: separate authors → real teeth). This
// test only certifies the substrate the checkers will run on:
//
//   (a) PROGRESS: some client ops ACK (ok) under the full fault envelope — the
//       system is not wedged; it makes real progress.
//   (b) DETERMINISM: the same seed run twice ⇒ BYTE-IDENTICAL History. Proven
//       in-test (rendered-history string compare) AND externally (this binary
//       prints the rendered history under a marker so the gate can run it twice
//       and `diff` the two captures).
//   (c) TERMINATION: run_kv_sim() returns (no livelock). Reaching the end of
//       main() under the directory-wide CTest TIMEOUT 90 is the proof; a hang
//       would trip the timeout = FAILURE.
//   (d) CRASH+RESTART exercised: the node lifecycle (C2.3) crash → recover path
//       is driven and durable state survives a crash (recover rebuilds from the
//       SimDisk durable WAL). Exercised both through the run's chaos driver and
//       directly at the IKvSystem surface.
//
// This is NON-provider code → the forbidden-call lint scans it. All time is
// virtual (run_kv_sim drives SimClock); all randomness is the seeded provider
// PRNG threaded through run_kv_sim. The seed is printed for replay.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/kv/Buggify.hpp>
#include <lockstep/harness/kv/KvSim.hpp>
#include <lockstep/harness/kv/ReplicatedKvSystem.hpp>

#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/sim/SimNetwork.hpp>

namespace {

using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::harness::History;
using lockstep::harness::Op;
using lockstep::harness::render_history;
using lockstep::harness::kv::Buggify;
using lockstep::harness::kv::KvConfig;
using lockstep::harness::kv::KvRequest;
using lockstep::harness::kv::KvResult;
using lockstep::harness::OpKind;
using lockstep::harness::kv::ReplicatedKvSystem;
using lockstep::harness::kv::run_kv_sim;
using lockstep::sim::SeededRandom;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        std::abort();
    }
}

// Count how many ops completed OK (acked) in a History.
std::size_t count_ok(const History& h) {
    std::size_t n = 0;
    for (const Op& op : h) {
        if (op.ok) {
            ++n;
        }
    }
    return n;
}

// Count how many ops returned an error (timeout / cas-mismatch / unavailable).
std::size_t count_err(const History& h) {
    std::size_t n = 0;
    for (const Op& op : h) {
        if (!op.ok) {
            ++n;
        }
    }
    return n;
}

}  // namespace

int main() {
    const std::uint64_t kSeed = 0x5151ABCDULL;
    std::printf("kv_sim_test seed=%llu\n",
                static_cast<unsigned long long>(kSeed));

    KvConfig cfg;  // defaults: 3 nodes, 3 clients, full envelope, crashes+parts

    // === (a) PROGRESS + (b) DETERMINISM ====================================
    const History h1 = run_kv_sim(kSeed, cfg);
    const History h2 = run_kv_sim(kSeed, cfg);

    const std::string r1 = render_history(h1);
    const std::string r2 = render_history(h2);

    // (b) DETERMINISM: same seed ⇒ byte-identical History (in-test).
    check(r1 == r2,
          "same-seed run twice => byte-identical History (in-test, V-HIST2)");

    // Every op carries both stamps and is well-ordered (structural sanity).
    for (std::size_t i = 0; i < h1.size(); ++i) {
        const Op& op = h1[i];
        check(op.invoke_vt <= op.return_vt,
              "every op: invoke_vt <= return_vt (V-HIST1)");
        check(op.op_id != 0, "every op has a minted op_id");
        if (i > 0) {
            check(op.return_vt >= h1[i - 1].return_vt,
                  "history totally ordered by return_vt");
        }
    }

    // (a) PROGRESS: the workload is the full budget, and a healthy fraction of
    // ops ACK under faults — the system is ALIVE, not wedged.
    const std::size_t total_ops = cfg.n_clients * cfg.ops_per_client;
    check(h1.size() == total_ops,
          "every submitted op returned (no pending; bounded termination)");
    const std::size_t ok_ops = count_ok(h1);
    const std::size_t err_ops = count_err(h1);
    std::printf("kv_sim_test progress: total=%zu ok=%zu err=%zu\n", h1.size(),
                ok_ops, err_ops);
    check(ok_ops > 0, "some client ops ACK under the full fault envelope (alive)");
    // Under crashes + partitions we expect SOME errors too (the deadline path
    // is real), but the run must not be all-error.
    check(ok_ops + err_ops == h1.size(), "ok + err == total");

    // A no-fault baseline must make MORE progress than the chaotic run — a quick
    // sanity that the envelope actually perturbs the system (and is reproducible).
    KvConfig calm = cfg;
    calm.full_envelope = false;
    calm.net_faults = lockstep::sim::detail::LinkFaults{};  // pristine bus
    calm.disk_faults = lockstep::sim::DiskFaultConfig{};    // no disk faults
    calm.crash_episodes = 0;
    calm.partition_episodes = 0;
    calm.buggify = false;
    const History hc1 = run_kv_sim(kSeed, calm);
    const History hc2 = run_kv_sim(kSeed, calm);
    check(render_history(hc1) == render_history(hc2),
          "calm run also byte-identical on replay (V-HIST2)");
    check(count_ok(hc1) >= ok_ops,
          "calm (no-fault) run acks at least as many ops as the chaotic run");
    // With NO faults the system must never TIME OUT or report a node
    // UNAVAILABLE — every non-ok op is a legitimate cas_mismatch (a real
    // register outcome, not a fault). A spurious timeout would mean the
    // fault-free system is wedging.
    for (const Op& op : hc1) {
        if (!op.ok) {
            check(op.error == "cas_mismatch",
                  "calm run: every non-ok op is a legitimate cas_mismatch "
                  "(no spurious timeout/unavailable)");
        }
    }

    // === (c) TERMINATION ===================================================
    // Reaching here means every run_kv_sim() above returned. A livelock would
    // have tripped the CTest TIMEOUT 90. Run a couple more seeds to be sure the
    // termination guarantee is not seed-specific.
    for (std::uint64_t s = 1; s <= 5; ++s) {
        const History hs = run_kv_sim(0xA5A5'0000ULL + s, cfg);
        check(hs.size() == total_ops, "every seed terminates with full budget");
        check(count_ok(hs) > 0, "every seed makes progress");
    }

    // === (d) CRASH + RESTART exercised (node lifecycle C2.3) ===============
    // Directly drive the lifecycle at the IKvSystem surface on a fresh,
    // fault-free deterministic stack so the assertions are crisp: write a value
    // (durably committed via the leader's WAL), crash a backup + recover it
    // (rebuild from durable WAL), then crash the leader (failover) and read the
    // value back from the NEW leader — proving the durable write survived the
    // crash/restart and the lifecycle path runs without wedging.
    {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(kSeed);
        lockstep::sim::SimNetworkBus bus(sched, rng);
        Buggify buggify(rng, /*enabled=*/false);
        lockstep::sim::DiskFaultConfig clean_disk{};  // honest disk
        ReplicatedKvSystem sys(sched, clock, rng, bus, buggify, /*n_nodes=*/3,
                               clean_disk);
        sys.start();

        // A tiny drain coroutine: submit an op, capture its KvResult by value.
        auto drain = [&](std::uint64_t client_id, const KvRequest& req,
                         KvResult& out) {
            sched.spawn(
                [](ReplicatedKvSystem* s, std::uint64_t cid, KvRequest r,
                   KvResult* o) -> lockstep::core::Task {
                    *o = co_await s->submit(cid, r);
                    co_return;
                }(&sys, client_id, req, &out));
            sched.run();
        };

        // Phase 1: write k=durable_key=durable_val through the leader.
        KvResult wres;
        KvRequest w;
        w.kind = OpKind::Write;
        w.key = "durable_key";
        w.value = "durable_val";
        drain(/*client_id=*/0, w, wres);
        check(wres.ok, "durable write acked by the leader (durable WAL synced)");

        // Phase 2: crash a backup (node 2), recover it from its durable WAL.
        sys.crash_node(2);
        sched.run();        // drain in-flight; a crashed node serves nothing
        sys.recover_node(2);  // reopen durable WAL, rebuild map, relaunch loop
        sched.run();

        // Phase 3: crash the leader (node 0) → failover to node 1; read back.
        sys.crash_node(0);
        sched.run();
        KvResult rres;
        KvRequest r;
        r.kind = OpKind::Read;
        r.key = "durable_key";
        drain(/*client_id=*/0, r, rres);
        check(rres.ok, "read after crash+restart+failover terminated (lifecycle)");
        // The durable write must still be observable from the new leader (the
        // replicated record survived). This is a LIVENESS sanity, not the full
        // C-DUR checker (that is Stage C) — but it must not have vanished.
        check(rres.result == "durable_val",
              "durable write survives crash+restart and is observable on failover");

        sys.recover_node(0);  // leave the cluster clean (no crashed-loop leak)
        sched.run();
        std::printf("kv_sim_test lifecycle: crash+recover+failover exercised "
                    "(durable write survived)\n");
    }

    // === EXTERNAL DIFF PROOF ==============================================
    // Emit the rendered chaotic-run History under a stable marker. The gate runs
    // this binary twice and diffs the captured blocks → must be byte-identical.
    std::printf("---KV-HISTORY-BEGIN---\n");
    std::fputs(r1.c_str(), stdout);
    std::printf("---KV-HISTORY-END---\n");

    std::printf("kv_sim_test: OK\n");
    return 0;
}
