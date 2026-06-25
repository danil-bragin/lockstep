#pragma once

// ProdParallelExecutor — the production impl of query::IParallelExecutor: runs a columnar
// aggregate's partition folds on real OS threads (allowed here; the forbidden-call lint confines
// std::thread to providers/). It is the multi-core lever behind SQL analytics. The SQL engine
// guarantees each parallel_for callback writes only its own slot and merges results in a fixed
// order, so the OUTPUT is deterministic even though execution is not.
//
// A persistent worker pool (one set of threads for the engine's lifetime) — dispatching a
// parallel_for wakes the idle workers rather than spawning threads per call, so per-query
// dispatch is a couple of condvar hops, not N thread creations.

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <lockstep/query/ParallelExecutor.hpp>

namespace lockstep::prod {

class ProdParallelExecutor final : public query::IParallelExecutor {
public:
    explicit ProdParallelExecutor(std::size_t workers)
        : workers_(workers < 1 ? 1 : workers) {
        // Pool holds workers_-1 threads; the calling thread runs partition 0 itself, so a
        // W-way split uses W cores with W-1 helper threads.
        for (std::size_t i = 1; i < workers_; ++i) {
            threads_.emplace_back(&ProdParallelExecutor::worker_loop, this, i);
        }
    }

    ProdParallelExecutor(const ProdParallelExecutor&) = delete;
    ProdParallelExecutor& operator=(const ProdParallelExecutor&) = delete;

    ~ProdParallelExecutor() override {
        {
            std::unique_lock<std::mutex> lk(mu_);
            stop_ = true;
            cv_.notify_all();
        }
        for (std::thread& th : threads_) {
            th.join();
        }
    }

    [[nodiscard]] std::size_t workers() const override { return workers_; }

    // Run fn(0..n-1), partition 0 on this thread and 1..min(n,workers_)-1 on the pool. Blocks
    // until all have finished (a barrier). Indices >= workers_ run inline on this thread after
    // the pooled ones, so n may exceed workers_ safely (the engine sizes n <= workers_).
    void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) override {
        if (n == 0) return;
        if (n == 1 || workers_ == 1) {
            for (std::size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        const std::size_t pooled = std::min(n, workers_) - 1;  // indices 1..pooled on the pool
        {
            std::unique_lock<std::mutex> lk(mu_);
            fn_ = &fn;
            job_n_ = n;
            pending_ = pooled;
            ++generation_;
            cv_.notify_all();
        }
        fn(0);                                   // partition 0 on the caller
        for (std::size_t i = workers_; i < n; ++i) fn(i);  // overflow (engine keeps n<=workers_)
        {
            std::unique_lock<std::mutex> lk(mu_);
            done_cv_.wait(lk, [&] { return pending_ == 0; });
            fn_ = nullptr;
        }
    }

private:
    void worker_loop(std::size_t index) {
        std::uint64_t last_seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || generation_ != last_seen; });
            if (stop_) return;
            last_seen = generation_;
            const std::function<void(std::size_t)>* fn = fn_;
            const bool mine = index < job_n_;
            lk.unlock();
            if (mine && fn) {
                (*fn)(index);
            }
            lk.lock();
            if (mine) {
                if (--pending_ == 0) done_cv_.notify_one();
            }
        }
    }

    const std::size_t workers_;
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_;       // wakes workers on a new job
    std::condition_variable done_cv_;  // wakes the caller when all partitions finished
    const std::function<void(std::size_t)>* fn_ = nullptr;
    std::size_t job_n_ = 0;
    std::size_t pending_ = 0;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace lockstep::prod
