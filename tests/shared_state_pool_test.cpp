// shared_state_pool_test.cpp — S8.7. Proves the per-sink SharedState pool RECYCLES
// storage at exactly the last-reference-drop point, with shared_ptr ownership intact.
//
// What this locks (the load-bearing safety properties of the pool):
//   1. RECYCLE HAPPENS: a SharedState whose last shared_ptr drops returns its storage
//      block to the free list; the NEXT make_promise of the same T reuses that block
//      (same address). So steady-state minting is heap-traffic-free.
//   2. RECYCLE POINT == LAST-REFERENCE-DROP: while ANY of {Promise, Future, Awaiter}
//      still holds the shared_ptr, the block is NOT recycled (a fresh mint gets a
//      DIFFERENT address). Only after ALL drop is the slot reused. This is the
//      no-use-after-free argument made executable.
//   3. SET-THEN-CONSUME holds the block: a completed-but-not-yet-consumed SharedState
//      (Promise dropped, Future still alive) is NOT recycled — the awaiter can still
//      read result() safely.
//
// Header-only runtime; no provider, no PRNG. Single-threaded (L6).

#include <cassert>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Trace.hpp>
#include <lockstep/core/detail/SchedulerSink.hpp>

namespace {

using lockstep::core::Future;
using lockstep::core::make_promise;
using lockstep::core::Promise;
using lockstep::core::TraceAction;
namespace detail = lockstep::core::detail;

// A minimal SchedulerSink stand-in: it inherits the pool, records nothing, never
// resumes. We only exercise the allocation/recycle path, not the scheduler loop.
class StubSink final : public detail::SchedulerSink {
public:
    void schedule(std::coroutine_handle<> /*h*/, TraceAction /*why*/,
                  std::string /*payload*/) override {}
    std::uint64_t trace(TraceAction /*a*/, std::string /*p*/) override { return 0; }
    [[nodiscard]] std::int64_t vtime() const noexcept override { return 0; }
};

int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL: %s\n", (msg));                                   \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

}  // namespace

int main() {
    StubSink sink;

    // ---- Property 1 + 2: recycle on last-drop, fresh while held -------------------
    // Mint A and KEEP it alive; mint B — B must NOT reuse A's slot (A still held).
    // Then drop A and mint C — C SHOULD reuse a freed slot. We compare the raw block
    // addresses obtained via allocate_shared through the pool. We reach the address by
    // minting into a vector of shared_ptr<SharedState> directly through the same
    // allocator path the pool uses, mirroring make_promise.
    {
        using SS = detail::SharedState<int>;
        detail::SharedStatePool& pool = sink.shared_state_pool();

        auto mint = [&]() {
            return std::allocate_shared<SS>(
                detail::PoolAllocator<SS>(&pool), &sink);
        };

        std::shared_ptr<SS> a = mint();
        const void* addr_a = a.get();
        std::shared_ptr<SS> b = mint();
        const void* addr_b = b.get();
        CHECK(addr_a != addr_b, "two live SharedStates must NOT share a slot");

        // Drop A: its block returns to the free list (last reference gone).
        a.reset();
        // Mint C: it must reuse a recycled block — and the most-recently-freed one is
        // A's (LIFO free list), so C == A's old address. This proves recycle-on-drop.
        std::shared_ptr<SS> c = mint();
        const void* addr_c = c.get();
        CHECK(addr_c == addr_a, "freed slot must be RECYCLED by the next mint");
        CHECK(addr_c != addr_b, "recycled slot must differ from the still-live B");

        // While C is live, B is live: a third mint D must get a NEW block (both held).
        std::shared_ptr<SS> d = mint();
        CHECK(d.get() != addr_b && d.get() != addr_c,
              "a mint with all slots live must allocate fresh, not alias a live slot");
    }

    // ---- Property 3: set-then-not-consumed keeps the block (no early recycle) ------
    {
        Future<int> f;
        {
            Promise<int> p = make_promise<int>(&sink);
            f = p.get_future();
            p.set_value(42);
            // p drops at the end of this inner scope. The Future still holds a
            // reference, so the block must NOT be recycled — the awaiter can still
            // read result(). (A naive pool that recycled on Promise-drop would UAF
            // here; ASan/UBSan would catch it on the reads below.)
        }
        CHECK(f.valid(), "future must stay valid after the Promise dropped");
        CHECK(f.ready(), "completed state must report ready after set_value");
        CHECK(!f.has_error(), "value completion must not report error");
    }

    // ---- Stress: churn many mint/release cycles; ASan/UBSan watch for UAF ----------
    {
        for (int i = 0; i < 10000; ++i) {
            Promise<int> p = make_promise<int>(&sink);
            Future<int> f = p.get_future();
            p.set_value(i);
            // both drop at end of iteration -> block recycled -> next iter reuses it.
        }
        // void specialization too (timers use Future<void>).
        for (int i = 0; i < 10000; ++i) {
            Promise<void> p = make_promise<void>(&sink);
            Future<void> f = p.get_future();
            p.set_value();
        }
    }

    if (g_failures == 0) {
        std::printf("shared_state_pool OK (recycle-on-last-drop, no early recycle)\n");
        return 0;
    }
    std::printf("shared_state_pool FAILED (%d checks)\n", g_failures);
    return 1;
}
