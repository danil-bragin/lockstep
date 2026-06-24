#pragma once

// ProdUring.hpp — Phase 9 S9.2. A minimal, RAII io_uring ring for the PROD IO path,
// built directly on the RAW io_uring syscalls (io_uring_setup / io_uring_enter) — NO
// liburing dependency (the dev image ships the kernel header linux/io_uring.h but not
// liburing; raw syscalls keep the dependency surface zero for a single-purpose ring).
//
// ----------------------------------------------------------------------------
// WHAT THIS RING IS FOR (and what it deliberately is NOT)
// ----------------------------------------------------------------------------
// S9.2 PROFILE FINDING (strace -c -w, Release, container, pbench commit load): at the
// steady commit-throughput ceiling (in-flight depth 64) the IO-WORK syscall fraction
// is ~1% of the busy reactor time — fdatasync is already coalesced ~68:1 by the FIFO
// persist worker (S8.5: fsyncs_per_commit=0.016), and send/recv/epoll_ctl are each a
// fraction of a percent. The dominant per-op cost is single-reactor coroutine-frame
// CPU, which io_uring CANNOT touch. So this ring targets the ONE place io_uring buys a
// real, durability-correct overlap WITHOUT the classic buffer-lifetime hazard:
//
//   ASYNC fdatasync (IORING_OP_FSYNC | IORING_FSYNC_DATASYNC).
//
// The durability BARRIER moves to the CQE: an entry is acked/committed ONLY after its
// fsync COMPLETION (the CQE) is reaped — never before. fsync submits NO user buffer
// (just the fd), so there is NO pointer handed to the kernel that could be invalidated
// by a growable container (V-RKV1 is satisfied vacuously for fsync). pwrite/recv/send
// are intentionally LEFT on the synchronous/epoll path: making THEM async would hand
// the kernel a pointer into a churning buffer (the classic io_uring use-after-free) for
// a sub-1% syscall-time win — not worth the lifetime risk per the profile.
//
// ----------------------------------------------------------------------------
// REACTOR INTEGRATION (alongside epoll — NOT a replacement)
// ----------------------------------------------------------------------------
// The ring's fd is itself EPOLL-pollable: when a CQE is ready the ring fd becomes
// EPOLLIN-readable. So the reactor registers the ring fd on its EXISTING epoll set and,
// on EPOLLIN, calls reap() to harvest completions and resolve the parked promises (each
// resolution SCHEDULES the waiting coroutine via the SchedulerSink — L1, never an inline
// resume). The network path stays on epoll untouched; only disk fdatasync flows through
// the ring. This is the minimal, cleanest integration: epoll keeps doing what it already
// does well; the ring adds async-fsync overlap as one more additive fd branch.
//
// ----------------------------------------------------------------------------
// SINGLE-THREAD-PER-SHARD. One ProdUring is owned by one ProdReactor (one shard's one
// thread). No cross-thread ring sharing; no atomics needed for the user-side state.
// The kernel updates the CQ head/tail with release/acquire ordering, which we honor with
// the documented io_uring memory barriers on the shared ring indices.
//
// LINUX-ONLY (io_uring is Linux). Guarded by __linux__; the macOS host build never sees
// it. providers/prod/ is the lint-exempt boundary zone where these syscalls are allowed.
// If io_uring_setup fails (e.g. Docker's default seccomp BLOCKS it — run with
// --security-opt seccomp=unconfined), valid() is false and every caller transparently
// FALLS BACK to the synchronous path: correctness never depends on the ring existing.

#ifdef __linux__

#include <sys/mman.h>     // mmap, munmap — ALLOWED only under providers/
#include <sys/syscall.h>  // syscall, __NR_io_uring_*
#include <unistd.h>       // close, syscall
#include <linux/io_uring.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lockstep::prod {

// Raw syscall wrappers (no liburing). The numbers are the stable Linux ABI; the headers
// also provide __NR_io_uring_* on this kernel, but we define fallbacks for safety.
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif

inline int prod_io_uring_setup(unsigned entries, io_uring_params* p) noexcept {
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}
inline int prod_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                               unsigned flags) noexcept {
    return static_cast<int>(
        ::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                  static_cast<void*>(nullptr), 0));
}

// ---------------------------------------------------------------------------
// ProdUring — one ring, RAII. Setup mmaps the SQ ring, CQ ring, and SQE array; the
// destructor munmaps them and closes the ring fd (no leak under ASan/LSan). All ring
// state lives in this object; a moved-from / failed ring is simply !valid().
// ---------------------------------------------------------------------------
class ProdUring {
public:
    // user_data sentinel layout: we pack a monotonically-increasing op id into the SQE
    // user_data; reap() hands it back so the reactor can match a CQE to a parked promise.
    using OpId = std::uint64_t;

    // Construct + set up a ring of `entries` SQEs (rounded up to a power of two by the
    // kernel). On failure (ENOSYS / EPERM under default seccomp) valid() stays false and
    // the owner falls back to synchronous IO — NO throw, NO abort.
    explicit ProdUring(unsigned entries = 256) noexcept { setup(entries); }

    ProdUring(const ProdUring&) = delete;
    ProdUring& operator=(const ProdUring&) = delete;
    ProdUring(ProdUring&&) = delete;
    ProdUring& operator=(ProdUring&&) = delete;

    ~ProdUring() { teardown(); }

    [[nodiscard]] bool valid() const noexcept { return ring_fd_ >= 0; }
    [[nodiscard]] int ring_fd() const noexcept { return ring_fd_; }

    // Submit an async fdatasync of `fd`, tagged with `user_data`. Returns false if the
    // ring is full this turn (the caller then falls back to a synchronous fdatasync for
    // THIS op — never blocks, never drops durability). On success the completion arrives
    // later as a CQE carrying `user_data`; the fsync's CQE IS the durability barrier.
    [[nodiscard]] bool submit_fdatasync(int fd, OpId user_data) noexcept {
        if (ring_fd_ < 0) {
            return false;
        }
        const unsigned tail = *sq_tail_;
        const unsigned head = load_acquire(sq_head_);
        if ((tail - head) >= sq_ring_entries_) {
            return false; // SQ full this turn — caller does a sync fdatasync instead.
        }
        const unsigned index = tail & sq_ring_mask_;
        io_uring_sqe* sqe = &sqes_[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_FSYNC;
        sqe->fd = fd;
        sqe->fsync_flags = IORING_FSYNC_DATASYNC; // fdatasync, not full fsync
        sqe->user_data = user_data;
        // No-SQARRAY kernels still expose the array; we populate it for portability.
        sq_array_[index] = index;
        store_release(sq_tail_, tail + 1);
        // Enter the kernel to submit (no SQPOLL thread; we submit explicitly). We do NOT
        // wait here (min_complete=0): submission must not block the reactor.
        const int rc = prod_io_uring_enter(ring_fd_, 1, 0, 0);
        if (rc < 0) {
            // Roll the tail back: this SQE was not accepted. Caller falls back to sync.
            store_release(sq_tail_, tail);
            return false;
        }
        ++inflight_;
        return true;
    }

    // Number of submitted ops whose CQE has not yet been reaped (introspection / the
    // reactor uses it to know whether the ring needs polling).
    [[nodiscard]] std::uint64_t inflight() const noexcept { return inflight_; }

    // BLOCK in the kernel until at least `min_complete` CQEs are available (graceful
    // shutdown flush). Submits nothing (to_submit=0) and waits via GETEVENTS. The caller
    // then reap()s. A no-op on an unusable ring. EINTR is benign (the caller loops).
    void wait_completions(unsigned min_complete) noexcept {
        if (ring_fd_ < 0 || inflight_ == 0) {
            return;
        }
        prod_io_uring_enter(ring_fd_, 0, min_complete, IORING_ENTER_GETEVENTS);
    }

    // Reap up to `max` ready completions WITHOUT blocking. For each, invoke
    // `on_complete(user_data, res)` where res>=0 is success (fsync done == durable) and
    // res<0 is -errno (fsync FAILED — the caller MUST surface IoFault, never fake
    // durability). Returns the number of completions reaped. The reactor calls this on
    // the ring fd's EPOLLIN (a CQE is ready) and also opportunistically each turn.
    template <class F>
    std::uint64_t reap(F&& on_complete, unsigned max = 256) noexcept {
        if (ring_fd_ < 0) {
            return 0;
        }
        std::uint64_t reaped = 0;
        unsigned head = *cq_head_;
        const unsigned tail = load_acquire(cq_tail_);
        while (head != tail && reaped < max) {
            const unsigned index = head & cq_ring_mask_;
            const io_uring_cqe* cqe = &cqes_[index];
            const OpId user_data = cqe->user_data;
            const std::int32_t res = cqe->res;
            ++head;
            ++reaped;
            on_complete(user_data, res);
        }
        if (reaped > 0) {
            store_release(cq_head_, head);
            inflight_ -= reaped;
        }
        return reaped;
    }

private:
    void setup(unsigned entries) noexcept {
        io_uring_params params;
        std::memset(&params, 0, sizeof(params));
        const int fd = prod_io_uring_setup(entries, &params);
        if (fd < 0) {
            ring_fd_ = -1; // unusable — owner falls back to synchronous IO.
            return;
        }
        ring_fd_ = fd;

        // SQ + CQ rings are mmapped from the ring fd. On modern kernels (FEAT_SINGLE_MMAP)
        // SQ and CQ share one mapping; we map them separately for simplicity/portability
        // (the kernel reports the byte offsets in params).
        const std::size_t sq_sz =
            params.sq_off.array + params.sq_entries * sizeof(unsigned);
        const std::size_t cq_sz =
            params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

        sq_ring_ = ::mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
        cq_ring_ = ::mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
        const std::size_t sqes_sz = params.sq_entries * sizeof(io_uring_sqe);
        sqes_ = static_cast<io_uring_sqe*>(
            ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES));

        if (sq_ring_ == MAP_FAILED || cq_ring_ == MAP_FAILED || sqes_ == MAP_FAILED) {
            teardown();
            ring_fd_ = -1;
            return;
        }
        sq_sz_ = sq_sz;
        cq_sz_ = cq_sz;
        sqes_sz_ = sqes_sz;

        auto* sqb = static_cast<unsigned char*>(sq_ring_);
        auto* cqb = static_cast<unsigned char*>(cq_ring_);
        sq_head_ = reinterpret_cast<unsigned*>(sqb + params.sq_off.head);
        sq_tail_ = reinterpret_cast<unsigned*>(sqb + params.sq_off.tail);
        sq_ring_mask_ = *reinterpret_cast<unsigned*>(sqb + params.sq_off.ring_mask);
        sq_ring_entries_ = *reinterpret_cast<unsigned*>(sqb + params.sq_off.ring_entries);
        sq_array_ = reinterpret_cast<unsigned*>(sqb + params.sq_off.array);

        cq_head_ = reinterpret_cast<unsigned*>(cqb + params.cq_off.head);
        cq_tail_ = reinterpret_cast<unsigned*>(cqb + params.cq_off.tail);
        cq_ring_mask_ = *reinterpret_cast<unsigned*>(cqb + params.cq_off.ring_mask);
        cqes_ = reinterpret_cast<io_uring_cqe*>(cqb + params.cq_off.cqes);
    }

    void teardown() noexcept {
        if (sqes_ != nullptr && sqes_ != MAP_FAILED) {
            ::munmap(sqes_, sqes_sz_);
            sqes_ = nullptr;
        }
        if (cq_ring_ != nullptr && cq_ring_ != MAP_FAILED) {
            ::munmap(cq_ring_, cq_sz_);
            cq_ring_ = nullptr;
        }
        if (sq_ring_ != nullptr && sq_ring_ != MAP_FAILED) {
            ::munmap(sq_ring_, sq_sz_);
            sq_ring_ = nullptr;
        }
        if (ring_fd_ >= 0) {
            ::close(ring_fd_);
            ring_fd_ = -1;
        }
    }

    // io_uring shared-index memory ordering: the kernel publishes the CQ tail and reads
    // the SQ tail with release/acquire semantics; we mirror that on the user side so a
    // CQE we read was fully written, and an SQE we publish is visible before we enter().
    static unsigned load_acquire(const unsigned* p) noexcept {
        return std::atomic_ref<const unsigned>(*p).load(std::memory_order_acquire);
    }
    static void store_release(unsigned* p, unsigned v) noexcept {
        std::atomic_ref<unsigned>(*p).store(v, std::memory_order_release);
    }

    int ring_fd_ = -1;
    void* sq_ring_ = nullptr;
    void* cq_ring_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;
    std::size_t sq_sz_ = 0;
    std::size_t cq_sz_ = 0;
    std::size_t sqes_sz_ = 0;

    unsigned* sq_head_ = nullptr;
    unsigned* sq_tail_ = nullptr;
    unsigned sq_ring_mask_ = 0;
    unsigned sq_ring_entries_ = 0;
    unsigned* sq_array_ = nullptr;

    unsigned* cq_head_ = nullptr;
    unsigned* cq_tail_ = nullptr;
    unsigned cq_ring_mask_ = 0;
    io_uring_cqe* cqes_ = nullptr;

    std::uint64_t inflight_ = 0;
};

} // namespace lockstep::prod

#endif // __linux__
