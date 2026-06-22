#pragma once

// Bench.hpp — Phase 3 §5 step 7 (C3.7). The DETERMINISTIC benchmark harness that
// SETTLES D4 tuning EMPIRICALLY (master-plan D4 / storage-engine.md §6: "tuning
// decided empirically via the benchmark harness, not guessed"). It drives a
// configurable, SEEDED workload (put/get/scan/del mixes, hand-rolled key/value
// size distributions) against the WalEngine (LSM) over SimDisk and measures, per
// config:
//
//   * WRITE THROUGHPUT = write ops / virtual-time-units consumed by the write
//     phase. READ THROUGHPUT = read ops / virtual-time-units of the read phase.
//     Time is the scheduler's VIRTUAL clock (core::Tick) — NOT wall-clock; the
//     sim disk models per-op latency, so vtime is the modeled cost. Deterministic.
//
//   * WRITE AMPLIFICATION = total bytes the engine APPENDED to every IDisk (WAL +
//     manifest + every SSTable) / the user-data bytes written (Σ key+value bytes
//     over committed puts/dels). LSM rewrites data on flush, so this exceeds 1.
//     We instrument it with a COUNTING IDisk WRAPPER (CountingDisk) around each
//     SimDisk — every append() the engine issues adds its requested length to a
//     shared counter, so we measure exactly what the engine wrote, disk-agnostic.
//
//   * SPACE AMPLIFICATION = on-disk DURABLE bytes (Σ durable_len over WAL +
//     manifest + SSTables after a final sync) / LIVE user-data bytes (Σ key+value
//     of the live, non-tombstoned versions at the final snapshot). LSM keeps the
//     full WAL + every flushed SSTable (no compaction landed yet), so this is the
//     headline number compaction must drive down — exactly what D4 tuning weighs.
//
// SWEEP DIMENSIONS (the knobs that matter for D4): block size, bloom bits-per-key,
// flush threshold (memtable version count). See BenchConfig + the note in
// bench_main.cpp about which knobs WalEngine currently EXPOSES vs which the engine
// should be extended to expose (recommendation back to the engine owner).
//
// DETERMINISM (the cardinal property): the whole bench is a PURE FUNCTION of
// (seed, BenchConfig). Same seed+config ⇒ byte-identical numbers (virtual time +
// byte counts are deterministic; this is a SIM benchmark measuring MODELED cost,
// reproducible, never wall-clock). No std::*_distribution (we hand-roll size +
// op-mix draws from the sim SeededRandom). No threads, no ambient clock.
//
// FORBIDDEN (bench/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, <random>, raw file IO. All randomness is the injected sim
// SeededRandom; all time is the scheduler's virtual tick.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/SSTable.hpp>
#include <lockstep/storage/WalEngine.hpp>

namespace lockstep::bench {

using core::Error;
using core::IDisk;
using core::Offset;
using core::Scheduler;
using core::SimClock;
using core::Task;
using sim::DiskFaultConfig;
using sim::SeededRandom;
using sim::SimDisk;
using storage::IDiskFactory;
using storage::Key;
using storage::Range;
using storage::Seq;
using storage::Snapshot;
using storage::Value;
using storage::WalEngine;

// ---------------------------------------------------------------------------
// ByteMeter — a shared, deterministic counter of bytes the engine APPENDED.
// Each CountingDisk adds the requested append length here. This is the user's
// view of write traffic to the device (what the engine asked to persist), which
// is the numerator of write amplification. A plain accumulator — no atomics
// (single-threaded cooperative scheduler), no float.
// ---------------------------------------------------------------------------
struct ByteMeter {
    std::uint64_t appended_bytes = 0;  // Σ requested append lengths across disks.
    void add(std::uint64_t n) noexcept { appended_bytes += n; }
};

// ---------------------------------------------------------------------------
// CountingDisk — a transparent IDisk decorator that forwards every op to an
// inner SimDisk and records the requested append length in a shared ByteMeter.
// It counts the REQUESTED length (not the torn/kept length) because write
// amplification measures the bytes the engine PUSHED to the device, regardless
// of a modeled torn write. read()/sync() forward unchanged. The decorator owns
// no scheduling — it is a pure pass-through, so determinism is preserved (the
// inner SimDisk still drives latency on the virtual clock).
// ---------------------------------------------------------------------------
class CountingDisk final : public IDisk {
public:
    CountingDisk(SimDisk& inner, ByteMeter& meter) noexcept
        : inner_(&inner), meter_(&meter) {}

    [[nodiscard]] core::Future<Error> append(std::span<const std::byte> data,
                                             Offset& out_offset) override {
        meter_->add(static_cast<std::uint64_t>(data.size()));
        return inner_->append(data, out_offset);
    }
    [[nodiscard]] core::Future<Error> read(Offset at, std::span<std::byte> into) override {
        return inner_->read(at, into);
    }
    [[nodiscard]] core::Future<Error> sync() override { return inner_->sync(); }

    [[nodiscard]] SimDisk& inner() noexcept { return *inner_; }

private:
    SimDisk* inner_;
    ByteMeter* meter_;
};

// ---------------------------------------------------------------------------
// BenchDiskFactory — the IDiskFactory the engine uses to mint per-SSTable disks.
// It owns a pool of SimDisks (the durable backing) AND a parallel pool of
// CountingDisks wrapping them, handing the engine the COUNTING wrapper so SSTable
// flush bytes are metered too. durable_bytes() sums durable_len over all SSTable
// SimDisks (the space-amp on-disk numerator contribution).
// ---------------------------------------------------------------------------
class BenchDiskFactory final : public IDiskFactory {
public:
    BenchDiskFactory(Scheduler& sched, SimClock& clock, SeededRandom& rng,
                     DiskFaultConfig cfg, ByteMeter& meter) noexcept
        : sched_(&sched), clock_(&clock), rng_(&rng), cfg_(cfg), meter_(&meter) {}

    [[nodiscard]] IDisk& disk_for(std::uint64_t sstable_id) override {
        while (sims_.size() <= sstable_id) {
            sims_.push_back(std::make_unique<SimDisk>(*sched_, *clock_, *rng_, cfg_));
            counters_.push_back(
                std::make_unique<CountingDisk>(*sims_.back(), *meter_));
        }
        return *counters_[sstable_id];
    }

    // Durable bytes summed over every SSTable backing (after the final sync).
    [[nodiscard]] std::uint64_t durable_bytes() const noexcept {
        std::uint64_t n = 0;
        for (const auto& d : sims_) {
            n += static_cast<std::uint64_t>(d->durable_len());
        }
        return n;
    }

    [[nodiscard]] std::size_t count() const noexcept { return sims_.size(); }

private:
    Scheduler* sched_;
    SimClock* clock_;
    SeededRandom* rng_;
    DiskFaultConfig cfg_;
    ByteMeter* meter_;
    std::vector<std::unique_ptr<SimDisk>> sims_;
    std::vector<std::unique_ptr<CountingDisk>> counters_;
};

// ---------------------------------------------------------------------------
// BenchConfig — ONE point in the D4 sweep space. Every field is a knob the bench
// varies (or a fixed workload parameter). The triple {block_target_bytes,
// bloom_bits_per_key, flush_threshold} are the D4 tuning dimensions; the rest fix
// the workload so two configs are compared on the SAME op stream per seed.
// ---------------------------------------------------------------------------
struct BenchConfig {
    // --- workload shape (fixed across a sweep so configs are comparable) -----
    std::uint64_t write_ops = 2000;   // put/del ops in the write phase.
    std::uint64_t read_ops = 2000;    // get/scan ops in the read phase.
    std::uint64_t n_keys = 256;       // key space (keys are k<0..n_keys>).
    double del_fraction = 0.15;       // P(a write op is a del vs a put).
    double scan_fraction = 0.10;      // P(a read op is a scan vs a get).
    std::uint64_t value_min = 8;      // value byte length, hand-rolled uniform
    std::uint64_t value_max = 200;    //   over [value_min, value_max].
    std::uint64_t scan_width = 16;    // scan range width in key index units.

    // --- D4 TUNING DIMENSIONS (the sweep axes) -------------------------------
    // Flush threshold: memtable version count that triggers a flush to SSTable.
    // This IS exposed by WalEngine (its ctor arg) → varied directly.
    std::uint64_t flush_threshold = 64;
    // Block target bytes + bloom bits-per-key: SSTable layout knobs. These are
    // CURRENTLY HARD-CODED inside SSTable.hpp (kSstTargetBlockBytes = 256;
    // BloomFilter default bits_per_key = 10, nhash = 7), NOT yet ctor-exposed by
    // WalEngine. The bench records the requested values so the sweep is honest
    // about intent; see bench_main.cpp for the recommendation to expose them. We
    // still report the column so the architect can see which we COULD vary once
    // the engine exposes them (and the bench is ready to vary them the moment it
    // does — only these two fields change).
    std::uint64_t block_target_bytes = 256;  // engine-fixed today (recommendation).
    std::uint64_t bloom_bits_per_key = 10;   // engine-fixed today (recommendation).

    // A short human label for the report row (the config's identity).
    std::string label;
};

// ---------------------------------------------------------------------------
// BenchResult — the measured numbers for one (seed, BenchConfig) run. All are
// deterministic functions of the inputs. Throughput is reported as a fixed-point
// ratio scaled by 1000 (ops per 1000 virtual ticks) to keep the table integer-
// stable across platforms (no float formatting drift in the determinism proof).
// Amplifications are scaled by 1000 similarly (milli-units; 1000 == ratio 1.0).
// ---------------------------------------------------------------------------
struct BenchResult {
    // Raw counters (all integers ⇒ byte-identical across platforms).
    std::uint64_t write_ops = 0;
    std::uint64_t read_ops = 0;
    std::uint64_t write_vticks = 0;     // virtual ticks consumed by the write phase.
    std::uint64_t read_vticks = 0;      // virtual ticks consumed by the read phase.
    std::uint64_t appended_bytes = 0;   // Σ bytes the engine appended (write-amp num).
    std::uint64_t user_write_bytes = 0; // Σ key+value bytes committed (write-amp den).
    std::uint64_t durable_bytes = 0;    // Σ durable on-disk bytes (space-amp num).
    std::uint64_t live_user_bytes = 0;  // Σ key+value of live versions (space-amp den).
    std::uint64_t sstable_count = 0;    // SSTables flushed (a flush-cost signal).

    // Derived, fixed-point (×1000) ratios. milli-ops/tick and milli-ratios.
    [[nodiscard]] std::uint64_t write_tput_milli() const noexcept {
        return write_vticks == 0 ? 0 : (write_ops * 1000ULL) / write_vticks;
    }
    [[nodiscard]] std::uint64_t read_tput_milli() const noexcept {
        return read_vticks == 0 ? 0 : (read_ops * 1000ULL) / read_vticks;
    }
    // True when the read phase advanced NO virtual time: WalEngine resolves
    // get()/scan() fully in memory (all SSTable blocks loaded at flush), so reads
    // are "instant" in the model. The report renders this as "inmem" rather than a
    // misleading 0 throughput. A real finding for the engine recommendation.
    [[nodiscard]] bool reads_in_memory() const noexcept { return read_vticks == 0; }
    [[nodiscard]] std::uint64_t write_amp_milli() const noexcept {
        return user_write_bytes == 0 ? 0
                                      : (appended_bytes * 1000ULL) / user_write_bytes;
    }
    [[nodiscard]] std::uint64_t space_amp_milli() const noexcept {
        return live_user_bytes == 0 ? 0
                                     : (durable_bytes * 1000ULL) / live_user_bytes;
    }
};

// ---------------------------------------------------------------------------
// Live-data tracker — the bench's own trivial reference of the LIVE key→value at
// the final snapshot, so space-amp's denominator (live user bytes) is exact and
// independent of the engine. It mirrors the Oracle's rule (last write wins; a del
// removes the key). Pure, deterministic, in-memory.
// ---------------------------------------------------------------------------
class LiveTracker {
public:
    void put(const Key& k, const Value& v) {
        std::size_t i = find(k);
        if (i < entries_.size() && entries_[i].first == k) {
            entries_[i].second = v;
        } else {
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i),
                            std::pair<Key, Value>{k, v});
        }
    }
    void del(const Key& k) {
        std::size_t i = find(k);
        if (i < entries_.size() && entries_[i].first == k) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // Σ key+value bytes of every live (non-deleted) key.
    [[nodiscard]] std::uint64_t live_bytes() const noexcept {
        std::uint64_t n = 0;
        for (const auto& e : entries_) {
            n += static_cast<std::uint64_t>(e.first.size() + e.second.size());
        }
        return n;
    }

private:
    [[nodiscard]] std::size_t find(const Key& k) const {
        std::size_t i = 0;
        while (i < entries_.size() && entries_[i].first < k) {
            ++i;
        }
        return i;
    }
    std::vector<std::pair<Key, Value>> entries_;  // sorted by key.
};

// ---------------------------------------------------------------------------
// The workload op stream is a PURE function of (seed, BenchConfig). We hand-roll
// every draw from SeededRandom (no std::*_distribution): key index, value length,
// op-kind coin flips, scan bounds. These helpers keep the draw ORDER fixed so the
// stream is byte-stable.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Key key_of(std::uint64_t i) {
    return std::string("k") + std::to_string(i);
}

[[nodiscard]] inline Value make_value(SeededRandom& rng, const BenchConfig& cfg,
                                      std::uint64_t seq_salt) {
    const std::uint64_t len = static_cast<std::uint64_t>(
        rng.uniform_range(static_cast<std::int64_t>(cfg.value_min),
                          static_cast<std::int64_t>(cfg.value_max)));
    // Fill deterministically from a rolling byte derived from the salt + index so
    // values are non-trivial yet reproducible (no randomness needed per byte).
    Value v;
    v.reserve(len);
    std::uint64_t x = seq_salt + 0x9E3779B97F4A7C15ULL;
    for (std::uint64_t b = 0; b < len; ++b) {
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL + b;
        v.push_back(static_cast<char>(static_cast<std::uint8_t>(x & 0x7Fu) + 1u));
    }
    return v;
}

// ---------------------------------------------------------------------------
// run_one — drive ONE (seed, BenchConfig) benchmark to completion and return the
// measured BenchResult. The phases (write, then read) are timed by the scheduler
// virtual clock so each phase's vtick cost is isolated. A final sync() makes the
// space-amp durable numerator meaningful. Pure function of (seed, cfg).
// ---------------------------------------------------------------------------
[[nodiscard]] inline BenchResult run_one(std::uint64_t seed, const BenchConfig& cfg) {
    Scheduler sched;
    SimClock clock(sched);
    // The ONE randomness source for the whole run (workload + disk faults share
    // it in a fixed draw order ⇒ pure fn of seed). A benign salt keeps the bench
    // seed space distinct from other harnesses' use of the same seeds.
    SeededRandom rng(seed ^ 0xB3C7u);

    // HONEST disk (no faults) with a small, fixed latency window so vtime reflects
    // modeled IO cost deterministically. The bench measures performance, not
    // crash-consistency (that is the engine test's job), so faults are off.
    DiskFaultConfig dc;
    dc.latency_min = 1;
    dc.latency_max = 3;

    ByteMeter meter;
    SimDisk wal_sim(sched, clock, rng, dc);
    SimDisk man_sim(sched, clock, rng, dc);
    CountingDisk wal(wal_sim, meter);
    CountingDisk man(man_sim, meter);
    BenchDiskFactory factory(sched, clock, rng, dc, meter);

    WalEngine eng(sched, wal, man, factory,
                  static_cast<std::size_t>(cfg.flush_threshold));

    LiveTracker live;
    BenchResult res;
    res.user_write_bytes = 0;

    // ---- WRITE PHASE -------------------------------------------------------
    // Drive write_ops put/del mutations, syncing periodically (group commit) so
    // durability cost is part of the modeled write time. The op stream + sizes are
    // hand-rolled from rng in a fixed order. We mirror each commit into LiveTracker
    // so the live-bytes denominator is exact.
    const std::int64_t t_write_start = sched.vtime();
    {
        // The write coroutine: awaits each commit (so vtime advances by the disk
        // latency) and periodically syncs. We capture counters by reference into
        // res/live; the coroutine holds no pointer into a growable container across
        // an await (it re-derives keys/values into locals each iteration; V-RKV1).
        struct WriteState {
            std::uint64_t committed = 0;
        };
        WriteState ws;
        sched.spawn([](WalEngine& e, SeededRandom& r, const BenchConfig& c,
                       LiveTracker& lv, BenchResult& rr, WriteState& st) -> Task {
            for (std::uint64_t i = 0; i < c.write_ops; ++i) {
                const std::uint64_t ki = r.uniform(c.n_keys);
                const Key k = key_of(ki);
                const bool is_del = r.chance(c.del_fraction);
                if (is_del) {
                    co_await e.del(k);
                    rr.user_write_bytes += static_cast<std::uint64_t>(k.size());
                    lv.del(k);
                } else {
                    Value v = make_value(r, c, i + 1);
                    rr.user_write_bytes +=
                        static_cast<std::uint64_t>(k.size() + v.size());
                    co_await e.put(k, v);  // v moved after byte-count taken.
                    lv.put(k, v);
                }
                ++st.committed;
                // Group-commit every 32 mutations (a realistic batching cadence).
                if ((i % 32u) == 31u) {
                    co_await e.sync();
                }
            }
            co_await e.sync();  // final durability barrier (space-amp meaningful).
            co_return;
        }(eng, rng, cfg, live, res, ws));
        sched.run();
        res.write_ops = ws.committed;
    }
    const std::int64_t t_write_end = sched.vtime();
    res.write_vticks = static_cast<std::uint64_t>(t_write_end - t_write_start);

    // Snapshot AFTER all writes are durable: reads run as-of the final version.
    Snapshot snap{eng.last_seq()};

    // ---- READ PHASE --------------------------------------------------------
    // Drive read_ops get/scan ops as-of the final snapshot. Reads touch memtable +
    // every SSTable (bloom skip + index seek), so read vtime reflects the LSM read
    // cost the block-size / bloom knobs influence.
    const std::int64_t t_read_start = sched.vtime();
    {
        struct ReadState {
            std::uint64_t done = 0;
        };
        ReadState rs;
        sched.spawn([](WalEngine& e, SeededRandom& r, const BenchConfig& c,
                       Snapshot s, ReadState& st) -> Task {
            for (std::uint64_t i = 0; i < c.read_ops; ++i) {
                if (r.chance(c.scan_fraction)) {
                    const std::uint64_t lo_i = r.uniform(c.n_keys);
                    Range range;
                    range.lo = key_of(lo_i);
                    range.hi = key_of(lo_i + c.scan_width);
                    range.hi_unbounded = false;
                    std::vector<storage::KeyValue> out = co_await e.scan(range, s);
                    (void)out;  // result consumed; we measure cost, not values.
                } else {
                    const std::uint64_t ki = r.uniform(c.n_keys);
                    std::optional<Value> g = co_await e.get(key_of(ki), s);
                    (void)g;
                }
                ++st.done;
            }
            co_return;
        }(eng, rng, cfg, snap, rs));
        sched.run();
        res.read_ops = rs.done;
    }
    const std::int64_t t_read_end = sched.vtime();
    res.read_vticks = static_cast<std::uint64_t>(t_read_end - t_read_start);

    // ---- METRICS -----------------------------------------------------------
    res.appended_bytes = meter.appended_bytes;
    res.live_user_bytes = live.live_bytes();
    res.sstable_count = static_cast<std::uint64_t>(eng.sstable_count());
    // Durable on-disk bytes = WAL + manifest + every SSTable, after the final sync.
    res.durable_bytes = static_cast<std::uint64_t>(wal_sim.durable_len()) +
                        static_cast<std::uint64_t>(man_sim.durable_len()) +
                        factory.durable_bytes();
    return res;
}

}  // namespace lockstep::bench
