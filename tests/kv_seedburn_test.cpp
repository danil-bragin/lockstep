// kv_seedburn_test.cpp — Phase 2 batch 2 (C2.7 seed-burn farm + C2.8 shrinking
// gate). Asserts the seed/replay/shrinking contract (specs/checker-framework.md
// §6.1, §7: V-SEED1, V-SEED2, V-SHRINK1).
//
// WHAT THIS ASSERTS (binding):
//   (A) SEED-BURN runs N seeds of the HONEST KV system + full §4 checker set
//       under the FULL fault envelope and the MUST-HOLD invariants HOLD on every
//       seed: C-DUR never fires, C-INT/INT-1 (fabrication) never fires (§6.1).
//       The expected failover anomalies (C-LIN/C-MONO/INT-2) MAY appear and are
//       only TRACKED — never asserted absent (accepted non-consensus limit).
//   (B) SHRINKING: a KNOWN failure from a buggy variant (FABRICATE_VALUE — the
//       §6.1 fabrication MUST-CATCH class) auto-reduces to a SMALLER config that
//       STILL triggers the SAME violation class, and the minimal case is a
//       proper subset of the start config (strictly smaller). The shrinker
//       PRESERVES the violation (a shrink that loses the bug is rejected).
//   (C) REPLAY (V-SEED1): re-running a seed reproduces a byte-identical history
//       render (the replay surface) — twice, for both honest and buggy.
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider
// PRNG threaded through run_kv_sim_with; all time virtual. NON-provider code →
// forbidden-call lint scans it. Bounded (inherits CTest TIMEOUT 90). Seeds are
// printed for replay (V-CHK2).

#include <cstdint>
#include <cstdio>
#include <string>

#include <lockstep/harness/SeedBurn.hpp>
#include <lockstep/harness/kv/BuggyKvSystems.hpp>
#include <lockstep/harness/kv/KvSim.hpp>

namespace {

using lockstep::harness::kv::KvConfig;
using lockstep::harness::kv::detail::SystemFactory;
namespace sb = lockstep::harness::seedburn;
namespace buggy = lockstep::harness::kv::buggy;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

// A crude "config size" metric: the total budget/episode/node knobs. Used only
// to prove the shrink produced a STRICTLY SMALLER config (a real reduction).
std::uint64_t config_size(const KvConfig& c) {
    return c.ops_per_client + c.n_clients + c.n_keys + c.n_nodes +
           c.crash_episodes + c.partition_episodes + c.kill_episodes;
}

// =====================================================================
// (A) SEED-BURN — honest system, MUST-HOLD holds across N seeds (V-SEED2).
// =====================================================================
// This sweep is ALSO the regression test for V-RKV1 (no Entry* held across a
// co_await in ReplicatedKvSystem): it drives the honest system across many
// honest seeds under the full envelope. Before the fix, ASan flagged a
// container-overflow (use-after-realloc) here; it must run clean now.
void seedburn_honest_must_hold() {
    std::printf("(A) SEED-BURN honest must-hold (full envelope):\n");
    KvConfig cfg;
    const std::uint64_t kStart = 0x5EEDBA5EULL;
    const std::uint64_t kCount = 256;  // modest gate size; fast + has teeth.

    const sb::SweepSummary sum = sb::seed_burn_sweep(kStart, kCount, cfg);

    std::printf(
        "  seeds=%llu  MUST-HOLD-failures=%llu  "
        "TRACKED{C-LIN=%llu C-MONO=%llu INT-2=%llu DUR-1=%llu}\n",
        static_cast<unsigned long long>(sum.count),
        static_cast<unsigned long long>(sum.must_hold_failures),
        static_cast<unsigned long long>(sum.lin_anomalies),
        static_cast<unsigned long long>(sum.mono_anomalies),
        static_cast<unsigned long long>(sum.lost_ack_anomalies),
        static_cast<unsigned long long>(sum.dur_rejected_anomalies));

    check(sum.count == kCount, "seed-burn ran the requested seed count");
    check(sum.must_hold_failures == 0,
          "HONEST: 0 MUST-HOLD failures across the sweep (C-DUR=0, no INT-1 "
          "fabrication) [§6.1]");
    check(sum.failing_seeds.empty(),
          "HONEST: no failing seeds stored (must-hold held)");
    // We intentionally do NOT assert the tracked anomalies absent (§6.1 limit).
}

// =====================================================================
// (B) SHRINKING — a buggy-variant failure shrinks to a smaller repro (V-SHRINK1).
// =====================================================================
void shrink_known_failure() {
    std::printf("(B) SHRINK known failure (FABRICATE_VALUE → minimal repro):\n");

    // A larger-than-default START config so the shrink has room to reduce.
    KvConfig start;
    start.n_nodes = 5;
    start.n_clients = 5;
    start.n_keys = 6;
    start.ops_per_client = 40;
    start.crash_episodes = 4;
    start.partition_episodes = 3;
    start.kill_episodes = 0;

    const SystemFactory factory = buggy::FabricateValue::factory();

    // Find a seed that triggers the fabrication MUST-CATCH class under `start`.
    std::uint64_t found_seed = 0;
    sb::ViolationClass target;
    bool have = false;
    for (std::uint64_t s = 0xFAB00000ULL; s < 0xFAB00000ULL + 64; ++s) {
        const sb::RunOutcome o = sb::run_one(s, factory, start);
        if (o.vclass.int_fabrication || o.vclass.dur_fab2) {
            found_seed = s;
            // Preserve EXACTLY the fabrication class (the §6.1 must-catch).
            target.int_fabrication = o.vclass.int_fabrication;
            target.dur_fab2 = o.vclass.dur_fab2;
            have = true;
            break;
        }
    }
    check(have, "found a seed triggering the fabrication class under start cfg");
    if (!have) {
        return;
    }

    std::printf("  start seed=0x%llX  target=%s\n",
                static_cast<unsigned long long>(found_seed),
                target.label().c_str());

    const sb::ShrinkResult r =
        sb::shrink_failure(found_seed, factory, start, target);

    const std::uint64_t before_sz = config_size(r.before);
    const std::uint64_t after_sz = config_size(r.after);
    std::printf(
        "  BEFORE: nodes=%llu clients=%llu keys=%llu ops=%llu crash=%llu "
        "part=%llu  size=%llu  class=%s\n",
        static_cast<unsigned long long>(r.before.n_nodes),
        static_cast<unsigned long long>(r.before.n_clients),
        static_cast<unsigned long long>(r.before.n_keys),
        static_cast<unsigned long long>(r.before.ops_per_client),
        static_cast<unsigned long long>(r.before.crash_episodes),
        static_cast<unsigned long long>(r.before.partition_episodes),
        static_cast<unsigned long long>(before_sz), r.before_label.c_str());
    std::printf(
        "  AFTER : nodes=%llu clients=%llu keys=%llu ops=%llu crash=%llu "
        "part=%llu  size=%llu  class=%s  (iters=%llu)\n",
        static_cast<unsigned long long>(r.after.n_nodes),
        static_cast<unsigned long long>(r.after.n_clients),
        static_cast<unsigned long long>(r.after.n_keys),
        static_cast<unsigned long long>(r.after.ops_per_client),
        static_cast<unsigned long long>(r.after.crash_episodes),
        static_cast<unsigned long long>(r.after.partition_episodes),
        static_cast<unsigned long long>(after_sz), r.after_label.c_str(),
        static_cast<unsigned long long>(r.iterations));

    // V-SHRINK1: the minimal case STILL triggers the SAME violation class.
    check(r.preserved,
          "SHRINK: minimal config still triggers the SAME violation class "
          "(violation PRESERVED) [V-SHRINK1]");
    // A real reduction: the after config is strictly smaller than the before.
    check(after_sz < before_sz,
          "SHRINK: minimal config is STRICTLY smaller than the start config");
    // The shrinker actually did work (at least one reduction stuck).
    check(r.iterations > 0, "SHRINK: at least one reduction step converged");

    // V-SHRINK1 byte-identical replay of the MINIMAL case.
    const sb::RunOutcome m1 = sb::run_one(found_seed, factory, r.after);
    const sb::RunOutcome m2 = sb::run_one(found_seed, factory, r.after);
    check(m1.history_render == m2.history_render,
          "SHRINK: minimal case replays BYTE-IDENTICALLY [V-SHRINK1]");
    check(m1.vclass.contains(target),
          "SHRINK: independent replay of minimal case still in target class");
}

// =====================================================================
// (C) REPLAY — same seed ⇒ byte-identical history render (V-SEED1).
// =====================================================================
void replay_byte_identical() {
    std::printf("(C) REPLAY byte-identical (V-SEED1):\n");
    KvConfig cfg;

    // Honest system: a seed re-run produces the identical render twice.
    {
        const std::uint64_t seed = 0x5EED0001ULL;
        const sb::RunOutcome a = sb::run_one_honest(seed, cfg);
        const sb::RunOutcome b = sb::run_one_honest(seed, cfg);
        check(a.history_render == b.history_render,
              "REPLAY honest: same seed ⇒ byte-identical history render");
        std::printf("  honest seed=0x%llX render_bytes=%zu identical=%s\n",
                    static_cast<unsigned long long>(seed),
                    a.history_render.size(),
                    (a.history_render == b.history_render) ? "yes" : "NO");
    }

    // Buggy system: same seed ⇒ identical render AND identical violation class.
    {
        const std::uint64_t seed = 0xFAB00007ULL;
        const SystemFactory factory = buggy::FabricateValue::factory();
        const sb::RunOutcome a = sb::run_one(seed, factory, cfg);
        const sb::RunOutcome b = sb::run_one(seed, factory, cfg);
        check(a.history_render == b.history_render,
              "REPLAY buggy: same seed ⇒ byte-identical history render");
        check(a.vclass.label() == b.vclass.label(),
              "REPLAY buggy: same seed ⇒ identical violation class");
        std::printf("  buggy  seed=0x%llX render_bytes=%zu class=%s identical=%s\n",
                    static_cast<unsigned long long>(seed),
                    a.history_render.size(), a.vclass.label().c_str(),
                    (a.history_render == b.history_render) ? "yes" : "NO");
    }
}

}  // namespace

int main() {
    std::printf("kv_seedburn_test: C2.7 seed-burn + C2.8 shrinking gate\n");

    seedburn_honest_must_hold();
    shrink_known_failure();
    replay_byte_identical();

    if (g_failures != 0) {
        std::fprintf(stderr, "kv_seedburn_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("kv_seedburn_test: OK\n");
    return 0;
}
