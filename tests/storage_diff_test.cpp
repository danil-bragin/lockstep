// storage_diff_test.cpp — Phase 3 §5 step 1 self-test for the storage
// verification machinery (Engine interface + Oracle + differential harness).
//
// WHAT IT PROVES (one block per requirement; storage-engine.md §4/§5 step 1):
//   (a) ALWAYS-MATCH BASELINE: drive Oracle-vs-Oracle through run_diff across a
//       seed sweep → every run reports ok with NO witness. This proves the
//       comparator actually COMPARES (two identical engines agree everywhere) and
//       that the harness does not spuriously flag a correct engine.
//   (b) TEETH (the load-bearing proof): plug DELIBERATELY-WRONG engines in as the
//       SUT against the honest Oracle reference. Each MUST be FLAGGED with a
//       witness. A harness that passes a known-wrong engine is itself the bug, so
//       we assert !ok AND that the witness carries the seed + a note.
//         * StaleBoundOracle    — off-by-one on the snapshot bound (uses seq < at
//           instead of seq <= at): a read AT a version it just wrote misses it.
//         * DeleteIgnoredOracle — del is a no-op (still consumes a Seq, so V-MONO
//           parity holds), so a deleted key keeps reading its old value while the
//           honest oracle reads ∅ (V-SNAP delete-visibility violation).
//   (c) DETERMINISM: run_diff is a pure function of (seed). Run a wrong-engine
//       diff TWICE on the same seed and assert byte-identical witnesses. The
//       EXTERNAL byte-identical proof is re-running the whole binary (verify
//       receipt: storage_diff_test run twice ⇒ identical stderr).
//
// This file is non-provider code → the forbidden-call lint scans it. No
// <chrono>/<thread>/<random>: the only randomness is the sim SeededRandom inside
// the harness; everything else is deterministic.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>

#include <lockstep/storage/DiffHarness.hpp>
#include <lockstep/storage/Engine.hpp>
#include <lockstep/storage/Oracle.hpp>

namespace {

using lockstep::core::make_promise;
using lockstep::core::Scheduler;
using lockstep::storage::DiffConfig;
using lockstep::storage::DiffVerdict;
using lockstep::storage::DiffWitness;
using lockstep::storage::Future;
using lockstep::storage::Key;
using lockstep::storage::Oracle;
using lockstep::storage::run_diff;
using lockstep::storage::Seq;
using lockstep::storage::Value;

// ---------------------------------------------------------------------------
// DELIBERATELY-WRONG ENGINES (the teeth). Each is an Oracle subclass that
// perturbs EXACTLY ONE seam so the defect is isolated + obvious — the same
// pattern Phase-2's BuggyKvSystems used to prove the checker set had teeth.
// ---------------------------------------------------------------------------

// BUG: off-by-one on the snapshot bound. The correct MVCC rule is "newest version
// with seq <= at"; this serves "newest with seq <= at-1" (== seq < at), so a read
// AT the exact version a value was committed misses it (returns the prior version
// or ∅). It reuses the base's correct walk at a shifted bound — minimal + clear.
class StaleBoundOracle final : public Oracle {
public:
    using Oracle::Oracle;

protected:
    [[nodiscard]] std::optional<Value> lookup(const Key& key, Seq at) const override {
        if (at == 0) {
            return std::nullopt;
        }
        return Oracle::lookup(key, at - 1);  // THE BUG: excludes version == at.
    }
};

// BUG: a del that does not delete. It still consumes a Seq (via a no-op commit
// under a reserved key) so commit-seq parity with the honest reference holds and
// the bug is ISOLATED to read visibility: the deleted key keeps its last live
// version, so a post-delete read returns the old value while the reference reads
// ∅. The canonical "delete that doesn't delete" defect.
class DeleteIgnoredOracle final : public Oracle {
public:
    explicit DeleteIgnoredOracle(Scheduler& sched) : Oracle(sched), sched_(&sched) {}

    [[nodiscard]] Future<Seq> del(Key /*key*/) override {
        // No-op delete: bump the global Seq via a commit under a reserved key so
        // V-MONO parity holds, but DO NOT tombstone the requested key.
        const Seq seq = commit(std::string("\x01__noop__"), Value{}, /*tombstone=*/false);
        auto p = make_promise<Seq>(sched_);
        auto f = p.get_future();
        p.set_value(seq);
        return f;
    }

private:
    Scheduler* sched_;
};

// ---------------------------------------------------------------------------
// Tiny test harness helpers.
// ---------------------------------------------------------------------------

void expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        std::abort();
    }
}

bool witnesses_equal(const DiffWitness& a, const DiffWitness& b) {
    return a.seed == b.seed && a.step == b.step && a.kind == b.kind &&
           a.key == b.key && a.snap_at == b.snap_at && a.expected == b.expected &&
           a.got == b.got && a.note == b.note;
}

}  // namespace

int main() {
    DiffConfig cfg;
    cfg.steps = 300;
    cfg.n_keys = 5;
    cfg.n_values = 4;

    // -----------------------------------------------------------------
    // (a) ALWAYS-MATCH BASELINE — Oracle vs Oracle across a seed sweep.
    // -----------------------------------------------------------------
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        Scheduler sched;
        Oracle sut(sched);
        Oracle ref(sched);
        DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
        expect(v.ok, "baseline: oracle-vs-oracle must MATCH at every step");
        expect(v.witness.expected.empty() && v.witness.got.empty(),
               "baseline: a matching run carries no witness");
    }
    std::fprintf(stderr, "[ok] (a) oracle-vs-oracle always-match baseline (64 seeds)\n");

    // -----------------------------------------------------------------
    // (b) TEETH — deliberately-wrong engines MUST be flagged with a witness.
    // -----------------------------------------------------------------

    // (b1) off-by-one snapshot bound.
    {
        bool flagged_some = false;
        DiffWitness first;
        for (std::uint64_t seed = 1; seed <= 64; ++seed) {
            Scheduler sched;
            StaleBoundOracle sut(sched);
            Oracle ref(sched);
            DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
            if (!v.ok) {
                if (!flagged_some) {
                    first = v.witness;
                }
                flagged_some = true;
                expect(!v.witness.note.empty(), "stale-bound: witness has a note");
                expect(v.witness.seed == seed, "stale-bound: witness carries seed");
            }
        }
        expect(flagged_some,
               "TEETH: off-by-one snapshot-bound engine MUST be flagged");
        std::fprintf(stderr,
                     "[ok] (b1) stale-bound FLAGGED: seed=%llu step=%llu key=%s "
                     "snap=%llu expected=%s got=%s note=%s\n",
                     static_cast<unsigned long long>(first.seed),
                     static_cast<unsigned long long>(first.step), first.key.c_str(),
                     static_cast<unsigned long long>(first.snap_at),
                     first.expected.c_str(), first.got.c_str(), first.note.c_str());
    }

    // (b2) delete-ignored (a delete that doesn't delete).
    {
        bool flagged_some = false;
        DiffWitness first;
        for (std::uint64_t seed = 1; seed <= 64; ++seed) {
            Scheduler sched;
            DeleteIgnoredOracle sut(sched);
            Oracle ref(sched);
            DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
            if (!v.ok) {
                if (!flagged_some) {
                    first = v.witness;
                }
                flagged_some = true;
                expect(v.witness.seed == seed, "delete-ignored: witness carries seed");
            }
        }
        expect(flagged_some,
               "TEETH: delete-ignored engine MUST be flagged");
        std::fprintf(stderr,
                     "[ok] (b2) delete-ignored FLAGGED: seed=%llu step=%llu key=%s "
                     "snap=%llu expected=%s got=%s note=%s\n",
                     static_cast<unsigned long long>(first.seed),
                     static_cast<unsigned long long>(first.step), first.key.c_str(),
                     static_cast<unsigned long long>(first.snap_at),
                     first.expected.c_str(), first.got.c_str(), first.note.c_str());
    }

    // -----------------------------------------------------------------
    // (c) DETERMINISM — same seed ⇒ byte-identical witness (in-process).
    // -----------------------------------------------------------------
    {
        const std::uint64_t seed = 7;
        DiffWitness w1;
        DiffWitness w2;
        {
            Scheduler sched;
            StaleBoundOracle sut(sched);
            Oracle ref(sched);
            DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
            expect(!v.ok, "determinism: wrong engine flagged on seed 7");
            w1 = v.witness;
        }
        {
            Scheduler sched;
            StaleBoundOracle sut(sched);
            Oracle ref(sched);
            DiffVerdict v = run_diff(sched, sut, ref, seed, cfg);
            expect(!v.ok, "determinism: wrong engine flagged on seed 7 (replay)");
            w2 = v.witness;
        }
        expect(witnesses_equal(w1, w2),
               "DETERMINISM: same seed => byte-identical witness");
        std::fprintf(stderr, "[ok] (c) determinism: seed 7 witness byte-identical\n");
    }

    std::fprintf(stderr, "storage_diff_test: ALL PASS\n");
    return 0;
}
