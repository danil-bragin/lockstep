#pragma once

// ProdReactor.hpp — Phase 7 S4a. The PRODUCTION REACTOR: a real epoll-driven,
// single-threaded event loop that implements the SAME two core interfaces the sim
// Scheduler implements (core::IScheduler + core::detail::SchedulerSink), so the
// entire verified async core (Future/Promise/Task) runs on it UNCHANGED. This is
// the real-time analogue of core::Scheduler::run(): a FIFO ready queue plus a set
// of pending timers, driven by epoll_wait instead of a virtual-time jump.
//
// THE SEAM (no core change). Future/Promise/providers hold a SchedulerSink* and
// call sink->schedule(handle, why, payload) to resume a waiter; co_await parks the
// waiter; the ONE driver loop in run() is the only place a handle is resumed (L1).
// ProdReactor satisfies that surface exactly like Scheduler does, so swapping the
// reactor for the sim scheduler is a constructor change, not a core rewrite.
//
// DESIGN (ready queue + timers + epoll_fd):
//   * Ready queue  — detail::ReadyQueue (the SAME strict FIFO the sim uses). A
//     completion SCHEDULES onto it; it is NEVER resumed inline (L1). run() pops
//     and resumes in FIFO order — deterministic dequeue as far as real time lets.
//   * Timers       — a std::vector<Timer{deadline, arm_seq, Promise<void>}> kept
//     ordered by the deterministic (deadline, arm_seq) key, exactly mirroring the
//     sim's timer ordering. delay() arms one; the loop fires every timer whose
//     real deadline has passed (now() >= deadline), in (deadline, arm_seq) order.
//   * epoll_fd     — owned from construction via epoll_create1(EPOLL_CLOEXEC),
//     even with NO fds registered yet. Timers use the epoll_wait TIMEOUT argument
//     (timeout = earliest_deadline - now(), clamped to >=0), NOT a timerfd — this
//     keeps the timer path allocation-free and means S4b plugs SOCKET fds into the
//     SAME epoll set (EPOLL_CTL_ADD + an fd->handler table) and the SAME run loop
//     WITHOUT touching the timer code: epoll_wait then returns either on a ready fd
//     (dispatch its handler -> schedule its continuation) or on the timer timeout
//     (fire due timers). The loop body already handles "woke with no fd events"
//     (timeout) vs "woke with fd events" (S4b) as separate, additive branches.
//
// HOW THE CORE RUNS UNCHANGED:
//   * IScheduler::spawn(Task&&)  — same as Scheduler: adopt the frame, set its
//     sink to `this`, push onto the ready queue. Never run inline.
//   * IScheduler::run()          — the cooperative loop below (to quiescence).
//   * SchedulerSink::schedule()  — push onto the FIFO ready queue (L1).
//   * SchedulerSink::trace()     — append to an in-memory trace, return a seq.
//   * SchedulerSink::vtime()     — ProdClock::now() ticks (real monotonic time),
//     the prod analogue of virtual time used for trace stamping.
//
// LINUX-ONLY. epoll is Linux. The whole class is compiled only under __linux__;
// the CMake target + its test are added only on Linux, so the macOS host build
// stays green (the reactor target is simply absent there).
//
// providers/prod/ is the lint-exempt boundary zone: epoll/eventfd syscalls and
// std::chrono are permitted HERE (and only here). Single-threaded; no real
// threads; epoll_wait BLOCKS (never a busy spin). RAII closes the epoll fd.

#ifdef __linux__

#include <sys/epoll.h> // epoll_create1, epoll_wait, epoll_ctl — ALLOWED only under providers/
#include <unistd.h>    // close

#include <cstdint>
#include <coroutine>
#include <functional>
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
#include <lockstep/prod/ProdClock.hpp>

namespace lockstep::prod {

// The real-time reactor. Implements the same surfaces as core::Scheduler so the
// verified async core runs on real epoll + real timers with no core change. Also
// implements prod::ITimerRegistrar so a ProdClock can bind to it and route
// delay() to the reactor's real timers (ProdClock(ITimerRegistrar*)).
class ProdReactor final : public core::IScheduler,
                          public core::detail::SchedulerSink,
                          public ITimerRegistrar {
public:
    // Construct the loop and its epoll instance. The clock is the real monotonic
    // source (ProdClock::now() in ns ticks) used for timer deadlines and vtime().
    // epoll_create1 gives us an empty epoll set NOW; S4b registers socket fds into
    // it. EPOLL_CLOEXEC so a future fork()/exec() does not leak the descriptor.
    ProdReactor() noexcept : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {}

    ProdReactor(const ProdReactor&) = delete;
    ProdReactor& operator=(const ProdReactor&) = delete;
    ProdReactor(ProdReactor&&) = delete;
    ProdReactor& operator=(ProdReactor&&) = delete;

    // RAII: close the epoll fd (no fd leak under LSan). Destroy any frames the
    // reactor still owns — a clean run to quiescence leaves none, but defensive
    // cleanup keeps ASan/LSan quiet (mirrors Scheduler's destructor).
    ~ProdReactor() override {
        for (auto& frame : owned_frames_) {
            if (frame) {
                frame.destroy();
            }
        }
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    // True if epoll_create1 succeeded. A reactor whose epoll fd failed to open is
    // unusable; surface it rather than spinning.
    [[nodiscard]] bool valid() const noexcept { return epoll_fd_ >= 0; }
    [[nodiscard]] int epoll_fd() const noexcept { return epoll_fd_; }

    // ---- core::IScheduler ------------------------------------------------

    // Enqueue a coroutine. Adopt the frame (Task::release) so it outlives the
    // temporary Task; record a spawn event; push onto the FIFO ready queue. Does
    // NOT run it inline. Byte-for-byte the sim Scheduler's spawn shape.
    void spawn(core::Task&& work) override {
        core::Task::handle_type h = work.release();
        if (!h) {
            return;
        }
        h.promise().sink = this;
        std::uint64_t id = next_task_id_++;
        h.promise().task_id = id;
        owned_frames_.push_back(h);
        trace(core::TraceAction::Spawn, std::string("id=") + std::to_string(id));
        ready_.push(core::detail::ReadyItem{h, enqueue_seq_++});
    }

    // The cooperative loop, driven to QUIESCENCE (no ready work AND no pending
    // timers). While the ready queue is non-empty, pop (FIFO) + resume the next
    // continuation. When it empties: if timers pend, epoll_wait with a timeout of
    // (earliest deadline - now()); on the timeout (or any wake) fire all DUE
    // timers (now() >= deadline), which SCHEDULES their waiters; loop. With no
    // ready work and no timers, return. A `stop_` flag lets a long-lived server
    // (S5) break out; tests drive to quiescence and never set it.
    void run() override {
        trace(core::TraceAction::RunStart, {});
        for (;;) {
            if (stop_) {
                break;
            }
            if (!ready_.empty()) {
                core::detail::ReadyItem item = ready_.pop();
                trace(core::TraceAction::Resume,
                      std::string("seq=") + std::to_string(item.seq));
                item.handle.resume(); // the ONE and ONLY resume site (L1)
                continue;
            }
            // Ready queue empty. Quiesce if nothing can ever wake us (no timers AND
            // no registered fds). A node with a listen socket registered does NOT
            // quiesce here — it uses run_until / run_with_deadline instead.
            if (timers_.empty() && handlers_.empty()) {
                break;
            }
            wait_and_dispatch(/*deadline_ns=*/0);
        }
        trace(core::TraceAction::RunEnd, {});
    }

    // Request the loop stop at the next turn (for a long-lived server loop, S5).
    // Idempotent; safe to call from a scheduled continuation. Tests do not use it.
    void stop() noexcept { stop_ = true; }

    // ---- fd registration (S4b — sockets plug into the SAME epoll set) ----
    //
    // The S4a design note's promise made real: a socket fd is registered with
    // EPOLL_CTL_ADD + an entry in an fd->handler table; the ONE epoll_wait loop in
    // run() then dispatches a ready fd to its handler (which SCHEDULES continuations
    // via the SchedulerSink, never resumes inline) AND fires due timers, as two
    // additive branches. The timer path is UNCHANGED — timers still use the
    // epoll_wait TIMEOUT argument; fds just make epoll_wait ALSO return on IO.
    //
    // `events` is an EPOLLIN/EPOLLOUT mask (EPOLLET is NOT used — level-triggered so
    // a half-drained socket re-wakes; simplest correct model). `on_ready` is invoked
    // with the events that fired for that fd. Single-threaded: the handler runs on
    // the reactor thread, inside run(), between timer turns.

    using FdHandler = std::function<void(std::uint32_t /*revents*/)>;

    // Register `fd` on the epoll set with interest `events`. Returns false if
    // epoll_ctl fails (e.g. fd already registered — use mod_fd then). The handler
    // table is a sorted-by-fd vector: deterministic iteration, no unordered map.
    bool add_fd(int fd, std::uint32_t events, FdHandler on_ready) {
        if (fd < 0 || epoll_fd_ < 0) {
            return false;
        }
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            return false;
        }
        insert_handler(fd, events, std::move(on_ready));
        trace(core::TraceAction::Schedule,
              std::string("fd_add fd=") + std::to_string(fd) +
                  " ev=" + std::to_string(events));
        return true;
    }

    // Change the interest mask for an already-registered fd (e.g. arm/disarm
    // EPOLLOUT as the write buffer fills/drains). Keeps the same handler.
    bool mod_fd(int fd, std::uint32_t events) {
        if (fd < 0 || epoll_fd_ < 0) {
            return false;
        }
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
            return false;
        }
        const std::size_t i = handler_index(fd);
        if (i != kNoHandler) {
            handlers_[i].events = events;
        }
        return true;
    }

    // Unregister `fd` from the epoll set and drop its handler. Idempotent: a
    // missing fd is a no-op (the caller may also have closed it). NEVER closes the
    // fd — ownership stays with the registrant (ProdNetwork RAII-closes its own).
    void remove_fd(int fd) {
        if (fd < 0) {
            return;
        }
        if (epoll_fd_ >= 0) {
            // EPOLL_CTL_DEL: ignore ENOENT (already gone) / EBADF (closed already).
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        const std::size_t i = handler_index(fd);
        if (i != kNoHandler) {
            handlers_.erase(handlers_.begin() + static_cast<std::ptrdiff_t>(i));
            trace(core::TraceAction::Schedule,
                  std::string("fd_del fd=") + std::to_string(fd));
        }
    }

    [[nodiscard]] std::size_t registered_fd_count() const noexcept {
        return handlers_.size();
    }

    // ---- stoppable / bounded run loops (servers + networked tests) -------
    //
    // run() drives to QUIESCENCE: no ready work, no timers, AND no registered fds.
    // A networked node has a listen fd registered, so it would NOT quiesce — it
    // would block in epoll_wait forever. So networked tests/servers use one of:
    //
    //   run_until(pred)        — pump the loop until `pred()` is true (checked each
    //                            turn). The bounded networked-test driver.
    //   run_with_deadline(ns)  — pump until the reactor's now() passes an ABSOLUTE
    //                            deadline (a HARD wall guard so a lost frame / half-
    //                            open socket can NEVER hang the loop), OR quiescence.
    //   run_until(pred, ns)    — both: stop on pred OR the absolute deadline.
    //
    // All share the SAME loop body as run(): drain ready (FIFO, the one resume site
    // L1), then epoll_wait (dispatch ready fds to handlers + fire due timers).

    // Pump until `pred()` returns true OR an absolute now()-deadline passes (ns).
    // deadline_ns <= 0 means "no deadline" (only pred stops it). Returns true if
    // pred fired, false if the deadline tripped first.
    bool run_until(const std::function<bool()>& pred, core::Tick deadline_ns = 0) {
        trace(core::TraceAction::RunStart, {});
        bool pred_fired = false;
        for (;;) {
            if (stop_) {
                break;
            }
            if (pred && pred()) {
                pred_fired = true;
                break;
            }
            if (deadline_ns > 0 && clock_.now() >= deadline_ns) {
                break; // HARD wall guard: never hang on a lost frame / half-open fd.
            }
            if (!ready_.empty()) {
                core::detail::ReadyItem item = ready_.pop();
                trace(core::TraceAction::Resume,
                      std::string("seq=") + std::to_string(item.seq));
                item.handle.resume(); // the ONE and ONLY resume site (L1)
                continue;
            }
            // Ready queue empty. If nothing can ever wake us, quiesce.
            if (timers_.empty() && handlers_.empty()) {
                break;
            }
            wait_and_dispatch(deadline_ns);
        }
        trace(core::TraceAction::RunEnd, {});
        return pred_fired;
    }

    // Pump until an absolute now()-deadline passes (ns) or the loop quiesces.
    void run_with_deadline(core::Tick deadline_ns) {
        run_until(std::function<bool()>{}, deadline_ns);
    }

    // ---- core::detail::SchedulerSink ------------------------------------

    void schedule(std::coroutine_handle<> h, core::TraceAction why,
                  std::string payload) override {
        trace(why, std::move(payload));
        ready_.push(core::detail::ReadyItem{h, enqueue_seq_++});
    }

    std::uint64_t trace(core::TraceAction action, std::string payload) override {
        return trace_.record(action, vtime(), std::move(payload));
    }

    // Real monotonic time (ProdClock ns ticks) — the prod analogue of virtual
    // time, used to stamp the trace. Never decreases (steady_clock guarantee).
    [[nodiscard]] std::int64_t vtime() const noexcept override { return clock_.now(); }

    // ---- prod::ITimerRegistrar (drives ProdClock::delay()) --------------

    // Arm a real timer `d` ticks (ns) ahead of now() and return a Future<void>
    // the loop completes once real time passes the deadline. d <= 0 arms a timer
    // due at now() (it fires on the very next loop turn — completes promptly,
    // never hangs, never blocks epoll_wait since the timeout is clamped to 0).
    // Minted from the reactor-as-SchedulerSink so the waiter routes through L1.
    // This is the ITimerRegistrar implementation a bound ProdClock forwards to.
    [[nodiscard]] core::Future<void> arm_timer(core::Duration d) override {
        core::Promise<void> p = core::make_promise<void>(this);
        core::Future<void> f = p.get_future();
        const core::Tick deadline = clock_.now() + (d > 0 ? d : 0);
        const std::uint64_t arm = timer_arm_seq_++;
        timers_.push_back(Timer{deadline, arm, std::move(p)});
        trace(core::TraceAction::TimerArm,
              std::string("due=") + std::to_string(deadline) + " arm=" + std::to_string(arm));
        return f;
    }

    // ---- introspection (tests) ------------------------------------------

    [[nodiscard]] const core::Trace& event_trace() const noexcept { return trace_; }
    [[nodiscard]] std::string trace_text() const { return trace_.render(); }
    [[nodiscard]] std::size_t pending_timer_count() const noexcept { return timers_.size(); }
    [[nodiscard]] core::Tick now() const noexcept { return clock_.now(); }

private:
    // A pending real-time timer. Ordered by (deadline, arm_seq): earliest deadline
    // first, ties broken by arm order — the SAME deterministic monotonic key the
    // sim uses, so firing order is deterministic where real time permits.
    struct Timer {
        core::Tick deadline = 0;
        std::uint64_t arm_seq = 0;
        core::Promise<void> promise; // fulfilled (SCHEDULES the waiter, L1) on fire
    };

    static bool timer_less(const Timer& a, const Timer& b) noexcept {
        if (a.deadline != b.deadline) {
            return a.deadline < b.deadline;
        }
        return a.arm_seq < b.arm_seq;
    }

    // An fd registered on the epoll set + its handler. The handler SCHEDULES
    // continuations (never resumes inline). Kept in a sorted-by-fd vector so all
    // iteration is deterministic and lookup is a tight scan / binary search.
    struct FdEntry {
        int fd = -1;
        std::uint32_t events = 0;
        FdHandler on_ready{};
    };

    static constexpr std::size_t kNoHandler = static_cast<std::size_t>(-1);

    // Index of fd in the sorted handler table, or kNoHandler. Binary search.
    [[nodiscard]] std::size_t handler_index(int fd) const noexcept {
        std::size_t lo = 0;
        std::size_t hi = handlers_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (handlers_[mid].fd < fd) {
                lo = mid + 1;
            } else if (handlers_[mid].fd > fd) {
                hi = mid;
            } else {
                return mid;
            }
        }
        return kNoHandler;
    }

    // Insert a handler keeping handlers_ sorted by fd (no duplicate fd — caller
    // guarantees a fresh EPOLL_CTL_ADD'd fd).
    void insert_handler(int fd, std::uint32_t events, FdHandler on_ready) {
        std::size_t pos = 0;
        while (pos < handlers_.size() && handlers_[pos].fd < fd) {
            ++pos;
        }
        handlers_.insert(handlers_.begin() + static_cast<std::ptrdiff_t>(pos),
                         FdEntry{fd, events, std::move(on_ready)});
    }

    // The deadline of the earliest pending timer (deterministic min by the same
    // (deadline, arm_seq) key used to fire). Caller ensures timers_ is non-empty.
    [[nodiscard]] core::Tick earliest_deadline() const noexcept {
        std::size_t min_idx = 0;
        for (std::size_t i = 1; i < timers_.size(); ++i) {
            if (timer_less(timers_[i], timers_[min_idx])) {
                min_idx = i;
            }
        }
        return timers_[min_idx].deadline;
    }

    // Block in epoll_wait until the earliest timer's deadline / the run deadline /
    // a ready fd, then DISPATCH every ready fd to its handler (which SCHEDULES, never
    // resumes inline — L1) AND fire EVERY timer whose deadline has passed, in
    // (deadline, arm_seq) order. Two additive branches on the SAME wait, exactly as
    // the S4a design note promised. Called only when the ready queue is empty and at
    // least one timer OR one fd pends.
    //
    // `deadline_ns` is an OPTIONAL absolute now() bound (a hard wall guard for the
    // networked loop; 0 = none): the wait timeout is the MIN of (earliest timer due)
    // and (run deadline), so a node with only a listen fd and no timers still wakes
    // by the deadline instead of blocking forever on a lost frame.
    void wait_and_dispatch(core::Tick deadline_ns) {
        // Compute the epoll_wait timeout = (min wake instant) - now(), in
        // MILLISECONDS, rounded UP so we never wake before it, clamped to >= 0.
        const core::Tick now = clock_.now();
        core::Tick wake_at = 0;
        bool have_wake = false;
        if (!timers_.empty()) {
            wake_at = earliest_deadline();
            have_wake = true;
        }
        if (deadline_ns > 0 && (!have_wake || deadline_ns < wake_at)) {
            wake_at = deadline_ns;
            have_wake = true;
        }
        // -1 == block indefinitely (only legal when an fd can wake us; we only reach
        // here with a timer, a deadline, or a registered fd, so an indefinite block
        // here is always woken by a socket event).
        int timeout_ms = -1;
        if (have_wake) {
            timeout_ms = 0;
            if (wake_at > now) {
                const core::Tick ns = wake_at - now;
                const core::Tick ms = (ns + 999'999) / 1'000'000;
                constexpr core::Tick kMaxMs = 60'000; // 60s ceiling: bounded waits
                timeout_ms = static_cast<int>(ms > kMaxMs ? kMaxMs : ms);
            }
        }

        // epoll_wait BLOCKS (never a busy spin). Returns on the timeout, or on ready
        // socket fds (events[0..n)) for dispatch.
        epoll_event events[kMaxEvents];
        const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
        // n < 0 (EINTR) is fine: fall through, fire due timers, loop. n == 0 is the
        // timeout. n > 0 dispatches each ready fd to its handler.
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t revents = events[i].events;
            const std::size_t hi = handler_index(fd);
            if (hi != kNoHandler) {
                // Copy the handler out before invoking: the handler may add/remove
                // fds (mutating handlers_), so we must not hold an index/reference
                // into the vector across the call (V-RKV1: no live ref across a
                // mutation of a growable container).
                FdHandler h = handlers_[hi].on_ready;
                h(revents);
            }
        }

        fire_due_timers();
    }

    // Fire every timer whose deadline has passed (now() >= deadline), in the
    // deterministic (deadline, arm_seq) order. Collect due indices, sort by the
    // key, MOVE out the timers (so erasing does not invalidate a Promise held
    // mid-loop — V-RKV1: no live reference into timers_ across the erase/fire),
    // erase them, then fire (each set_value SCHEDULES its waiter, L1). If real time
    // has not yet reached the earliest deadline (a spurious/early epoll wake), this
    // fires nothing and run() loops back to wait again — never a busy spin because
    // the next wait blocks until the deadline.
    void fire_due_timers() {
        const core::Tick now = clock_.now();
        std::vector<std::size_t> due_now;
        for (std::size_t i = 0; i < timers_.size(); ++i) {
            if (timers_[i].deadline <= now) {
                due_now.push_back(i);
            }
        }
        if (due_now.empty()) {
            return; // early wake before any deadline: loop and wait again.
        }
        // Insertion-sort the due indices by (deadline, arm_seq) (small N; stable).
        for (std::size_t a = 1; a < due_now.size(); ++a) {
            const std::size_t key = due_now[a];
            std::size_t b = a;
            while (b > 0 && timer_less(timers_[key], timers_[due_now[b - 1]])) {
                due_now[b] = due_now[b - 1];
                --b;
            }
            due_now[b] = key;
        }
        // Move out the timers to fire BEFORE erasing (no dangling reference).
        std::vector<Timer> firing;
        firing.reserve(due_now.size());
        for (const std::size_t idx : due_now) {
            firing.push_back(std::move(timers_[idx]));
        }
        // Erase fired entries. due_now is now in (deadline, arm_seq) order, NOT
        // ascending index order, so erase from a copy sorted DESCENDING by index
        // to keep the remaining indices valid across erasures.
        std::vector<std::size_t> idx_desc(due_now);
        for (std::size_t a = 1; a < idx_desc.size(); ++a) {
            const std::size_t key = idx_desc[a];
            std::size_t b = a;
            while (b > 0 && idx_desc[b - 1] < key) {
                idx_desc[b] = idx_desc[b - 1];
                --b;
            }
            idx_desc[b] = key;
        }
        for (const std::size_t idx : idx_desc) {
            timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(idx));
        }
        // Fire: fulfilling each Promise SCHEDULES its waiter (L1). Trace each.
        for (Timer& t : firing) {
            trace(core::TraceAction::TimerFire, std::string("arm=") + std::to_string(t.arm_seq));
            t.promise.set_value();
        }
    }

    // epoll_wait scratch capacity. S4a registers no fds (n is always 0 on the
    // timeout path); sized for S4b's socket fan-in without a per-wait alloc.
    static constexpr int kMaxEvents = 64;

    ProdClock clock_{};                                     // real monotonic clock
    int epoll_fd_ = -1;                                     // owned; RAII-closed
    core::detail::ReadyQueue ready_{};                      // strict FIFO (L3)
    std::vector<Timer> timers_{};                           // (deadline, arm_seq) keyed
    std::vector<FdEntry> handlers_{};                       // sorted-by-fd; socket fds
    std::vector<std::coroutine_handle<>> owned_frames_{};   // frames the reactor owns
    core::Trace trace_{};                                   // in-memory run trace
    std::uint64_t enqueue_seq_ = 0;                         // ready-queue enqueue seq
    std::uint64_t timer_arm_seq_ = 0;                       // timer-arm seq
    std::uint64_t next_task_id_ = 0;                        // spawned-task id seq
    bool stop_ = false;                                     // long-lived-loop break
};

} // namespace lockstep::prod

#endif // __linux__
