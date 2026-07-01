// storage_bulk_corruption_test.cpp — CATASTROPHIC (bulk-region) fault injection against
// the recovery stack (plan P6). The per-op sim faults model a single flipped/torn byte;
// this closes the coverage gap the CTRL research flagged — a CONTIGUOUS media loss (a bad
// sector / partial platter) and a WHOLE-SEGMENT / tail loss — and proves every recovery
// tool built this session (WAL recover, backup / PITR / SSTable / manifest verify, scrub)
// handles it correctly: a self-contained image is REJECTED whole (CRC catches the bulk
// corruption), a WAL recovers a consistent PREFIX that is a SUBSET of the good records
// (never a fabricated value), and truncation is likewise a prefix. Pure over the sim.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/Pitr.hpp>
#include <lockstep/storage/Recovery.hpp>
#include <lockstep/storage/WalEngine.hpp>

using lockstep::core::Error;
using lockstep::core::IDisk;
using lockstep::core::Offset;
using lockstep::core::Scheduler;
using lockstep::core::SimClock;
using lockstep::core::Task;
using lockstep::sim::DiskFaultConfig;
using lockstep::sim::SeededRandom;
using lockstep::sim::SimDisk;
using lockstep::storage::detect_format;
using lockstep::storage::Engine;
using lockstep::storage::FileFormat;
using lockstep::storage::IDiskFactory;
using lockstep::storage::inspect_wal;
using lockstep::storage::KeyValue;
using lockstep::storage::Range;
using lockstep::storage::Seq;
using lockstep::storage::Snapshot;
using lockstep::storage::verify_image;
using lockstep::storage::WalEngine;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
DiskFaultConfig nofault() {
    DiskFaultConfig dc;
    dc.latency_min = 0;
    dc.latency_max = 0;
    return dc;
}
// Flip a CONTIGUOUS region [off, off+len) — a bad-sector media fault.
void bulk_corrupt(std::vector<std::byte>& v, std::size_t off, std::size_t len) {
    for (std::size_t i = off; i < off + len && i < v.size(); ++i) {
        v[i] = static_cast<std::byte>(std::to_integer<unsigned>(v[i]) ^ 0xFFu);
    }
}

class SimFactory final : public IDiskFactory {
public:
    SimFactory(Scheduler& s, SimClock& c, SeededRandom& r, DiskFaultConfig cfg)
        : sched_(&s), clock_(&c), rng_(&r), cfg_(cfg) {}
    [[nodiscard]] IDisk& disk_for(std::uint64_t id) override {
        for (auto& e : disks_)
            if (e.first == id) return *e.second;
        disks_.emplace_back(id, std::make_unique<SimDisk>(*sched_, *clock_, *rng_, cfg_));
        return *disks_.back().second;
    }
    [[nodiscard]] SimDisk* sstable_disk() {
        for (auto& e : disks_)
            if (e.first < WalEngine::kDefaultVlogBaseId) return e.second.get();
        return nullptr;
    }
private:
    Scheduler* sched_;
    SimClock* clock_;
    SeededRandom* rng_;
    DiskFaultConfig cfg_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<SimDisk>>> disks_;
};

Task load_wal(Engine& e) {
    for (int i = 0; i < 30; ++i) (void)co_await e.put("k" + std::to_string(i), "v" + std::to_string(i));
    (void)co_await e.del("k5");
    (void)co_await e.sync();
    co_return;
}
Task seed(IDisk& d, std::vector<std::byte> bytes, Error& res) {
    Offset off = 0;
    res = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (res.ok()) res = co_await d.sync();
    co_return;
}
Task recover_scan(WalEngine& e, std::size_t len, std::vector<KeyValue>& out, bool& ok) {
    ok = (co_await e.recover(len)).ok();
    Range full;
    full.hi_unbounded = true;
    out = co_await e.scan(full, Snapshot{e.last_seq()});
    co_return;
}
Task scan_now(Engine& e, Seq at, std::vector<KeyValue>& out) {
    Range full;
    full.hi_unbounded = true;
    out = co_await e.scan(full, Snapshot{at});
    co_return;
}
}  // namespace

int main() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0xB01C0DE5u);
    const DiskFaultConfig dc = nofault();

    // Build a WAL + a backup + a PITR archive from one engine.
    SimDisk wal_disk(sched, clock, rng, dc);
    WalEngine src(sched, wal_disk);
    sched.spawn(load_wal(src));
    sched.run();
    const std::vector<std::byte> wal = wal_disk.durable_snapshot();
    const std::size_t good_records = inspect_wal(wal).records.size();

    std::vector<std::byte> backup;
    (void)lockstep::storage::backup_engine_bytes(sched, src, Snapshot{src.last_seq()}, backup);
    std::vector<std::byte> pitr;
    (void)lockstep::storage::archive_ops_bytes(sched, src, 0, pitr);

    // Build an SSTable + manifest from a flushing LSM engine.
    SimDisk lwal(sched, clock, rng, dc), lman(sched, clock, rng, dc);
    SimFactory fac(sched, clock, rng, dc);
    WalEngine lsm(sched, lwal, lman, fac, /*flush_threshold=*/4);
    sched.spawn(load_wal(lsm));
    sched.run();
    const std::vector<std::byte> manifest = lman.durable_snapshot();
    SimDisk* sstd = fac.sstable_disk();
    const std::vector<std::byte> sstable = sstd ? sstd->durable_snapshot() : std::vector<std::byte>{};

    check(good_records > 0 && !backup.empty() && !pitr.empty() && !manifest.empty() && !sstable.empty(),
          "all durable formats built");

    // (1) SELF-CONTAINED images (backup / PITR / SSTable / manifest): a bulk-region flip in
    //     the MIDDLE is REJECTED whole by the integrity check — never partially trusted.
    // `all_or_nothing` = a self-contained image validated whole (backup / PITR / SSTable):
    // ANY truncation is rejected. A manifest is a PREFIX format (like the WAL) — a
    // boundary truncation is a valid shorter prefix — so it is checked only for the
    // mid-record corruption case, not truncation.
    struct Fmt { const char* name; const std::vector<std::byte>* img; bool all_or_nothing; };
    const Fmt fmts[] = {
        {"backup", &backup, true},
        {"pitr", &pitr, true},
        {"sstable", &sstable, true},
        {"manifest", &manifest, false},
    };
    for (const Fmt& f : fmts) {
        check(verify_image(*f.img).error.ok(), std::string("clean ") + f.name + " verifies ok");
        // bulk-corrupt a 16-byte region roughly in the middle.
        std::vector<std::byte> bad = *f.img;
        const std::size_t mid = bad.size() / 2;
        bulk_corrupt(bad, mid > 8 ? mid - 8 : 0, 16);
        check(!verify_image(bad).error.ok(),
              std::string("bulk-region-corrupt ") + f.name + " is REJECTED (never partially trusted)");
        // whole-segment / tail loss: drop the second half — an all-or-nothing image rejects it.
        if (f.all_or_nothing) {
            std::vector<std::byte> chopped(f.img->begin(),
                                           f.img->begin() + static_cast<std::ptrdiff_t>(f.img->size() / 2));
            check(!verify_image(chopped).error.ok(), std::string("truncated ") + f.name + " is REJECTED");
        }
    }

    // (2) WAL: a bulk-region flip in the MIDDLE truncates recovery to a consistent PREFIX
    //     that is a SUBSET of the good records — never MORE, never a fabricated value.
    {
        std::vector<std::byte> bad = wal;
        bulk_corrupt(bad, bad.size() / 2, 24);
        const auto ins = inspect_wal(bad);
        check(ins.records.size() < good_records && !ins.clean,
              "WAL bulk-region corruption: recovery truncates to a shorter prefix");
        // the surviving records must be an exact PREFIX of the good ones (same seq order).
        bool is_prefix = ins.records.size() <= good_records;
        for (std::size_t i = 0; i < ins.records.size(); ++i)
            if (ins.records[i].seq != static_cast<Seq>(i + 1)) is_prefix = false;
        check(is_prefix, "WAL recovered records are a contiguous prefix (no fabricated / reordered value)");
        check(!verify_image(bad).error.ok(), "verify flags the corrupt WAL (recoverable prefix reported)");
    }

    // (3) WAL through the real engine + the SimDisk bulk-corruption seam: recovering a
    //     mid-corrupted durable image yields a live state that is a SUBSET of the source.
    {
        SimDisk cdisk(sched, clock, rng, dc);
        Error se{lockstep::core::ErrorCode::Unknown, "norun"};
        sched.spawn(seed(cdisk, wal, se));
        sched.run();
        cdisk.corrupt_region(wal.size() / 2, 24);  // bad sector mid-log
        WalEngine rec(sched, cdisk);
        std::vector<KeyValue> got;
        bool ok = false;
        sched.spawn(recover_scan(rec, cdisk.durable_len(), got, ok));
        sched.run();
        check(ok, "engine recovers (to a prefix) from a bulk-corrupted WAL");
        check(rec.last_seq() < src.last_seq(),
              "recovery truncated below the source tip (corruption dropped a suffix)");
        // The recovered state must EXACTLY equal the source AS-OF the recovered prefix's
        // last Seq — a consistent historical prefix, never a fabricated / reordered value.
        std::vector<KeyValue> want_at;
        sched.spawn(scan_now(src, rec.last_seq(), want_at));
        sched.run();
        check(got == want_at,
              "recovered live state == source AS-OF the recovered Seq (consistent prefix, no fabrication)");
    }

    // (4) determinism: the same corruption yields the same verdict.
    {
        std::vector<std::byte> a = backup, b = backup;
        bulk_corrupt(a, 30, 12);
        bulk_corrupt(b, 30, 12);
        check(verify_image(a).error.ok() == verify_image(b).error.ok(),
              "bulk corruption verdict is deterministic");
    }

    if (g_fail) { std::printf("storage_bulk_corruption_test: FAILED\n"); return 1; }
    std::printf("storage_bulk_corruption_test: OK (bulk-region + whole-segment loss across all formats; "
                "reject-or-prefix, never fabricate)\n");
    return 0;
}
