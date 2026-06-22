#pragma once

// Report.hpp — Phase 3 §5 step 7. The DETERMINISTIC report: format a sweep of
// (config -> {write tput, read tput, write-amp, space-amp}) as a fixed-width
// table the architect reads to settle D4 tuning. Every number is an integer
// fixed-point value (×1000 milli-units from BenchResult), so the rendered text is
// BYTE-IDENTICAL across platforms for the same (seed, configs) — the determinism
// proof re-runs the binary and diffs this text.
//
// Throughput columns are milli-ops/vtick (1000 == 1 op per virtual tick; higher
// is better). Amp columns are milli-ratios (1000 == ratio 1.0×; lower is better).
// We print the integer and a derived "X.YYY" by integer division only (no float),
// so formatting never drifts.

#include <cstdint>
#include <string>
#include <vector>

#include <lockstep/bench/Bench.hpp>

namespace lockstep::bench {

// One rendered row: a config label + its measured result (already averaged or for
// a single seed). Kept separate from BenchConfig/BenchResult so the renderer is a
// pure text function.
struct ReportRow {
    std::string label;
    BenchResult result;
};

// Render a fixed-point milli-value (×1000) as "INT.FFF" using ONLY integer
// arithmetic (no float ⇒ no platform formatting drift). e.g. 1234 -> "1.234".
[[nodiscard]] inline std::string milli_to_fixed(std::uint64_t milli) {
    const std::uint64_t whole = milli / 1000ULL;
    const std::uint64_t frac = milli % 1000ULL;
    std::string s = std::to_string(whole);
    s.push_back('.');
    // Zero-pad the fraction to 3 digits.
    if (frac < 100) {
        s.push_back('0');
    }
    if (frac < 10) {
        s.push_back('0');
    }
    s += std::to_string(frac);
    return s;
}

// Left/right pad a cell to width `w` (right-justified numbers, left-justified
// labels). Deterministic, ASCII-only.
[[nodiscard]] inline std::string pad_left(const std::string& s, std::size_t w) {
    if (s.size() >= w) {
        return s;
    }
    return std::string(w - s.size(), ' ') + s;
}
[[nodiscard]] inline std::string pad_right(const std::string& s, std::size_t w) {
    if (s.size() >= w) {
        return s;
    }
    return s + std::string(w - s.size(), ' ');
}

// Render the full report table to a string. Columns:
//   config | flush | block | bloom | wTput | rTput | wAmp | sAmp | ssts
// (wTput/rTput in milli-ops/vtick; wAmp/sAmp as X.YYY ratios; ssts = SSTable count)
[[nodiscard]] inline std::string render_report(const std::vector<ReportRow>& rows) {
    std::string out;
    const std::size_t wc = 22, wn = 7, wf = 9, wr = 7;
    out += pad_right("config", wc) + " | " + pad_left("wTput", wn) + " | " +
           pad_left("rTput", wn) + " | " + pad_left("wAmp", wf) + " | " +
           pad_left("sAmp", wf) + " | " + pad_left("ssts", wr) + "\n";
    out += std::string(wc, '-') + "-+-" + std::string(wn, '-') + "-+-" +
           std::string(wn, '-') + "-+-" + std::string(wf, '-') + "-+-" +
           std::string(wf, '-') + "-+-" + std::string(wr, '-') + "\n";
    for (const ReportRow& r : rows) {
        const std::string rtput = r.result.reads_in_memory()
                                      ? std::string("inmem")
                                      : std::to_string(r.result.read_tput_milli());
        out += pad_right(r.label, wc) + " | " +
               pad_left(std::to_string(r.result.write_tput_milli()), wn) + " | " +
               pad_left(rtput, wn) + " | " +
               pad_left(milli_to_fixed(r.result.write_amp_milli()), wf) + " | " +
               pad_left(milli_to_fixed(r.result.space_amp_milli()), wf) + " | " +
               pad_left(std::to_string(r.result.sstable_count), wr) + "\n";
    }
    return out;
}

// Aggregate a config across a seed range by SUMMING the raw counters, then the
// derived ratios are computed on the totals (ratio-of-sums, the statistically
// sound way to average rates/amplifications — never an average-of-ratios). Pure
// fn of (configs, seeds). Returns one ReportRow per config.
[[nodiscard]] inline ReportRow run_config_over_seeds(const BenchConfig& cfg,
                                                     std::uint64_t seed_lo,
                                                     std::uint64_t seed_hi) {
    BenchResult agg;
    for (std::uint64_t seed = seed_lo; seed <= seed_hi; ++seed) {
        const BenchResult r = run_one(seed, cfg);
        agg.write_ops += r.write_ops;
        agg.read_ops += r.read_ops;
        agg.write_vticks += r.write_vticks;
        agg.read_vticks += r.read_vticks;
        agg.appended_bytes += r.appended_bytes;
        agg.user_write_bytes += r.user_write_bytes;
        agg.durable_bytes += r.durable_bytes;
        agg.live_user_bytes += r.live_user_bytes;
        agg.sstable_count += r.sstable_count;  // total across seeds (a flush signal).
    }
    ReportRow row;
    row.label = cfg.label;
    row.result = agg;
    return row;
}

}  // namespace lockstep::bench
