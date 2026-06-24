#pragma once

// SimBus.hpp — the shared, single-threaded in-memory message bus that backs the
// sim INetwork provider (spec C2.1). This is the lint-exempt providers/ zone, but
// it is written determinism-clean regardless: NO wall-clock, NO real sockets, NO
// threads/atomics/mutex, NO std::*_distribution / std::shuffle / std::rand. ALL
// fault decisions derive from core::IRandom; ALL delivery is scheduled on the
// Scheduler's virtual-time timers (core::Scheduler::delay) — never wall-clock.
//
// One SimBus is shared by every node. Each node gets a SimNetwork handle (see
// SimNetwork.hpp) bound to its Endpoint; the handle forwards send()/recv() to the
// bus. The bus owns: the per-node mailboxes, the fault-link config, the live
// partition state, and the message-event recorder (folded into the Scheduler's
// Trace so the whole run is byte-comparable on replay).
//
// DETERMINISM CONTRACT (mirrors docs/runtime-determinism.md):
//   - Every fault coin-flip / latency / jitter draw comes from IRandom in a
//     FIXED, send-ordered sequence. Same seed + same send order ⇒ identical draws.
//   - Delivery is a delay()-armed timer; same (due, arm_seq) ordering as the
//     scheduler. Same-tick deliveries are tie-broken by a monotonic send seq.
//   - No unordered_map/set anywhere on the scheduling path. Endpoint→mailbox is a
//     sorted std::vector keyed by endpoint id; partition membership is a sorted
//     vector of ids. Iteration order is therefore deterministic.
//   - Network events are recorded into the Scheduler's Trace via the public
//     trace() sink, so two same-seed runs render byte-identical traces.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/core/Trace.hpp>

namespace lockstep::sim::detail {

using core::Endpoint;
using core::Error;
using core::ErrorCode;
using core::Future;
using core::IRandom;
using core::Message;
using core::Promise;
using core::Scheduler;
using core::Task;
using core::Tick;
using core::TraceAction;

// Per-link fault knobs. Probabilities are deterministic coin-flips via
// IRandom::chance; latencies are virtual-tick ranges drawn via uniform_range.
// A "link" here is directionless bus-wide policy (applied to every send); the
// model is faithful enough for the Phase 2 envelope and stays fully seeded.
struct LinkFaults {
    double drop_prob = 0.0;       // P(message silently dropped)
    double dup_prob = 0.0;        // P(message duplicated — a second copy delivered)
    double reorder_prob = 0.0;    // P(extra reorder jitter added, perturbing order)
    Tick latency_min = 1;         // min base delivery latency (ticks), >= 1
    Tick latency_max = 1;         // max base delivery latency (ticks)
    Tick reorder_jitter_max = 0;  // extra jitter ticks added when reorder fires
};

// A node's mailbox: messages already delivered but not yet recv()'d, plus recv
// waiters parked on a promise. Deterministic FIFO on both queues; the backing
// byte buffers are owned here (the INetwork Message view is non-owning).
struct Mailbox {
    Endpoint owner{};
    // Delivered-but-unconsumed payloads, in delivery (timer-fire) order.
    std::vector<std::vector<std::byte>> ready{};
    std::vector<Endpoint> ready_from{};
    // Parked recv() promises, in call order (FIFO). Front is served first.
    std::vector<Promise<Message>> waiters{};
    // Stable backing store for a payload currently handed to a waiter via a
    // non-owning span. Kept alive until the next recv on this mailbox.
    std::vector<std::vector<std::byte>> retained{};
};

// The shared bus. Single owner; nodes hold a non-owning pointer (via SimNetwork).
class SimBus {
public:
    SimBus(Scheduler& sched, IRandom& rng) noexcept : sched_(&sched), rng_(&rng) {}

    SimBus(const SimBus&) = delete;
    SimBus& operator=(const SimBus&) = delete;
    SimBus(SimBus&&) = delete;
    SimBus& operator=(SimBus&&) = delete;

    // Register a node; idempotent on id. Mailboxes are kept sorted by id so all
    // iteration is deterministic (no unordered container on the schedule path).
    void add_node(Endpoint e) {
        if (mailbox_index(e) != kNpos) {
            return;
        }
        Mailbox mb;
        mb.owner = e;
        // Insert keeping the vector sorted by endpoint id.
        std::size_t pos = 0;
        while (pos < mailboxes_.size() && mailboxes_[pos].owner.id < e.id) {
            ++pos;
        }
        mailboxes_.insert(mailboxes_.begin() + static_cast<std::ptrdiff_t>(pos),
                          std::move(mb));
    }

    void set_faults(const LinkFaults& f) { faults_ = f; }
    [[nodiscard]] const LinkFaults& faults() const noexcept { return faults_; }

    // ---- partition control (deterministic, virtual-time) -----------------

    // Install a live partition splitting the cluster into exactly two sides:
    // `side_a` (sorted ids) vs everyone else. No message crosses the cut while it
    // is live. Replaces any prior partition. Recorded into the trace.
    void partition(std::vector<std::uint64_t> side_a) {
        // Sort + de-dup for deterministic membership tests.
        insertion_sort(side_a);
        side_a.erase(unique_adjacent(side_a), side_a.end());
        partition_side_a_ = std::move(side_a);
        partitioned_ = true;
        record("net_partition", render_ids(partition_side_a_));
    }

    // Heal: remove the partition; delivery is restored deterministically. Already
    // in-flight messages that were armed BEFORE the cut still respect the cut at
    // delivery time (checked at fire) — so a message armed during a partition is
    // dropped at delivery unless healed by then. Recorded into the trace.
    void heal() {
        partitioned_ = false;
        partition_side_a_.clear();
        record("net_heal", {});
    }

    [[nodiscard]] bool is_blocked(Endpoint from, Endpoint to) const noexcept {
        if (!partitioned_) {
            return false;
        }
        bool a_from = in_side_a(from.id);
        bool a_to = in_side_a(to.id);
        return a_from != a_to; // opposite sides ⇒ cut
    }

    // ---- the INetwork surface, per node ----------------------------------

    // Accept `payload` from `from` to `to`. Returns immediately-ready Future<Error>
    // (Ok = accepted for delivery; Unavailable = link partitioned at send time).
    // All fault decisions are drawn HERE, in send order, from IRandom — so the
    // fault schedule is a pure function of (seed, send sequence). Delivery itself
    // is a delay()-armed timer spawned as a Task (virtual time only).
    Future<Error> send(Endpoint from, Endpoint to, std::span<const std::byte> payload) {
        std::uint64_t sseq = send_seq_++;
        // Copy out the bytes now (caller need not keep payload alive — INetwork).
        std::vector<std::byte> bytes(payload.begin(), payload.end());

        // If the link is cut at SEND time, fail fast with Unavailable. This is the
        // "no message crosses a live partition" invariant at the send boundary.
        if (is_blocked(from, to)) {
            record("net_send_blocked",
                   std::string("from=") + std::to_string(from.id) + " to=" +
                       std::to_string(to.id) + " s=" + std::to_string(sseq));
            return ready_error(Error{ErrorCode::Unavailable, "partitioned link"});
        }

        // Fault draws — FIXED order: drop, dup, latency, reorder. Never reordered.
        bool drop = rng_->chance(faults_.drop_prob);
        bool dup = rng_->chance(faults_.dup_prob);
        Tick base_lat = draw_latency();
        bool reorder = rng_->chance(faults_.reorder_prob);
        Tick jitter = reorder ? draw_jitter() : 0;

        record("net_send",
               std::string("from=") + std::to_string(from.id) + " to=" +
                   std::to_string(to.id) + " s=" + std::to_string(sseq) + " bytes=" +
                   std::to_string(bytes.size()) + " drop=" + (drop ? "1" : "0") +
                   " dup=" + (dup ? "1" : "0") + " lat=" + std::to_string(base_lat) +
                   " reorder=" + (reorder ? "1" : "0") + " jit=" + std::to_string(jitter));

        if (drop) {
            // Dropped: accepted by the bus, never delivered (matches INetwork:
            // send completes Ok once accepted, NOT once received).
            return ready_error(Error{});
        }

        // Schedule the primary delivery at base_lat + jitter ticks ahead.
        arm_delivery(from, to, bytes, base_lat + jitter, sseq, /*copy=*/0);
        if (dup) {
            // A duplicate copy: independent latency draw so the two copies can
            // arrive in either order (still deterministic — drawn in send order).
            Tick dup_lat = draw_latency();
            arm_delivery(from, to, bytes, dup_lat, sseq, /*copy=*/1);
        }
        return ready_error(Error{});
    }

    // recv() for `owner`: if a message is ready, complete immediately; else park a
    // promise served in FIFO order when the next delivery lands.
    Future<Message> recv(Endpoint owner) {
        Mailbox& mb = mailbox(owner);
        Promise<Message> p = core::make_promise<Message>(sched_);
        Future<Message> f = p.get_future();
        if (!mb.ready.empty()) {
            deliver_front_to(mb, std::move(p));
        } else {
            mb.waiters.push_back(std::move(p));
            record("net_recv_park", std::string("at=") + std::to_string(owner.id));
        }
        return f;
    }

private:
    // "not found" sentinel for mailbox_index(). EQUIVALENT-MUTANT NOTE: the
    // mutation gate flags the literal here (e.g. -1 -> -2) as surviving. It is a
    // PROVEN equivalent mutant: kNpos is only ever produced by mailbox_index() and
    // consumed by `== kNpos` / `!= kNpos` (uses ~L96/L299/L303); it is never an
    // arithmetic operand and never collides with a real index (a handful of
    // mailboxes), so any value outside the valid index range (SIZE_MAX, SIZE_MAX-1,
    // ...) is behaviourally identical. Not killable without coverage theatre;
    // documented instead (cf. the Scheduler insertion-sort guard).
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    // ---- delivery (virtual-time timer Task) ------------------------------

    // Spawn a Task that waits `lat` virtual ticks then deposits the message. The
    // partition is re-checked AT DELIVERY: a message armed before a heal that is
    // still cut when it fires is dropped (no crossing a live partition, ever).
    void arm_delivery(Endpoint from, Endpoint to, std::vector<std::byte> bytes,
                      Tick lat, std::uint64_t sseq, int copy) {
        sched_->spawn(delivery_task(this, from, to, std::move(bytes),
                                    lat > 0 ? lat : 1, sseq, copy));
    }

    static Task delivery_task(SimBus* bus, Endpoint from, Endpoint to,
                              std::vector<std::byte> bytes, Tick lat,
                              std::uint64_t sseq, int copy) {
        co_await bus->sched_->delay(lat);
        bus->deposit(from, to, std::move(bytes), sseq, copy);
        co_return;
    }

    // Deposit a delivered message into the recipient mailbox (or drop it if the
    // link is cut at delivery time). Wakes a parked recv waiter if present.
    void deposit(Endpoint from, Endpoint to, std::vector<std::byte> bytes,
                 std::uint64_t sseq, int copy) {
        if (is_blocked(from, to)) {
            record("net_drop_cut",
                   std::string("from=") + std::to_string(from.id) + " to=" +
                       std::to_string(to.id) + " s=" + std::to_string(sseq) + " c=" +
                       std::to_string(copy));
            return;
        }
        Mailbox& mb = mailbox(to);
        record("net_deliver",
               std::string("from=") + std::to_string(from.id) + " to=" +
                   std::to_string(to.id) + " s=" + std::to_string(sseq) + " c=" +
                   std::to_string(copy) + " bytes=" + std::to_string(bytes.size()));
        if (!mb.waiters.empty()) {
            // Serve the oldest waiter (FIFO) immediately.
            Promise<Message> p = std::move(mb.waiters.front());
            mb.waiters.erase(mb.waiters.begin());
            mb.retained.push_back(std::move(bytes));
            std::span<const std::byte> view(mb.retained.back().data(),
                                            mb.retained.back().size());
            p.set_value(Message{from, view});
        } else {
            mb.ready.push_back(std::move(bytes));
            mb.ready_from.push_back(from);
        }
    }

    // Complete a recv promise from the front of the ready queue (FIFO).
    void deliver_front_to(Mailbox& mb, Promise<Message> p) {
        std::vector<std::byte> bytes = std::move(mb.ready.front());
        Endpoint from = mb.ready_from.front();
        mb.ready.erase(mb.ready.begin());
        mb.ready_from.erase(mb.ready_from.begin());
        mb.retained.push_back(std::move(bytes));
        std::span<const std::byte> view(mb.retained.back().data(),
                                        mb.retained.back().size());
        p.set_value(Message{from, view});
    }

    // ---- fault draws (all from IRandom, fixed order) ---------------------

    Tick draw_latency() {
        Tick lo = faults_.latency_min > 0 ? faults_.latency_min : 1;
        Tick hi = faults_.latency_max >= lo ? faults_.latency_max : lo;
        return rng_->uniform_range(lo, hi);
    }
    Tick draw_jitter() {
        if (faults_.reorder_jitter_max <= 0) {
            return 0;
        }
        return rng_->uniform_range(1, faults_.reorder_jitter_max);
    }

    // ---- mailbox lookup (sorted vector; deterministic) -------------------

    [[nodiscard]] std::size_t mailbox_index(Endpoint e) const noexcept {
        for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
            if (mailboxes_[i].owner.id == e.id) {
                return i;
            }
        }
        return kNpos;
    }
    Mailbox& mailbox(Endpoint e) {
        std::size_t i = mailbox_index(e);
        if (i == kNpos) {
            add_node(e);
            i = mailbox_index(e);
        }
        return mailboxes_[i];
    }

    // ---- partition membership (sorted vector) ----------------------------

    [[nodiscard]] bool in_side_a(std::uint64_t id) const noexcept {
        // Binary search over the sorted side-A id vector.
        std::size_t lo = 0;
        std::size_t hi = partition_side_a_.size();
        while (lo < hi) {
            std::size_t mid = lo + (hi - lo) / 2;
            if (partition_side_a_[mid] < id) {
                lo = mid + 1;
            } else if (partition_side_a_[mid] > id) {
                hi = mid;
            } else {
                return true;
            }
        }
        return false;
    }

    // ---- trace + small helpers -------------------------------------------

    void record(const char* tag, std::string detail) {
        std::string payload = tag;
        if (!detail.empty()) {
            payload += ' ';
            payload += detail;
        }
        // Fold network events into the scheduler's Trace via the schedule-event
        // action so the rendered stream is byte-comparable across same-seed runs.
        sched_->trace(TraceAction::Schedule, std::move(payload));
    }

    Future<Error> ready_error(Error e) {
        Promise<Error> p = core::make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        p.set_value(e);
        return f;
    }

    static std::string render_ids(const std::vector<std::uint64_t>& ids) {
        std::string s = "sideA=[";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (i != 0) {
                s += ',';
            }
            s += std::to_string(ids[i]);
        }
        s += ']';
        return s;
    }

    static void insertion_sort(std::vector<std::uint64_t>& v) {
        for (std::size_t a = 1; a < v.size(); ++a) {
            std::uint64_t key = v[a];
            std::size_t b = a;
            while (b > 0 && v[b - 1] > key) {
                v[b] = v[b - 1];
                --b;
            }
            v[b] = key;
        }
    }
    static std::vector<std::uint64_t>::iterator
    unique_adjacent(std::vector<std::uint64_t>& v) {
        if (v.empty()) {
            return v.end();
        }
        std::size_t w = 1;
        for (std::size_t r = 1; r < v.size(); ++r) {
            if (v[r] != v[w - 1]) {
                v[w++] = v[r];
            }
        }
        return v.begin() + static_cast<std::ptrdiff_t>(w);
    }

    Scheduler* sched_;
    IRandom* rng_;
    LinkFaults faults_{};
    std::vector<Mailbox> mailboxes_{};            // sorted by owner id
    std::vector<std::uint64_t> partition_side_a_{}; // sorted ids of side A
    bool partitioned_ = false;
    std::uint64_t send_seq_ = 0; // monotonic send sequence (tie-break / trace)
};

} // namespace lockstep::sim::detail
