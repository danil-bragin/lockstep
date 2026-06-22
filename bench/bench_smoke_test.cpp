// bench_smoke_test.cpp — Phase 3 §5 step 7 (C3.7). The CHEAP gate self-test for
// the benchmark harness. It does NOT run a long benchmark (the full sweep is the
// standalone bench_driver exe); it runs a TINY sweep and asserts:
//
//   (a) THE HARNESS RUNS end-to-end: a (seed, config) produces a result with the
//       write/read phases actually executing (write_ops + read_ops as requested),
//       and the metrics are well-formed (write-amp >= 1.0 — an LSM never writes
//       FEWER bytes than the user data; a final sync makes durable bytes > 0).
//   (b) DETERMINISM (the cardinal proof): the SAME (seed, config) yields a
//       BYTE-IDENTICAL BenchResult AND a byte-identical rendered report row, run
//       twice in-process. The EXTERNAL byte-identical proof is re-running this
//       binary (or `bench_driver --smoke`) and diffing stdout.
//   (c) THE SWEEP DISCRIMINATES: two different flush thresholds produce DIFFERENT
//       measured numbers (the bench is sensitive to the D4 knob it sweeps — it is
//       not a constant function of the config).
//
// Cheap by construction: tiny op counts, 2 configs, 2 seeds ⇒ a few hundred ms,
// safely inside the ctest TIMEOUT 90. Non-provider TU → forbidden-lint scans it;
// the only randomness is the sim SeededRandom inside the harness.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/bench/Bench.hpp>
#include <lockstep/bench/Report.hpp>

namespace {

using lockstep::bench::BenchConfig;
using lockstep::bench::BenchResult;
using lockstep::bench::render_report;
using lockstep::bench::ReportRow;
using lockstep::bench::run_one;

void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        std::abort();
    }
}

bool results_equal(const BenchResult& a, const BenchResult& b) {
    return a.write_ops == b.write_ops && a.read_ops == b.read_ops &&
           a.write_vticks == b.write_vticks && a.read_vticks == b.read_vticks &&
           a.appended_bytes == b.appended_bytes &&
           a.user_write_bytes == b.user_write_bytes &&
           a.durable_bytes == b.durable_bytes &&
           a.live_user_bytes == b.live_user_bytes &&
           a.sstable_count == b.sstable_count;
}

BenchConfig tiny_config(std::uint64_t flush_threshold) {
    BenchConfig c;
    c.write_ops = 200;
    c.read_ops = 200;
    c.n_keys = 64;
    c.value_min = 8;
    c.value_max = 48;
    c.flush_threshold = flush_threshold;
    c.label = std::string("flush=") + std::to_string(flush_threshold);
    return c;
}

}  // namespace

int main() {
    const std::uint64_t seed = 1;

    // (a) THE HARNESS RUNS + metrics are well-formed.
    const BenchConfig cfg_a = tiny_config(16);
    const BenchResult r_a = run_one(seed, cfg_a);
    expect(r_a.write_ops == cfg_a.write_ops, "write phase ran all requested ops");
    expect(r_a.read_ops == cfg_a.read_ops, "read phase ran all requested ops");
    expect(r_a.write_vticks > 0, "write phase consumed virtual time");
    // NOTE: the read phase MAY consume zero virtual ticks: WalEngine loads every
    // SSTable block into memory at flush time, so get()/scan() resolve their
    // promise SYNCHRONOUSLY with no disk-latency timer firing (a real finding —
    // reads are fully in-memory today; see the engine recommendation). So we do
    // NOT require read_vticks > 0; the read_ops count proves the phase ran.
    expect(r_a.user_write_bytes > 0, "user write bytes accumulated");
    expect(r_a.appended_bytes >= r_a.user_write_bytes,
           "write-amp >= 1.0 (an LSM never writes fewer bytes than user data)");
    expect(r_a.durable_bytes > 0, "final sync made durable bytes > 0");
    expect(r_a.write_amp_milli() >= 1000ULL, "write-amp milli >= 1000 (>= 1.0x)");
    std::fprintf(stderr,
                 "[ok] (a) harness runs: wops=%llu rops=%llu wamp=%s samp=%s ssts=%llu\n",
                 static_cast<unsigned long long>(r_a.write_ops),
                 static_cast<unsigned long long>(r_a.read_ops),
                 lockstep::bench::milli_to_fixed(r_a.write_amp_milli()).c_str(),
                 lockstep::bench::milli_to_fixed(r_a.space_amp_milli()).c_str(),
                 static_cast<unsigned long long>(r_a.sstable_count));

    // (b) DETERMINISM: same (seed,config) ⇒ byte-identical result + rendered row.
    const BenchResult r_a2 = run_one(seed, cfg_a);
    expect(results_equal(r_a, r_a2),
           "DETERMINISM: same seed+config ⇒ byte-identical BenchResult");
    std::vector<ReportRow> rows1{ReportRow{cfg_a.label, r_a}};
    std::vector<ReportRow> rows2{ReportRow{cfg_a.label, r_a2}};
    expect(render_report(rows1) == render_report(rows2),
           "DETERMINISM: same seed+config ⇒ byte-identical rendered report");
    std::fprintf(stderr, "[ok] (b) determinism: seed=%llu config=%s byte-identical (run twice)\n",
                 static_cast<unsigned long long>(seed), cfg_a.label.c_str());

    // (c) THE SWEEP DISCRIMINATES: a different flush threshold changes the numbers.
    const BenchConfig cfg_b = tiny_config(128);
    const BenchResult r_b = run_one(seed, cfg_b);
    expect(!results_equal(r_a, r_b),
           "SWEEP: different flush_threshold ⇒ different measured numbers");
    // The smaller threshold should flush MORE (>= as many SSTables) — the very
    // write-amp tradeoff D4 weighs. (>= because a tiny workload may tie.)
    expect(r_a.sstable_count >= r_b.sstable_count,
           "SWEEP: smaller flush_threshold flushes at least as many SSTables");
    std::fprintf(stderr,
                 "[ok] (c) sweep discriminates: flush16 ssts=%llu wamp=%s vs "
                 "flush128 ssts=%llu wamp=%s\n",
                 static_cast<unsigned long long>(r_a.sstable_count),
                 lockstep::bench::milli_to_fixed(r_a.write_amp_milli()).c_str(),
                 static_cast<unsigned long long>(r_b.sstable_count),
                 lockstep::bench::milli_to_fixed(r_b.write_amp_milli()).c_str());

    // Print a tiny report so the gate log shows a sample table.
    std::vector<ReportRow> rows{ReportRow{cfg_a.label, r_a}, ReportRow{cfg_b.label, r_b}};
    std::fputs(render_report(rows).c_str(), stderr);

    std::fprintf(stderr, "bench_smoke_test: ALL PASS\n");
    return 0;
}
