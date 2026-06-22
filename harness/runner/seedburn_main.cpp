// seedburn_main.cpp — Phase 2 batch 2 (C2.7 seed-burn farm runner + V-SEED1
// one-command replay). The executable wrapper around the pure SeedBurn library
// (harness/include/lockstep/harness/SeedBurn.hpp).
//
// SPEC: specs/checker-framework.md §6.1, §7 (V-SEED1, V-SEED2, V-SHRINK1).
//
// MODES:
//   (default sweep)  : sweep [--start S] [--count N] seeds of the HONEST KV
//                      system + full §4 checker set under the FULL fault
//                      envelope. LOG every seed; assert MUST-HOLD (C-DUR=0, no
//                      INT-1/DUR-2 fabrication); TRACK (count) the failover
//                      anomalies (C-LIN/C-MONO/INT-2). STORE every MUST-HOLD
//                      breach to a file (--store, default seedburn_failures.log)
//                      so it is surfaced + replayable. Print a summary. Exit
//                      non-zero iff any MUST-HOLD breach (the gate teeth).
//   --seed S         : ONE-COMMAND REPLAY (V-SEED1). Re-run exactly seed S and
//                      print its violation class, witness, and history render
//                      fingerprint — byte-identical to the sweep's run of S.
//
// CONFIG (env + arg controlled, C2.7 "configurable range"):
//   --start S / LOCKSTEP_SEEDBURN_START   first seed (default 0x5EED0000).
//   --count N / LOCKSTEP_SEEDBURN_COUNT   how many seeds (default 2000 local).
//   --seed  S                             single-seed replay (overrides sweep).
//   --store PATH                          failing-seed log (default below).
//   --quiet                               suppress the per-seed LOG line.
//
// This is the RUNNER (not a ctest target by default — kept fast at the modest
// default, large sweeps via --count). It is NON-provider code → the
// forbidden-call lint scans it; all randomness is the seeded provider PRNG; all
// time virtual. The ONLY side effects are reading argv/env and writing the
// failing-seed log (std::ofstream — NOT a forbidden raw syscall).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <lockstep/harness/SeedBurn.hpp>
#include <lockstep/harness/kv/KvSim.hpp>

namespace {

using lockstep::harness::kv::KvConfig;
namespace sb = lockstep::harness::seedburn;

// Parse a uint64 from a decimal-or-0x-hex string; returns fallback on failure.
std::uint64_t parse_u64(const std::string& s, std::uint64_t fallback) {
    if (s.empty()) {
        return fallback;
    }
    int base = 10;
    std::size_t start = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = 2;
    }
    std::uint64_t v = 0;
    for (std::size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<std::uint64_t>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            d = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            d = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return fallback;
        }
        v = v * static_cast<std::uint64_t>(base) + d;
    }
    return v;
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);  // NOLINT — config only, not a clock/RNG.
    return (v != nullptr) ? std::string(v) : fallback;
}

// A short, stable fingerprint of a history render (so the replay receipt fits on
// one line yet still proves byte-identity vs the sweep). Pure FNV-1a — no
// std::*_distribution, no clock; a deterministic function of the bytes.
std::uint64_t fingerprint(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    // ---- defaults (env first, then arg overrides below) --------------------
    std::uint64_t start =
        parse_u64(env_or("LOCKSTEP_SEEDBURN_START", ""), 0x5EED0000ULL);
    std::uint64_t count =
        parse_u64(env_or("LOCKSTEP_SEEDBURN_COUNT", ""), 2000);
    std::uint64_t single_seed = 0;
    bool replay = false;
    bool quiet = false;
    std::string store_path = "seedburn_failures.log";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::uint64_t fb) -> std::uint64_t {
            if (i + 1 < argc) {
                return parse_u64(argv[++i], fb);
            }
            return fb;
        };
        if (a == "--start") {
            start = next(start);
        } else if (a == "--count") {
            count = next(count);
        } else if (a == "--seed") {
            single_seed = next(0);
            replay = true;
        } else if (a == "--store") {
            if (i + 1 < argc) {
                store_path = argv[++i];
            }
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: seedburn [--start S] [--count N] [--seed S] "
                "[--store PATH] [--quiet]\n"
                "  sweep N seeds of the honest KV system (MUST-HOLD gate), or\n"
                "  --seed S to REPLAY one seed byte-identically (V-SEED1).\n");
            return 0;
        }
    }

    KvConfig cfg;  // defaults: full fault envelope (DECISION-A).

    // ---- one-command replay (V-SEED1) --------------------------------------
    if (replay) {
        const sb::RunOutcome o = sb::run_one_honest(single_seed, cfg);
        std::printf("REPLAY seed=0x%llX\n",
                    static_cast<unsigned long long>(single_seed));
        std::printf("  violation_class = %s\n", o.vclass.label().c_str());
        std::printf("  must_hold       = %s\n",
                    o.vclass.must_hold_violation() ? "VIOLATED" : "ok");
        std::printf("  history_fp      = 0x%016llX (%zu bytes)\n",
                    static_cast<unsigned long long>(
                        fingerprint(o.history_render)),
                    o.history_render.size());
        if (!o.first_witness.empty()) {
            std::printf("  witness         = %s\n", o.first_witness.c_str());
        }
        // Replay is byte-identical iff a second run produces the same render.
        const sb::RunOutcome o2 = sb::run_one_honest(single_seed, cfg);
        std::printf("  byte_identical  = %s\n",
                    (o.history_render == o2.history_render) ? "yes" : "NO");
        return o.vclass.must_hold_violation() ? 2 : 0;
    }

    // ---- the seed-burn farm (C2.7 / V-SEED2) -------------------------------
    std::printf(
        "SEED-BURN FARM: honest KV + §4 checkers, FULL fault envelope\n"
        "  range = [0x%llX, 0x%llX)  (%llu seeds)\n",
        static_cast<unsigned long long>(start),
        static_cast<unsigned long long>(start + count),
        static_cast<unsigned long long>(count));

    const sb::SweepSummary sum = sb::seed_burn_sweep(
        start, count, cfg,
        [&](std::uint64_t seed, const sb::RunOutcome& o) {
            // LOG every seed (V-SEED2). One terse line; --quiet suppresses.
            if (!quiet) {
                std::printf("  seed=0x%llX class=%s\n",
                            static_cast<unsigned long long>(seed),
                            o.vclass.any() ? o.vclass.label().c_str() : "clean");
            }
        });

    // STORE failing seeds so they are surfaced + replayable (V-SEED2).
    if (!sum.failing_seeds.empty()) {
        std::ofstream out(store_path, std::ios::trunc);
        if (out) {
            out << "# seed-burn MUST-HOLD failures (§6.1). Replay: seedburn "
                   "--seed <SEED>\n";
            for (const sb::FailingSeed& f : sum.failing_seeds) {
                out << "0x" << std::hex << f.seed << std::dec
                    << " class=" << f.vclass_label
                    << " witness=" << f.witness << "\n";
            }
        }
        std::fprintf(stderr,
                     "  !! stored %zu failing seed(s) to %s\n",
                     sum.failing_seeds.size(), store_path.c_str());
    }

    // ---- summary -----------------------------------------------------------
    std::printf("\nSEED-BURN SUMMARY\n");
    std::printf("  seeds run            = %llu\n",
                static_cast<unsigned long long>(sum.count));
    std::printf("  MUST-HOLD failures   = %llu  %s\n",
                static_cast<unsigned long long>(sum.must_hold_failures),
                sum.ok() ? "(C-DUR=0, no INT-1/DUR-2 fabrication)" : "(!! HALT)");
    std::printf("  TRACKED anomalies    : C-LIN=%llu  C-MONO=%llu  "
                "INT-2-lostack=%llu  DUR-1-rejected=%llu  "
                "(failover/imprecision limit, §6.1; not failures)\n",
                static_cast<unsigned long long>(sum.lin_anomalies),
                static_cast<unsigned long long>(sum.mono_anomalies),
                static_cast<unsigned long long>(sum.lost_ack_anomalies),
                static_cast<unsigned long long>(sum.dur_rejected_anomalies));
    if (!sum.failing_seeds.empty()) {
        std::printf("  failing seeds        :\n");
        for (const sb::FailingSeed& f : sum.failing_seeds) {
            std::printf("    0x%llX  %s\n",
                        static_cast<unsigned long long>(f.seed),
                        f.vclass_label.c_str());
        }
    }

    if (!sum.ok()) {
        std::fprintf(stderr,
                     "SEED-BURN: FAIL — %llu MUST-HOLD breach(es) (§6.1 HALT)\n",
                     static_cast<unsigned long long>(sum.must_hold_failures));
        return 1;
    }
    std::printf("SEED-BURN: PASS — MUST-HOLD held across all %llu seeds.\n",
                static_cast<unsigned long long>(sum.count));
    return 0;
}
