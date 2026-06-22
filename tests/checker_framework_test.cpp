// checker_framework_test.cpp — Phase 2 batch 2 (stage A) self-test for the
// history recorder + checker framework FOUNDATION (specs/checker-framework.md
// §2/§3; briefs/phase2-batch2.md LOCKED API CONTRACT, BRIEF B2A).
//
// WHAT IT PROVES (one assertion block per invariant):
//   V-HIST1: every op records BOTH invoke_vt and return_vt — we record a small
//            synthetic read/write/cas history via HistoryRecorder and assert
//            every surfaced op has both stamps and the (return_vt, seq) order.
//   V-HIST2: history = pure function of (seed/inputs) → byte-identical on
//            replay. We build the SAME synthetic history twice and assert the
//            rendered text is byte-identical, AND assert the verdict list is
//            byte-identical across the two runs.
//   V-HIST3: recorder is PASSIVE — it holds no clock/scheduler; callers pass
//            virtual time. Demonstrated structurally (no clock is touched here).
//   V-CHK1: every checker cites spec_ref() — asserted non-empty for each.
//   V-CHK2: a violation Verdict carries a WITNESS + the seed → replayable. We
//            run a checker against a deliberately-malformed op and assert the
//            verdict is !ok, has a non-empty witness, and carries the run seed.
//
// This file is non-provider code → the forbidden-call lint scans it. No
// <chrono>/<thread>/<random>: all time is core::Tick (virtual), no ambient
// randomness, deterministic throughout.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/CheckerRunner.hpp>
#include <lockstep/harness/History.hpp>

namespace {

using lockstep::core::Tick;
using lockstep::harness::Checker;
using lockstep::harness::CheckerRunner;
using lockstep::harness::History;
using lockstep::harness::HistoryRecorder;
using lockstep::harness::InvokeBeforeReturnChecker;
using lockstep::harness::Op;
using lockstep::harness::OpKind;
using lockstep::harness::render_history;
using lockstep::harness::Verdict;
using lockstep::harness::verdict_ok;
using lockstep::harness::verdict_violation;

// A second EXAMPLE checker: "every surfaced op carries a minted op_id" — a
// structural well-formedness property. The recorder mints op_ids from 1, so a
// zero op_id in a surfaced history means a fabricated/uninitialized record.
// NOTE: op_ids are monotonic in INVOKE order, NOT in the (return_vt, seq)
// history order — an op that returns later may have a smaller op_id — so this
// checker asserts EXACTLY nonzero-ness (V-CHK4 spirit: no stronger, no weaker),
// not monotonicity. It demonstrates a checker that asserts a different level
// than InvokeBeforeReturnChecker.
class WellFormedOpIdChecker final : public Checker {
public:
    [[nodiscard]] Verdict final(const History& history) override {
        for (const Op& op : history) {
            if (op.op_id == 0) {
                return verdict_violation("op_id=0",
                                         "op_id must be nonzero (recorder mints "
                                         "ids from 1)");
            }
        }
        return verdict_ok();
    }
    [[nodiscard]] std::string name() const override {
        return "wellformed_op_id";
    }
    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §2 (op_id minted nonzero by "
               "recorder)";
    }
};

// Build a small synthetic KV-register history through the recorder. PURE
// function of its inputs — given the same call sequence it produces a
// byte-identical history (V-HIST2). Returns the recorder by value (moved) so
// the caller can render + check it.
//
// Note: op_ids are monotonic in the (return_vt, seq) view only because the
// synthetic ops below complete in invoke order; the checkers do not rely on
// that beyond what they assert.
HistoryRecorder build_synthetic_history() {
    HistoryRecorder rec;

    // client 1 writes k=a, then reads it back.
    const std::uint64_t w1 =
        rec.on_invoke(1, OpKind::Write, "k", "a", "", Tick{10});
    rec.on_return(w1, true, "ack", "", Tick{12});

    const std::uint64_t r1 = rec.on_invoke(1, OpKind::Read, "k", "", "", Tick{13});
    rec.on_return(r1, true, "a", "", Tick{15});

    // client 2 cas k a->b, overlapping in invoke order.
    const std::uint64_t c1 =
        rec.on_invoke(2, OpKind::Cas, "k", "b", "a", Tick{14});
    rec.on_return(c1, true, "ack", "", Tick{18});

    // client 2 errored read of a missing key.
    const std::uint64_t r2 =
        rec.on_invoke(2, OpKind::Read, "missing", "", "", Tick{16});
    rec.on_return(r2, false, "", "not_found", Tick{17});

    return rec;
}

// Render a verdict list to stable text (byte-reproducibility surface for the
// determinism assertion). Pure function of the verdicts.
std::string render_verdicts(const std::vector<Verdict>& verdicts) {
    std::string out;
    for (const Verdict& v : verdicts) {
        out += "ok=";
        out += v.ok ? "1" : "0";
        out += " seed=";
        out += std::to_string(v.seed);
        out += " witness=";
        out += v.witness;
        out += " explanation=";
        out += v.explanation;
        out += '\n';
    }
    return out;
}

// Run the standard checker set over a recorder's history at a given seed.
// Returns (rendered history, rendered verdicts) so callers can diff replays.
std::pair<std::string, std::string> run_checks(const HistoryRecorder& rec,
                                               std::uint64_t seed) {
    const History& h = rec.history();

    CheckerRunner runner;
    runner.add(std::make_unique<InvokeBeforeReturnChecker>());
    runner.add(std::make_unique<WellFormedOpIdChecker>());

    // Online surface: fan each op through on_event in history order.
    for (const Op& op : h) {
        runner.observe(op);
    }

    std::vector<Verdict> verdicts = runner.finalize(h, seed);
    return {render_history(h), render_verdicts(verdicts)};
}

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        std::abort();
    }
}

}  // namespace

int main() {
    const std::uint64_t kSeed = 0xC0FFEEULL;

    // --- V-HIST1: both invoke_vt and return_vt recorded; total order is by
    //     (return_vt, seq). ---------------------------------------------------
    {
        HistoryRecorder rec = build_synthetic_history();
        check(!rec.has_pending(), "no pending ops (every invoke returned)");
        const History& h = rec.history();
        check(h.size() == 4, "four completed ops surfaced");
        Tick prev_return = h.front().return_vt;
        for (std::size_t i = 0; i < h.size(); ++i) {
            const Op& op = h[i];
            // V-HIST1: both stamps present and ordered.
            check(op.invoke_vt <= op.return_vt,
                  "invoke_vt <= return_vt for every op (V-HIST1)");
            check(op.op_id != 0, "op_id minted (nonzero)");
            if (i > 0) {
                check(op.return_vt >= prev_return,
                      "history totally ordered by return_vt (V-HIST1 order)");
            }
            prev_return = op.return_vt;
        }
        // The errored read (return_vt=17) sorts before the cas (return_vt=18).
        check(h.back().kind == OpKind::Cas, "cas is last by return_vt");
    }

    // --- V-HIST2: pure function of inputs → byte-identical history AND
    //     verdicts on replay. -------------------------------------------------
    std::string hist_a;
    std::string verd_a;
    {
        HistoryRecorder rec = build_synthetic_history();
        auto [hist, verd] = run_checks(rec, kSeed);
        hist_a = hist;
        verd_a = verd;
    }
    {
        HistoryRecorder rec = build_synthetic_history();
        auto [hist, verd] = run_checks(rec, kSeed);
        check(hist == hist_a, "history render byte-identical on replay (V-HIST2)");
        check(verd == verd_a, "verdicts byte-identical on replay (V-HIST2)");
    }

    // --- Clean run: both example checkers pass; each cites spec_ref (V-CHK1). -
    {
        HistoryRecorder rec = build_synthetic_history();
        const History& h = rec.history();
        CheckerRunner runner;
        runner.add(std::make_unique<InvokeBeforeReturnChecker>());
        runner.add(std::make_unique<WellFormedOpIdChecker>());
        std::vector<Verdict> v = runner.finalize(h, kSeed);
        check(v.size() == 2, "one verdict per checker");
        for (const Verdict& vd : v) {
            check(vd.ok, "clean synthetic history passes both checkers");
            check(vd.seed == kSeed, "verdict carries the run seed (V-CHK2)");
        }
        // V-CHK1: spec_ref non-empty for each checker.
        InvokeBeforeReturnChecker c1;
        WellFormedOpIdChecker c2;
        check(!c1.spec_ref().empty() && !c1.name().empty(),
              "InvokeBeforeReturnChecker cites spec_ref + name (V-CHK1)");
        check(!c2.spec_ref().empty() && !c2.name().empty(),
              "WellFormedOpIdChecker cites spec_ref + name (V-CHK1)");
    }

    // --- V-CHK2: a violation carries a witness + the seed → replayable. We
    //     hand-build a MALFORMED op (return before invoke) directly (the
    //     recorder cannot produce this; we test the checker's teeth). ---------
    {
        History bad;
        Op op;
        op.client_id = 7;
        op.op_id = 42;
        op.kind = OpKind::Read;
        op.key = "k";
        op.invoke_vt = Tick{100};  // invoked AFTER it returned: malformed.
        op.return_vt = Tick{90};
        op.ok = true;
        op.result = "x";
        op.seq = 0;
        op.returned = true;
        bad.push_back(op);

        CheckerRunner runner;
        runner.add(std::make_unique<InvokeBeforeReturnChecker>());
        const std::uint64_t bad_seed = 0xBAD5EEDULL;
        std::vector<Verdict> v = runner.finalize(bad, bad_seed);
        check(v.size() == 1, "one verdict for the violating checker");
        check(!v[0].ok, "checker FLAGS the malformed op (has teeth)");
        check(!v[0].witness.empty(), "violation carries a witness (V-CHK2)");
        check(!v[0].explanation.empty(), "violation carries an explanation");
        check(v[0].seed == bad_seed,
              "violation carries the seed → replayable (V-CHK2)");
        // The witness must name the offending op so the failure is locatable.
        check(v[0].witness.find("op_id=42") != std::string::npos,
              "witness names the offending op_id (minimal evidence)");
    }

    std::printf("checker_framework_test: OK\n");
    return 0;
}
