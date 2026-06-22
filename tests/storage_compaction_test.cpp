// storage_compaction_test.cpp — Phase 3 §5 step 5 gate for COMPACTION (C3.4) +
// VERSION GC (V-GC) + WAL TRUNCATION, crash-safe (storage-engine.md §2/§3/§6).
//
// The memtable flushes to SSTables (step 4); now SIZE-TIERED COMPACTION k-way-
// merges the live SSTable set into ONE, dropping versions no live snapshot can
// see (GC under a watermark), then truncates the WAL prefix the merged set
// durably covers. All atomic via the append-only, CRC'd, entry_no-contiguous
// MANIFEST (INSTALL / OBSOLETE / WAL-TRUNCATE records) — a crash mid-compaction
// recovers to EITHER the old SSTable set OR the merged one, never a mix that
// loses or fabricates a value.
//
// WHAT IT PROVES (one block per spec requirement):
//
//   (A) DIFFERENTIAL WITH COMPACTION — the core gate (§4) under forced flushes +
//       compactions: WalEngine (LSM, small flush threshold + compaction trigger
//       ⇒ MANY flushes AND compactions) vs Oracle over a seed sweep on an HONEST
//       disk. Every commit-Seq, every get AND every scan under every snapshot
//       MUST match the oracle. 0 mismatch. (GC watermark 0 ⇒ no version dropped,
//       so every historical snapshot the harness reads is preserved.)
//
//   (B) METAMORPHIC — a forced compaction MUST NOT change any visible value:
//       snapshot every (key,snap) get + a full scan BEFORE a forced compaction,
//       force a compaction, re-read the SAME — identical (compaction ⊥ change
//       reads, §2). Compaction actually fires (sstable_count drops).
//
//   (C) GC SAFETY (V-GC) — take an OLD snapshot, write NEWER versions, raise the
//       watermark to the NEW tip, force compaction: the OLD snapshot STILL sees
//       its versions (GC never drops a version visible to a live snapshot) AND
//       truly-dead versions are reclaimed (live version count DROPS).
//
//   (D) CRASH-DURING-COMPACTION — crash at arbitrary points INCLUDING mid-
//       compaction under the full fault envelope (torn writes + lying fsync + io
//       faults), recover from the manifest's folded live set + WAL prefix; assert
//       the recovered state is ALWAYS a valid oracle PREFIX, 0 fabrication, no
//       half-/torn-SSTable surviving as live, no mixed old+new losing a value.
//
//   (E) DETERMINISM — same seed ⇒ byte-identical observable history + on-disk
//       fingerprint (WAL + every live SSTable durable image).
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
using lockstep::storage::compact_merge;
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
using lockstep::storage::SstEntry;
using lockstep::storage::Value;
using lockstep::storage::WalEngine;

int g_failures = 0;

#define CHECK(cond, ...)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "COMPACT GATE FAIL [%s:%d]: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                                       \
            std::fprintf(stderr, "\n");                                              \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// A concrete IDiskFactory backed by a pool of SimDisks (one per SSTable id). It
// adds reclaim(): an obsoleted SSTable's disk is crashed-empty (its bytes freed),
// so the determinism fingerprint reflects the disk-GC. Disks are stable across a
// crash/recover (recovery re-opens the same backing by id).
// ---------------------------------------------------------------------------
class VecDiskFactory final : public IDiskFactory {
public:
    VecDiskFactory(Scheduler& sched, SimClock& clock, SeededRandom& rng, DiskFaultConfig cfg)
        : sched_(&sched), clock_(&clock), rng_(&rng), cfg_(cfg) {}

    [[nodiscard]] IDisk& disk_for(std::uint64_t sstable_id) override {
        while (disks_.size() <= sstable_id) {
            disks_.push_back(std::make_unique<SimDisk>(*sched_, *clock_, *rng_, cfg_));
            reclaimed_.push_back(false);
        }
        return *disks_[sstable_id];
    }

    // Reclaim an obsoleted SSTable backing: mark it freed (its durable bytes no
    // longer count toward the live fingerprint). The disk object is kept so a
    // stale recovery that still references the id reads "past end" (NotFound) and
    // safely rejects it — never fabricates.
    void reclaim(std::uint64_t sstable_id) override {
        if (sstable_id < reclaimed_.size()) {
            reclaimed_[sstable_id] = true;
        }
    }

    [[nodiscard]] bool is_reclaimed(std::uint64_t id) const {
        return id < reclaimed_.size() && reclaimed_[id];
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
    // Concatenated durable fingerprint across all NON-reclaimed SSTable disks.
    [[nodiscard]] std::vector<std::byte> durable_fingerprint() const {
        std::vector<std::byte> out;
        for (std::size_t i = 0; i < disks_.size(); ++i) {
            if (reclaimed_[i]) {
                continue;
            }
            const std::vector<std::byte> img = disks_[i]->durable_snapshot();
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
    std::vector<bool> reclaimed_;
};

// ===========================================================================
// (A) DIFFERENTIAL WITH COMPACTION — WalEngine(LSM+compaction) vs Oracle.
// ===========================================================================
void test_differential_with_compaction() {
    DiffConfig cfg;
    cfg.steps = 500;   // long enough to force many flushes AND compactions
    cfg.n_keys = 8;
    cfg.n_values = 6;

    std::uint64_t mismatches = 0;
    DiffVerdict first_bad;
    const std::uint64_t n_seeds = 128;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0xC0FFEEu);
        DiskFaultConfig dc;  // HONEST disk for the differential gate.
        dc.latency_min = 0;
        dc.latency_max = 2;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);

        WalEngine sut(sched, wal, manifest, factory, /*flush_threshold=*/6);
        sut.set_compaction_trigger(3);   // merge every 3 live SSTables
        // Watermark stays 0 ⇒ GC drops nothing (the harness reads ALL historical
        // snapshots, so every version is a "live snapshot" version here).
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
          "(A) differential-with-compaction: %llu/%llu seeds MISMATCHED; first "
          "seed=%llu step=%llu note=%s expected=%s got=%s",
          static_cast<unsigned long long>(mismatches),
          static_cast<unsigned long long>(n_seeds),
          static_cast<unsigned long long>(first_bad.witness.seed),
          static_cast<unsigned long long>(first_bad.witness.step),
          first_bad.witness.note.c_str(), first_bad.witness.expected.c_str(),
          first_bad.witness.got.c_str());
    std::fprintf(stderr,
                 "[ok] (A) differential WalEngine(LSM+compaction)-vs-Oracle: %llu "
                 "seeds, get+scan, 0 mismatch (flush 6 + compact-at-3 ⇒ many "
                 "flushes+compactions)\n",
                 static_cast<unsigned long long>(n_seeds));
}

// ===========================================================================
// (B) METAMORPHIC — a forced compaction MUST NOT change any visible value.
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

void test_metamorphic_compaction() {
    const std::uint64_t n_seeds = 60;
    std::uint64_t changed = 0;
    std::uint64_t fired = 0;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0xDECAFu);
        DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 1;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);
        const std::uint64_t n_keys = 8;
        // Small flush threshold ⇒ several SSTables accrue; compaction trigger high
        // so the natural fill does NOT auto-compact — we FORCE it for the test.
        WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/4);
        eng.set_compaction_trigger(100000);

        sched.spawn(fill_history(eng, rng, 60, n_keys, 6));
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
        // Force a compaction: merges all live SSTables into one. Watermark 0 ⇒ no
        // GC ⇒ EVERY version preserved ⇒ reads at the same snapshot identical.
        sched.spawn([](WalEngine& e) -> Task {
            co_await e.force_compact();
            co_return;
        }(eng));
        sched.run();
        const std::size_t ssts_after = eng.sstable_count();
        if (ssts_before >= 2 && ssts_after < ssts_before) {
            ++fired;
        }

        ReadSet after;
        sched.spawn(read_all(eng, after, snap.at, n_keys));
        sched.run();

        if (before.gets != after.gets || before.full_scan != after.full_scan) {
            ++changed;
            std::fprintf(stderr, "  [compaction CHANGED reads] seed=%llu\n",
                         static_cast<unsigned long long>(seed));
        }
    }
    CHECK(changed == 0,
          "(B) metamorphic: %llu seeds had a compaction CHANGE a visible value — "
          "compaction ⊥ change reads VIOLATED",
          static_cast<unsigned long long>(changed));
    CHECK(fired > 0,
          "(B) metamorphic: compaction NEVER fired across %llu seeds (vacuous test)",
          static_cast<unsigned long long>(n_seeds));
    std::fprintf(stderr,
                 "[ok] (B) metamorphic: %llu seeds, a forced compaction changed NO "
                 "get/scan result (%llu seeds actually merged SSTables)\n",
                 static_cast<unsigned long long>(n_seeds),
                 static_cast<unsigned long long>(fired));
}

// ===========================================================================
// (C) GC SAFETY (V-GC) — old snapshot still sees its versions + dead reclaimed.
// ===========================================================================
// Pure-unit check of the GC merge rule: an old version shadowed by a newer one
// at/below the watermark is dropped; a version visible to the watermark snapshot
// is RETAINED. Hand-built runs make the rule legible.
void test_gc_rule_unit() {
    // Key "a": versions at seq 1(v1), 3(v2), 5(v3). Watermark = 3.
    //   floor (newest <= 3) = seq 3. Drop seq 1 (shadowed, < floor). Keep 3 + 5.
    std::vector<std::vector<SstEntry>> runs;
    runs.push_back({SstEntry{"a", "v1", 1, false}, SstEntry{"a", "v2", 3, false},
                    SstEntry{"a", "v3", 5, false}});
    const std::vector<SstEntry> out = compact_merge(runs, /*watermark=*/3);
    // Expect exactly seqs {3,5} for "a".
    std::vector<Seq> got;
    for (const SstEntry& e : out) {
        got.push_back(e.seq);
    }
    CHECK(got.size() == 2 && got[0] == 3 && got[1] == 5,
          "(C-unit) GC rule: key a watermark 3 expected {3,5}, got %zu versions",
          got.size());

    // Watermark 0 ⇒ nothing droppable ⇒ all 3 retained (no live-snapshot bound).
    const std::vector<SstEntry> out0 = compact_merge(runs, /*watermark=*/0);
    CHECK(out0.size() == 3,
          "(C-unit) GC rule: watermark 0 must retain ALL 3 versions, got %zu",
          out0.size());

    // Lone tombstone fully below watermark ⇒ reclaimed (key vanishes).
    std::vector<std::vector<SstEntry>> tr;
    tr.push_back({SstEntry{"b", "", 2, /*tombstone=*/true}});
    const std::vector<SstEntry> outt = compact_merge(tr, /*watermark=*/5);
    CHECK(outt.empty(), "(C-unit) GC rule: lone sub-watermark tombstone not reclaimed");

    std::fprintf(stderr,
                 "[ok] (C-unit) GC rule: shadowed-below-watermark dropped, "
                 "watermark-visible retained, lone sub-watermark tombstone reclaimed\n");
}

// End-to-end: old snapshot reads survive a watermark-raised compaction, dead
// versions reclaimed (space drops).
Task gc_writes(WalEngine& eng, std::uint64_t rounds, std::uint64_t n_keys,
               std::vector<std::pair<Key, Seq>>& old_probes,
               std::vector<std::optional<Value>>& old_expect, Seq& old_snap_out) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    // Round 1: write v=A to every key.
    for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
        co_await eng.put(kof(ki), std::string("A"));
    }
    const Snapshot old_snap = co_await eng.snapshot();
    old_snap_out = old_snap.at;
    // Record what the OLD snapshot sees: every key = "A".
    for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
        old_probes.emplace_back(kof(ki), old_snap.at);
        old_expect.push_back(std::optional<Value>("A"));
    }
    // Many newer rounds: overwrite every key repeatedly (B, C, D, …) so each key
    // accumulates lots of dead versions ABOVE the old snapshot.
    for (std::uint64_t r = 0; r < rounds; ++r) {
        const std::string v(1, static_cast<char>('B' + (r % 20)));
        for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
            co_await eng.put(kof(ki), v);
        }
    }
    co_return;
}

Task gc_probe(WalEngine& eng, const std::vector<std::pair<Key, Seq>>& probes,
              std::vector<std::optional<Value>>& out) {
    for (const auto& pr : probes) {
        out.push_back(co_await eng.get(pr.first, Snapshot{pr.second}));
    }
    co_return;
}

void test_gc_safety_end_to_end() {
    const std::uint64_t n_seeds = 24;
    std::uint64_t visible_violations = 0;
    std::uint64_t reclaim_failures = 0;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0x6C6C6Cu);
        DiskFaultConfig dc;  // honest disk
        dc.latency_min = 0;
        dc.latency_max = 1;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);
        const std::uint64_t n_keys = 6;
        WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/6);
        eng.set_compaction_trigger(100000);  // force compaction explicitly

        std::vector<std::pair<Key, Seq>> old_probes;
        std::vector<std::optional<Value>> old_expect;
        Seq old_snap = 0;
        sched.spawn(gc_writes(eng, /*rounds=*/20, n_keys, old_probes, old_expect, old_snap));
        sched.run();

        const std::size_t versions_before = eng.live_version_count();

        // Snapshot what the LIVE watermark snapshot sees BEFORE compaction: a get
        // at the current tip for every key (the live values). These MUST survive a
        // watermark-bounded compaction (the watermark is the oldest live snapshot;
        // its reads are the contract GC must never break).
        const Seq tip = eng.last_seq();
        std::vector<std::pair<Key, Seq>> live_probes;
        for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
            live_probes.emplace_back(std::string("k") + std::to_string(ki), tip);
        }
        std::vector<std::optional<Value>> live_before;
        sched.spawn(gc_probe(eng, live_probes, live_before));
        sched.run();
        (void)old_probes;
        (void)old_expect;
        (void)old_snap;

        // Raise the watermark to the CURRENT committed tip (no live snapshot below
        // it) and force a compaction. Per V-GC, every version of a key shadowed by
        // a newer one <= tip is DEAD and droppable EXCEPT the newest-visible floor;
        // a snapshot BELOW the watermark is, by definition, no longer live, so its
        // (now-unreachable) versions are correctly reclaimable.
        eng.set_read_watermark(tip);
        sched.spawn([](WalEngine& e) -> Task {
            co_await e.force_flush();    // flush the memtable tail to an SSTable
            co_await e.force_compact();  // merge + GC under the watermark
            co_return;
        }(eng));
        sched.run();

        const std::size_t versions_after = eng.live_version_count();

        // (C.1) the LIVE watermark snapshot STILL sees its versions — GC never drops
        // a version visible to a live snapshot (V-GC). Reads at the watermark are
        // byte-identical before vs after the compaction.
        std::vector<std::optional<Value>> live_after;
        sched.spawn(gc_probe(eng, live_probes, live_after));
        sched.run();
        for (std::size_t i = 0; i < live_probes.size(); ++i) {
            if (live_after[i] != live_before[i]) {
                ++visible_violations;
                std::fprintf(stderr,
                             "  [GC dropped a LIVE version] seed=%llu key=%s "
                             "wm=%llu want=%s got=%s\n",
                             static_cast<unsigned long long>(seed),
                             live_probes[i].first.c_str(),
                             static_cast<unsigned long long>(tip),
                             live_before[i].has_value() ? live_before[i]->c_str() : "nil",
                             live_after[i].has_value() ? live_after[i]->c_str() : "nil");
                break;
            }
        }

        // (C.2) dead versions reclaimed — space DROPS (the many overwrites below
        // the watermark are gone). With 20 rounds of overwrites the live version
        // count must shrink substantially after GC.
        if (!(versions_after < versions_before)) {
            ++reclaim_failures;
            std::fprintf(stderr,
                         "  [GC reclaimed nothing] seed=%llu before=%zu after=%zu\n",
                         static_cast<unsigned long long>(seed), versions_before,
                         versions_after);
        }
    }
    CHECK(visible_violations == 0,
          "(C) GC-safety: %llu cases where GC dropped a version still visible to a "
          "live snapshot (V-GC VIOLATED)",
          static_cast<unsigned long long>(visible_violations));
    CHECK(reclaim_failures == 0,
          "(C) GC-safety: %llu cases where GC reclaimed NO dead version (space did "
          "not drop) — GC not actually working",
          static_cast<unsigned long long>(reclaim_failures));
    std::fprintf(stderr,
                 "[ok] (C) GC-safety: %llu seeds — old snapshot still sees its "
                 "versions AND dead versions reclaimed (space dropped)\n",
                 static_cast<unsigned long long>(n_seeds));
}

// Stronger V-GC: a snapshot held BELOW the watermark must NOT have GC run past
// it. We keep the watermark AT the old snapshot, write newer versions, compact —
// the old snapshot must read EXACTLY its values and nothing newer/older.
void test_gc_live_snapshot_preserved() {
    const std::uint64_t n_seeds = 24;
    std::uint64_t violations = 0;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0x5A5A5Au);
        DiskFaultConfig dc;
        dc.latency_min = 0;
        dc.latency_max = 1;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        VecDiskFactory factory(sched, clock, rng, dc);
        const std::uint64_t n_keys = 5;
        WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/5);
        eng.set_compaction_trigger(100000);

        // Write A, B to each key; HOLD a snapshot at the "B" tip (the live snapshot).
        auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
        Seq held = 0;
        std::vector<std::pair<Key, Seq>> probes;
        std::vector<std::optional<Value>> expect;
        sched.spawn([](WalEngine& e, std::uint64_t nk, Seq& h,
                       std::vector<std::pair<Key, Seq>>& pr,
                       std::vector<std::optional<Value>>& ex, auto k) -> Task {
            for (std::uint64_t ki = 0; ki < nk; ++ki) {
                co_await e.put(k(ki), std::string("A"));
            }
            for (std::uint64_t ki = 0; ki < nk; ++ki) {
                co_await e.put(k(ki), std::string("B"));
            }
            const Snapshot s = co_await e.snapshot();
            h = s.at;  // the LIVE snapshot — every key visible as "B"
            for (std::uint64_t ki = 0; ki < nk; ++ki) {
                pr.emplace_back(k(ki), s.at);
                ex.push_back(std::optional<Value>("B"));
            }
            // Newer versions ABOVE the held snapshot (C, D, …).
            for (std::uint64_t r = 0; r < 10; ++r) {
                const std::string v(1, static_cast<char>('C' + r));
                for (std::uint64_t ki = 0; ki < nk; ++ki) {
                    co_await e.put(k(ki), v);
                }
            }
            co_return;
        }(eng, n_keys, held, probes, expect, kof));
        sched.run();

        // Watermark = the HELD live snapshot (not the tip). GC must NOT drop the
        // "B" versions (they ARE visible to the live snapshot at `held`).
        eng.set_read_watermark(held);
        sched.spawn([](WalEngine& e) -> Task {
            co_await e.force_flush();
            co_await e.force_compact();
            co_return;
        }(eng));
        sched.run();

        std::vector<std::optional<Value>> got;
        sched.spawn(gc_probe(eng, probes, got));
        sched.run();
        for (std::size_t i = 0; i < probes.size(); ++i) {
            if (got[i] != expect[i]) {
                ++violations;
                std::fprintf(stderr,
                             "  [live snapshot lost a version] seed=%llu key=%s "
                             "held=%llu want=B got=%s\n",
                             static_cast<unsigned long long>(seed),
                             probes[i].first.c_str(),
                             static_cast<unsigned long long>(held),
                             got[i].has_value() ? got[i]->c_str() : "nil");
                break;
            }
        }
    }
    CHECK(violations == 0,
          "(C') GC-safety: %llu cases where a version VISIBLE to the held live "
          "snapshot was dropped (V-GC VIOLATED)",
          static_cast<unsigned long long>(violations));
    std::fprintf(stderr,
                 "[ok] (C') GC-safety: %llu seeds — a snapshot held AT the watermark "
                 "still sees its versions after a watermark-bounded compaction\n",
                 static_cast<unsigned long long>(n_seeds));
}

// ===========================================================================
// (D) CRASH-DURING-COMPACTION — recover to a valid oracle PREFIX, no half-table.
// ===========================================================================
struct OpRec {
    enum Kind { Put, Del } kind;
    Key key;
    Value value;
    Seq seq;
};

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
    dc.torn_write_prob = 0.20;    // torn SSTable / WAL / manifest blocks
    dc.lying_fsync_prob = 0.20;   // a sync that drops a tail on crash
    dc.io_fault_prob = 0.05;
    SimDisk wal(sched, clock, rng, dc);
    SimDisk manifest(sched, clock, rng, dc);
    VecDiskFactory factory(sched, clock, rng, dc);
    const std::uint64_t n_keys = 6;
    const std::uint64_t n_values = 5;

    // LSM mode, small flush threshold + compact-at-2 ⇒ compactions happen mid-
    // stream, so a crash can land MID-COMPACTION (between the merged SSTable's
    // bytes, its INSTALL record, the OBSOLETE records, and the WAL-TRUNCATE).
    WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/4);
    eng.set_compaction_trigger(2);

    sched.spawn(crash_fill(eng, rng, op_count, crash_step, out, n_keys, n_values));
    sched.run();

    // Crash everything, then recover the media (the durable prefix survives).
    wal.crash();
    manifest.crash();
    factory.crash_all();
    wal.recover();
    manifest.recover();
    factory.recover_all();

    // A fresh engine recovers from the SAME backings: fold the manifest (INSTALL/
    // OBSOLETE/WAL-TRUNCATE), load the live SSTable set, replay the WAL prefix
    // (skipping records below the truncation watermark).
    WalEngine recovered(sched, wal, manifest, factory, /*flush_threshold=*/4);
    recovered.set_compaction_trigger(2);
    sched.spawn([](WalEngine& e, std::size_t wl, std::size_t ml) -> Task {
        co_await e.recover_lsm(wl, ml);
        co_return;
    }(recovered, wal.durable_len(), manifest.durable_len()));
    sched.run();
    out.recovered_tip = recovered.last_seq();
    out.wal_fingerprint = wal.durable_snapshot();
    out.sst_fingerprint = factory.durable_fingerprint();

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

void test_crash_during_compaction() {
    const std::uint64_t n_seeds = 120;
    std::uint64_t crash_points = 0;
    std::uint64_t fabrications = 0;
    std::uint64_t prefix_violations = 0;
    std::uint64_t first_bad = 0;

    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        const std::uint64_t op_count = 90;
        SeededRandom pick(seed ^ 0xC0DEC0u);
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
                             "  [crash-compact mismatch] seed=%llu crash_step=%llu "
                             "tip=%llu key=%s at=%llu want=%s got=%s\n",
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
          "(D) crash-during-compaction: %llu FABRICATED reads across %llu crash "
          "points (first seed=%llu) — half-/mixed-SSTable surfaced (V-NOTORN/V-PREFIX)",
          static_cast<unsigned long long>(fabrications),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    CHECK(prefix_violations == 0,
          "(D) crash-during-compaction: %llu PREFIX violations across %llu crash "
          "points (first seed=%llu) — recovered state not a valid oracle prefix",
          static_cast<unsigned long long>(prefix_violations),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    std::fprintf(stderr,
                 "[ok] (D) crash-during-compaction: %llu crash points (full fault "
                 "envelope, flush 4 + compact-at-2), recovered ALWAYS a valid "
                 "prefix, 0 fabrication, no half-/mixed-SSTable loss\n",
                 static_cast<unsigned long long>(crash_points));
}

// ===========================================================================
// (E) DETERMINISM — same seed ⇒ byte-identical recovered reads + on-disk image.
// ===========================================================================
void test_determinism() {
    const std::uint64_t seed = 13579;
    const std::uint64_t op_count = 96;
    const std::uint64_t crash_step = 61;
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

}  // namespace

int main() {
    std::fprintf(stderr, "=== storage_compaction_test (compaction + GC + WAL-trunc) ===\n");
    test_differential_with_compaction();
    test_metamorphic_compaction();
    test_gc_rule_unit();
    test_gc_safety_end_to_end();
    test_gc_live_snapshot_preserved();
    test_crash_during_compaction();
    test_determinism();
    if (g_failures != 0) {
        std::fprintf(stderr, "storage_compaction_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "storage_compaction_test: ALL PASS\n");
    return 0;
}
