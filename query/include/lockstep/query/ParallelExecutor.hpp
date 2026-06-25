#pragma once

// ParallelExecutor.hpp — the MORSEL-PARALLELISM seam for the SQL engine. A pure interface (no
// threads here — the forbidden-call lint confines std::thread to providers/). The SQL engine
// splits a columnar aggregate's chunks into N independent partitions and calls parallel_for(N,
// fn); fn(i) folds partition i into its own partial slot (no shared state), and the engine merges
// the partials in a FIXED order so the result is BYTE-IDENTICAL regardless of thread scheduling.
//
// The DEFAULT is SerialExecutor (runs fn sequentially) — the engine's behaviour when nothing is
// injected, so single-process callers + the deterministic sim are unaffected. A providers/prod
// impl (ProdParallelExecutor) runs the partitions on a real thread pool for multi-core SQL.

#include <cstddef>
#include <functional>

namespace lockstep::query {

class IParallelExecutor {
public:
    virtual ~IParallelExecutor() = default;
    // Run fn(0..n-1), possibly concurrently. fn(i) MUST be independent (writes only its own slot).
    // Returns when ALL have completed (a barrier).
    virtual void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) = 0;
    // Suggested partition count (>= 1). The engine uses this to size the split.
    [[nodiscard]] virtual std::size_t workers() const = 0;
};

// Serial default — no threads, deterministic. The engine uses this when no executor is injected.
class SerialExecutor final : public IParallelExecutor {
public:
    void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) override {
        for (std::size_t i = 0; i < n; ++i) {
            fn(i);
        }
    }
    [[nodiscard]] std::size_t workers() const override { return 1; }
};

}  // namespace lockstep::query
