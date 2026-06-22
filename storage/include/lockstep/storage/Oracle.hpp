#pragma once

// Oracle.hpp — Phase 3 §4. The TRIVIAL, obviously-correct reference engine: the
// MVCC correctness ground truth the differential harness judges every real
// engine against (storage-engine.md §4 ORACLE; §5 build step 1).
//
// WHAT THIS IS: an in-memory map  key → (ascending list of (Seq, Value|tombstone)).
// It assigns a monotonic Seq per commit and answers get(k, snap) as "the newest
// version of k with seq <= snap.at". NO disk, NO LSM, NO WAL, NO faults, NO
// background work — just the version semantics, kept so-simple-it-is-obviously
// -right. If the oracle were complicated it could not be trusted as the truth;
// every method is a few lines of straight-line logic over sorted vectors.
//
// WHY ASYNC IF THERE IS NO IO: the Engine seam is async (Future-returning) so a
// real disk-backed engine fits. The oracle has nothing to await, so it computes
// the answer immediately and fulfills the Future right away. We still mint the
// Future from a scheduler-bound Promise so it composes with co_await exactly like
// a real engine's result (completion SCHEDULES the waiter; it never resumes
// inline — Future.hpp L1). The harness therefore drives oracle and SUT through
// the identical co_await path.
//
// DETERMINISM (binding): the store is a SORTED std::vector per key, kept in key
// order; versions are appended in commit order so they are already Seq-ascending.
// No unordered_map, no hash iteration, no wall-clock, no randomness. Same op
// stream ⇒ identical Seq assignment ⇒ identical answers (V-DET).

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>

#include <lockstep/storage/Engine.hpp>

namespace lockstep::storage {

using core::make_promise;
using core::Promise;
using core::Scheduler;

// Oracle — the reference MVCC engine. Construct with the scheduler so it can mint
// scheduler-bound Promises for its (immediately-ready) Futures.
class Oracle : public Engine {
public:
    explicit Oracle(Scheduler& sched) noexcept : sched_(&sched) {}

    [[nodiscard]] Future<Seq> put(Key key, Value value) override {
        const Seq seq = commit(std::move(key), std::move(value), /*tombstone=*/false);
        Promise<Seq> p = make_promise<Seq>(sched_);
        Future<Seq> f = p.get_future();
        p.set_value(seq);
        return f;
    }

    [[nodiscard]] Future<Seq> del(Key key) override {
        const Seq seq = commit(std::move(key), Value{}, /*tombstone=*/true);
        Promise<Seq> p = make_promise<Seq>(sched_);
        Future<Seq> f = p.get_future();
        p.set_value(seq);
        return f;
    }

    [[nodiscard]] Future<std::optional<Value>> get(Key key, Snapshot snap) override {
        Promise<std::optional<Value>> p = make_promise<std::optional<Value>>(sched_);
        Future<std::optional<Value>> f = p.get_future();
        p.set_value(lookup(key, snap.at));
        return f;
    }

    [[nodiscard]] Future<Snapshot> snapshot() override {
        Promise<Snapshot> p = make_promise<Snapshot>(sched_);
        Future<Snapshot> f = p.get_future();
        p.set_value(Snapshot{last_seq_});
        return f;
    }

    [[nodiscard]] Future<Error> sync() override {
        // The oracle is purely in-memory: there is nothing to make durable, so the
        // barrier trivially succeeds. (Crash-consistency is a real-engine concern;
        // the oracle is the never-crashing ground truth.)
        Promise<Error> p = make_promise<Error>(sched_);
        Future<Error> f = p.get_future();
        p.set_value(Error{});
        return f;
    }

    [[nodiscard]] Future<std::vector<KeyValue>> scan(Range range, Snapshot snap) override {
        Promise<std::vector<KeyValue>> p = make_promise<std::vector<KeyValue>>(sched_);
        Future<std::vector<KeyValue>> f = p.get_future();
        p.set_value(scan_impl(range, snap.at));
        return f;
    }

protected:
    // One MVCC version of a key: the value, and whether it is a tombstone (delete).
    struct Version {
        Seq seq = kNoSeq;
        Value value;
        bool tombstone = false;
    };

    // All versions of one key, ascending by Seq (append order == commit order).
    struct KeyVersions {
        Key key;
        std::vector<Version> versions;
    };

    // Assign the next Seq and append a version to `key`. Pure, synchronous; the
    // ONLY mutation path. Returns the assigned commit Seq (V-MONO: ++ so strictly
    // increasing, never reused, never gapped). `commit` is the single seam a
    // deliberately-WRONG oracle subclass perturbs (see lookup() too).
    Seq commit(Key key, Value value, bool tombstone) {
        const Seq seq = ++last_seq_;  // 1,2,3,... — 0 stays the ∅ sentinel.
        KeyVersions& kv = versions_for(std::move(key));
        kv.versions.push_back(Version{seq, std::move(value), tombstone});
        return seq;
    }

    // The MVCC read: newest version of `key` with seq <= at; ∅ if none or that
    // newest version is a tombstone. This is the obviously-correct definition,
    // and the seam a wrong oracle subclass perturbs to prove the harness has
    // teeth. Linear scan of an ascending list — trivially right.
    [[nodiscard]] virtual std::optional<Value> lookup(const Key& key, Seq at) const {
        const KeyVersions* kv = find(key);
        if (kv == nullptr) {
            return std::nullopt;
        }
        // Walk ascending; remember the newest version whose seq <= at.
        const Version* newest = nullptr;
        for (const Version& v : kv->versions) {
            if (v.seq <= at) {
                newest = &v;  // ascending ⇒ later hits are strictly newer
            } else {
                break;  // versions beyond `at` are not visible to this snapshot
            }
        }
        if (newest == nullptr || newest->tombstone) {
            return std::nullopt;
        }
        return newest->value;
    }

    // The MVCC range read: for every key in [range.lo, range.hi) (key-ascending,
    // since keys_ is sorted), take the newest version with seq <= at and emit it
    // if it is a live value. Reuses lookup() so the per-key rule is identical to
    // get — the obviously-correct definition the harness checks scan against.
    [[nodiscard]] virtual std::vector<KeyValue> scan_impl(const Range& range, Seq at) const {
        std::vector<KeyValue> out;
        for (const KeyVersions& kv : keys_) {
            if (kv.key < range.lo) {
                continue;
            }
            if (!range.hi_unbounded && !(kv.key < range.hi)) {
                continue;  // key >= hi: past the half-open upper bound.
            }
            const std::optional<Value> v = lookup(kv.key, at);
            if (v.has_value()) {
                out.emplace_back(kv.key, *v);
            }
        }
        return out;
    }

    // Find the version list for `key`, or nullptr. Keys are kept sorted; a linear
    // scan is fine for the oracle (correctness over speed — it is the truth, not
    // the fast path).
    [[nodiscard]] const KeyVersions* find(const Key& key) const {
        for (const KeyVersions& kv : keys_) {
            if (kv.key == key) {
                return &kv;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Seq last_seq() const noexcept { return last_seq_; }

private:
    // Get (or create, in sorted position) the version list for `key`.
    KeyVersions& versions_for(Key key) {
        std::size_t pos = 0;
        while (pos < keys_.size() && keys_[pos].key < key) {
            ++pos;
        }
        if (pos < keys_.size() && keys_[pos].key == key) {
            return keys_[pos];
        }
        KeyVersions kv;
        kv.key = std::move(key);
        keys_.insert(keys_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(kv));
        return keys_[pos];
    }

    Scheduler* sched_;
    std::vector<KeyVersions> keys_;  // sorted by key; each list Seq-ascending
    Seq last_seq_ = kNoSeq;          // last assigned commit Seq (0 == none yet)
};

}  // namespace lockstep::storage
