// storage_wisckey_test.cpp — Phase 3 §5 step 6 gate for WISCKEY LARGE-VALUE
// SEPARATION (C3.6), crash-consistent (storage-engine.md §1/§2/§3/§6, D4).
//
// A value STRICTLY LONGER than a configurable threshold is written ONCE to an
// append-only VALUE LOG and the LSM (WAL record + memtable + SSTable) stores only
// a small fixed pointer {gen, offset, len, crc}. Small values stay inline. The
// point (master-plan D4): a large value is never rewritten by compaction —
// compaction rewrites only the tiny pointer — cutting write amplification.
//
// WHAT IT PROVES (one block per spec requirement):
//
//   (A) DIFFERENTIAL WITH LARGE VALUES — the core gate (§4) on a workload mixing
//       SMALL + LARGE values so vlog separation actually fires: WalEngine(LSM +
//       WiscKey, small flush/compaction triggers ⇒ flushes + compactions +
//       vlog rewrites) vs an INLINE Oracle over a seed sweep. The oracle stores
//       every value inline; the engine separates large ones — get AND scan MUST
//       return byte-identical values either way. 0 mismatch.
//
//   (B) WRITE-AMP IMPROVEMENT (the WiscKey point) — under a large-value workload,
//       compaction MUST NOT rewrite the large values: the bytes written to the
//       SSTable disks during a compaction with vlog ON are FAR LESS than the
//       inline bytes those same versions would occupy. We meter SSTable-disk bytes
//       written across a compaction and assert vlog-on << inline-equivalent.
//
//   (C) CRASH-CONSISTENCY incl. crash AROUND vlog append / BETWEEN vlog and WAL —
//       crash at arbitrary points under the full fault envelope (torn writes +
//       lying fsync on BOTH the WAL and the vlog + io faults), recover; the
//       recovered state is ALWAYS a valid oracle PREFIX, 0 fabrication, and NO
//       pointer ever resolves to a torn/fabricated value (V-NOTORN/V-PREFIX).
//
//   (D) DETERMINISM — same seed ⇒ byte-identical observable history + on-disk
//       fingerprint (WAL + every SSTable + every live vlog generation).
//
// Non-provider TU → forbidden-call lint scans it; only randomness is sim
// SeededRandom; all time virtual; no <chrono>/<thread>/<random>.

#include <cstddef>
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

#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/Oracle.hpp>
#include <lockstep/storage/ValueLog.hpp>
#include <lockstep/storage/WalEngine.hpp>

namespace {

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::IDiskFactory;
using lockstep::storage::kDefaultValueThreshold;
using lockstep::storage::Key;
using lockstep::storage::KeyValue;
using lockstep::storage::Oracle;
using lockstep::storage::Range;
using lockstep::storage::Seq;
using lockstep::storage::Snapshot;
using lockstep::storage::Value;
using lockstep::storage::VlogPtr;
using lockstep::storage::WalEngine;

int g_failures = 0;

#define CHECK(cond, ...)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "WISCKEY GATE FAIL [%s:%d]: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                                       \
            std::fprintf(stderr, "\n");                                              \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

// The separation threshold the engine uses in these tests.
constexpr std::size_t kThreshold = 256;

// Make a value of a given size, content keyed on (id) so distinct ids ⇒ distinct
// bytes (a vlog deref returning the wrong record would surface as a mismatch).
[[nodiscard]] Value big_value(std::uint64_t id, std::size_t len) {
    Value v;
    v.reserve(len);
    const std::string tag = "V" + std::to_string(id) + ":";
    for (std::size_t i = 0; i < len; ++i) {
        v.push_back(tag[i % tag.size()]);
    }
    return v;
}
[[nodiscard]] Value small_value(std::uint64_t id) { return "s" + std::to_string(id); }

// ---------------------------------------------------------------------------
// A SimDisk-backed IDiskFactory that ALSO meters bytes written (append payload),
// reclaims obsoleted backings, and supports crash/recover across all backings —
// covering BOTH SSTable ids (low) AND vlog generation ids (high). Determinism
// fingerprint concatenates every non-reclaimed backing's durable image.
// ---------------------------------------------------------------------------
class MeteredFactory final : public IDiskFactory {
public:
    MeteredFactory(Scheduler& sched, SimClock& clock, SeededRandom& rng, DiskFaultConfig cfg)
        : sched_(&sched), clock_(&clock), rng_(&rng), cfg_(cfg) {}

    [[nodiscard]] IDisk& disk_for(std::uint64_t id) override {
        Slot& s = slot(id);
        return *s.disk;
    }

    void reclaim(std::uint64_t id) override {
        if (auto it = index_.find_idx(id); it >= 0) {
            slots_[static_cast<std::size_t>(it)].reclaimed = true;
        }
    }

    // Bytes written meter: a wrapper disk records the payload of each append. We
    // meter via the SimDisk's logical growth instead (durable+lying+staged), which
    // equals the total appended payload (torn writes keep a prefix). Simpler: we
    // snapshot the high-water before/after a window.
    [[nodiscard]] std::uint64_t total_bytes_written(bool sstables_only) const {
        std::uint64_t n = 0;
        for (const Slot& s : slots_) {
            if (sstables_only && s.id >= WalEngine::kDefaultVlogBaseId) {
                continue;
            }
            n += s.disk->logical_len();
        }
        return n;
    }

    void crash_all() {
        for (Slot& s : slots_) {
            s.disk->crash();
        }
    }
    void recover_all() {
        for (Slot& s : slots_) {
            s.disk->recover();
        }
    }

    [[nodiscard]] std::vector<std::byte> durable_fingerprint() const {
        std::vector<std::byte> out;
        for (const Slot& s : slots_) {
            if (s.reclaimed) {
                continue;
            }
            const std::vector<std::byte> img = s.disk->durable_snapshot();
            out.insert(out.end(), img.begin(), img.end());
        }
        return out;
    }

private:
    struct Slot {
        std::uint64_t id = 0;
        std::unique_ptr<SimDisk> disk;
        bool reclaimed = false;
    };
    // Tiny ordered index: id → slot index (deterministic, no hash iteration in any
    // ordering key — we only use it for O(n) lookup, fine for the sim sizes).
    struct Index {
        const std::vector<Slot>* slots = nullptr;
        [[nodiscard]] std::ptrdiff_t find_idx(std::uint64_t id) const {
            for (std::size_t i = 0; i < slots->size(); ++i) {
                if ((*slots)[i].id == id) {
                    return static_cast<std::ptrdiff_t>(i);
                }
            }
            return -1;
        }
    };

    Slot& slot(std::uint64_t id) {
        for (Slot& s : slots_) {
            if (s.id == id) {
                return s;
            }
        }
        Slot s;
        s.id = id;
        s.disk = std::make_unique<SimDisk>(*sched_, *clock_, *rng_, cfg_);
        slots_.push_back(std::move(s));
        index_.slots = &slots_;
        return slots_.back();
    }

    Scheduler* sched_;
    SimClock* clock_;
    SeededRandom* rng_;
    DiskFaultConfig cfg_;
    std::vector<Slot> slots_;
    Index index_{&slots_};
};

// ===========================================================================
// (A) DIFFERENTIAL WITH LARGE VALUES — WalEngine(LSM+WiscKey) vs inline Oracle.
// A custom driver (the shared DiffHarness uses tiny values) drives an identical
// seeded op stream of MIXED small/large puts + dels + gets + scans against BOTH.
// ===========================================================================
struct DiffOut {
    bool ok = true;
    std::uint64_t step = 0;
    std::string note;
    std::string expected;
    std::string got;
};

[[nodiscard]] std::string render_opt(const std::optional<Value>& v) {
    return v.has_value() ? ("len=" + std::to_string(v->size())) : std::string("nil");
}

using lockstep::storage::Engine;

Task run_diff_large(Scheduler& /*sched*/, Engine& sut, Engine& ref, SeededRandom& rng,
                    std::uint64_t steps, std::uint64_t n_keys, DiffOut& out) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    Seq frontier = 0;
    std::uint64_t val_id = 0;
    for (std::uint64_t step = 0; step < steps; ++step) {
        const std::uint64_t roll = rng.uniform(20);
        const Key key = kof(rng.uniform(n_keys));
        if (roll < 9) {
            // put — half large (separated), half small (inline). Distinct bytes.
            ++val_id;
            const bool large = (rng.uniform(2) == 0);
            const Value v = large ? big_value(val_id, kThreshold + 1 + rng.uniform(512))
                                  : small_value(val_id);
            const Seq sref = co_await ref.put(key, v);
            const Seq ssut = co_await sut.put(key, v);
            if (ssut != sref) {
                out = DiffOut{false, step, "put seq disagreement",
                              std::to_string(sref), std::to_string(ssut)};
                co_return;
            }
            if (sref > frontier) {
                frontier = sref;
            }
        } else if (roll < 12) {
            const Seq sref = co_await ref.del(key);
            const Seq ssut = co_await sut.del(key);
            if (ssut != sref) {
                out = DiffOut{false, step, "del seq disagreement",
                              std::to_string(sref), std::to_string(ssut)};
                co_return;
            }
            if (sref > frontier) {
                frontier = sref;
            }
        } else if (roll < 13) {
            co_await sut.sync();  // a durability barrier (oracle has nothing to do).
            co_await ref.sync();
        } else if (roll < 17) {
            const Seq at = static_cast<Seq>(rng.uniform(frontier + 1));
            const std::optional<Value> vref = co_await ref.get(key, Snapshot{at});
            const std::optional<Value> vsut = co_await sut.get(key, Snapshot{at});
            if (vref != vsut) {
                out = DiffOut{false, step, "get value disagreement (V-SNAP/deref)",
                              render_opt(vref), render_opt(vsut)};
                co_return;
            }
        } else {
            const Seq at = static_cast<Seq>(rng.uniform(frontier + 1));
            const std::uint64_t a = rng.uniform(n_keys);
            const std::uint64_t b = rng.uniform(n_keys + 1);
            Range range;
            range.lo = kof(a);
            const bool unbounded = (rng.uniform(3) == 0);
            range.hi_unbounded = unbounded;
            range.hi = unbounded ? Key{} : kof(b);
            const std::vector<KeyValue> rref = co_await ref.scan(range, Snapshot{at});
            const std::vector<KeyValue> rsut = co_await sut.scan(range, Snapshot{at});
            if (rref != rsut) {
                out = DiffOut{false, step, "scan disagreement (V-SNAP/deref/order)",
                              "kvs=" + std::to_string(rref.size()),
                              "kvs=" + std::to_string(rsut.size())};
                co_return;
            }
        }
    }
    co_return;
}

void test_differential_large_values() {
    const std::uint64_t n_seeds = 128;
    std::uint64_t mismatches = 0;
    DiffOut first_bad;
    std::uint64_t first_seed = 0;
    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        Scheduler sched;
        SimClock clock(sched);
        SeededRandom rng(seed ^ 0x57495343u);  // 'WISC'
        SeededRandom op_rng(seed ^ 0xABCDEFu);
        DiskFaultConfig dc;  // HONEST disk for the differential gate.
        dc.latency_min = 0;
        dc.latency_max = 2;
        SimDisk wal(sched, clock, rng, dc);
        SimDisk manifest(sched, clock, rng, dc);
        MeteredFactory factory(sched, clock, rng, dc);

        WalEngine sut(sched, wal, manifest, factory, /*flush_threshold=*/6);
        sut.set_compaction_trigger(3);
        sut.set_value_log(kThreshold);  // WiscKey ON
        Oracle ref(sched);

        DiffOut out;
        sched.spawn(run_diff_large(sched, sut, ref, op_rng, /*steps=*/300, /*n_keys=*/8, out));
        sched.run();
        if (!out.ok) {
            if (mismatches == 0) {
                first_bad = out;
                first_seed = seed;
            }
            ++mismatches;
        }
    }
    CHECK(mismatches == 0,
          "(A) differential-large-values: %llu/%llu seeds MISMATCHED; first seed=%llu "
          "step=%llu note=%s expected=%s got=%s",
          static_cast<unsigned long long>(mismatches),
          static_cast<unsigned long long>(n_seeds),
          static_cast<unsigned long long>(first_seed),
          static_cast<unsigned long long>(first_bad.step), first_bad.note.c_str(),
          first_bad.expected.c_str(), first_bad.got.c_str());
    std::fprintf(stderr,
                 "[ok] (A) differential WalEngine(LSM+WiscKey)-vs-inline-Oracle: %llu "
                 "seeds, mixed small/large, get+scan, 0 mismatch (flush 6 + compact-at-3 "
                 "+ vlog rewrites)\n",
                 static_cast<unsigned long long>(n_seeds));
}

// ===========================================================================
// (B) WRITE-AMP IMPROVEMENT — compaction does NOT rewrite large values.
// We fill a large-value workload across several flushes, then FORCE a compaction
// and meter the SSTable-disk bytes written by that compaction. With WiscKey ON
// only the tiny pointers are merged into the new SSTable; the large values stay in
// the (already-written-once) vlog. We compare against the bytes the SAME live
// versions would occupy if compaction had to rewrite them inline.
// ===========================================================================
Task wa_fill(WalEngine& eng, std::uint64_t rounds, std::uint64_t n_keys,
             std::size_t value_len, std::uint64_t& val_id) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    for (std::uint64_t r = 0; r < rounds; ++r) {
        for (std::uint64_t ki = 0; ki < n_keys; ++ki) {
            co_await eng.put(kof(ki), big_value(++val_id, value_len));
        }
        co_await eng.sync();
    }
    co_return;
}

void test_write_amp_improvement() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0xA11Cu);
    DiskFaultConfig dc;  // honest
    dc.latency_min = 0;
    dc.latency_max = 1;
    SimDisk wal(sched, clock, rng, dc);
    SimDisk manifest(sched, clock, rng, dc);
    MeteredFactory factory(sched, clock, rng, dc);

    const std::uint64_t n_keys = 6;
    const std::size_t value_len = 1024;  // well above threshold
    WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/n_keys);
    eng.set_compaction_trigger(100000);  // force compaction explicitly
    eng.set_value_log(kThreshold);

    std::uint64_t val_id = 0;
    sched.spawn(wa_fill(eng, /*rounds=*/6, n_keys, value_len, val_id));
    sched.run();
    sched.spawn([](WalEngine& e) -> Task {
        co_await e.force_flush();  // flush the tail so all live versions are in SSTables
        co_return;
    }(eng));
    sched.run();

    const std::size_t ssts_before = eng.sstable_count();
    const std::size_t live_versions = eng.live_version_count();
    // Meter SSTable-disk bytes written across the compaction (vlog ids excluded).
    const std::uint64_t sst_bytes_before = factory.total_bytes_written(/*sstables_only=*/true);
    sched.spawn([](WalEngine& e) -> Task {
        co_await e.force_compact();
        co_return;
    }(eng));
    sched.run();
    const std::uint64_t sst_bytes_after = factory.total_bytes_written(/*sstables_only=*/true);
    const std::uint64_t compaction_sst_bytes = sst_bytes_after - sst_bytes_before;

    // The inline-equivalent: if compaction had to rewrite the values inline, the
    // merged SSTable would carry ~live_versions * value_len value bytes (plus
    // framing). WiscKey writes only ~16-24-byte pointers per version into the SST.
    const std::uint64_t inline_equiv = static_cast<std::uint64_t>(live_versions) *
                                       static_cast<std::uint64_t>(value_len);

    CHECK(ssts_before >= 2 && eng.sstable_count() < ssts_before,
          "(B) write-amp: compaction did not actually fire (before=%zu after=%zu)",
          ssts_before, eng.sstable_count());
    CHECK(compaction_sst_bytes * 4 < inline_equiv,
          "(B) write-amp: compaction SSTable bytes=%llu NOT << inline-equiv=%llu "
          "(live_versions=%zu value_len=%zu) — large values were rewritten",
          static_cast<unsigned long long>(compaction_sst_bytes),
          static_cast<unsigned long long>(inline_equiv), live_versions, value_len);
    std::fprintf(stderr,
                 "[ok] (B) write-amp: compaction wrote %llu SSTable bytes vs %llu inline-"
                 "equivalent (%.1fx less) — large values NOT rewritten (%zu live versions, "
                 "%zu-byte values)\n",
                 static_cast<unsigned long long>(compaction_sst_bytes),
                 static_cast<unsigned long long>(inline_equiv),
                 inline_equiv > 0 ? static_cast<double>(inline_equiv) /
                                        static_cast<double>(compaction_sst_bytes == 0 ? 1
                                                                                      : compaction_sst_bytes)
                                  : 0.0,
                 live_versions, value_len);
}

// ===========================================================================
// (C) CRASH-CONSISTENCY incl. crash around vlog append / between vlog and WAL.
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
    std::vector<std::byte> wal_fp;
    std::vector<std::byte> disk_fp;
};

Task crash_fill(WalEngine& eng, SeededRandom& rng, std::uint64_t op_count,
                std::uint64_t crash_step, CrashScenario& out, std::uint64_t n_keys) {
    auto kof = [](std::uint64_t i) { return std::string("k") + std::to_string(i); };
    std::uint64_t vid = 0;
    for (std::uint64_t step = 0; step < op_count; ++step) {
        if (step == crash_step) {
            break;
        }
        const std::uint64_t roll = rng.uniform(10);
        if (roll < 6) {
            const Key k = kof(rng.uniform(n_keys));
            ++vid;
            // Mix large (separated) + small (inline) so a crash lands around vlog
            // appends, between vlog and WAL, and on plain inline records.
            const bool large = (rng.uniform(2) == 0);
            const Value v = large ? big_value(vid, kThreshold + 1 + rng.uniform(256))
                                  : small_value(vid);
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

CrashScenario run_crash(std::uint64_t seed, std::uint64_t op_count, std::uint64_t crash_step) {
    CrashScenario out;
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(seed);
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 2;
    dc.torn_write_prob = 0.20;   // torn SSTable / WAL / manifest / VLOG records
    dc.lying_fsync_prob = 0.20;  // a sync that drops a tail on crash (WAL + vlog)
    dc.io_fault_prob = 0.05;
    SimDisk wal(sched, clock, rng, dc);
    SimDisk manifest(sched, clock, rng, dc);
    MeteredFactory factory(sched, clock, rng, dc);
    const std::uint64_t n_keys = 6;

    WalEngine eng(sched, wal, manifest, factory, /*flush_threshold=*/4);
    eng.set_compaction_trigger(2);
    eng.set_value_log(kThreshold);

    sched.spawn(crash_fill(eng, rng, op_count, crash_step, out, n_keys));
    sched.run();

    wal.crash();
    manifest.crash();
    factory.crash_all();
    wal.recover();
    manifest.recover();
    factory.recover_all();

    WalEngine recovered(sched, wal, manifest, factory, /*flush_threshold=*/4);
    recovered.set_compaction_trigger(2);
    recovered.set_value_log(kThreshold);
    sched.spawn([](WalEngine& e, std::size_t wl, std::size_t ml) -> Task {
        co_await e.recover_lsm(wl, ml);
        co_return;
    }(recovered, wal.durable_len(), manifest.durable_len()));
    sched.run();
    out.recovered_tip = recovered.last_seq();
    out.wal_fp = wal.durable_snapshot();
    out.disk_fp = factory.durable_fingerprint();

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

void test_crash_around_vlog() {
    const std::uint64_t n_seeds = 160;
    std::uint64_t crash_points = 0;
    std::uint64_t fabrications = 0;
    std::uint64_t prefix_violations = 0;
    std::uint64_t first_bad = 0;

    for (std::uint64_t seed = 1; seed <= n_seeds; ++seed) {
        const std::uint64_t op_count = 80;
        SeededRandom pick(seed ^ 0x5644C0u);
        const std::uint64_t crash_step =
            4 + static_cast<std::uint64_t>(pick.uniform(op_count - 4));
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
                    ++fabrications;  // a torn/fabricated value surfaced.
                } else {
                    ++prefix_violations;
                }
                if (first_bad == 0) {
                    first_bad = seed;
                }
                std::fprintf(stderr,
                             "  [crash-vlog mismatch] seed=%llu crash_step=%llu tip=%llu "
                             "key=%s at=%llu want=%s got=%s\n",
                             static_cast<unsigned long long>(seed),
                             static_cast<unsigned long long>(crash_step),
                             static_cast<unsigned long long>(tip), k.c_str(),
                             static_cast<unsigned long long>(at),
                             want.has_value() ? ("len" + std::to_string(want->size())).c_str()
                                              : "nil",
                             got.has_value() ? ("len" + std::to_string(got->size())).c_str()
                                             : "nil");
                break;
            }
        }
    }
    CHECK(fabrications == 0,
          "(C) crash-around-vlog: %llu FABRICATED reads across %llu crash points (first "
          "seed=%llu) — a torn/missing vlog region resolved to a fabricated value "
          "(V-NOTORN VIOLATED)",
          static_cast<unsigned long long>(fabrications),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    CHECK(prefix_violations == 0,
          "(C) crash-around-vlog: %llu PREFIX violations across %llu crash points (first "
          "seed=%llu) — recovered state not a valid oracle prefix",
          static_cast<unsigned long long>(prefix_violations),
          static_cast<unsigned long long>(crash_points),
          static_cast<unsigned long long>(first_bad));
    std::fprintf(stderr,
                 "[ok] (C) crash-around-vlog: %llu crash points (full fault envelope on "
                 "WAL+manifest+SSTable+VLOG, flush 4 + compact-at-2), recovered ALWAYS a "
                 "valid prefix, 0 fabrication, no pointer resolved to a torn value\n",
                 static_cast<unsigned long long>(crash_points));
}

// ===========================================================================
// (D) DETERMINISM — same seed ⇒ byte-identical recovered reads + on-disk image.
// ===========================================================================
void test_determinism() {
    const std::uint64_t seed = 24680;
    const std::uint64_t op_count = 76;
    const std::uint64_t crash_step = 49;
    CrashScenario a = run_crash(seed, op_count, crash_step);
    CrashScenario b = run_crash(seed, op_count, crash_step);
    CHECK(a.recovered_tip == b.recovered_tip,
          "(D) determinism: recovered tip differs (%llu vs %llu)",
          static_cast<unsigned long long>(a.recovered_tip),
          static_cast<unsigned long long>(b.recovered_tip));
    CHECK(a.wal_fp == b.wal_fp, "(D) determinism: WAL durable fingerprint differs (%zu vs %zu)",
          a.wal_fp.size(), b.wal_fp.size());
    CHECK(a.disk_fp == b.disk_fp,
          "(D) determinism: SSTable+VLOG durable fingerprint differs (%zu vs %zu)",
          a.disk_fp.size(), b.disk_fp.size());
    CHECK(a.probe_reads == b.probe_reads, "(D) determinism: recovered probe reads differ");
    std::fprintf(stderr,
                 "[ok] (D) determinism: seed %llu replays byte-identical (tip=%llu, wal=%zu B, "
                 "sst+vlog=%zu B, %zu probe reads identical)\n",
                 static_cast<unsigned long long>(seed),
                 static_cast<unsigned long long>(a.recovered_tip), a.wal_fp.size(),
                 a.disk_fp.size(), a.probe_reads.size());
}

// A small UNIT check that the vlog pointer + deref round-trip + reject torn bytes.
void test_vlog_codec_unit() {
    using lockstep::storage::encode_vlog_record;
    using lockstep::storage::vlog_deref;
    const Value v(700, 'X');
    const auto enc = encode_vlog_record(v, /*base_off=*/0, /*gen=*/42);
    CHECK(enc.ptr.gen == 42 && enc.ptr.vlen == 700, "(unit) vlog ptr fields wrong");
    const std::optional<Value> got = vlog_deref(enc.bytes, enc.ptr);
    CHECK(got.has_value() && *got == v, "(unit) vlog deref round-trip failed");
    // Flip a byte in the value region → CRC must reject (no fabrication).
    std::vector<std::byte> torn = enc.bytes;
    torn[20] = static_cast<std::byte>(std::to_integer<unsigned>(torn[20]) ^ 0xFFu);
    const std::optional<Value> bad = vlog_deref(torn, enc.ptr);
    CHECK(!bad.has_value(), "(unit) vlog deref accepted a torn record (V-NOTORN)");
    // A pointer past the image → reject.
    VlogPtr oob = enc.ptr;
    oob.vlen = 1u << 30;
    CHECK(!vlog_deref(enc.bytes, oob).has_value(), "(unit) vlog deref accepted OOB pointer");
    std::fprintf(stderr, "[ok] (unit) vlog codec: round-trip ok, torn + OOB rejected\n");
}

}  // namespace

int main() {
    std::fprintf(stderr, "=== storage_wisckey_test (WiscKey large-value separation) ===\n");
    test_vlog_codec_unit();
    test_differential_large_values();
    test_write_amp_improvement();
    test_crash_around_vlog();
    test_determinism();
    if (g_failures != 0) {
        std::fprintf(stderr, "storage_wisckey_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "storage_wisckey_test: ALL PASS\n");
    return 0;
}
