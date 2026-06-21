// DIRTY: coordination atomics. std::memory_order_*, atomic_thread_fence.
// NOTE: bare std::atomic<int> for a plain counter is intentionally NOT flagged.
#include <atomic>

namespace lockstep::bad {
std::atomic<int> counter{0};  // gray: allowed, not flagged on its own.
void coordinate() {
    counter.store(1, std::memory_order_release);
    int v = counter.load(std::memory_order_acquire);
    (void)v;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
}  // namespace lockstep::bad
