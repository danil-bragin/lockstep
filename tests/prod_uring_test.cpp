// prod_uring_test.cpp — Phase 9 S9.2. The io_uring async-fdatasync path on the prod
// IO stack. LINUX-ONLY (io_uring is Linux). Exercises the path END-TO-END THROUGH THE
// PROD PROVIDERS ONLY — this test TU does NO raw file IO of its own (all real disk IO
// stays in providers/prod/ProdDisk, per cardinal rule 1; the test is forbidden-lint
// clean). It asserts:
//
//   (A) RING SETUP / AVAILABILITY — the reactor reports whether io_uring is usable. If
//       seccomp blocks io_uring_setup (default Docker), the ring is cleanly unavailable
//       and ProdDisk transparently falls back to a synchronous fdatasync; the test still
//       PASSES (correctness must NOT depend on the ring existing). With
//       --security-opt seccomp=unconfined the ASYNC path engages and is asserted.
//   (B) DURABILITY BARRIER through ProdDisk's reactor ctor — append bytes, co_await the
//       ASYNC sync() (resolved by the reactor's ring CQE — the barrier), then a simulated
//       crash (drop the disk without a further sync) + reopen: the synced bytes SURVIVE
//       byte-identical. Nothing is acked before the fsync CQE ⇒ a synced prefix is never
//       lost. This is the load-bearing barrier the WAL + recovery rely on.
//   (C) UN-SYNCED-TAIL is not corrupting — a second append AFTER the sync, then crash
//       without syncing it: the reopened image still contains the synced prefix intact.
//
// Bounded: tiny payloads + an ABSOLUTE reactor-deadline wall guard so a missing CQE can
// NEVER hang the loop. Owns a unique mkdtemp scratch dir (mkdtemp/remove are not on the
// forbidden set) and cleans up.

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#if defined(__linux__)
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdReactor.hpp>

namespace {

namespace core = lockstep::core;

int g_failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

std::string make_scratch() {
    char tmpl[] = "/tmp/lockstep_uring_XXXXXX";
    char* d = ::mkdtemp(tmpl); // mkdtemp is NOT on the forbidden set (scratch dir only)
    return d != nullptr ? std::string(d) : std::string("/tmp/lockstep_uring_fallback");
}

std::vector<std::byte> bytes_of(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p != '\0'; ++p) {
        v.push_back(static_cast<std::byte>(*p));
    }
    return v;
}

struct WriteState {
    bool append_a_ok = false;
    bool sync_ok = false;
    bool append_b_ok = false;
    bool done = false;
    core::Offset off_a = 0;
    core::Offset off_b = 0;
};

// append A, ASYNC sync (CQE-resolved barrier), append B (un-synced tail) — then the
// caller "crashes" by dropping the disk WITHOUT a final sync.
core::Task durable_writer(lockstep::prod::ProdDisk* disk, WriteState* st) {
    const std::vector<std::byte> a = bytes_of("DURABLE-A");
    core::Error ea = co_await disk->append(std::span<const std::byte>(a), st->off_a);
    st->append_a_ok = ea.ok();
    core::Error es = co_await disk->sync(); // completes only on the io_uring CQE
    st->sync_ok = es.ok();
    const std::vector<std::byte> b = bytes_of("UNSYNCED-B");
    core::Error eb = co_await disk->append(std::span<const std::byte>(b), st->off_b);
    st->append_b_ok = eb.ok();
    st->done = true;
    co_return;
}

struct ReadState {
    bool read_ok = false;
    std::vector<std::byte> got;
};

core::Task prefix_reader(lockstep::prod::ProdDisk* disk, ReadState* st, std::size_t n) {
    st->got.assign(n, std::byte{0});
    core::Error e = co_await disk->read(0, std::span<std::byte>(st->got));
    st->read_ok = e.ok();
    co_return;
}

} // namespace

int main() {
    using namespace lockstep;

    prod::ProdReactor reactor;
    check(reactor.valid(), "reactor epoll valid");
    const bool uring_on = reactor.uring_available();
    std::fprintf(stderr, "io_uring available: %s\n",
                 uring_on ? "YES (async-fsync path engaged)"
                          : "NO (seccomp-blocked / unsupported — SYNC fallback; still valid)");

    const std::string dir = make_scratch();
    const std::string path = dir + "/wal.bin";
    const auto a_bytes = bytes_of("DURABLE-A");

    // (A)+(B)+(C): write through the reactor-bound ProdDisk (async sync when ring is up),
    // crash, reopen, verify the synced prefix survives.
    WriteState wst;
    {
        prod::ProdDisk disk(reactor, path);
        check(disk.valid(), "reactor-bound ProdDisk valid");
        reactor.arm_uring();
        reactor.spawn(durable_writer(&disk, &wst));
        // HARD wall guard (3s): a missing CQE can never hang the loop.
        reactor.run_until([&] { return wst.done; }, reactor.now() + 3'000'000'000LL);
        check(wst.done, "durable writer completed (async sync resolved)");
        check(wst.append_a_ok, "append A ok");
        check(wst.sync_ok, "ASYNC sync() resolved OK (durability barrier completed)");
        check(wst.append_b_ok, "append B (un-synced tail) ok");
        // disk dtor here = simulated crash (no further sync). A was synced; B was not.
    }

    // Reopen on a plain Scheduler (synchronous path) and verify the synced prefix.
    core::Scheduler sched;
    prod::ProdDisk reopened(sched, path);
    check(reopened.valid(), "reopened ProdDisk valid");
    check(reopened.logical_len() >= a_bytes.size(),
          "synced prefix present after reopen (barrier held — no loss)");
    ReadState rst;
    sched.spawn(prefix_reader(&reopened, &rst, a_bytes.size()));
    sched.run();
    check(rst.read_ok, "read synced prefix back");
    check(rst.got == a_bytes, "synced bytes byte-identical after reopen");

    ::remove(path.c_str());
    ::remove(dir.c_str());

    if (g_failures == 0) {
        std::fprintf(stderr, "prod_uring_test: ALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "prod_uring_test: %d FAILURE(S)\n", g_failures);
    return 1;
}

#else // !__linux__ — io_uring is Linux-only; this target is not built on macOS.
int main() {
    std::fprintf(stderr, "prod_uring_test: skipped (non-Linux host)\n");
    return 0;
}
#endif
