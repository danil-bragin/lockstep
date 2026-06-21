#pragma once

// ReadyQueue.hpp — the scheduler's deterministic ready queue and the abstract
// "resumable" handle it stores. Factored out so Future/Task/Scheduler share ONE
// definition of what "schedule a waiter" means (L1) without circular includes.
//
// DETERMINISM (L3): the ready queue is a strict FIFO. Items are appended on
// enqueue and removed from the front on dequeue. Order is therefore the order in
// which work became ready — never pointer address, never hash/unordered_map
// iteration order. This file deliberately uses std::deque (insertion-ordered),
// NOT any associative/unordered container.

#include <coroutine>
#include <cstdint>
#include <deque>

namespace lockstep::core::detail {

// A unit of resumable work: a coroutine handle plus the monotonically-increasing
// sequence number it was enqueued with. The sequence is recorded in the trace so
// the FIFO order is observable and replayable; it never affects dequeue order
// (the deque already gives FIFO), it only labels events deterministically.
struct ReadyItem {
    std::coroutine_handle<> handle{}; // the continuation to resume
    std::uint64_t seq = 0;            // enqueue sequence (for the trace only)
};

// Strict FIFO ready queue. The ONLY ordering policy in the runtime. Documented
// in docs/runtime-determinism.md; future agents must not swap this for a
// pointer- or hash-ordered structure (that would break L3/L5).
class ReadyQueue {
public:
    [[nodiscard]] bool empty() const noexcept { return q_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return q_.size(); }

    // Append to the back (FIFO enqueue).
    void push(ReadyItem item) { q_.push_back(item); }

    // Remove and return the front (FIFO dequeue). Caller must check !empty().
    [[nodiscard]] ReadyItem pop() {
        ReadyItem front = q_.front();
        q_.pop_front();
        return front;
    }

private:
    std::deque<ReadyItem> q_;
};

} // namespace lockstep::core::detail
