// bench_main.cpp — Phase 3 §5 step 7 (C3.7). The standalone benchmark DRIVER that
// settles D4 tuning empirically. It sweeps the tuning knobs WalEngine exposes,
// runs each config over a seed range, and prints ONE deterministic report table
// (config -> {write tput, read tput, write-amp, space-amp}) for the architect.
//
// USAGE:
//   bench_driver            — the FULL D4 sweep (a few seconds; not in the gate).
//   bench_driver --smoke    — a tiny deterministic sweep (a few hundred ms) used
//                             by the gate's smoke ctest to prove the harness runs
//                             + is deterministic WITHOUT a long benchmark.
//
// DETERMINISM: the whole program is a pure function of (the fixed seed range, the
// configs). Two runs print byte-identical text — `bench_driver --smoke | diff`.
// No wall-clock, no <random> (all draws are the sim SeededRandom), no threads.
//
// D4 KNOB EXPOSURE (recommendation back to the engine owner): flush_threshold IS
// a WalEngine ctor arg, so it is swept directly. block_target_bytes (SSTable
// kSstTargetBlockBytes) and bloom_bits_per_key (BloomFilter default) are CURRENTLY
// HARD-CODED in SSTable.hpp — the bench reports the intended column but cannot yet
// vary them per config. RECOMMENDATION: thread a small StorageTuning{block_bytes,
// bloom_bits_per_key, bloom_nhash} struct through the WalEngine ctor into
// SSTableBuilder::build + BloomFilter::build so the bench can sweep them; the
// harness is already shaped to vary only those two fields the moment it lands.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/bench/Bench.hpp>
#include <lockstep/bench/Report.hpp>

namespace {

using lockstep::bench::BenchConfig;
using lockstep::bench::ReportRow;
using lockstep::bench::render_report;
using lockstep::bench::run_config_over_seeds;

// Build the FULL D4 sweep: vary flush_threshold (the exposed knob) across a range,
// holding the workload fixed so configs are comparable on the same op stream per
// seed. We also vary the (recorded) block/bloom columns to show the report SHAPE
// the architect will read once the engine exposes them.
std::vector<BenchConfig> full_sweep() {
    std::vector<BenchConfig> cfgs;
    BenchConfig base;
    base.write_ops = 3000;
    base.read_ops = 3000;
    base.n_keys = 384;
    base.del_fraction = 0.15;
    base.scan_fraction = 0.10;
    base.value_min = 16;
    base.value_max = 256;

    // Sweep flush threshold (the live D4 knob): small ⇒ many small SSTables (high
    // write-amp from frequent flush), large ⇒ fewer flushes (lower write-amp,
    // bigger memtable). The crossover is exactly what D4 must settle.
    for (std::uint64_t thr : {16ULL, 32ULL, 64ULL, 128ULL, 256ULL, 512ULL}) {
        BenchConfig c = base;
        c.flush_threshold = thr;
        c.label = std::string("flush=") + std::to_string(thr);
        cfgs.push_back(c);
    }
    return cfgs;
}

// A tiny smoke sweep: 2 configs, small op counts, 2 seeds. A few hundred ms total
// so the gate's smoke ctest is cheap. Same code path as the full sweep ⇒ proves
// the harness runs end-to-end + is deterministic.
std::vector<BenchConfig> smoke_sweep() {
    std::vector<BenchConfig> cfgs;
    BenchConfig base;
    base.write_ops = 200;
    base.read_ops = 200;
    base.n_keys = 64;
    base.value_min = 8;
    base.value_max = 48;
    for (std::uint64_t thr : {16ULL, 128ULL}) {
        BenchConfig c = base;
        c.flush_threshold = thr;
        c.label = std::string("flush=") + std::to_string(thr);
        cfgs.push_back(c);
    }
    return cfgs;
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        }
    }

    const std::vector<BenchConfig> cfgs = smoke ? smoke_sweep() : full_sweep();
    const std::uint64_t seed_lo = 1;
    const std::uint64_t seed_hi = smoke ? 2ULL : 16ULL;

    std::vector<ReportRow> rows;
    rows.reserve(cfgs.size());
    for (const BenchConfig& c : cfgs) {
        rows.push_back(run_config_over_seeds(c, seed_lo, seed_hi));
    }

    std::printf("====================================================================\n");
    std::printf(" Lockstep storage bench (C3.7) — D4 tuning report%s\n",
                smoke ? "  [SMOKE]" : "");
    std::printf(" seeds=%llu..%llu  configs=%zu  (deterministic: pure fn of seed+config)\n",
                static_cast<unsigned long long>(seed_lo),
                static_cast<unsigned long long>(seed_hi), cfgs.size());
    std::printf(" wTput/rTput = milli-ops per virtual tick (higher better)\n");
    std::printf(" wAmp = bytes-appended / user-bytes ; sAmp = durable-bytes / live-bytes (lower better)\n");
    std::printf(" ssts = total SSTables flushed across the seed range\n");
    std::printf("====================================================================\n");
    const std::string table = render_report(rows);
    std::fputs(table.c_str(), stdout);
    std::printf("--------------------------------------------------------------------\n");
    std::printf("D4 knob exposure: flush_threshold is swept (WalEngine ctor arg).\n");
    std::printf("block_target_bytes + bloom_bits_per_key are HARD-CODED in SSTable.hpp\n");
    std::printf("today; RECOMMEND a StorageTuning struct on the WalEngine ctor so the\n");
    std::printf("bench can sweep them (the harness is ready to vary only those fields).\n");
    return 0;
}
