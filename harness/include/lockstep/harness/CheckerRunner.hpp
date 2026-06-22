#pragma once

// CheckerRunner.hpp — drives a SET of checkers over one run (spec:
// specs/checker-framework.md §3; LOCKED API CONTRACT in
// briefs/phase2-batch2.md). It fans observed ops to each checker's on_event()
// during the run (V-CHK3 online surface) and runs every checker's final()
// after, collecting the verdicts and stamping the run seed into each
// (V-CHK2 → replayable).
//
// DESIGN INVARIANTS (binding):
//   V-CHK1: each added Checker already cites its spec_ref(); the runner does not
//           weaken that — it preserves checker order (insertion order) so the
//           verdict list is deterministic and each entry traces to its checker.
//   V-CHK2: finalize() stamps the run `seed` into EVERY Verdict (ok and
//           violation alike), so a surfaced violation is replayable even if the
//           checker body forgot to set it.
//   Determinism: checkers run in insertion order; on_event fans in history
//           order. Same seed/inputs ⇒ byte-identical verdict list.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting order, any nondeterminism.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/History.hpp>

namespace lockstep::harness {

// CheckerRunner — owns a set of checkers and aggregates their verdicts.
// Single-threaded, deterministic. Not copyable (it owns unique_ptrs).
class CheckerRunner {
public:
    CheckerRunner() = default;
    CheckerRunner(const CheckerRunner&) = delete;
    CheckerRunner& operator=(const CheckerRunner&) = delete;
    CheckerRunner(CheckerRunner&&) = default;
    CheckerRunner& operator=(CheckerRunner&&) = default;

    // Register a checker. Insertion order is preserved → deterministic verdict
    // order. The runner takes ownership.
    void add(std::unique_ptr<Checker> checker) {
        checkers_.push_back(std::move(checker));
    }

    // Fan one observed op to every checker's online on_event() in insertion
    // order. Call once per op as the run produces it (V-CHK3 online surface).
    void observe(const Op& ev) {
        for (const std::unique_ptr<Checker>& c : checkers_) {
            c->on_event(ev);
        }
    }

    // Run every checker's final() over the history in insertion order, stamp
    // the run `seed` into each Verdict (V-CHK2 → replayable), and return the
    // FULL verdict list (one per checker, in checker order). Callers inspect
    // .ok to find violations; the seed + witness on a !ok verdict make it
    // reproducible. Deterministic: same (checkers, history, seed) ⇒
    // byte-identical verdicts.
    [[nodiscard]] std::vector<Verdict> finalize(const History& history,
                                                std::uint64_t seed) {
        std::vector<Verdict> verdicts;
        verdicts.reserve(checkers_.size());
        for (const std::unique_ptr<Checker>& c : checkers_) {
            Verdict v = c->final(history);
            v.seed = seed;  // V-CHK2: every verdict is replayable.
            verdicts.push_back(std::move(v));
        }
        return verdicts;
    }

    // Number of registered checkers (for reporting / self-tests).
    [[nodiscard]] std::size_t size() const { return checkers_.size(); }

private:
    std::vector<std::unique_ptr<Checker>> checkers_;
};

}  // namespace lockstep::harness
