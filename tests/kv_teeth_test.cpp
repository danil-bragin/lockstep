// kv_teeth_test.cpp — Phase 2 batch 2, the §6 harness-has-teeth GATE
// (specs/checker-framework.md §6 V-TEETH1 + §6.1). Author = a THIRD independent
// agent (≠ system author, ≠ checker author — §9 DECISION-D). Independence is the
// whole point: this proves the §4 checker set has TEETH by feeding it
// DELIBERATELY-BROKEN KV systems through the SAME run_kv_sim_with driver and
// asserting the checkers FLAG each (with a witness + replayable seed). A checker
// set that passes a known-buggy system IS the bug (§6).
//
// WHAT THIS ASSERTS (binding):
//   (A) FOR EACH buggy variant, across several seeds under the full fault
//       envelope: at LEAST one checker FLAGS it, with a non-empty witness and the
//       stamped (replayable) seed. The test FAILS if any buggy variant goes
//       UNFLAGGED across all its seeds — that would mean the checkers lack teeth.
//   (B) The FABRICATE_VALUE and LOSE_DURABLE variants are specifically caught by
//       the DURABILITY/FABRICATION checkers (C-INT and/or C-DUR) — the must-hold
//       levels per §6.1 (a fabrication/durability violation MUST be caught).
//   (C) The HONEST system has NO durability/fabrication FALSE POSITIVES across
//       the same seeds: C-DUR never fires, and C-INT never fires its INT-1
//       (fabricated value) half on the (WAL-CRC-fixed) honest system. The
//       expected failover anomalies (C-LIN non-linearizable, C-MONO
//       read-your-writes, C-INT/INT-2 lost-ack) MAY appear on the honest system
//       and are NOT asserted absent — that is the accepted non-consensus limit
//       (§6.1). We report them, we do not fail on them.
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider
// PRNG threaded through run_kv_sim_with; all time is virtual. This is NON-
// provider code → the forbidden-call lint scans it. Seeds are printed for replay
// (V-CHK2). Every run is bounded (inherits CTest TIMEOUT 90).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/CheckerRunner.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/checkers/Durability.hpp>
#include <lockstep/harness/checkers/Integrity.hpp>
#include <lockstep/harness/checkers/Linearizable.hpp>
#include <lockstep/harness/checkers/SessionMonotonic.hpp>
#include <lockstep/harness/kv/BuggyKvSystems.hpp>
#include <lockstep/harness/kv/KvSim.hpp>

namespace {

using lockstep::harness::CheckerRunner;
using lockstep::harness::History;
using lockstep::harness::Verdict;
using lockstep::harness::checkers::DurabilityChecker;
using lockstep::harness::checkers::IntegrityChecker;
using lockstep::harness::checkers::LinearizableChecker;
using lockstep::harness::checkers::SessionMonotonicChecker;
using lockstep::harness::kv::KvConfig;
using lockstep::harness::kv::run_kv_sim_with;
using lockstep::harness::kv::detail::SystemFactory;
namespace buggy = lockstep::harness::kv::buggy;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

// A fresh checker set (insertion order = C-INT, C-MONO, C-LIN, C-DUR).
void add_all_checkers(CheckerRunner& runner) {
    runner.add(std::make_unique<IntegrityChecker>());
    runner.add(std::make_unique<SessionMonotonicChecker>());
    runner.add(std::make_unique<LinearizableChecker>());
    runner.add(std::make_unique<DurabilityChecker>());
}

// Which checker a verdict came from, by its checker order index.
enum CkIdx { CK_INT = 0, CK_MONO = 1, CK_LIN = 2, CK_DUR = 3 };

// Per-run flag summary: which checkers fired (and whether C-INT fired its
// fabrication half INT-1 specifically vs its lost-ack half INT-2).
struct Flags {
    bool any = false;
    bool int_fired = false;
    bool int_fabrication = false;  // INT-1 (fabricated/torn value)
    bool int_lost_ack = false;     // INT-2 (lost ack'd write)
    bool mono_fired = false;
    bool lin_fired = false;
    bool dur_fired = false;
    bool dur_fabrication = false;   // DUR-1/DUR-2 (rejected/fabricated durable)
    std::string sample_witness;     // the first violation witness (for the receipt)
    std::uint64_t sample_seed = 0;
};

// Run one (factory, seed) and return which checkers flagged it.
Flags run_and_classify(const SystemFactory& factory, std::uint64_t seed,
                       const KvConfig& cfg) {
    CheckerRunner runner;
    add_all_checkers(runner);
    const History h = run_kv_sim_with(seed, factory, &runner, cfg);
    const std::vector<Verdict> vs = runner.finalize(h, seed);

    Flags f;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        const Verdict& v = vs[i];
        if (v.ok) {
            continue;
        }
        if (!f.any) {
            f.any = true;
            f.sample_witness = v.witness;
            f.sample_seed = v.seed;
        }
        switch (static_cast<CkIdx>(i)) {
            case CK_INT:
                f.int_fired = true;
                if (v.witness.find("FABRICATED READ") != std::string::npos) {
                    f.int_fabrication = true;
                }
                if (v.witness.find("LOST ACK'D WRITE") != std::string::npos) {
                    f.int_lost_ack = true;
                }
                break;
            case CK_MONO:
                f.mono_fired = true;
                break;
            case CK_LIN:
                f.lin_fired = true;
                break;
            case CK_DUR:
                f.dur_fired = true;
                if (v.witness.find("FABRICATED DURABLE") != std::string::npos ||
                    v.witness.find("REJECTED WRITE SURFACED") != std::string::npos) {
                    f.dur_fabrication = true;
                }
                break;
        }
    }
    return f;
}

// Outcome of sweeping one buggy variant across its seeds.
struct VariantResult {
    int seeds = 0;
    int flagged = 0;           // seeds where SOME checker fired
    bool ever_int = false;
    bool ever_mono = false;
    bool ever_lin = false;
    bool ever_dur = false;
    bool ever_int_fab = false;
    bool ever_int_lostack = false;
    bool ever_dur_fab = false;
    std::string sample_witness;
    std::uint64_t sample_seed = 0;
};

VariantResult sweep_variant(const char* name, const SystemFactory& factory,
                            std::uint64_t seed_base, int n_seeds,
                            const KvConfig& cfg) {
    VariantResult r;
    for (int s = 0; s < n_seeds; ++s) {
        const std::uint64_t seed = seed_base + static_cast<std::uint64_t>(s);
        const Flags f = run_and_classify(factory, seed, cfg);
        ++r.seeds;
        if (f.any) {
            ++r.flagged;
            if (r.sample_witness.empty()) {
                r.sample_witness = f.sample_witness;
                r.sample_seed = f.sample_seed;
            }
        }
        r.ever_int = r.ever_int || f.int_fired;
        r.ever_mono = r.ever_mono || f.mono_fired;
        r.ever_lin = r.ever_lin || f.lin_fired;
        r.ever_dur = r.ever_dur || f.dur_fired;
        r.ever_int_fab = r.ever_int_fab || f.int_fabrication;
        r.ever_int_lostack = r.ever_int_lostack || f.int_lost_ack;
        r.ever_dur_fab = r.ever_dur_fab || f.dur_fabrication;
    }
    std::printf(
        "  [%-24s] seeds=%d flagged=%d  fired{INT=%d MONO=%d LIN=%d DUR=%d}\n",
        name, r.seeds, r.flagged, r.ever_int ? 1 : 0, r.ever_mono ? 1 : 0,
        r.ever_lin ? 1 : 0, r.ever_dur ? 1 : 0);
    if (!r.sample_witness.empty()) {
        std::printf("       sample seed=0x%llX witness=%s\n",
                    static_cast<unsigned long long>(r.sample_seed),
                    r.sample_witness.c_str());
    }
    return r;
}

// =====================================================================
// (A)/(B) BUGGY VARIANTS — each MUST be flagged; durability/fabrication
//         variants MUST be caught by C-INT/C-DUR.
// =====================================================================

void teeth_buggy_variants() {
    std::printf("BUGGY-VARIANT TEETH (full envelope; each MUST be flagged):\n");
    KvConfig cfg;  // defaults: 3 nodes, 3 clients, full envelope, crashes+parts
    const int kSeeds = 12;

    // DROP_WRITE_ON_PARTITION → lost ack'd write → C-INT/INT-2.
    {
        VariantResult r = sweep_variant("DROP_WRITE_ON_PARTITION",
                                        buggy::DropWriteOnPartition::factory(),
                                        0xDEAD0000ULL, kSeeds, cfg);
        check(r.flagged > 0,
              "DROP_WRITE_ON_PARTITION is FLAGGED by some checker (teeth)");
        check(r.ever_int,
              "DROP_WRITE_ON_PARTITION caught by C-INT (lost ack'd write)");
    }

    // STALE_READ → read-your-writes / non-linearizable → C-MONO and/or C-LIN.
    {
        VariantResult r =
            sweep_variant("STALE_READ", buggy::StaleRead::factory(),
                          0xBEEF0000ULL, kSeeds, cfg);
        check(r.flagged > 0, "STALE_READ is FLAGGED by some checker (teeth)");
        check(r.ever_mono || r.ever_lin,
              "STALE_READ caught by C-MONO and/or C-LIN");
    }

    // SKIP_CAS_COMPARE → a cas commits against a non-matching value → C-LIN.
    {
        VariantResult r =
            sweep_variant("SKIP_CAS_COMPARE", buggy::SkipCasCompare::factory(),
                          0xCA50000ULL, kSeeds, cfg);
        check(r.flagged > 0,
              "SKIP_CAS_COMPARE is FLAGGED by some checker (teeth)");
        check(r.ever_lin, "SKIP_CAS_COMPARE caught by C-LIN (non-linearizable)");
    }

    // FABRICATE_VALUE → fabricated/torn value → C-INT/INT-1 AND C-DUR/DUR-2.
    // This is a §6.1 MUST-CATCH (fabrication class).
    {
        VariantResult r =
            sweep_variant("FABRICATE_VALUE", buggy::FabricateValue::factory(),
                          0xFAB00000ULL, kSeeds, cfg);
        check(r.flagged > 0,
              "FABRICATE_VALUE is FLAGGED by some checker (teeth)");
        check(r.ever_int_fab,
              "FABRICATE_VALUE caught by C-INT/INT-1 (fabricated read) "
              "[§6.1 MUST-CATCH]");
        check(r.ever_dur_fab,
              "FABRICATE_VALUE caught by C-DUR/DUR-2 (fabricated durable) "
              "[§6.1 MUST-CATCH]");
    }

    // LOSE_DURABLE_ON_RECOVER → ack'd durable write vanishes → C-INT/INT-2.
    // This is a §6.1 MUST-CATCH (durability class).
    {
        VariantResult r = sweep_variant("LOSE_DURABLE_ON_RECOVER",
                                        buggy::LoseDurableOnRecover::factory(),
                                        0x105E0000ULL, kSeeds, cfg);
        check(r.flagged > 0,
              "LOSE_DURABLE_ON_RECOVER is FLAGGED by some checker (teeth)");
        check(r.ever_int_lostack,
              "LOSE_DURABLE_ON_RECOVER caught by C-INT/INT-2 (lost durable "
              "ack'd write) [§6.1 MUST-CATCH durability class]");
    }
}

// =====================================================================
// (C) HONEST SYSTEM — NO durability/fabrication FALSE POSITIVES.
//     C-DUR must NEVER fire; C-INT must NEVER fire its INT-1 fabrication half.
//     (Expected failover anomalies C-LIN / C-MONO / INT-2 MAY appear — NOT
//     asserted absent; §6.1 accepted non-consensus limit.)
// =====================================================================

void honest_no_durability_false_positives() {
    std::printf(
        "HONEST SYSTEM (no durability/fabrication false positives, C-DUR=0):\n");
    KvConfig cfg;
    const std::uint64_t kSeedBase = 0xC0FFEE00ULL;
    const int kSeeds = 40;

    int dur_fires = 0;
    int int_fab_fires = 0;
    int lin_fires = 0;   // expected/tracked, not asserted absent
    int mono_fires = 0;  // expected/tracked, not asserted absent
    int int_lostack_fires = 0;  // expected/tracked

    for (int s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = kSeedBase + static_cast<std::uint64_t>(s);
        // The honest system is the default factory (run_kv_sim_with default).
        const Flags f =
            run_and_classify(lockstep::harness::kv::detail::default_factory,
                             seed, cfg);
        if (f.dur_fired) {
            ++dur_fires;
            std::fprintf(stderr,
                         "  !! HONEST C-DUR FALSE POSITIVE seed=0x%llX wit=%s\n",
                         static_cast<unsigned long long>(seed),
                         f.sample_witness.c_str());
        }
        if (f.int_fabrication) {
            ++int_fab_fires;
            std::fprintf(stderr,
                         "  !! HONEST C-INT/INT-1 FABRICATION FALSE POSITIVE "
                         "seed=0x%llX wit=%s\n",
                         static_cast<unsigned long long>(seed),
                         f.sample_witness.c_str());
        }
        if (f.lin_fired) {
            ++lin_fires;
        }
        if (f.mono_fired) {
            ++mono_fires;
        }
        if (f.int_lost_ack) {
            ++int_lostack_fires;
        }
    }

    std::printf(
        "  honest run: %d seeds  MUST-HOLD{C-DUR=%d INT-1-fab=%d}  "
        "TRACKED{C-LIN=%d C-MONO=%d INT-2-lostack=%d}\n",
        kSeeds, dur_fires, int_fab_fires, lin_fires, mono_fires,
        int_lostack_fires);

    // MUST HOLD (§6.1): durability + no-fabrication, on the WAL-fixed honest
    // system. A fabrication/durability violation HALTS the gate.
    check(dur_fires == 0,
          "HONEST: C-DUR never fires (no durability/fabrication false positive)");
    check(int_fab_fires == 0,
          "HONEST: C-INT/INT-1 never fires (no fabricated-value false positive)");
    // (We intentionally do NOT assert C-LIN/C-MONO/INT-2 absent — accepted
    //  non-consensus failover limit, §6.1. They are reported above.)
}

// =====================================================================
// DETERMINISM — a buggy variant run twice on a seed ⇒ identical verdicts.
// (Byte-identical proof at the binary level is the external double-run diff;
//  this in-process check guards the same property cheaply.)
// =====================================================================

void determinism_run() {
    std::printf("DETERMINISM (same seed ⇒ identical buggy verdicts):\n");
    KvConfig cfg;
    const std::uint64_t seed = 0xD37EE7711ULL;
    const SystemFactory factory = buggy::FabricateValue::factory();

    auto run = [&](std::string& out) {
        CheckerRunner runner;
        add_all_checkers(runner);
        const History h = run_kv_sim_with(seed, factory, &runner, cfg);
        const std::vector<Verdict> vs = runner.finalize(h, seed);
        out.clear();
        for (const Verdict& v : vs) {
            out += (v.ok ? "OK|" : "VIOL|");
            out += v.witness;
            out += "\n";
        }
    };
    std::string a;
    std::string b;
    run(a);
    run(b);
    check(a == b, "same seed ⇒ byte-identical buggy verdict set (deterministic)");
}

}  // namespace

int main() {
    std::printf("kv_teeth_test: §6 harness-has-teeth gate (V-TEETH1)\n");

    teeth_buggy_variants();
    honest_no_durability_false_positives();
    determinism_run();

    if (g_failures != 0) {
        std::fprintf(stderr, "kv_teeth_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("kv_teeth_test: OK\n");
    return 0;
}
