// sim_disk_test.cpp — C2.2 gate for the simulated IDisk (sim::SimDisk).
//
// Drives the disk through the deterministic Scheduler/SimClock + seeded PRNG and
// asserts the BRIEF S2 hard invariants. This is non-provider code, so the
// forbidden-call lint scans it: all time is virtual, all randomness is
// sim::SeededRandom — no <chrono>/<thread>/<random>.
//
// Assertions (each maps to a brief requirement):
//   (1) write -> sync -> crash -> recover yields a consistent PREFIX: the durable
//       bytes are present and correct after recovery; nothing un-synced survives.
//   (2) a LYING-FSYNC write is correctly LOST after a crash: sync() ACKED ok yet
//       the un-promoted tail vanishes on crash (acked != durable).
//   (3) a TORN WRITE is observable as partial: a read after the fault sees a
//       partial page, not all-or-nothing.
//   (4) same seed twice => byte-identical scheduler event trace (byte compare).
//   (5) the seed is PRINTED so any failure replays from the logged seed.
//
// Every run logs its seed. To replay a failure: rerun with the printed seed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

namespace {

using lockstep::core::Error;
using lockstep::core::ErrorCode;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;

int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "DISK GATE FAIL [%s:%d]: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                                  \
            std::fprintf(stderr, "\n");                                         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// Helper: turn a string literal into a byte span over its characters.
std::span<const std::byte> bytes_of(const std::string& s) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string to_string(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

// ---------------------------------------------------------------------------
// (1) write -> sync -> crash -> recover yields a consistent PREFIX.
// An HONEST disk (no faults): everything synced is durable, the un-synced tail
// is dropped by the crash, recover() exposes exactly the durable prefix.
// ---------------------------------------------------------------------------
struct PrefixResult {
    Error append_err{};
    Error sync_err{};
    std::vector<std::byte> recovered{};
    bool ok = false;
};

Task prefix_driver(SimDisk& disk, PrefixResult& out) {
    // Two durable records, then sync (the durability barrier).
    Offset o0 = 0, o1 = 0;
    out.append_err = co_await disk.append(bytes_of(std::string("ALPHA")), o0);
    Error a1 = co_await disk.append(bytes_of(std::string("BETA")), o1);
    out.sync_err = co_await disk.sync();

    // A THIRD record appended but NOT synced — must NOT survive the crash.
    Offset o2 = 0;
    Error a2 = co_await disk.append(bytes_of(std::string("GAMMA-unsynced")), o2);
    (void)a1;
    (void)a2;

    // Crash: drops the un-synced GAMMA. Recover: durable prefix only.
    disk.crash();
    disk.recover();
    out.recovered = disk.durable_snapshot();
    out.ok = true;
    co_return;
}

bool test_prefix(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig cfg; // honest disk: all fault probs 0, latency only
    SimDisk disk(sched, clock, rng, cfg);

    PrefixResult r;
    sched.spawn(prefix_driver(disk, r));
    sched.run();

    bool ok = true;
    CHECK(r.ok, "prefix driver did not finish");
    CHECK(!r.append_err && !r.sync_err, "honest append/sync should succeed");
    // Durable prefix is exactly ALPHA+BETA; GAMMA (un-synced) is gone.
    const std::string got = to_string(r.recovered);
    CHECK(got == "ALPHABETA",
          "consistent prefix wrong: got \"%s\" want \"ALPHABETA\"", got.c_str());
    if (r.recovered.empty() || got != "ALPHABETA" || !r.ok || r.append_err || r.sync_err) {
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (2) a LYING-FSYNC write is correctly LOST after a crash.
// Force lying_fsync_prob = 1.0: sync() acks ok but promotes only a prefix; the
// remainder is LYING and must be gone after the crash.
// ---------------------------------------------------------------------------
struct LyingResult {
    Error sync_err{};
    std::size_t durable_before_crash = 0;
    std::size_t lying_before_crash = 0;
    std::size_t staged_after_append = 0;
    std::vector<std::byte> recovered{};
    bool ok = false;
};

Task lying_driver(SimDisk& disk, LyingResult& out) {
    Offset o0 = 0;
    co_await disk.append(bytes_of(std::string("DURABLE-PAYLOAD-MANY-BYTES")), o0);
    out.staged_after_append = disk.staged_len();

    // sync() LIES: it returns ok but only promotes a prefix; tail -> lying.
    out.sync_err = co_await disk.sync();
    out.durable_before_crash = disk.durable_len();
    out.lying_before_crash = disk.lying_len();

    disk.crash();    // lying bytes LOST here
    disk.recover();
    out.recovered = disk.durable_snapshot();
    out.ok = true;
    co_return;
}

bool test_lying_fsync(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig cfg;
    cfg.lying_fsync_prob = 1.0; // force the lie
    SimDisk disk(sched, clock, rng, cfg);

    LyingResult r;
    sched.spawn(lying_driver(disk, r));
    sched.run();

    bool ok = true;
    CHECK(r.ok, "lying driver did not finish");
    // sync() ACKED success despite lying — that is the danger.
    CHECK(!r.sync_err, "lying sync still ACKs ok (code=%u)",
          static_cast<unsigned>(r.sync_err.code));
    // There WERE lying bytes (the tail not truly persisted).
    CHECK(r.lying_before_crash > 0,
          "expected lying bytes after a forced lying fsync (got %zu)", r.lying_before_crash);
    // After crash+recover the survivor count == the durable (truly-persisted)
    // prefix, and is STRICTLY LESS than what sync() acked (durable+lying).
    const std::size_t acked_durable = r.durable_before_crash + r.lying_before_crash;
    CHECK(r.recovered.size() == r.durable_before_crash,
          "survivors %zu != durable-before-crash %zu", r.recovered.size(),
          r.durable_before_crash);
    CHECK(r.recovered.size() < acked_durable,
          "lying-fsync bytes survived: recovered %zu, acked %zu", r.recovered.size(),
          acked_durable);
    if (!r.ok || r.sync_err || r.lying_before_crash == 0 ||
        r.recovered.size() != r.durable_before_crash || r.recovered.size() >= acked_durable) {
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (3) a TORN WRITE is observable as partial.
// Force torn_write_prob = 1.0: the append lands only a prefix of its bytes. We
// read back and observe a PARTIAL page (fewer durable bytes than written), and
// the device's logical length is < the requested length.
// ---------------------------------------------------------------------------
struct TornResult {
    Error append_err{};
    std::size_t requested = 0;
    std::size_t landed = 0; // bytes actually staged after the torn append
    Error read_full_err{};  // reading the FULL requested span must be short (NotFound)
    bool ok = false;
};

Task torn_driver(SimDisk& disk, TornResult& out) {
    const std::string payload = "THIS-PAGE-WILL-BE-TORN-IN-HALF";
    out.requested = payload.size();
    Offset o0 = 0;
    out.append_err = co_await disk.append(bytes_of(payload), o0);
    out.landed = disk.staged_len();

    // A read of the FULL requested length now runs past the (partial) end ->
    // short read, reported as NotFound. This is the observable partial page.
    std::vector<std::byte> buf(out.requested, std::byte{0});
    out.read_full_err = co_await disk.read(0, std::span<std::byte>(buf.data(), buf.size()));
    out.ok = true;
    co_return;
}

bool test_torn_write(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig cfg;
    cfg.torn_write_prob = 1.0; // force the tear
    SimDisk disk(sched, clock, rng, cfg);

    TornResult r;
    sched.spawn(torn_driver(disk, r));
    sched.run();

    bool ok = true;
    CHECK(r.ok, "torn driver did not finish");
    // The append still ACKs ok (a real drive does not know it tore)...
    CHECK(!r.append_err, "torn append still completes ok (code=%u)",
          static_cast<unsigned>(r.append_err.code));
    // ...but only a PARTIAL prefix landed: strictly fewer bytes than requested,
    // and at least one byte (observable-partial, not all-or-nothing).
    CHECK(r.landed >= 1 && r.landed < r.requested,
          "torn write not partial: landed %zu of %zu", r.landed, r.requested);
    // Reading the full requested span runs past the partial end -> NotFound.
    CHECK(r.read_full_err.code == ErrorCode::NotFound,
          "full read over torn page should be short (NotFound), got code=%u",
          static_cast<unsigned>(r.read_full_err.code));
    if (!r.ok || r.append_err || !(r.landed >= 1 && r.landed < r.requested) ||
        r.read_full_err.code != ErrorCode::NotFound) {
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Run a FULL fault-storm scenario and return the rendered scheduler trace, for
// the byte-identical replay check (assertion 4). Exercises append/sync/read/
// crash/recover with several faults enabled so the trace is rich.
// ---------------------------------------------------------------------------
Task storm_driver(SimDisk& disk) {
    Offset off = 0;
    for (int i = 0; i < 6; ++i) {
        std::string rec = "record-" + std::to_string(i) + "-payload";
        co_await disk.append(bytes_of(rec), off);
        if (i % 2 == 1) {
            co_await disk.sync();
        }
        std::vector<std::byte> buf(4, std::byte{0});
        co_await disk.read(0, std::span<std::byte>(buf.data(), buf.size()));
    }
    disk.crash();
    disk.recover();
    co_return;
}

std::string run_storm_trace(std::uint64_t seed) {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig cfg;
    cfg.io_fault_prob = 0.15;
    cfg.torn_write_prob = 0.30;
    cfg.lying_fsync_prob = 0.40;
    cfg.bit_rot_prob = 0.25;
    SimDisk disk(sched, clock, rng, cfg);

    sched.spawn(storm_driver(disk));
    sched.run();
    return sched.trace_text();
}

} // namespace

int main() {
    // (5) PRINT the seed: any failure below replays from exactly this seed.
    constexpr std::uint64_t kSeed = 0xD15CAFE5EED1234ULL;
    std::printf("[sim_disk_test] seed=%llu (replay any failure with this seed)\n",
                static_cast<unsigned long long>(kSeed));

    // (1) consistent prefix.
    const bool t1 = test_prefix(kSeed);
    std::printf("  [1] write->sync->crash->recover prefix : %s\n", t1 ? "PASS" : "FAIL");

    // (2) lying fsync lost on crash.
    const bool t2 = test_lying_fsync(kSeed);
    std::printf("  [2] lying-fsync write LOST after crash : %s\n", t2 ? "PASS" : "FAIL");

    // (3) torn write observable as partial.
    const bool t3 = test_torn_write(kSeed);
    std::printf("  [3] torn write observable as partial   : %s\n", t3 ? "PASS" : "FAIL");

    // (4) same seed twice => byte-identical event trace.
    const std::string trace_a = run_storm_trace(kSeed);
    const std::string trace_b = run_storm_trace(kSeed);
    const bool t4 = (trace_a == trace_b);
    std::printf("  [4] same-seed byte-identical trace     : %s (%zu bytes)\n",
                t4 ? "PASS" : "FAIL", trace_a.size());
    if (!t4) {
        std::fprintf(stderr,
                     "DETERMINISM FAIL: storm traces differ for seed=%llu\n"
                     "--- run A ---\n%s\n--- run B ---\n%s\n",
                     static_cast<unsigned long long>(kSeed), trace_a.c_str(),
                     trace_b.c_str());
    }

    // A DIFFERENT seed must yield a different fault schedule (proves the seed
    // actually threads the fault decisions — catches a dead-seed bug).
    const std::string trace_c = run_storm_trace(kSeed ^ 0xFFFFFFFFFFFFFFFFULL);
    const bool t5 = (trace_c != trace_a);
    std::printf("  [+] different seed => different trace   : %s\n", t5 ? "PASS" : "FAIL");
    if (!t5) {
        std::fprintf(stderr, "SEED FAIL: different seed produced identical trace\n");
    }

    const bool all = t1 && t2 && t3 && t4 && t5 && (g_failures == 0);
    std::printf("[sim_disk_test] %s (failures=%d, seed=%llu)\n",
                all ? "ALL PASS" : "FAILED", g_failures,
                static_cast<unsigned long long>(kSeed));
    return all ? 0 : 1;
}
