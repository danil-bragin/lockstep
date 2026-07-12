// storage_sstable_test.cpp — Phase 3 §5 step 4 gate for the SSTable layer:
// memtable flush → immutable on-disk SSTable (per-block CRC + sparse index +
// bloom + CRC'd footer), atomic-install via a CRC'd Seq-contiguous MANIFEST, an
// LSM newest→oldest read+scan path, and crash-safe flush. Extends the step-2
// engine; reads now span memtable + SSTables (storage-engine.md §2/§3/§6).
//
// WHAT IT PROVES (one block per spec requirement):
//
//   (A) DIFFERENTIAL WITH FLUSHES — the core gate (§4) under forced flushes:
//       WalEngine in LSM mode (small flush threshold ⇒ MANY flushes) vs Oracle
//       over a seed sweep, on an HONEST disk. Every commit-Seq, every get AND
//       every scan under every snapshot MUST match the oracle. 0 mismatch.
//
//   (B) METAMORPHIC — a flush MUST NOT change any visible value: drive ops,
//       snapshot every (key,snap) get + a full scan BEFORE a forced flush, force
//       a flush, re-read the SAME — identical. (flush ⊥ change reads, §2.)
//
//   (C) CRASH-DURING-FLUSH — crash at arbitrary points INCLUDING mid-flush under
//       the full fault envelope (torn writes + lying fsync + io faults), recover
//       from the manifest's committed SSTable set + WAL prefix, assert the
//       recovered state is ALWAYS a valid oracle PREFIX, 0 fabrication, and NO
//       half-SSTable surviving as valid (a torn/un-committed SSTable is invisible).
//
//   (D) BLOOM no-false-negative: for every key actually present in a built
//       SSTable, the bloom filter reports "maybe present" (never skips a present
//       key). False positives are fine. Checked over many random tables.
//
//   (E) DETERMINISM — same seed ⇒ byte-identical observable history + on-disk
//       fingerprint (the WAL durable image + every SSTable durable image).
//
// Non-provider TU → forbidden-call lint scans it; only randomness is sim
// SeededRandom; all time virtual; no <chrono>/<thread>/<random>.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/storage/DiffHarness.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/Oracle.hpp>
#include <lockstep/storage/SSTable.hpp>
#include <lockstep/storage/WalEngine.hpp>

namespace {

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::BloomFilter;
using lockstep::storage::DiffConfig;
using lockstep::storage::DiffVerdict;
using lockstep::storage::IDiskFactory;
using lockstep::storage::Key;
using lockstep::storage::KeyValue;
using lockstep::storage::Oracle;
using lockstep::storage::Range;
using lockstep::storage::run_diff;
using lockstep::storage::Seq;
using lockstep::storage::Snapshot;
using lockstep::storage::SSTableBuilder;
using lockstep::storage::SstBuildResult;
using lockstep::storage::SstEntry;
using lockstep::storage::Value;
using lockstep::storage::WalEngine;

int g_failures = 0;

#define CHECK(cond, ...)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "SSTABLE GATE FAIL [%s:%d]: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                                      \
            std::fprintf(stderr, "\n");                                             \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// A concrete IDiskFactory backed by a pool of SimDisks (one per SSTable id). The
// engine only ever talks to a returned IDisk via append/read/sync. Disks are
// stable across a crash/recover (recovery re-opens the same backing by id).
// ---------------------------------------------------------------------------
class VecDiskFactory final : public IDiskFactory {
public:
    VecDiskFactory(Scheduler& sched, SimClock& clock, SeededRandom& rng, DiskFaultConfig cfg)
        : sched_(&sched), clock_(&clock), rng_(&rng), cfg_(cfg) {}

    [[nodiscard]] IDisk& disk_for(std::uint64_t sstable_id) override {
        while (disks_.size() <= sstable_id) {
            disks_.push_back(std::make_unique<SimDisk>(*sched_, *clock_, *rng_, cfg_));
        }
        return *disks_[sstable_id];
    }

    [[nodiscard]] SimDisk* raw(std::uint64_t id) {
        return id < disks_.size() ? disks_[id].get() : nullptr;
    }
    [[nodiscard]] std::size_t count() const noexcept { return disks_.size(); }

    void crash_all() {
        for (auto& d : disks_) {
            d->crash();
        }
    }
    void recover_all() {
        for (auto& d : disks_) {
            d->recover();
        }
    }
    // Concatenated durable fingerprint across all SSTable disks (determinism).
    [[nodiscard]] std::vector<std::byte> durable_fingerprint() const {
        std::vector<std::byte> out;
        for (const auto& d : disks_) {
            const std::vector<std::byte> img = d->durable_snapshot();
            out.insert(out.end(), img.begin(), img.end());
        }
        return out;
    }

private:
    Scheduler* sched_;
    SimClock* clock_;
    SeededRandom* rng_;
    DiskFaultConfig cfg_;
    std::vector<std::unique_ptr<SimDisk>> disks_;
};

// ===========================================================================
// (A) DIFFERENTIAL WITH FLUSHES — WalEngine (LSM) vs Oracle, get + scan.
// ===========================================================================
void test_differential_with_flush() {
    DiffConfig cfg;
    cfg.steps = 400;   // long enough to force many flushes at threshold 8
    cfg.n_keys = 8;
    cfg.n_values = 6;

    std::uint64_t mismatches = 0;
    DiffVerdict first_bad;
    const std::uint64_t n_seeds = 128;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0x55Au);
        DiskFaultConfig dc;  // HONEST disk (no faults) for the differential gate.
        dc.latency_min = 0;
        dc.latency_max = 2;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);

        WalEngine sut(sched, wal, manifest, factory, /*flush_threshold=*/8);
        Oracle ref(sched);
        DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
        if (!v.ok) {
            if (mismatches == 0) {
                first_bad = v;
            }
            ++mismatches;
        }
    }
    CHECK(mismatches == 0,
          "(A) differential-with-flush: %llu/%llu seeds MISMATCHED; first seed=%llu "
          "step=%llu note=%s expected=%s got=%s",
          static_cast<unsigned long long>(mismatches),
          static_cast<unsigned long long>(n_seeds),
          static_cast<unsigned long long>(first_bad.witness.seed),
          static_cast<unsigned long long>(first_bad.witness.step),
          first_bad.witness.note.c_str(), first_bad.witness.expected.c_str(),
          first_bad.witness.got.c_str());
    std::fprintf(stderr,
                 "[ok] (A) differential WalEngine(LSM)-vs-Oracle: %llu seeds, get+scan, "
                 "0 mismatch (flush threshold 8 ⇒ many flushes)\n",
                 static_cast<unsigned long long>(n_seeds));
}

// ===========================================================================
// (B) METAMORPHIC — a forced flush MUST NOT change any visible value.
// ===========================================================================
struct ReadSet {
    std::vector<std::optional<Value>> gets;
    std::vector<KeyValue> full_scan;
};

Task fill_history(WalEngine& eng, SeededRandom& rng, std::uint64_t ops,
                  std::uint64_t n_keys, std::uint64_t n_values) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    auto vof = [](std::uint64_t i) { return std::string("v") + std::to_string(i); };
    for (std::uint64_t i = 0; i < ops; ++i) {
        const std::uint64_t roll = rng.uniform(10);
        if (roll < 7) {
            co_await eng.put(kof(rng.uniform(n_keys)), vof(rng.uniform(n_values)));
        } else {
            co_await eng.del(kof(rng.uniform(n_keys)));
        }
    }
    co_return;
}

Task read_all(WalEngine& eng, ReadSet& out, Seq snap_at, std::uint64_t n_keys) {
    for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
        const Key k = std::string("k") + std::to_string(ki);
        out.gets.push_back(co_await eng.get(k, Snapshot{snap_at}));
    }
    Range full;
    full.hi_unbounded = true;
    out.full_scan = co_await eng.scan(full, Snapshot{snap_at});
    co_return;
}

void test_metamorphic_flush() {
    const std::uint64_t n_seeds = 60;
    std::uint64_t changed = 0;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0xBEEFu);
        DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 1;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);
        const std::uint64_t n_keys = 8;
        // High threshold so the natural fill does NOT auto-flush; we force it.
        WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/100000);

        sched.spawn(fill_history(eng, rng, 40, n_keys, 6));
        sched.run();

        Snapshot snap;
        sched.spawn([](WalEngine& e, Snapshot& s) -> Task {
            s = co_await e.snapshot();
            co_return;
        }(eng, snap));
        sched.run();

        ReadSet before;
        sched.spawn(read_all(eng, before, snap.at, n_keys));
        sched.run();

        const std::size_t ssts_before = eng.sstable_count();
        // Force a flush of the current memtable to a new SSTable (no new commits ⇒
        // the committed history is unchanged; only WHERE the versions live moves
        // from memtable to SSTable). Reads at the same snapshot must be identical.
        sched.spawn([](WalEngine& e) -> Task {
            co_await e.force_flush();
            co_return;
        }(eng));
        sched.run();
        const std::size_t ssts_after = eng.sstable_count();

        ReadSet after;
        sched.spawn(read_all(eng, after, snap.at, n_keys));
        sched.run();

        if (before.gets != after.gets || before.full_scan != after.full_scan) {
            ++changed;
            std::fprintf(stderr, "  [flush CHANGED reads] seed=%llu\n",
                         static_cast<unsigned long long>(seed));
        }
        // Sanity: the flush actually produced an SSTable (else the test is vacuous).
        CHECK(ssts_after >= ssts_before + 1 || before.gets.empty(),
              "(B) metamorphic: forced flush did not create an SSTable (seed=%llu)",
              static_cast<unsigned long long>(seed));
    }
    CHECK(changed == 0,
          "(B) metamorphic: %llu seeds had a flush CHANGE a visible value — "
          "flush ⊥ change reads VIOLATED",
          static_cast<unsigned long long>(changed));
    std::fprintf(stderr,
                 "[ok] (B) metamorphic: %llu seeds, a forced flush changed NO get/scan "
                 "result\n",
                 static_cast<unsigned long long>(n_seeds));
}

// ===========================================================================
// (C) CRASH-DURING-FLUSH — recover to a valid oracle PREFIX, no half-SSTable.
// ===========================================================================
struct OpRec {
    enum Kind { Put, Del } kind;
    Key key;
    Value value;
    Seq seq;
};

// Ground-truth read over the recorded ops, truncated to `tip`.
std::optional<Value> oracle_read(const std::vector<OpRec>& ops, const Key& key, Seq at,
                                 Seq tip) {
    const Seq bound = (at < tip) ? at : tip;
    std::optional<Value> result;
    Seq best = 0;
    for (const OpRec& o : ops) {
        if (o.key != key || o.seq > bound || o.seq < best) {
            continue;
        }
        best = o.seq;
        result = (o.kind == OpRec::Del) ? std::nullopt : std::optional<Value>(o.value);
    }
    return result;
}

struct CrashScenario {
    std::vector<OpRec> ops;
    Seq recovered_tip = 0;
    std::vector<std::optional<Value>> probe_reads;
    std::vector<std::pair<Key, Seq>> probes;
    std::vector<std::byte> wal_fingerprint;
    std::vector<std::byte> sst_fingerprint;
};

Task crash_fill(WalEngine& eng, SeededRandom& rng, std::uint64_t op_count,
                std::uint64_t crash_step, CrashScenario& out, std::uint64_t n_keys,
                std::uint64_t n_values) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    auto vof = [](std::uint64_t i) { return std::string("v") + std::to_string(i); };
    for (std::uint64_t step = 0; step < op_count; ++step) {
        if (step == crash_step) {
            break;
        }
        const std::uint64_t roll = rng.uniform(10);
        if (roll < 6) {
            const Key k = kof(rng.uniform(n_keys));
            const Value v = vof(rng.uniform(n_values));
            const Seq s = co_await eng.put(k, v);
            out.ops.push_back(OpRec{OpRec::Put, k, v, s});
        } else if (roll < 8) {
            const Key k = kof(rng.uniform(n_keys));
            const Seq s = co_await eng.del(k);
            out.ops.push_back(OpRec{OpRec::Del, k, Value{}, s});
        } else {
            co_await eng.sync();
        }
    }
    co_return;
}

Task probe(WalEngine& eng, CrashScenario& out) {
    for (const auto& pr : out.probes) {
        out.probe_reads.push_back(co_await eng.get(pr.first, Snapshot{pr.second}));
    }
    co_return;
}

CrashScenario run_crash(std::uint64_t seed, std::uint64_t op_count,
                        std::uint64_t crash_step) {
    CrashScenario out;
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 2;
    dc.torn_write_prob = 0.20;    // torn SSTable / WAL blocks
    dc.lying_fsync_prob = 0.20;   // a sync that drops a tail on crash
    dc.io_fault_prob = 0.05;
    SimDisk wal(sched, clock, rng, dc);
    SimDisk manifest(sched, clock, rng, dc);
    VecDiskFactory factory(sched, clock, rng, dc);
    const std::uint64_t n_keys = 6;
    const std::uint64_t n_values = 5;

    // LSM mode, small threshold ⇒ flushes happen mid-stream (so a crash can land
    // mid-flush — between an SSTable's bytes and its manifest commit).
    WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/5);

    sched.spawn(crash_fill(eng, rng, op_count, crash_step, out, n_keys, n_values));
    sched.run();

    // Crash everything (WAL + manifest + every SSTable disk), then recover media.
    const std::size_t wal_len = wal.durable_len();
    const std::size_t man_len = manifest.durable_len();
    wal.crash();
    manifest.crash();
    factory.crash_all();
    wal.recover();
    manifest.recover();
    factory.recover_all();

    // A fresh engine recovers from the SAME backings (a real reopen): load the
    // manifest's committed SSTable set, then replay the WAL prefix.
    WalEngine recovered(sched, wal, manifest, factory, /*flush_threshold=*/5);
    sched.spawn([](WalEngine& e, std::size_t wl, std::size_t ml) -> Task {
        co_await e.recover_lsm(wl, ml);
        co_return;
    }(recovered, wal.durable_len(), manifest.durable_len()));
    sched.run();
    out.recovered_tip = recovered.last_seq();
    out.wal_fingerprint = wal.durable_snapshot();
    out.sst_fingerprint = factory.durable_fingerprint();
    (void)wal_len;
    (void)man_len;

    Seq max_seq = 0;
    for (const OpRec& o : out.ops) {
        if (o.seq > max_seq) {
            max_seq = o.seq;
        }
    }
    for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
        const Key k = std::string("k") + std::to_string(ki);
        for (Seq at = 0; at <= max_seq + 1; ++at) {
            out.probes.emplace_back(k, at);
        }
    }
    sched.spawn(probe(recovered, out));
    sched.run();
    return out;
}

void test_crash_during_flush() {
    const std::uint64_t n_seeds = 96;
    std::uint64_t crash_points = 0;
    std::uint64_t fabrications = 0;
    std::uint64_t prefix_violations = 0;
    std::uint64_t first_bad = 0;

    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        const std::uint64_t op_count = 80;
        SeededRandom pick(seed ^ 0xF10751u);
        const std::uint64_t crash_step =
            5 + static_cast<std::uint64_t>(pick.uniform(op_count - 5));
        CrashScenario sc = run_crash(seed, op_count, crash_step);
        ++crash_points;

        const Seq tip = sc.recovered_tip;
        Seq max_committed = 0;
        for (const OpRec& o : sc.ops) {
            if (o.seq > max_committed) {
                max_committed = o.seq;
            }
        }
        if (tip > max_committed) {
            ++prefix_violations;
            if (first_bad == 0) {
                first_bad = seed;
            }
        }
        for (std::size_t i = 0; i < sc.probes.size(); ++i) {
            const Key& k = sc.probes[i].first;
            const Seq at = sc.probes[i].second;
            const std::optional<Value> got = sc.probe_reads[i];
            const std::optional<Value> want = oracle_read(sc.ops, k, at, tip);
            if (got != want) {
                if (got.has_value() && (!want.has_value() || *got != *want)) {
                    ++fabrications;
                } else {
                    ++prefix_violations;
                }
                if (first_bad == 0) {
                    first_bad = seed;
                }
                std::fprintf(stderr,
                             "  [crash-flush mismatch] seed=%llu crash_step=%llu tip=%llu "
                             "key=%s at=%llu want=%s got=%s\n",
                             static_cast<unsigned long long>(seed),
                             static_cast<unsigned long long>(crash_step),
                             static_cast<unsigned long long>(tip), k.c_str(),
                             static_cast<unsigned long long>(at),
                             want.has_value() ? want->c_str() : "nil",
                             got.has_value() ? got->c_str() : "nil");
                break;
            }
        }
    }
    CHECK(fabrications == 0,
          "(C) crash-during-flush: %llu FABRICATED reads across %llu crash points "
          "(first seed=%llu) — half-SSTable/torn surfaced as valid (V-NOTORN/V-PREFIX)",
          static_cast<unsigned long long>(fabrications),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    CHECK(prefix_violations == 0,
          "(C) crash-during-flush: %llu PREFIX violations across %llu crash points "
          "(first seed=%llu) — recovered state not a valid oracle prefix",
          static_cast<unsigned long long>(prefix_violations),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    std::fprintf(stderr,
                 "[ok] (C) crash-during-flush: %llu crash points (full fault envelope, "
                 "flush threshold 5), recovered state ALWAYS a valid prefix, 0 "
                 "fabrication, no half-SSTable surviving\n",
                 static_cast<unsigned long long>(crash_points));
}

// ===========================================================================
// (D) BLOOM — never a false negative (a present key is never skipped).
// ===========================================================================
void test_bloom_no_false_negative() {
    const std::uint64_t n_tables = 400;
    std::uint64_t false_negatives = 0;
    std::uint64_t total_keys = 0;
    for (std::uint64_t seed = 1; seed <= n_tables; ++seed) {
        SeededRandom rng(seed ^ 0xB100Au);
        const std::uint64_t n = 1 + rng.uniform(60);
        std::vector<Key> keys;
        for (std::uint64_t i = 0; i < n; ++i) {
            // Random-length opaque keys over a wide alphabet.
            std::string k;
            const std::uint64_t len = 1 + rng.uniform(12);
            for (std::uint64_t j = 0; j < len; ++j) {
                k.push_back(static_cast<char>('a' + rng.uniform(26)));
            }
            keys.push_back(k);
        }
        const BloomFilter bf = BloomFilter::build(keys);
        for (const Key& k : keys) {
            ++total_keys;
            if (!bf.maybe_contains(k)) {
                ++false_negatives;  // a PRESENT key reported absent — fatal.
            }
        }
    }
    CHECK(false_negatives == 0,
          "(D) bloom: %llu FALSE NEGATIVES over %llu present keys — a present key "
          "was skipped (V correctness of the read path VIOLATED)",
          static_cast<unsigned long long>(false_negatives),
          static_cast<unsigned long long>(total_keys));
    std::fprintf(stderr,
                 "[ok] (D) bloom: %llu present keys across %llu tables, 0 false "
                 "negatives (present key never skipped)\n",
                 static_cast<unsigned long long>(total_keys),
                 static_cast<unsigned long long>(n_tables));
}

// ===========================================================================
// (E) DETERMINISM — same seed ⇒ byte-identical recovered reads + on-disk image.
// ===========================================================================
void test_determinism() {
    const std::uint64_t seed = 24680;
    const std::uint64_t op_count = 90;
    const std::uint64_t crash_step = 53;
    CrashScenario a = run_crash(seed, op_count, crash_step);
    CrashScenario b = run_crash(seed, op_count, crash_step);
    CHECK(a.recovered_tip == b.recovered_tip,
          "(E) determinism: recovered tip differs (%llu vs %llu)",
          static_cast<unsigned long long>(a.recovered_tip),
          static_cast<unsigned long long>(b.recovered_tip));
    CHECK(a.wal_fingerprint == b.wal_fingerprint,
          "(E) determinism: WAL durable fingerprint differs (%zu vs %zu bytes)",
          a.wal_fingerprint.size(), b.wal_fingerprint.size());
    CHECK(a.sst_fingerprint == b.sst_fingerprint,
          "(E) determinism: SSTable durable fingerprint differs (%zu vs %zu bytes)",
          a.sst_fingerprint.size(), b.sst_fingerprint.size());
    CHECK(a.probe_reads == b.probe_reads,
          "(E) determinism: recovered probe reads differ");
    std::fprintf(stderr,
                 "[ok] (E) determinism: seed %llu replays byte-identical (tip=%llu, "
                 "wal=%zu B, sst=%zu B, %zu probe reads identical)\n",
                 static_cast<unsigned long long>(seed),
                 static_cast<unsigned long long>(a.recovered_tip),
                 a.wal_fingerprint.size(), a.sst_fingerprint.size(),
                 a.probe_reads.size());
}

// ===========================================================================
// (F) W2 FORMAT VERSION — the footer carries a format_version; the reader accepts
// version==known and REFUSES an unknown (future) version, fail-closed. The refuse
// is distinct from a CRC failure: we re-CRC the footer after bumping the version so
// the table is byte-consistent and ONLY the version is unexpected — proving the
// version gate rejects on its own, not incidentally via the checksum. This is the
// W2.4 canary teeth: a later format bump that forgets to handle old images shows up
// here as a table the current reader must reject rather than silently mis-decode.
// ===========================================================================
void test_format_version() {
    using lockstep::storage::SSTableReader;
    using lockstep::storage::SSTableLoader;
    using lockstep::storage::kSstFooterBytes;
    using lockstep::storage::format::kSstableVersion;

    // Build a normal, current-version SSTable image.
    std::vector<lockstep::storage::SstEntry> entries;
    entries.push_back({"alpha", "one", 1, false, false});
    entries.push_back({"bravo", "two", 2, false, false});
    entries.push_back({"charlie", "three", 3, false, false});
    const auto res = SSTableBuilder::build(entries);
    std::vector<std::byte> img = res.bytes;

    // Positive: the current-version image parses.
    SSTableReader r_ok;
    CHECK(SSTableLoader::parse(img, /*id=*/1, r_ok),
          "(F) version: a current-version (v%u) SSTable must parse",
          static_cast<unsigned>(kSstableVersion));

    // Footer version field sits at [n-52 .. n-48); magic [n-48..n-44); crc [n-4..n).
    const std::size_t n = img.size();
    std::byte* f = img.data() + (n - kSstFooterBytes);
    // Sanity: the field really holds the known version.
    CHECK(lockstep::storage::get_u32(f + 40) == kSstableVersion,
          "(F) version: footer version field mismatch (expected %u)",
          static_cast<unsigned>(kSstableVersion));

    // Overwrite a u32 in place, little-endian (matches Codec put_u32).
    const auto poke_u32 = [](std::byte* p, std::uint32_t v) {
        p[0] = static_cast<std::byte>(v & 0xFFu);
        p[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
        p[2] = static_cast<std::byte>((v >> 16) & 0xFFu);
        p[3] = static_cast<std::byte>((v >> 24) & 0xFFu);
    };
    // Bump to an unknown FUTURE version and re-CRC so the image is byte-consistent.
    poke_u32(f + 40, kSstableVersion + 1000u);
    const std::uint32_t recrc = lockstep::storage::Crc32::compute(
        std::span<const std::byte>(f, kSstFooterBytes - 4));
    poke_u32(f + (kSstFooterBytes - 4), recrc);

    // Teeth: the current reader MUST refuse the future-version image (fail-closed),
    // NOT mis-decode it. (Pre-W2 the reader had no version field to check → it would
    // parse the bytes as if current.)
    SSTableReader r_future;
    CHECK(!SSTableLoader::parse(img, /*id=*/1, r_future),
          "(F) version: a FUTURE-version (v%u) SSTable must be REFUSED, not parsed",
          static_cast<unsigned>(kSstableVersion + 1000u));

    std::fprintf(stderr,
                 "[ok] (F) format version: v%u parses; future v%u refused (fail-closed)\n",
                 static_cast<unsigned>(kSstableVersion),
                 static_cast<unsigned>(kSstableVersion + 1000u));
}

}  // namespace

// K4.9: adopt_built == parse — the reader the engine installs for its own built
// image must be INDISTINGUISHABLE from the reader recovery would decode from the
// same bytes: identical blocks, index keys, seq bounds, lookups, and bloom answers.
void test_adopt_equals_parse() {
    using namespace lockstep::storage;
    std::vector<SstEntry> entries;
    for (int k = 0; k < 500; ++k) {
        const std::string key = "k" + std::to_string(1000 + k);
        entries.push_back(SstEntry{key, "v" + std::to_string(k), Seq(k * 2 + 1), false, false});
        if (k % 7 == 0) {  // extra version + tombstones sprinkled in
            entries.push_back(SstEntry{key, "", Seq(k * 2 + 2), true, false});
        }
    }
    const SstBuildResult built = SSTableBuilder::build(entries);
    SSTableReader parsed, adopted;
    CHECK(SSTableLoader::parse(built.bytes, 42, parsed), "loader parses own image");
    adopted.adopt_built(built, 42);
    CHECK(parsed.block_count() == adopted.block_count(), "block counts equal");
    for (std::size_t b = 0; b < parsed.block_count(); ++b) {
        const auto& pb = parsed.block(b);
        const auto& ab = adopted.block(b);
        CHECK(pb.size() == ab.size(), "block %zu size equal", b);
        for (std::size_t i = 0; i < pb.size() && i < ab.size(); ++i) {
            CHECK(pb[i].key == ab[i].key && pb[i].value == ab[i].value &&
                      pb[i].seq == ab[i].seq && pb[i].tombstone == ab[i].tombstone &&
                      pb[i].vlog == ab[i].vlog,
                  "block %zu entry %zu identical", b, i);
        }
    }
    CHECK(parsed.min_seq == adopted.min_seq && parsed.max_seq == adopted.max_seq,
          "seq bounds equal");
    for (const SstEntry& e : entries) {  // every key: same hit through both readers
        const auto ph = parsed.lookup(e.key, Seq(100000));
        const auto ah = adopted.lookup(e.key, Seq(100000));
        CHECK(ph.covered == ah.covered && ph.seq == ah.seq && ph.value == ah.value &&
                  ph.tombstone == ah.tombstone,
              "lookup('%s') identical", e.key.c_str());
    }
    const auto pm = parsed.lookup("zz-missing", Seq(100000));
    const auto am = adopted.lookup("zz-missing", Seq(100000));
    CHECK(pm.covered == am.covered, "missing-key answer identical");
}

// K4.16: size-classed tiering — sustained ingest must keep the live table count
// LOG-bounded (not linear in flushes), and every key must stay readable through
// the cascade of same-bucket merges.
void test_size_classed_tiering() {
    using namespace lockstep::storage;
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock(sched);
    lockstep::sim::SeededRandom rng(0x516ull);
    lockstep::sim::DiskFaultConfig dc;
    lockstep::sim::SimDisk wal(sched, clock, rng, dc), man(sched, clock, rng, dc);
    VecDiskFactory fac(sched, clock, rng, dc);
    WalEngine e(sched, wal, man, fac, /*flush_threshold=*/8);
    e.set_compaction_trigger(2);
    constexpr int kKeys = 400;  // ~50 flushes -> without classing, huge rewrite churn
    sched.spawn([](WalEngine& w) -> lockstep::core::Task {
        for (int i = 0; i < kKeys; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "k%06d", i);
            (void)co_await w.put(buf, "v" + std::to_string(i));
        }
        (void)co_await w.sync();
        co_return;
    }(e));
    sched.run();
    // Table count must be log-ish, not ~50 (unmerged) and not 1 (merge-everything).
    CHECK(e.sstable_count() >= 2 && e.sstable_count() <= 16,
          "size-classed tiering: live table count log-bounded (got %zu)", e.sstable_count());
    // Every key readable through the merged cascade.
    int missing = 0;
    sched.spawn([&missing](WalEngine& w) -> lockstep::core::Task {
        for (int i = 0; i < kKeys; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "k%06d", i);
            const auto v = co_await w.get(buf, lockstep::storage::Snapshot{w.last_seq()});
            if (!v.has_value() || *v != "v" + std::to_string(i)) ++missing;
        }
        co_return;
    }(e));
    sched.run();
    CHECK(missing == 0, "all %d keys readable after cascaded merges (missing %d)", kKeys, missing);
}

int main() {
    std::fprintf(stderr, "=== storage_sstable_test (flush + SSTable + manifest + scan) ===\n");
    test_adopt_equals_parse();
    test_size_classed_tiering();
    test_differential_with_flush();
    test_metamorphic_flush();
    test_crash_during_flush();
    test_bloom_no_false_negative();
    test_determinism();
    test_format_version();
    if (g_failures != 0) {
        std::fprintf(stderr, "storage_sstable_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "storage_sstable_test: ALL PASS\n");
    return 0;
}
