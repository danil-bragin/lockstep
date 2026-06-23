#pragma once

// LocalCluster.hpp — Phase 6 Stage B. AN IN-PROCESS SIM SERVER + DRIVER HARNESS.
//
// Source of truth: briefs/phase6.md C6.5 (the CLI "drives the driver against an
// in-process sim server") + C6.6 (the worked example + conformance run the SAME
// way). This is the small, reusable wiring that stands up the FULL stack —
// SimNetwork bus -> wire::Server -> Database -> txn executor -> MVCC store, plus a
// wire::ClientStub the Driver (Driver.hpp) wraps — on ONE deterministic Scheduler,
// seeded. It hides the boilerplate (bus nodes, net-handle lifetime, recv/pump
// budgets, the scheduler spawn+run) the protocol test (tests/query_protocol_test)
// open-codes, so the CLI / example / conformance suite share ONE setup.
//
// DETERMINISM: a LocalCluster is a pure function of (seed, faults, the scripted
// command sequence). The only entropy is the seed, consumed by SeededRandom for
// the net-fault model; same inputs => byte-identical trace + outcomes. NO wall-
// clock, NO threads (query/ is NOT lint-exempt). The whole run is bounded (the
// recv/pump budgets are finite — never an unbounded loop).
//
// USAGE:
//   LocalCluster lc(seed, faults);
//   lc.run([](Connection& conn) -> core::Task {  // a "driver program"
//       WriteOutcome w; co_await conn.put("k", "v", w);
//       ReadOutcome r;  co_await conn.get("k", r);
//       co_return;
//   });
//   // lc.trace() is byte-identical for the same (seed, faults, program).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimNetwork.hpp>

#include <lockstep/query/Driver.hpp>
#include <lockstep/query/wire/ClientStub.hpp>
#include <lockstep/query/wire/Server.hpp>

namespace lockstep::query {

// The link-fault profile a LocalCluster runs under. A thin re-export of the sim's
// knobs so a caller (the CLI) names faults without reaching into providers/detail.
using LinkFaults = lockstep::sim::detail::LinkFaults;

// A "driver program": a coroutine over a bound Connection. The cluster spawns it
// alongside the server recv-loop + the client pump, all on one scheduler.
using DriverProgram = std::function<core::Task(Connection&)>;

// ----------------------------------------------------------------------------
// THE LOCAL CLUSTER. Owns the scheduler, the sim bus, the server, the client stub,
// and the per-run net-handle storage. `run(program)` drives the program to
// completion deterministically and returns. Budgets are sized generously so dups +
// retries drain; they are FINITE (V: no unbounded loop).
// ----------------------------------------------------------------------------
class LocalCluster {
public:
    explicit LocalCluster(std::uint64_t seed, LinkFaults faults = {})
        : seed_(seed), faults_(faults) {}

    // Drive `program` against the in-process sim server. The server + pump get a
    // generous-but-bounded recv budget so a fault-heavy run still drains. Returns
    // after the scheduler quiesces. A second run() on a fresh LocalCluster with the
    // same (seed, faults, program) is byte-identical.
    void run(const DriverProgram& program, int budget = 4096) {
        core::Scheduler sched;
        core::SimClock clock(sched);
        sim::SeededRandom rng(seed_);
        sim::SimNetworkBus bus(sched, rng);
        bus.add_nodes({kServerEp, kClientEp});
        bus.set_faults(faults_);

        // SimNetwork is a thin value handle; keep both in stable storage so the
        // Server/ClientStub INetwork& stays valid for the run's lifetime.
        auto srv_net = std::make_unique<sim::SimNetwork>(bus.node(kServerEp));
        auto cli_net = std::make_unique<sim::SimNetwork>(bus.node(kClientEp));
        wire::Server srv(*srv_net);
        wire::ClientStub stub(*cli_net, clock, core::Endpoint{kServerEp});
        Connection conn(stub);

        sched.spawn(srv.serve(budget));
        sched.spawn(stub.pump(budget));
        sched.spawn(program(conn));
        sched.run();

        trace_ = sched.trace_text();
        applied_ = srv.applied_submits();
        rejected_ = srv.rejected();
        tip_ = srv.tip();
    }

    // The byte-identical event trace of the last run (the determinism witness).
    [[nodiscard]] const std::string& trace() const noexcept { return trace_; }
    // How many DISTINCT submits the server actually applied (exactly-once witness).
    [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }
    // How many torn/corrupt frames the server rejected at decode.
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    // The live committed tip after the last run.
    [[nodiscard]] Seq tip() const noexcept { return tip_; }

    static constexpr std::uint64_t kServerEp = 1;
    static constexpr std::uint64_t kClientEp = 2;

private:
    std::uint64_t seed_;
    LinkFaults faults_;
    std::string trace_;
    std::uint64_t applied_ = 0;
    std::uint64_t rejected_ = 0;
    Seq tip_ = 0;
};

}  // namespace lockstep::query
