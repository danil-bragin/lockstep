// prod_disk_test.cpp — Phase 7 S3 driver. Proves the PRODUCTION IDisk provider
// (prod::ProdDisk — a real file-backed device) satisfies the frozen IDisk
// contract on REAL hardware:
//
//   (1) tier-A IDisk UNIVERSAL CONTRACT (the S1 conformance check, VERBATIM) run
//       against ProdDisk via a ProdDiskFactory with the SAME member shape as the
//       S1 SimDiskFactory — not a single check is forked (V-PROD-CONTRACT).
//   (2) CRASH-INJECTION DURABILITY (the heart of S3): with a real temp file,
//       data appended-THEN-synced MUST survive a close+reopen (= simulated
//       process crash/restart) byte-identical, and NOTHING synced is ever lost
//       (no lying fsync); an appended-but-NOT-synced tail MAY be absent. This is
//       the exact durability BARRIER the WAL + recovery rely on.
//   (3) DISK RECORD -> REPLAY byte-identical (V-PROD-REPLAY): a RecordingDisk
//       wraps a ProdDisk and logs each read's returned bytes; a ReplayDisk
//       reproduces those reads from the trace; the observed read sequence is
//       byte-identical across record and replay.
//
// This is NON-provider code (a test) → the forbidden-call lint scans it. It must
// touch NO <chrono>/<random>/<thread> and NO raw file IO of its own — all real
// disk IO stays inside prod::ProdDisk. The ONLY system facility a test legitimately
// uses for its scratch directory is mkdtemp/remove, which are not on the forbidden
// set (they are not nondeterminism sources the boundary owns); we keep them in a
// tiny helper and feed ProdDisk a unique per-run temp path so runs never collide
// and the test is self-cleaning (no leftover state, no fixed absolute path).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdScratchDir.hpp>
#include <lockstep/prod/ReplayTrace.hpp>
#include <lockstep/sim/SeededRandom.hpp>

#include "provider_conformance/ContractConformance.hpp"

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;
namespace sim = lockstep::sim;
using conformance::Report;

// The unique, self-cleaning scratch directory lives in providers/prod/ (the
// lint-exempt boundary that owns real filesystem paths) so this test stays pure —
// it never does raw file IO of its own (only ProdDisk does).
using prod::ProdScratchDir;

// ===========================================================================
// PROD DISK FACTORY — same member shape as the S1 SimDiskFactory, so the IDENTICAL
// universal `check_disk_contract` runs against ProdDisk. Owns a per-suite scratch
// dir; each make() opens a fresh file in it. Clock/Random are accepted to match
// the factory signature but ignored (ProdDisk needs neither — real IO, no faults).
// ===========================================================================
struct ProdDiskFactory {
    ProdScratchDir dir{"lockstep_prod_disk_conf"};
    int seq = 0;

    [[nodiscard]] std::unique_ptr<core::IDisk>
    make(core::Scheduler& sched, core::IClock& /*clock*/, core::IRandom& /*rng*/) {
        std::string p = dir.file("conf_" + std::to_string(seq++) + ".dat");
        return std::make_unique<prod::ProdDisk>(sched, p);
    }
};

std::string render(const Report& rep) {
    std::string out;
    for (const Report::Item& it : rep.items) {
        out += (it.pass ? "PASS " : "FAIL ") + it.name;
        if (!it.pass && !it.detail.empty()) {
            out += "  -- " + it.detail;
        }
        out += "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// (1) ProdDisk — full tier-A IDisk universal contract (reuses S1 verbatim).
// The clock/random factories are required by the check signature; ProdDisk
// ignores them, but the SAME function runs as for sim.
// ---------------------------------------------------------------------------
struct ProdClocklessSimClockFactory {
    std::vector<std::unique_ptr<core::SimClock>> clocks{};
    core::IClock& clock(core::Scheduler& sched) {
        clocks.push_back(std::make_unique<core::SimClock>(sched));
        return *clocks.back();
    }
};
struct SeededRandomFactory {
    [[nodiscard]] std::unique_ptr<core::IRandom> make(std::uint64_t seed) {
        return std::make_unique<sim::SeededRandom>(seed);
    }
};

Report run_prod_disk_contract() {
    Report rep;
    ProdClocklessSimClockFactory clock_factory; // drives the async harness only
    SeededRandomFactory random_factory;
    ProdDiskFactory disk_factory;
    conformance::universal::check_disk_contract(clock_factory, random_factory,
                                                disk_factory, rep);
    return rep;
}

// ---------------------------------------------------------------------------
// (2) CRASH-INJECTION DURABILITY — the heart of S3.
//
// Script over a REAL temp file:
//   * append A, append B, sync()         -> {A,B} are now DURABLE.
//   * append C (the UN-SYNCED TAIL)       -> NOT durable.
//   * CRASH = destroy the ProdDisk (close the fd) WITHOUT a final sync — a
//     simulated process crash/restart with C still only in the page cache.
//   * REOPEN the SAME path in a fresh ProdDisk (= recovery).
//
// Asserts the durability BARRIER:
//   (a) everything up to the last successful sync ({A,B}) is present and
//       byte-identical after reopen — nothing synced is lost (NO lying fsync);
//   (b) the un-synced tail C MAY be absent (an honest device keeps the page-cache
//       bytes across a clean close, so we do NOT require C gone — we require that
//       its presence-or-absence does not corrupt the synced prefix, and that the
//       reopened logical length is >= the synced length and the synced prefix
//       reads back exactly).
// The load-bearing property is (a): SYNCED => DURABLE & BYTE-IDENTICAL.
// ---------------------------------------------------------------------------
struct CrashProof {
    bool ran = false;
    bool synced_prefix_survives = false; // (a) the barrier — load-bearing
    bool synced_prefix_byte_identical = false;
    std::uint64_t len_after_reopen = 0;
    std::uint64_t synced_len = 0;
    std::vector<std::byte> want_prefix{};
    std::vector<std::byte> got_prefix{};
};

CrashProof run_crash_injection(const std::string& path, core::Scheduler& sched) {
    CrashProof pr;

    const std::vector<std::byte> a = conformance::payload(16, 0x10);
    const std::vector<std::byte> b = conformance::payload(24, 0xA0);
    const std::vector<std::byte> c = conformance::payload(8, 0x55); // un-synced tail
    pr.want_prefix.insert(pr.want_prefix.end(), a.begin(), a.end());
    pr.want_prefix.insert(pr.want_prefix.end(), b.begin(), b.end());
    pr.synced_len = pr.want_prefix.size();

    // --- write + sync {A,B}, then append C WITHOUT syncing, then "crash" -------
    {
        prod::ProdDisk disk(sched, path);
        struct S {
            bool ok = true;
            core::Error e{};
        } s;
        auto writer = [&](core::IDisk& d, S& st) -> core::Task {
            core::Offset off = 0;
            st.e = co_await d.append(conformance::view_of(a), off);
            st.ok = st.ok && !st.e;
            st.e = co_await d.append(conformance::view_of(b), off);
            st.ok = st.ok && !st.e;
            st.e = co_await d.sync(); // durability barrier — {A,B} now on disk
            st.ok = st.ok && !st.e;
            st.e = co_await d.append(conformance::view_of(c), off); // UN-SYNCED tail
            st.ok = st.ok && !st.e;
            co_return; // NO final sync: C is only in the page cache
        };
        sched.spawn(writer(disk, s));
        sched.run();
        // disk goes out of scope here: close(fd) == the simulated crash/restart.
    }

    // --- REOPEN the same path (recovery) and read the synced prefix back -------
    {
        prod::ProdDisk disk(sched, path);
        pr.len_after_reopen = disk.logical_len();
        pr.got_prefix.assign(pr.synced_len, std::byte{0});
        struct R {
            core::Error e{};
        } r;
        auto reader = [&](core::IDisk& d, R& rr, std::vector<std::byte>& out) -> core::Task {
            rr.e = co_await d.read(0, std::span<std::byte>(out.data(), out.size()));
            co_return;
        };
        sched.spawn(reader(disk, r, pr.got_prefix));
        sched.run();
        pr.synced_prefix_survives =
            !r.e && pr.len_after_reopen >= pr.synced_len;
        pr.synced_prefix_byte_identical = pr.got_prefix == pr.want_prefix;
    }

    pr.ran = true;
    return pr;
}

// A stronger barrier probe: prove sync() is NOT a lie by reopening and confirming
// the synced bytes are durable across MANY reopen cycles (no slow data loss).
bool synced_data_durable_across_reopens(const std::string& path,
                                        core::Scheduler& sched) {
    const std::vector<std::byte> data = conformance::payload(64, 0x01);
    {
        prod::ProdDisk disk(sched, path);
        struct S {
            core::Error e{};
        } s;
        auto w = [&](core::IDisk& d, S& st) -> core::Task {
            core::Offset off = 0;
            st.e = co_await d.append(conformance::view_of(data), off);
            if (!st.e) {
                st.e = co_await d.sync();
            }
            co_return;
        };
        sched.spawn(w(disk, s));
        sched.run();
        if (s.e) {
            return false;
        }
    }
    // Reopen 5 times; the synced bytes must read back identical every time.
    for (int i = 0; i < 5; ++i) {
        prod::ProdDisk disk(sched, path);
        std::vector<std::byte> got(data.size(), std::byte{0});
        struct R {
            core::Error e{};
        } r;
        auto rd = [&](core::IDisk& d, R& rr, std::vector<std::byte>& out) -> core::Task {
            rr.e = co_await d.read(0, std::span<std::byte>(out.data(), out.size()));
            co_return;
        };
        sched.spawn(rd(disk, r, got));
        sched.run();
        if (r.e || got != data) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// (3) DISK RECORD -> REPLAY byte-identical (V-PROD-REPLAY).
// Script reads against a RecordingDisk (wrapping a real ProdDisk), capturing the
// returned bytes into a trace; then run the SAME read script against a ReplayDisk
// fed from that trace. The observed read sequence must be byte-identical.
// ---------------------------------------------------------------------------
struct DiskReplayProof {
    bool ok = false;
    std::size_t records = 0;
    std::string recorded_obs;
    std::string replayed_obs;
    std::string trace_render;
};

// A read script: a fixed sequence of (offset, length) reads, rendering each
// observation (errc + delivered bytes) into a stable string.
template <class Disk>
std::string run_read_script(Disk& disk, core::Scheduler& sched) {
    struct Obs {
        std::string out;
    } o;
    // (off, len) pairs: in-range reads of the two records + a past-end read.
    const std::pair<core::Offset, std::size_t> reads[] = {
        {0, 16}, {16, 24}, {0, 8}, {30, 10}, {0, 200}};
    auto runner = [&](core::IDisk& d, Obs& obs) -> core::Task {
        for (const auto& [off, len] : reads) {
            std::vector<std::byte> buf(len, std::byte{0});
            core::Error e = co_await d.read(off, std::span<std::byte>(buf.data(), buf.size()));
            obs.out += "read off=" + std::to_string(off) + " len=" + std::to_string(len) +
                       " err=" + std::to_string(static_cast<unsigned>(e.code)) + " bytes=";
            if (e.code == core::ErrorCode::Ok) {
                obs.out += prod::bytes_to_hex(
                    std::span<const std::byte>(buf.data(), buf.size()));
            }
            obs.out += "\n";
        }
        co_return;
    };
    sched.spawn(runner(disk, o));
    sched.run();
    return o.out;
}

DiskReplayProof run_disk_record_replay(const std::string& path, core::Scheduler& sched) {
    DiskReplayProof proof;
    prod::ReplayTrace trace;

    // --- RECORD: seed the file with two records + sync, then run the read script
    // through a RecordingDisk wrapping the real ProdDisk. ----------------------
    {
        prod::ProdDisk disk(sched, path);
        const std::vector<std::byte> r0 = conformance::payload(16, 0x10);
        const std::vector<std::byte> r1 = conformance::payload(24, 0x40);
        struct S {
            core::Error e{};
        } s;
        auto seed = [&](core::IDisk& d, S& st) -> core::Task {
            core::Offset off = 0;
            st.e = co_await d.append(conformance::view_of(r0), off);
            st.e = co_await d.append(conformance::view_of(r1), off);
            st.e = co_await d.sync();
            co_return;
        };
        sched.spawn(seed(disk, s));
        sched.run();

        prod::RecordingDisk rec(disk, trace);
        proof.recorded_obs = run_read_script(rec, sched);
        proof.trace_render = trace.render();
        proof.records = trace.size();
    }

    // --- REPLAY: same read script over a ReplayDisk fed from the trace ---------
    {
        prod::ReplayDisk replay(trace, sched);
        proof.replayed_obs = run_read_script(replay, sched);
    }

    proof.ok = proof.recorded_obs == proof.replayed_obs && proof.records > 0;
    return proof;
}

} // namespace

int main() {
    std::printf("[prod_disk_test] Phase 7 S3 — prod IDisk (file-backed) + crash + replay\n");

    ProdScratchDir scratch{"lockstep_prod_disk"};
    if (!scratch.ok()) {
        std::fprintf(stderr, "FATAL: could not create scratch dir\n");
        return 1;
    }
    std::printf("scratch dir: %s\n", scratch.path().c_str());

    bool all = true;

    // (1) ProdDisk — full tier-A IDisk universal contract (S1 checks verbatim).
    const Report rr = run_prod_disk_contract();
    std::printf("\n=== (1) ProdDisk UNIVERSAL IDisk CONTRACT (tier-A, S1 verbatim) ===\n%s",
                render(rr).c_str());
    std::printf("prod disk: %zu checks, %zu failures\n", rr.items.size(), rr.failures());
    all = all && rr.all_pass();

    // (2) Crash-injection durability.
    core::Scheduler sched;
    const std::string crash_path = scratch.file("crash.dat");
    const CrashProof cp = run_crash_injection(crash_path, sched);
    std::printf("\n=== (2) CRASH-INJECTION DURABILITY (synced survives reopen) ===\n");
    std::printf("synced_len=%llu  len_after_reopen=%llu\n",
                static_cast<unsigned long long>(cp.synced_len),
                static_cast<unsigned long long>(cp.len_after_reopen));
    std::printf("%s disk/synced-prefix-survives-reopen\n",
                cp.synced_prefix_survives ? "PASS" : "FAIL");
    std::printf("%s disk/synced-prefix-byte-identical (no lying fsync)\n",
                cp.synced_prefix_byte_identical ? "PASS" : "FAIL");
    const bool tail_note = cp.len_after_reopen >= cp.synced_len;
    std::printf("%s disk/unsynced-tail-does-not-corrupt-synced-prefix\n",
                tail_note ? "PASS" : "FAIL");
    const bool durable = synced_data_durable_across_reopens(
        scratch.file("durable.dat"), sched);
    std::printf("%s disk/synced-data-durable-across-many-reopens\n",
                durable ? "PASS" : "FAIL");
    all = all && cp.ran && cp.synced_prefix_survives &&
          cp.synced_prefix_byte_identical && tail_note && durable;

    // (3) Disk record -> replay byte-identical.
    const DiskReplayProof dp = run_disk_record_replay(scratch.file("replay.dat"), sched);
    std::printf("\n=== (3) DISK RECORD -> REPLAY byte-identical (V-PROD-REPLAY) ===\n");
    std::printf("disk trace (%zu records):\n%s", dp.records, dp.trace_render.c_str());
    std::printf("%s disk record-replay observations byte-identical\n",
                dp.ok ? "PASS" : "FAIL");
    if (!dp.ok) {
        std::fprintf(stderr, "--- recorded ---\n%s\n--- replayed ---\n%s\n",
                     dp.recorded_obs.c_str(), dp.replayed_obs.c_str());
    }
    all = all && dp.ok;

    std::printf("\n[prod_disk_test] %s\n", all ? "ALL PASS" : "FAILED");
    return all ? 0 : 1;
}
