#pragma once

// SharedStatePool.hpp — S8.7. A per-scheduler-sink object pool that RECYCLES the
// fused control-block+object storage that `std::make_shared<SharedState<T>>` would
// otherwise heap-allocate per Promise. This is a pure MEMORY-REUSE optimization: it
// changes only WHERE a SharedState lives, never any value, ordering, or observable
// behavior, so the deterministic sim stays BYTE-IDENTICAL before vs after.
//
// WHY THIS SHAPE (the load-bearing correctness argument):
//   * Ownership is UNCHANGED. A SharedState is still owned by a std::shared_ptr; the
//     Promise (write end), the Future (read end), and the FutureAwaiter all hold
//     copies of that shared_ptr exactly as before. The refcount still drives the
//     lifetime. The ONLY change is the allocator the shared_ptr was minted with.
//   * RECYCLE POINT = last-reference-drop, EXACTLY as today. We mint the shared_ptr
//     with std::allocate_shared<SharedState<T>>(PoolAllocator{pool}, sink). When the
//     LAST shared_ptr reference drops (the shared_ptr control block hits zero), the
//     standard library destroys the SharedState and then calls the allocator's
//     deallocate() — which, instead of returning the block to the OS, links it onto a
//     per-size free list for reuse. So a slot is recycled at PRECISELY the moment the
//     old code would have `delete`d it: after BOTH the Promise has set it AND the
//     awaiter has consumed the value (whichever shared_ptr is last). There is no
//     earlier recycle, hence NO use-after-free and NO recycling a slot a coroutine
//     still references — the refcount is the single source of truth, untouched.
//   * No raw new/delete in the hot path that the analyzer must reason about as owning
//     memory: the std::shared_ptr control block + allocate_shared own the OBJECT
//     lifetime; the pool only owns RAW byte buffers it allocates with the over-aligned
//     ::operator new and frees with the matching over-aligned ::operator delete at pool
//     destruction (a clean, bounded set, tracked once in all_blocks_ — no leak, no double
//     free, freed when the sink/scheduler dies).
//
// SIZE-KEYED INTRUSIVE FREE LISTS. allocate_shared fuses the SharedState<T> object and
// the shared_ptr control block into ONE allocation whose size depends on T. We keep one
// free list PER distinct allocation size (the set of T is tiny and fixed). A request for
// size N pops a free block of size N if one exists, else allocates a fresh OVER-ALIGNED
// block of size N. On deallocate(N, p) the block — now unused storage — is threaded onto
// size N's free list INTRUSIVELY (a `next` pointer written into the block itself), so
// deallocate is allocation-free and TRULY noexcept (a shared_ptr deleter must not throw).
// Blocks are never shrunk back to the OS until the pool is destroyed (each is owned once
// by all_blocks_ and freed exactly once there), so steady-state churn is zero heap
// traffic and there is no double free.
//
// SINGLE-THREADED (L6). The sink owns exactly one pool and is driven by one thread
// (the sim Scheduler thread, or the prod reactor thread). No atomics, no locks — the
// free lists are plain vectors mutated only on that thread. This mirrors the rest of
// the async core, which is single-thread by contract.
//
// DETERMINISM. A correct memory pool is INVISIBLE to output: it hands back a block of
// the right size; the SharedState constructed in it has the same value layout and the
// same `sink_`/`waiter_`/`result_` semantics regardless of the block's address. The
// scheduler's ordering keys are arm/enqueue SEQUENCE numbers, never addresses (L3),
// so reusing an address changes nothing the trace renders. Verified byte-identical.

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace lockstep::core::detail {

// A per-sink free-list pool of raw, size-keyed storage blocks. Owns the blocks it
// allocates and frees ALL of them at destruction (with the sink/scheduler). Single
// threaded; no synchronization.
class SharedStatePool {
public:
    SharedStatePool() = default;

    SharedStatePool(const SharedStatePool&) = delete;
    SharedStatePool& operator=(const SharedStatePool&) = delete;
    SharedStatePool(SharedStatePool&&) = delete;
    SharedStatePool& operator=(SharedStatePool&&) = delete;

    // Free EVERY block this pool ever handed out, recycled or in-flight. By the time a
    // sink is destroyed the async run has quiesced (no live Promise/Future/Awaiter), so
    // every block has been returned to its free list; we own and free them ALL here from
    // all_blocks_ (the canonical ownership list — each block appears EXACTLY ONCE),
    // pairing the over-aligned operator delete with the over-aligned operator new in
    // acquire(). Freeing from all_blocks_ (not the free lists) means a block is deleted
    // exactly once whether or not it was recycled — no double free. A block still "in
    // flight" at pool death would be a caller bug (a live shared_ptr outliving its
    // scheduler); the standard run drains to quiescence first.
    ~SharedStatePool() {
        for (void* blk : all_blocks_) {
            ::operator delete(blk, kAlign);
        }
    }

    // Acquire a block of exactly `n` bytes: pop a recycled one of size `n` if present,
    // else allocate a fresh OVER-ALIGNED one (tracked once in all_blocks_ for clean
    // destruction). The hot path after warmup is an intrusive-list pop — no heap traffic.
    //
    // ALIGNMENT: every block is allocated over-aligned to kAlign (the max fundamental
    // alignment, __STDCPP_DEFAULT_NEW_ALIGNMENT__). So reusing a size-N block for ANY
    // type U whose fused node is N bytes is alignment-safe regardless of U's own
    // alignment requirement — the block is at least as aligned as any non-over-aligned
    // type needs. (allocate_shared never requests over-aligned-beyond-default node types
    // here; SharedState<T> holds only scalars/optional/Error, all default-aligned.)
    [[nodiscard]] void* acquire(std::size_t n) {
        Bucket& b = bucket_for(n);
        if (b.free_head != nullptr) {
            FreeNode* node = b.free_head;
            b.free_head = node->next;
            return static_cast<void*>(node);
        }
        void* p = ::operator new(n, kAlign);
        all_blocks_.push_back(p);
        return p;
    }

    // Return a block of size `n` to its free list for reuse — TRULY noexcept and
    // ALLOCATION-FREE: the freed block is unused storage, so we thread the free list
    // INTRUSIVELY through it (write a FreeNode{next} into the block's first bytes). No
    // vector push, no bad_alloc path — a shared_ptr deleter must never throw, and this
    // one cannot. The block stays owned by all_blocks_ (freed exactly once at ~pool).
    // `n` is guaranteed >= sizeof(FreeNode) because every SharedState<T> fused node is
    // far larger than a single pointer (it holds the result + waiter + sink + refcounts).
    void release(std::size_t n, void* p) noexcept {
        Bucket& b = bucket_for_nothrow(n);
        if (&b == &null_bucket_) {
            // bucket_for_nothrow could not find/make the bucket (it never allocates, so a
            // genuinely-unknown size with the bucket vector full is the only path). This
            // cannot happen for a block that was acquire()d (its bucket already exists),
            // but to keep release() total + noexcept we simply leave the block in
            // all_blocks_ to be freed at ~pool — correct, just not recycled.
            return;
        }
        auto* node = static_cast<FreeNode*>(p);
        node->next = b.free_head;
        b.free_head = node;
    }

private:
    // The max fundamental alignment; every pooled block is allocated at least this
    // aligned so a size-keyed block is reusable by any same-size default-aligned type.
    static constexpr std::align_val_t kAlign =
        static_cast<std::align_val_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);

    // Intrusive free-list node written INTO a freed block's storage (the block is unused
    // while free, so this is safe; it is overwritten by the next acquire()'s placement).
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    // One free list per distinct allocation size, as an intrusive LIFO (head pointer).
    // The set of sizes == the set of SharedState<T> the system instantiates: tiny and
    // fixed, so a linear-scanned vector of buckets is deterministic and fast.
    struct Bucket {
        std::size_t size = 0;
        FreeNode* free_head = nullptr;
    };

    // Find or CREATE the bucket for size `n` (may push_back; used only by acquire, which
    // is allowed to throw bad_alloc like any allocator's allocate()).
    Bucket& bucket_for(std::size_t n) {
        for (Bucket& b : buckets_) {
            if (b.size == n) {
                return b;
            }
        }
        buckets_.push_back(Bucket{n, nullptr});
        return buckets_.back();
    }

    // Find the bucket for size `n` WITHOUT ever allocating (noexcept). Returns a sentinel
    // null_bucket_ if absent. Every released block was acquire()d first, so its bucket
    // already exists and this finds it; the sentinel branch is unreachable in practice.
    Bucket& bucket_for_nothrow(std::size_t n) noexcept {
        for (Bucket& b : buckets_) {
            if (b.size == n) {
                return b;
            }
        }
        return null_bucket_;
    }

    std::vector<Bucket> buckets_{};      // size -> intrusive free-list head
    std::vector<void*> all_blocks_{};    // every block ever allocated; freed once at ~pool
    Bucket null_bucket_{};               // sentinel for the unreachable noexcept-miss path
};

// A stateful Allocator usable with std::allocate_shared. It forwards the single fused
// allocate_shared allocation (control block + SharedState<T>) to the pool, keyed by the
// allocation's byte size. Stateless rebind requirement is met: it carries only a pool
// pointer, comparable by identity, and rebinds to any U (allocate_shared rebinds the
// caller's allocator to its internal fused node type).
template <class T>
class PoolAllocator {
public:
    using value_type = T;

    explicit PoolAllocator(SharedStatePool* pool) noexcept : pool_(pool) {}

    // Rebinding copy ctor (allocate_shared rebinds to its fused-node type).
    template <class U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_(other.pool()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        // allocate_shared calls this once with count==1 for the fused node; honor a
        // general count for correctness. Size in bytes is what the pool keys on.
        const std::size_t bytes = count * sizeof(T);
        return static_cast<T*>(pool_->acquire(bytes));
    }

    void deallocate(T* p, std::size_t count) noexcept {
        const std::size_t bytes = count * sizeof(T);
        pool_->release(bytes, static_cast<void*>(p));
    }

    [[nodiscard]] SharedStatePool* pool() const noexcept { return pool_; }

    template <class U>
    bool operator==(const PoolAllocator<U>& o) const noexcept { return pool_ == o.pool(); }
    template <class U>
    bool operator!=(const PoolAllocator<U>& o) const noexcept { return pool_ != o.pool(); }

private:
    SharedStatePool* pool_ = nullptr;
};

}  // namespace lockstep::core::detail
