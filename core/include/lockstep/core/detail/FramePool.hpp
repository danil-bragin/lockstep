#pragma once

// FramePool.hpp — coroutine-FRAME recycling for Task::promise_type. S8.7 pooled the
// Future SharedState and localized the residual per-op cost to the COROUTINE FRAMES the
// compiler heap-allocates (one ::operator new + ::operator delete per Task, ~hundreds of
// bytes each). A coroutine's frame allocation IS a customization point: if the promise
// type defines `operator new`/`operator delete`, the compiler routes the frame alloc
// through them. This pool is that route — a size-keyed intrusive free list that recycles
// frame storage instead of round-tripping the OS allocator per spawn.
//
// WHY THREAD_LOCAL (not per-sink like SharedStatePool): a coroutine frame is allocated
// BEFORE its promise is constructed, so promise::operator new is STATIC and has no sink/
// scheduler to reach. The pool is therefore a thread_local singleton:
//   * SIM is single-threaded (L6) → exactly ONE pool, used in deterministic spawn/destroy
//     order → addresses recycle deterministically. A memory pool is INVISIBLE to output:
//     the scheduler orders work by enqueue/arm SEQUENCE numbers, never by frame/handle
//     address (verified — no coroutine_handle is stored in an address-keyed container or
//     compared by address), so reusing an address changes nothing a trace renders. The
//     sim fingerprint is BYTE-IDENTICAL before vs after (the same no-regression proof the
//     SharedState pool used).
//   * PROD is thread-per-shard (threads live only in providers/prod); each thread gets its
//     OWN thread_local pool → no shared state, no atomics, no locks, no data race. This
//     keeps the forbidden-list contract (no std::atomic/std::thread in core) intact:
//     thread_local is a storage duration, not a thread/atomic primitive.
//
// SIZE-KEYED, EXACTLY LIKE SharedStatePool: each distinct coroutine type has one fixed
// frame size; the set is small, so a linear-scanned vector of per-size intrusive LIFO
// free lists is deterministic and fast. acquire(n) pops a size-n block or mallocs one;
// release(n,p) threads the freed block onto size n's list (allocation-free, noexcept — a
// frame dealloc must never throw). The sized operator delete the compiler emits hands us
// the frame size back, so we never have to record it.
//
// ASAN BYPASS (the safety boundary): a raw frame free list HIDES a use-after-free or
// heap-overflow on a frame (a recycled block looks live to ASan). Unlike SharedStatePool,
// whose shared_ptr refcount is the lifetime source of truth, this pool owns raw frame
// bytes directly. So under AddressSanitizer we DISABLE the pool and route frames through
// the global allocator, letting ASan instrument every frame alloc/free and catch frame
// UAF. The determinism proof runs in the normal build (pool ON, byte-identical); the
// memory-safety proof runs in the ASan build (pool OFF, global new/delete). Both hold.

#include <cstddef>
#include <new>
#include <vector>

// Detect AddressSanitizer (Clang feature macro or GCC define).
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LOCKSTEP_FRAMEPOOL_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LOCKSTEP_FRAMEPOOL_ASAN)
#  define LOCKSTEP_FRAMEPOOL_ASAN 1
#endif

namespace lockstep::core::detail {

// A thread_local, size-keyed free-list pool of raw coroutine-frame storage. Owns every
// block it allocates and frees them ALL at thread exit (by then the run has quiesced and
// every frame is back on a free list). Single-threaded per instance; no synchronization.
class FramePool {
public:
    FramePool() = default;
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;
    FramePool& operator=(FramePool&&) = delete;

    // Free every block ever handed out (each appears EXACTLY ONCE in all_blocks_, so it is
    // deleted exactly once whether or not it was recycled — no double free). At thread exit
    // the async run has drained, so no frame is live.
    ~FramePool() {
        for (void* blk : all_blocks_) {
            ::operator delete(blk, kAlign);
        }
    }

    // Acquire a frame block of exactly `n` bytes: pop a recycled size-n block, else allocate
    // a fresh OVER-ALIGNED one (over-aligned to the max fundamental alignment so a size-n
    // block is reusable by any coroutine frame of that size). Post-warmup this is a pointer
    // pop — zero heap traffic.
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

    // Return a size-`n` block to its free list — noexcept + allocation-free (thread the
    // FreeNode through the now-unused frame storage). A coroutine frame is far larger than a
    // pointer, so n >= sizeof(FreeNode) always holds. The block stays owned by all_blocks_.
    void release(std::size_t n, void* p) noexcept {
        Bucket& b = bucket_for_nothrow(n);
        if (&b == &null_bucket_) {
            return;  // unreachable for an acquire()d block; leave it for ~FramePool.
        }
        auto* node = static_cast<FreeNode*>(p);
        node->next = b.free_head;
        b.free_head = node;
    }

private:
    static constexpr std::align_val_t kAlign =
        static_cast<std::align_val_t>(__STDCPP_DEFAULT_NEW_ALIGNMENT__);

    struct FreeNode {
        FreeNode* next = nullptr;
    };
    struct Bucket {
        std::size_t size = 0;
        FreeNode* free_head = nullptr;
    };

    Bucket& bucket_for(std::size_t n) {
        for (Bucket& b : buckets_) {
            if (b.size == n) {
                return b;
            }
        }
        buckets_.push_back(Bucket{n, nullptr});
        return buckets_.back();
    }
    Bucket& bucket_for_nothrow(std::size_t n) noexcept {
        for (Bucket& b : buckets_) {
            if (b.size == n) {
                return b;
            }
        }
        return null_bucket_;
    }

    std::vector<Bucket> buckets_{};
    std::vector<void*> all_blocks_{};
    Bucket null_bucket_{};
};

// The thread_local pool accessor. One pool per thread (one in sim; one per shard thread in
// prod). Constructed on first use, destroyed at thread exit.
inline FramePool& frame_pool() noexcept {
    thread_local FramePool pool;
    return pool;
}

// The two entry points the promise's operator new/delete forward to. Under ASan they route
// to the global allocator so the sanitizer instruments frame lifetimes (see header note).
// LOCKSTEP_NO_FRAMEPOOL forces the global allocator (A/B measurement of the pool's effect;
// not used by the shipped build).
[[nodiscard]] inline void* frame_alloc(std::size_t n) {
#if defined(LOCKSTEP_FRAMEPOOL_ASAN) || defined(LOCKSTEP_NO_FRAMEPOOL)
    return ::operator new(n);
#else
    return frame_pool().acquire(n);
#endif
}
inline void frame_free(void* p, std::size_t n) noexcept {
#if defined(LOCKSTEP_FRAMEPOOL_ASAN) || defined(LOCKSTEP_NO_FRAMEPOOL)
    ::operator delete(p, n);
#else
    frame_pool().release(n, p);
#endif
}

}  // namespace lockstep::core::detail
