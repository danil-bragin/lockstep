#pragma once

// Scheduler.hpp — C1.3 + C1.4. The deterministic execution engine and its bound
// virtual clock (SimClock). Single OS thread, cooperative, no preemption.
//
// LOCKED SEMANTICS enforced here:
//   L1: scheduling (schedule()) only ever pushes onto the ready queue; nothing
//       in this file calls handle.resume() except the ONE driver loop in run().
//   L3: the ready queue is a strict, documented FIFO (detail::ReadyQueue). Timer
//       firing order is a deterministic monotonic key (due_tick, arm_seq) — never
//       a pointer address, never unordered_map iteration order.
//   L4: virtual time advances ONLY when the ready queue is empty and timers
//       pend; it then jumps to the earliest timer's due tick and fires every
//       timer due at that tick (in arm order).
//   L5: run() is a pure function of (seed, initial tasks): the ready queue,
//       timer order, and trace are all insertion/key ordered with no nondetermin
//       -istic input. The seed lives in SeededRandom (providers/sim), threaded in
//       by the caller; the scheduler itself reads no clock and no randomness.
//   L6: no real threads, no locks, no atomics, no wall-clock.
//
// The Scheduler implements IScheduler (spawn/run) and detail::SchedulerSink (the
// narrow surface Future/Task use to schedule waiters + record trace). SimClock
// implements IClock and is bound to one Scheduler's virtual time.

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IScheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/core/Trace.hpp>
#include <lockstep/core/detail/ReadyQueue.hpp>
#include <lockstep/core/detail/SchedulerSink.hpp>

namespace lockstep::core {

class SimClock; // bound virtual clock, defined below

class Scheduler final : public IScheduler, public detail::SchedulerSink {
public:
    Scheduler() = default;
    ~Scheduler() override {
        // Destroy any spawned coroutine frames the scheduler still owns. A clean
        // run leaves none pending, but defensive cleanup keeps ASan/LSan quiet.
        for (auto& frame : owned_frames_) {
            if (frame) {
                frame.destroy();
            }
        }
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // ---- IScheduler ------------------------------------------------------

    // Enqueue a coroutine. Takes ownership of the frame (Task::release) so it
    // outlives the temporary Task; records a spawn event and schedules the
    // handle onto the ready queue (FIFO). Does NOT run it inline.
    void spawn(Task&& work) override {
        Task::handle_type h = work.release();
        if (!h) {
            return;
        }
        h.promise().sink = this;
        std::uint64_t id = next_task_id_++;
        h.promise().task_id = id;
        owned_frames_.push_back(h);
        trace(TraceAction::Spawn, std::string("id=") + std::to_string(id));
        ready_.push(detail::ReadyItem{h, enqueue_seq_++});
    }

    // The cooperative loop. While the ready queue is non-empty, dequeue (FIFO)
    // and resume the next continuation. When it empties but timers pend, advance
    // virtual time to the earliest timer and fire all timers due at that tick.
    // Returns when no work and no timers remain.
    void run() override {
        trace(TraceAction::RunStart, {});
        for (;;) {
            if (!ready_.empty()) {
                detail::ReadyItem item = ready_.pop();
                trace(TraceAction::Resume, std::string("seq=") + std::to_string(item.seq));
                item.handle.resume(); // the ONE and ONLY resume site (L1)
                continue;
            }
            // L4: ready queue empty. If timers pend, advance vtime to the
            // earliest and fire. Otherwise the run is finished.
            if (timers_.empty()) {
                break;
            }
            fire_earliest_timers();
        }
        trace(TraceAction::RunEnd, {});
    }

    // ---- detail::SchedulerSink ------------------------------------------

    void schedule(std::coroutine_handle<> h, TraceAction why, std::string payload) override {
        trace(why, std::move(payload));
        ready_.push(detail::ReadyItem{h, enqueue_seq_++});
    }

    std::uint64_t trace(TraceAction action, std::string payload) override {
        return trace_.record(action, vtime_, std::move(payload));
    }

    [[nodiscard]] std::int64_t vtime() const noexcept override { return vtime_; }

    // ---- virtual clock surface (bound by SimClock) ----------------------

    [[nodiscard]] Tick now() const noexcept { return vtime_; }

    // Arm a timer `d` ticks ahead and return a Future<void> that completes when
    // virtual time has advanced by `d`. d <= 0 completes at the current tick
    // (yields without advancing the clock). Used by SimClock::delay().
    [[nodiscard]] Future<void> delay(Duration d) {
        Promise<void> p = make_promise<void>(this);
        Future<void> f = p.get_future();
        Tick due = vtime_ + (d > 0 ? d : 0);
        std::uint64_t arm = timer_arm_seq_++;
        timers_.push_back(Timer{due, arm, std::move(p)});
        trace(TraceAction::TimerArm,
              std::string("due=") + std::to_string(due) + " arm=" + std::to_string(arm));
        return f;
    }

    // ---- introspection (used by tests / self-test) ----------------------

    [[nodiscard]] const Trace& event_trace() const noexcept { return trace_; }
    [[nodiscard]] std::string trace_text() const { return trace_.render(); }

private:
    // A pending virtual-time timer. Ordered by (due, arm_seq): earliest due tick
    // first, ties broken by arm order — a deterministic monotonic key (L3/L4),
    // never a pointer or hash order.
    struct Timer {
        Tick due = 0;
        std::uint64_t arm_seq = 0;
        Promise<void> promise; // fulfilled (which SCHEDULES the waiter, L1) on fire
    };

    // L4: advance virtual time to the earliest pending timer and fire every timer
    // due at that exact tick, in arm order. Firing fulfills the timer's Promise,
    // which (L1) SCHEDULES its waiter onto the ready queue. The clock only ever
    // moves forward and only from here (called when ready_ is empty).
    void fire_earliest_timers() {
        assert(!timers_.empty());
        // Find the minimum (due, arm_seq) deterministically.
        std::size_t min_idx = 0;
        for (std::size_t i = 1; i < timers_.size(); ++i) {
            if (timer_less(timers_[i], timers_[min_idx])) {
                min_idx = i;
            }
        }
        Tick target = timers_[min_idx].due;
        if (target > vtime_) {
            vtime_ = target; // jump forward; clock advances ONLY here (L4)
            trace(TraceAction::ClockAdvance, std::string("to=") + std::to_string(vtime_));
        }
        // Fire all timers due at `target`, in arm order, deterministically.
        // Collect their indices, sort by arm_seq, fire, then erase.
        std::vector<std::size_t> due_now;
        for (std::size_t i = 0; i < timers_.size(); ++i) {
            if (timers_[i].due == target) {
                due_now.push_back(i);
            }
        }
        // Insertion-sort indices by arm_seq (small N; stable + deterministic).
        for (std::size_t a = 1; a < due_now.size(); ++a) {
            std::size_t key = due_now[a];
            std::size_t b = a;
            while (b > 0 && timers_[due_now[b - 1]].arm_seq > timers_[key].arm_seq) {
                due_now[b] = due_now[b - 1];
                --b;
            }
            due_now[b] = key;
        }
        // Move out the timers to fire (so erasing does not invalidate them mid-loop).
        std::vector<Timer> firing;
        firing.reserve(due_now.size());
        for (std::size_t idx : due_now) {
            firing.push_back(std::move(timers_[idx]));
        }
        // Erase fired timers from the pending list (indices were ascending).
        for (std::size_t k = due_now.size(); k > 0; --k) {
            timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(due_now[k - 1]));
        }
        // Fire: fulfilling each Promise SCHEDULES its waiter (L1). Trace each.
        for (Timer& t : firing) {
            trace(TraceAction::TimerFire, std::string("arm=") + std::to_string(t.arm_seq));
            t.promise.set_value();
        }
    }

    static bool timer_less(const Timer& a, const Timer& b) noexcept {
        if (a.due != b.due) {
            return a.due < b.due;
        }
        return a.arm_seq < b.arm_seq;
    }

    detail::ReadyQueue ready_{};
    std::vector<Timer> timers_{};
    std::vector<std::coroutine_handle<>> owned_frames_{}; // frames the scheduler owns
    Trace trace_{};
    Tick vtime_ = 0;
    std::uint64_t enqueue_seq_ = 0;   // global ready-queue enqueue sequence
    std::uint64_t timer_arm_seq_ = 0; // global timer-arm sequence
    std::uint64_t next_task_id_ = 0;  // spawned-task id sequence
};

// ---------------------------------------------------------------------------
// SimClock — C1.4. Virtual-time IClock bound to one Scheduler. now()/delay()
// forward to the scheduler's virtual time; there is NO wall-clock. Virtual time
// advances only inside Scheduler::run() when the ready queue is empty (L4).
// ---------------------------------------------------------------------------
class SimClock final : public IClock {
public:
    explicit SimClock(Scheduler& sched) noexcept : sched_(&sched) {}

    [[nodiscard]] Tick now() const noexcept override { return sched_->now(); }
    [[nodiscard]] Future<void> delay(Duration d) override { return sched_->delay(d); }

private:
    Scheduler* sched_;
};

} // namespace lockstep::core
