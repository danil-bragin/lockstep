// storage_pitr_test.cpp — POINT-IN-TIME RECOVERY gate (storage/Pitr.hpp).
//
// PITR archives the committed op-log so a restore can rebuild the DB as-of ANY commit
// Seq. This proves: (1) a full-log archive restored to the tip reproduces the live
// state; (2) THE motivating case — a bad DELETE is UNDONE by restoring to the Seq just
// before it; (3) every intermediate target reproduces the source's live state as-of
// that Seq; (4) determinism (byte-identical archives); (5) target bounds (beyond the
// archive rejected, target 0 empty); (6) integrity teeth (bad magic / flipped byte /
// truncated / non-contiguous / miscounted — all rejected, no partial restore); (7) an
// honest refusal when a flush ate part of the op-log; (8) WiscKey vlog values are
// derefed to inline in the archive (portable) and restore correctly.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/storage/Pitr.hpp>
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
using lockstep::storage::archive_ops;
using lockstep::storage::Engine;
using lockstep::storage::ExportedOp;
using lockstep::storage::IDiskFactory;
using lockstep::storage::KeyValue;
using lockstep::storage::kPitrHeaderBytes;
using lockstep::storage::Range;
using lockstep::storage::restore_pitr;
using lockstep::storage::Seq;
using lockstep::storage::Snapshot;
using lockstep::storage::validate_pitr_archive;
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

// Minimal SimDisk-backed factory for the WiscKey (vlog) case — mints a disk per id.
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
private:
    Scheduler* sched_;
    SimClock* clock_;
    SeededRandom* rng_;
    DiskFaultConfig cfg_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<SimDisk>>> disks_;
};

Task scan_all(Engine& e, Snapshot snap, std::vector<KeyValue>& out) {
    Range full;
    full.hi_unbounded = true;
    out = co_await e.scan(full, snap);
    co_return;
}

Task write_raw(IDisk& d, std::vector<std::byte> bytes, Error& result) {
    Offset off = 0;
    const Error ae = co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (!ae.ok()) { result = ae; co_return; }
    result = co_await d.sync();
    co_return;
}

std::vector<KeyValue> scan_at(Scheduler& sched, Engine& e, Seq at) {
    std::vector<KeyValue> out;
    sched.spawn(scan_all(e, Snapshot{at}, out));
    sched.run();
    return out;
}

// Build a fresh engine, restore the archive `img` to `target`, and return its live state.
std::vector<KeyValue> restore_and_scan(Scheduler& sched, SimClock& clock, SeededRandom& rng,
                                       DiskFaultConfig dc, IDisk& archive, Seq target, bool& ok) {
    SimDisk dst_disk(sched, clock, rng, dc);
    WalEngine dst(sched, dst_disk);
    const Error e = restore_pitr(sched, archive, target, dst);
    ok = e.ok();
    if (!ok) return {};
    return scan_at(sched, dst, dst.last_seq());
}

// Load the source op stream, recording the commit Seq at three checkpoints:
//   s_load   = after the initial load (incl. "keep"="gold")
//   s_before = after an update to k5 (the Seq JUST BEFORE the bad delete)
//   s_after  = after the bad DELETE of "keep"
// then one more put past the incident, and sync.
Task load_src(Engine& e, Seq& s_load, Seq& s_before, Seq& s_after) {
    for (int i = 0; i < 20; ++i)
        (void)co_await e.put("k" + std::to_string(i), "v" + std::to_string(i));
    (void)co_await e.put("keep", "gold");       // the value a bad delete will wipe
    s_load = (co_await e.snapshot()).at;
    (void)co_await e.put("k5", "v5-updated");    // an update after the checkpoint
    s_before = (co_await e.snapshot()).at;
    (void)co_await e.del("keep");                // THE bad delete
    s_after = (co_await e.snapshot()).at;
    (void)co_await e.put("k7", "v7-after");      // more ops after the incident
    (void)co_await e.sync();
    co_return;
}
}  // namespace

int main() {
    Scheduler sched;
    SimClock clock(sched);
    SeededRandom rng(0x91771991u);
    const DiskFaultConfig dc = nofault();

    // ---- Source: a deterministic op stream, memtable-only (full op-log resident). ----
    // Track the Seq at three checkpoints so we can target them.
    SimDisk src_disk(sched, clock, rng, dc);
    WalEngine src(sched, src_disk);
    Seq seq_after_load = 0, seq_before_baddelete = 0, seq_after_baddelete = 0, tip = 0;
    sched.spawn(load_src(src, seq_after_load, seq_before_baddelete, seq_after_baddelete));
    sched.run();
    tip = src.last_seq();
    check(seq_after_load > 0 && seq_before_baddelete == seq_after_load + 1 &&
          seq_after_baddelete == seq_before_baddelete + 1 && tip == seq_after_baddelete + 1,
          "checkpoints are the expected contiguous Seqs");

    // (1) Full-log archive → restore to tip → live state == source @ tip.
    SimDisk arc(sched, clock, rng, dc);
    check(archive_ops(sched, src, /*from_seq=*/0, arc).ok(), "archive full op-log ok");
    bool ok = false;
    std::vector<KeyValue> at_tip = restore_and_scan(sched, clock, rng, dc, arc, tip, ok);
    check(ok, "restore to tip ok");
    check(at_tip == scan_at(sched, src, tip), "restore@tip == source@tip");

    // (2) THE motivating case — restore to the Seq JUST BEFORE the bad delete → "keep"
    //     is back with its pre-delete value; and it is GONE when restoring to after it.
    std::vector<KeyValue> before = restore_and_scan(sched, clock, rng, dc, arc, seq_before_baddelete, ok);
    check(ok, "restore to pre-incident Seq ok");
    check(before == scan_at(sched, src, seq_before_baddelete), "restore@before == source@before");
    bool keep_present = false, keep_gold = false;
    for (const KeyValue& kv : before)
        if (kv.first == "keep") { keep_present = true; keep_gold = (kv.second == "gold"); }
    check(keep_present && keep_gold, "bad DELETE undone: 'keep'='gold' restored @ pre-incident Seq");
    std::vector<KeyValue> after = restore_and_scan(sched, clock, rng, dc, arc, seq_after_baddelete, ok);
    check(ok, "restore to post-incident Seq ok");
    bool keep_gone = true;
    for (const KeyValue& kv : after) if (kv.first == "keep") keep_gone = false;
    check(keep_gone, "'keep' is gone when restoring to the post-DELETE Seq (prefix respected)");

    // (3) Every intermediate target reproduces source @ that Seq (exhaustive sweep).
    bool all_match = true;
    for (Seq t = 0; t <= tip; ++t) {
        std::vector<KeyValue> r = restore_and_scan(sched, clock, rng, dc, arc, t, ok);
        if (!ok || r != scan_at(sched, src, t)) { all_match = false; break; }
    }
    check(all_match, "restore@t == source@t for every t in [0, tip]");

    // (4) Determinism — the same op-log archives byte-identically.
    SimDisk arc2(sched, clock, rng, dc);
    check(archive_ops(sched, src, 0, arc2).ok(), "second archive ok");
    check(arc.durable_snapshot() == arc2.durable_snapshot(), "two archives of one log are byte-identical");

    // (5) Bounds — target beyond the archive is rejected; target 0 restores the empty prefix.
    {
        SimDisk d(sched, clock, rng, dc);
        WalEngine e(sched, d);
        check(!restore_pitr(sched, arc, tip + 100, e).ok(), "target beyond archive is rejected");
    }
    std::vector<KeyValue> empty = restore_and_scan(sched, clock, rng, dc, arc, 0, ok);
    check(ok && empty.empty(), "restore@0 yields the empty prefix");

    // (6) TEETH — integrity failures are rejected whole, never partially applied.
    {
        SimDisk empty_disk(sched, clock, rng, dc);
        WalEngine e(sched, empty_disk);
        check(!restore_pitr(sched, empty_disk, 1, e).ok(), "empty disk rejected (bad magic)");
    }
    {  // flipped payload byte → CRC mismatch.
        std::vector<std::byte> img = arc.durable_snapshot();
        check(img.size() > kPitrHeaderBytes, "archive has a payload");
        img[kPitrHeaderBytes + 2] ^= std::byte{0x20};
        check(!validate_pitr_archive(std::span<const std::byte>(img.data(), img.size())).ok(),
              "flipped payload byte rejected by CRC");
    }
    {  // truncated image (drop the last 5 bytes) → rejected.
        std::vector<std::byte> img = arc.durable_snapshot();
        img.resize(img.size() - 5);
        SimDisk d(sched, clock, rng, dc);
        Error w{lockstep::core::ErrorCode::Unknown, "norun"};
        sched.spawn(write_raw(d, img, w)); sched.run();
        SimDisk dd(sched, clock, rng, dc); WalEngine e(sched, dd);
        check(!restore_pitr(sched, d, 1, e).ok(), "truncated archive rejected");
    }
    {  // CRC-VALID but non-contiguous op-log (a gap) → rejected by the Seq-contiguity check.
        std::vector<ExportedOp> gappy;
        gappy.push_back(ExportedOp{1, "a", "1", false, false});
        gappy.push_back(ExportedOp{3, "c", "3", false, false});  // skips seq 2
        std::vector<std::byte> img;
        lockstep::storage::detail::build_archive_image(gappy, img);
        const Error ve = validate_pitr_archive(std::span<const std::byte>(img.data(), img.size()));
        check(!ve.ok(), "non-contiguous op Seq rejected (CRC-valid but has a gap)");
    }
    {  // CRC-VALID but out-of-order → rejected.
        std::vector<ExportedOp> disordered;
        disordered.push_back(ExportedOp{2, "b", "2", false, false});
        disordered.push_back(ExportedOp{1, "a", "1", false, false});
        std::vector<std::byte> img;
        lockstep::storage::detail::build_archive_image(disordered, img);
        check(!validate_pitr_archive(std::span<const std::byte>(img.data(), img.size())).ok(),
              "out-of-order op Seq rejected");
    }

    // (7) HONEST REFUSAL — once a flush ate part of the op-log, a from-0 archive refuses
    //     (rather than emit a log with a hole). LSM mode, tiny flush threshold.
    {
        SimDisk wal(sched, clock, rng, dc), man(sched, clock, rng, dc);
        SimFactory fac(sched, clock, rng, dc);
        WalEngine lsm(sched, wal, man, fac, /*flush_threshold=*/4);
        sched.spawn([](Engine& e) -> Task {
            for (int i = 0; i < 30; ++i)
                (void)co_await e.put("f" + std::to_string(i), "x" + std::to_string(i));
            (void)co_await e.sync();
            co_return;
        }(lsm));
        sched.run();
        check(lsm.sstable_count() > 0, "flush happened (SSTables exist)");
        SimDisk out(sched, clock, rng, dc);
        const Error ae = archive_ops(sched, lsm, 0, out);
        check(!ae.ok(), "archive from 0 after a flush is REFUSED (op-log gap, honest)");
    }

    // (8) WiscKey — large values live in the vlog as pointers; the archive DEREFS them
    //     to inline values (portable), and restore into a PLAIN engine reproduces them.
    {
        SimDisk wal(sched, clock, rng, dc), man(sched, clock, rng, dc);
        SimFactory fac(sched, clock, rng, dc);
        WalEngine w(sched, wal, man, fac, /*flush_threshold=*/0);  // flush disabled ⇒ full log resident
        w.set_value_log(/*threshold=*/8);                          // values > 8 bytes → vlog pointer
        sched.spawn([](Engine& e) -> Task {
            (void)co_await e.put("small", "tiny");
            (void)co_await e.put("big", std::string(200, 'Z'));  // large ⇒ separated to the vlog
            (void)co_await e.put("big2", std::string(500, 'Q'));
            (void)co_await e.sync();
            co_return;
        }(w));
        sched.run();
        const Seq wtip = w.last_seq();
        std::vector<KeyValue> want = scan_at(sched, w, wtip);
        SimDisk out(sched, clock, rng, dc);
        check(archive_ops(sched, w, 0, out).ok(), "archive with vlog values ok (derefed inline)");
        std::vector<KeyValue> got = restore_and_scan(sched, clock, rng, dc, out, wtip, ok);
        check(ok, "restore of vlog archive ok");
        check(got == want, "vlog large values round-trip through PITR (derefed → inline → restored)");
    }

    if (g_fail) { std::printf("storage_pitr_test: FAILED\n"); return 1; }
    std::printf("storage_pitr_test: OK (round-trip + undo-bad-delete + sweep + determinism + bounds + teeth + flush-refusal + vlog)\n");
    return 0;
}
