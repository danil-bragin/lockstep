#pragma once

// Checker.hpp — the checker contract (spec: specs/checker-framework.md §3;
// LOCKED API CONTRACT in briefs/phase2-batch2.md). A Checker is a written
// invariant + a verdict: it judges a run against EXACTLY one stated property
// (master-plan §6.3 "no stronger, no weaker"). Pluggable, decoupled from any
// concrete system-under-test.
//
// DESIGN INVARIANTS (binding):
//   V-CHK1: every Checker exposes spec_ref() citing the exact invariant it
//           asserts (e.g. "specs/checker-framework.md §4 C-LIN"). The interface
//           makes name()/spec_ref() pure-virtual so a checker cannot exist
//           without naming the property it checks. ⊥ vague checkers.
//   V-CHK2: a violation Verdict carries a WITNESS (minimal observed evidence) +
//           the SEED, so the failure is replayable. The Verdict struct holds
//           both; CheckerRunner::finalize() stamps the seed into every Verdict
//           it produces so a checker cannot forget it.
//
// Two judging surfaces (spec V-CHK3): on_event() for OPTIONAL online incremental
// checks during the run, and final() — REQUIRED — for the after-the-run history
// check. Online checkers can latch a violation observed mid-run and surface it
// from final().
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, any nondeterminism. A checker is a pure function of the
// History it is given.

#include <cstdint>
#include <string>
#include <utility>

#include <lockstep/harness/History.hpp>

namespace lockstep::harness {

// The result of a checker judging a run.
//   ok          : true ⇒ the asserted property held; false ⇒ violated.
//   witness     : on violation, the MINIMAL observed evidence (e.g. the offending
//                 op or pair of ops) rendered as text. Empty when ok.
//   explanation : human-readable why-this-violates, citing the property.
//   seed        : the run's seed, stamped so the violation is REPLAYABLE
//                 (V-CHK2). CheckerRunner::finalize() sets this for every Verdict.
struct Verdict {
    bool ok{};
    std::string witness;
    std::string explanation;
    std::uint64_t seed{};
};

// Convenience constructors keep checker bodies terse without hiding the shape.
[[nodiscard]] inline Verdict verdict_ok() {
    Verdict v;
    v.ok = true;
    return v;
}

[[nodiscard]] inline Verdict verdict_violation(std::string witness,
                                               std::string explanation) {
    Verdict v;
    v.ok = false;
    v.witness = std::move(witness);
    v.explanation = std::move(explanation);
    return v;
}

// Checker — the pluggable invariant interface (spec §3). Subclass and override
// final() (required) plus name()/spec_ref() (required, V-CHK1). Override
// on_event() to add an online incremental check (optional, V-CHK3).
class Checker {
public:
    virtual ~Checker() = default;

    // Optional online incremental check, called once per observed op IN
    // HISTORY ORDER during the run. Default: no-op. An online checker may stash
    // state here and report from final().
    virtual void on_event(const Op& ev) { (void)ev; }

    // Required offline check: judge the whole history. Pure function of the
    // History. Return verdict_ok() or verdict_violation(witness, explanation);
    // the runner stamps the seed.
    [[nodiscard]] virtual Verdict final(const History& history) = 0;

    // Short identifier for dashboards / reports. Required.
    [[nodiscard]] virtual std::string name() const = 0;

    // The exact written invariant this checker asserts (V-CHK1). Required.
    [[nodiscard]] virtual std::string spec_ref() const = 0;
};

// ---------------------------------------------------------------------------
// EXAMPLE checker — demonstrates the interface end-to-end. Decoupled from any
// system-under-test; it asserts a structural property of ANY well-formed
// history: every recorded op has invoke_vt <= return_vt (an op cannot return
// before it was invoked). This is the framework's own sanity invariant and a
// live demonstration of witness + spec_ref.
// ---------------------------------------------------------------------------
class InvokeBeforeReturnChecker final : public Checker {
public:
    [[nodiscard]] Verdict final(const History& history) override {
        for (const Op& op : history) {
            if (op.invoke_vt > op.return_vt) {
                std::string witness = "op_id=" + std::to_string(op.op_id) +
                                      " invoke_vt=" +
                                      std::to_string(static_cast<long long>(
                                          op.invoke_vt)) +
                                      " return_vt=" +
                                      std::to_string(static_cast<long long>(
                                          op.return_vt));
                return verdict_violation(
                    std::move(witness),
                    "operation returned before it was invoked "
                    "(invoke_vt > return_vt)");
            }
        }
        return verdict_ok();
    }

    [[nodiscard]] std::string name() const override {
        return "invoke_before_return";
    }

    [[nodiscard]] std::string spec_ref() const override {
        return "specs/checker-framework.md §2 V-HIST1 (invoke_vt <= return_vt)";
    }
};

}  // namespace lockstep::harness
