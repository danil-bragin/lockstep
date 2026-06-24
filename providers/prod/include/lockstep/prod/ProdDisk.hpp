#pragma once

// ProdDisk.hpp — Phase 7 S3. The PRODUCTION IDisk provider: a real, file-backed
// append-structured device over POSIX file IO. It is the HONEST counterpart to
// sim::SimDisk — same frozen core::IDisk contract, but a real platter underneath.
//
// V-PROD-CONTRACT: ProdDisk passes the SAME universal IDisk conformance suite
// (tests/provider_conformance/ContractConformance.hpp) as SimDisk. It is an
// HONEST device: NO torn writes, NO lying fsync, NO injected latency. Those are
// sim-only verification AIDS (tier B) that model worst-case real hardware; a real
// honest device does not manufacture them. ProdDisk's job is to be the device the
// sim WORST-CASE was modelling — and to honor the durability BARRIER the WAL +
// recovery rely on: data appended-then-synced survives a crash byte-identical;
// data appended-but-not-synced MAY be lost (the un-synced tail); sync() returns
// ok ONLY after the bytes are truly durable (NO lying fsync).
//
// ----------------------------------------------------------------------------
// IO MODEL: SYNCHRONOUS + INLINE (async/io_uring is a deferred PERF optimization)
// ----------------------------------------------------------------------------
// Every op does its real syscall (pwrite/pread/fdatasync) SYNCHRONOUSLY and then
// returns an ALREADY-READY Future<Error> (Promise::set_value before the caller
// even awaits → Future::await_ready() is true → the awaiting coroutine resumes
// without ever suspending). This is correct and simplest: a blocking pwrite that
// has returned has placed the bytes in the page cache; fdatasync that has returned
// has made them durable. The Future is a thin sync→async adaptor so ProdDisk slots
// into the same co_await call sites as SimDisk with ZERO core change.
//   PERF NOTE: an io_uring / async-completion ProdDisk (overlap IO with compute,
//   submit-then-poll) is an explicit Phase-7 PERF deferral — CORRECTNESS FIRST.
//   It would change the Future from ready-on-return to completed-by-the-reactor;
//   the IDisk contract is identical, so it is a drop-in later. Not now.
//
// ----------------------------------------------------------------------------
// ONE IDisk == ONE FILE (matches IDisk.hpp: "one append-structured object")
// ----------------------------------------------------------------------------
// A ProdDisk owns one file descriptor for one append-structured file. Higher
// layers compose many (a WAL segment, an SSTable backing) — exactly the sim model.
// The fd is RAII-closed in the destructor; no fd leaks (matters under ASan/LSan).
//
// ----------------------------------------------------------------------------
// DURABILITY: fdatasync + a ONE-TIME directory fsync
// ----------------------------------------------------------------------------
// sync() calls fdatasync(fd) to flush the file's DATA (we do not depend on the
// inode mtime, so fdatasync — not full fsync — is the right, cheaper barrier).
// On the FIRST sync of a FRESHLY-CREATED file we ALSO fsync the containing
// DIRECTORY once, so the file's *existence* (its dirent) is itself durable — a
// classic real-world durability bug is a synced file whose directory entry was
// never flushed, so the file vanishes after a crash. After that one dir-fsync the
// dirent is durable and we never repeat it.
//
// ----------------------------------------------------------------------------
// providers/prod/ is the lint-exempt boundary zone: raw POSIX file IO
// (open/pwrite/pread/fdatasync/fsync/close) is permitted HERE and ONLY here.
// ----------------------------------------------------------------------------

#include <fcntl.h>    // open, O_* — ALLOWED only under providers/ (rule 1)
#include <unistd.h>   // pwrite, pread, fdatasync, fsync, close, ftruncate
#include <sys/stat.h> // mode constants

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>

namespace lockstep::prod {

// macOS has no fdatasync; fsync is the durable barrier there. fdatasync skips the
// inode-metadata flush (cheaper) where it exists; both make the DATA durable.
[[nodiscard]] inline int prod_fdatasync(int fd) noexcept {
#if defined(__APPLE__)
    // F_FULLFSYNC asks the drive to flush its write cache to the platter — the
    // honest barrier on macOS (plain fsync may leave bytes in the drive cache).
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return 0;
    }
    return ::fsync(fd); // fall back if the device rejects F_FULLFSYNC
#else
    return ::fdatasync(fd);
#endif
}

// ---------------------------------------------------------------------------
// ProdDisk — one append-structured file. HONEST device. Synchronous inline IO.
// ---------------------------------------------------------------------------
class ProdDisk final : public core::IDisk {
public:
    // Open (creating if absent) `path` for read+write append-structured use, and
    // adopt its current size as the logical end. `sched` mints the inline-ready
    // Futures (a ProdDisk is still driven by awaiting its Futures on a Scheduler,
    // exactly like SimDisk — the scheduler is the harness, not a fault source).
    // `dir_fd` (optional, >=0) is an OPEN descriptor on the containing directory;
    // when provided AND this open created the file, the first sync() fsyncs it so
    // the new dirent is durable. Pass -1 to skip the dir-fsync (e.g. reopen).
    ProdDisk(core::Scheduler& sched, const std::string& path, int dir_fd = -1) noexcept
        : sched_(&sched), dir_fd_(dir_fd) {
        // O_CREAT|O_RDWR: append-structured but we pwrite at an explicit offset,
        // so we do NOT use O_APPEND (which would force every write to EOF and
        // defeat deterministic offset placement). 0644 perms.
        const bool existed = (::access(path.c_str(), F_OK) == 0);
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            open_errno_ = errno;
            return;
        }
        // First sync of a freshly-CREATED file should durably record its dirent.
        created_ = !existed;
        const off_t end = ::lseek(fd_, 0, SEEK_END);
        len_ = (end < 0) ? 0 : static_cast<std::uint64_t>(end);
    }

    ProdDisk(const ProdDisk&) = delete;
    ProdDisk& operator=(const ProdDisk&) = delete;
    ProdDisk(ProdDisk&&) = delete;
    ProdDisk& operator=(ProdDisk&&) = delete;

    // RAII: close the fd (and the dir fd if we own it). No fd leaks under LSan.
    ~ProdDisk() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // True if the underlying open() failed (the device is unusable; every op
    // reports IoFault). Lets a caller surface a clear construction failure.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int open_errno() const noexcept { return open_errno_; }

    // The logical end-of-device (durable + buffered bytes). Append-structured, so
    // the next append lands exactly here. Introspection only.
    [[nodiscard]] std::uint64_t logical_len() const noexcept { return len_; }

    // ---- S8.5 PROFILING counters (introspection only; off the durability path) --
    // Single-threaded reactor owns this disk, so plain (non-atomic) counters are
    // safe. They let the daemon report fdatasync count + total fdatasync wall-time
    // so a profiler can answer "is the commit path fsync-bound?" (fsyncs per
    // committed op, fsync latency) WITHOUT touching the durable byte stream.
    [[nodiscard]] std::uint64_t append_calls() const noexcept { return append_calls_; }
    [[nodiscard]] std::uint64_t sync_calls() const noexcept { return sync_calls_; }
    [[nodiscard]] std::uint64_t sync_total_ns() const noexcept { return sync_total_ns_; }
    [[nodiscard]] std::uint64_t bytes_appended() const noexcept { return bytes_appended_; }

    // ---- core::IDisk -----------------------------------------------------

    // Append `data` at the current logical end. The placement offset is written to
    // out_offset BEFORE completion (per the IDisk contract). Real pwrite at the
    // end offset; on success the logical length grows. NOT durable until sync().
    [[nodiscard]] core::Future<core::Error>
    append(std::span<const std::byte> data, core::Offset& out_offset) override {
        const core::Offset off = len_;
        out_offset = off;

        core::Error result{};
        if (fd_ < 0) {
            result = core::Error{core::ErrorCode::IoFault, "prod disk not open"};
        } else {
            // pwrite the whole span at the end offset, looping on short writes.
            std::size_t written = 0;
            const auto* base = reinterpret_cast<const unsigned char*>(data.data());
            while (written < data.size()) {
                const ssize_t n =
                    ::pwrite(fd_, base + written, data.size() - written,
                             static_cast<off_t>(off + written));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue; // retry an interrupted write
                    }
                    result = core::Error{core::ErrorCode::IoFault, "prod pwrite failed"};
                    break;
                }
                if (n == 0) {
                    result = core::Error{core::ErrorCode::IoFault, "prod pwrite wrote 0"};
                    break;
                }
                written += static_cast<std::size_t>(n);
            }
            if (result.ok()) {
                len_ += data.size();
                ++append_calls_;
                bytes_appended_ += data.size();
            }
        }
        return ready(result);
    }

    // Read up to into.size() bytes starting at `at`. A read whose window runs past
    // the logical end is a documented short read → NotFound (matches SimDisk, so
    // the universal `disk/read-past-end-notfound` check asserts ONE behavior for
    // both impls). On an in-range request, pread fills `into` exactly.
    [[nodiscard]] core::Future<core::Error>
    read(core::Offset at, std::span<std::byte> into) override {
        core::Error result{};
        if (fd_ < 0) {
            result = core::Error{core::ErrorCode::IoFault, "prod disk not open"};
            return ready(result);
        }
        const std::uint64_t want = into.size();
        // Same end-of-device predicate as SimDisk::fill_read: any part of the
        // window past the written end is NotFound (no partial fill — the contract
        // is "fills `into` or reports the short read").
        if (at > len_ || at + want > len_) {
            return ready(core::Error{core::ErrorCode::NotFound,
                                     "prod read past end-of-device"});
        }
        std::size_t got = 0;
        auto* base = reinterpret_cast<unsigned char*>(into.data());
        while (got < want) {
            const ssize_t n = ::pread(fd_, base + got, want - got,
                                      static_cast<off_t>(at + got));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                result = core::Error{core::ErrorCode::IoFault, "prod pread failed"};
                break;
            }
            if (n == 0) {
                // Unexpected EOF inside the (bounds-checked) window: treat as the
                // documented short read.
                result = core::Error{core::ErrorCode::NotFound, "prod read short at end"};
                break;
            }
            got += static_cast<std::size_t>(n);
        }
        return ready(result);
    }

    // Durability barrier. HONEST: returns ok ONLY after fdatasync makes the data
    // truly durable (no lying fsync). On the FIRST sync of a freshly-created file
    // it ALSO fsyncs the containing directory once so the new dirent is durable.
    [[nodiscard]] core::Future<core::Error> sync() override {
        if (fd_ < 0) {
            return ready(core::Error{core::ErrorCode::IoFault, "prod disk not open"});
        }
        // Time the real fdatasync (the durability barrier). PROFILING ONLY: a
        // monotonic-clock delta around the syscall, summed; never feeds any
        // deterministic ordering (this is the prod provider — no sim determinism).
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = prod_fdatasync(fd_);
        const auto t1 = std::chrono::steady_clock::now();
        ++sync_calls_;
        sync_total_ns_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (rc != 0) {
            return ready(core::Error{core::ErrorCode::IoFault, "prod fdatasync failed"});
        }
        // First durable sync of a NEW file: make its directory entry durable too.
        if (created_ && !dir_synced_ && dir_fd_ >= 0) {
            if (::fsync(dir_fd_) != 0) {
                return ready(core::Error{core::ErrorCode::IoFault,
                                         "prod dir fsync failed"});
            }
        }
        // Once the data is durable, the un-synced tail no longer exists: every
        // appended byte to this point is now on the platter.
        dir_synced_ = true;
        return ready(core::Error{});
    }

private:
    // Mint an ALREADY-READY Future<Error> carrying `e`. The promise is set before
    // the caller awaits, so await_ready() is true and the awaiter resumes inline —
    // the synchronous IO is surfaced through the async interface with no suspend.
    [[nodiscard]] core::Future<core::Error> ready(core::Error e) {
        core::Promise<core::Error> p = core::make_promise<core::Error>(sched_);
        core::Future<core::Error> f = p.get_future();
        p.set_value(e);
        return f;
    }

    core::Scheduler* sched_;
    int fd_ = -1;
    int dir_fd_ = -1;       // borrowed (not owned/closed here); -1 = none
    int open_errno_ = 0;    // errno captured if open() failed
    bool created_ = false;  // this open created the file (dir-fsync candidate)
    bool dir_synced_ = false; // the one-time directory fsync has happened
    std::uint64_t len_ = 0; // logical end-of-device (append placement offset)
    // S8.5 profiling counters (introspection only; not durability state).
    std::uint64_t append_calls_ = 0;
    std::uint64_t sync_calls_ = 0;
    std::uint64_t sync_total_ns_ = 0;
    std::uint64_t bytes_appended_ = 0;
};

} // namespace lockstep::prod
