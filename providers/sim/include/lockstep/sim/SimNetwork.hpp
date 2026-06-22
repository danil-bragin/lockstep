#pragma once

// SimNetwork.hpp — C2.1. The simulation INetwork provider: an in-memory message
// bus with per-link latency, reordering, duplication, drop, and PARTITION of
// arbitrary server subsets (with heal/unheal). Every fault decision derives from
// core::IRandom; every delivery is scheduled on the Scheduler's virtual-time
// timers. The whole bus is a pure function of (seed, inputs) ⇒ byte-identical
// event trace on replay.
//
// This file lives under providers/ (the lint-exempt boundary zone), but it is
// written determinism-clean: NO wall-clock, NO real sockets, NO threads/atomics/
// mutex, NO std::*_distribution / std::shuffle / std::random_device / std::rand.
//
// USAGE
//   Scheduler sched;
//   SimClock  clock(sched);
//   SeededRandom rng(seed);
//   SimNetworkBus bus(sched, rng);           // shared bus
//   bus.set_faults({.drop_prob = 0.1, ...}); // seeded fault knobs
//   bus.add_nodes({0,1,2});
//   SimNetwork n0 = bus.node(0);             // per-node INetwork handle
//   // n0.send(...), n0.recv(...) inside Tasks; bus.partition({0}) / bus.heal()
//
//   SimNetwork implements core::INetwork; SimNetworkBus owns shared state and the
//   partition/heal controls (those are bus-wide, not part of the INetwork surface).

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/detail/SimBus.hpp>

namespace lockstep::sim {

using core::Endpoint;
using core::Error;
using core::Future;
using core::INetwork;
using core::IRandom;
using core::Message;
using core::Scheduler;

// A per-node handle implementing core::INetwork. Thin: it carries its own
// Endpoint and a non-owning pointer to the shared bus. Copyable value type — many
// nodes share one bus. local()/send()/recv() forward to the bus.
class SimNetwork final : public INetwork {
public:
    SimNetwork(detail::SimBus& bus, Endpoint self) noexcept : bus_(&bus), self_(self) {}

    [[nodiscard]] Endpoint local() const noexcept override { return self_; }

    [[nodiscard]] Future<Error> send(Endpoint to,
                                     std::span<const std::byte> payload) override {
        return bus_->send(self_, to, payload);
    }

    [[nodiscard]] Future<Message> recv() override { return bus_->recv(self_); }

private:
    detail::SimBus* bus_;
    Endpoint self_;
};

// Owns the shared bus and exposes the cluster-wide controls (partition/heal,
// fault config, node registration) plus a factory for per-node SimNetwork
// handles. One SimNetworkBus per simulated cluster.
class SimNetworkBus {
public:
    SimNetworkBus(Scheduler& sched, IRandom& rng) : bus_(sched, rng) {}

    SimNetworkBus(const SimNetworkBus&) = delete;
    SimNetworkBus& operator=(const SimNetworkBus&) = delete;
    SimNetworkBus(SimNetworkBus&&) = delete;
    SimNetworkBus& operator=(SimNetworkBus&&) = delete;

    // Register nodes by id. Idempotent.
    void add_node(std::uint64_t id) { bus_.add_node(Endpoint{id}); }
    void add_nodes(std::initializer_list<std::uint64_t> ids) {
        for (std::uint64_t id : ids) {
            bus_.add_node(Endpoint{id});
        }
    }

    // Per-node INetwork handle. The node is auto-registered if unseen.
    [[nodiscard]] SimNetwork node(std::uint64_t id) {
        bus_.add_node(Endpoint{id});
        return SimNetwork{bus_, Endpoint{id}};
    }

    // Seeded fault knobs (drop/dup/reorder/latency). Applied to every send.
    void set_faults(const detail::LinkFaults& f) { bus_.set_faults(f); }
    [[nodiscard]] const detail::LinkFaults& faults() const noexcept {
        return bus_.faults();
    }

    // Partition the cluster into `side_a` vs the rest; no message crosses the cut
    // while live. heal() restores delivery deterministically. Both scheduled in
    // virtual time at the call site (recorded into the trace).
    void partition(std::vector<std::uint64_t> side_a) {
        bus_.partition(std::move(side_a));
    }
    void heal() { bus_.heal(); }
    [[nodiscard]] bool is_blocked(std::uint64_t from, std::uint64_t to) const noexcept {
        return bus_.is_blocked(Endpoint{from}, Endpoint{to});
    }

    [[nodiscard]] detail::SimBus& raw() noexcept { return bus_; }

private:
    detail::SimBus bus_;
};

} // namespace lockstep::sim
