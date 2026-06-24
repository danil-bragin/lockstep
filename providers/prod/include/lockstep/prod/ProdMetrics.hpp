#pragma once

// ProdMetrics.hpp — Phase 10 OBSERVABILITY. A small PROD-LAYER metrics registry: typed
// COUNTERS (monotonic event tallies) + GAUGES (current-state snapshots) for ONE node /
// shard, plus a Prometheus text-exposition encoder. This is PROD-LAYER instrumentation
// only — core/sim/consensus/txn are UNCHANGED. The gauges are READS of the existing
// consensus observables (role()/current_term()/commit_index()/log()), snapshotted on
// scrape; the counters are bumped at the REAL event sites in ProdConsensusNode /
// ProdShardRunner / lockstepd.
//
// ----------------------------------------------------------------------------
// THREADING MODEL — SINGLE-WRITER, LOCK-FREE READ.
// Each shard's ProdConsensusNode owns ONE ProdMetrics, mutated ONLY on that shard's
// ONE reactor thread (the writer). A scrape (the METRICS admin verb) is handled on the
// SAME reactor thread, so the common path is single-threaded with no contention at all.
// To be airtight against ANY future cross-thread read (e.g. a side-channel scrape on a
// different thread, or a TSan run under load), every counter/gauge is a std::atomic read
// and written with std::memory_order_relaxed: increments are single-writer (no lost
// updates possible — only this thread writes), and a relaxed read on another thread sees
// a value that is at worst slightly stale but never torn. Relaxed is sufficient because
// metrics carry NO happens-before obligation to any other data — a counter is a free-
// standing tally. This keeps the registry TSan-clean (no data race) while costing nothing
// on the single-writer hot path. NO LOCKS.
//
// COST: a scrape is O(#metrics) (a fixed, small set), NOT O(log) — it reads the snapshot
// counters + the cheap consensus observables (role/term/commit_index are member reads;
// log_size is span.size()). The full-log STATUS path is untouched; METRICS never walks
// the durable log, honoring the "scrape is cheap" lesson.
//
// LINUX-ONLY usage (the prod daemon is __linux__), but this header itself is pure C++ with
// no syscalls, so it compiles everywhere. providers/prod is the lint-exempt boundary.

#include <atomic>
#include <cstdint>
#include <string>

namespace lockstep::prod {

// A monotonic counter (single-writer relaxed-atomic). inc() is the only mutator on the
// hot path; get() is a lock-free read (single-writer => never torn, at worst stale).
class Counter {
public:
    void inc(std::uint64_t by = 1) noexcept {
        // Single writer: load+store under relaxed is safe (no other thread writes).
        v_.store(v_.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get() const noexcept {
        return v_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> v_{0};
};

// A gauge (current value; may go up or down). Set on scrape from a consensus observable
// or at an event site. Same single-writer/lock-free-read discipline as Counter.
class Gauge {
public:
    void set(std::uint64_t value) noexcept {
        v_.store(value, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get() const noexcept {
        return v_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> v_{0};
};

// ProdMetrics — the per-node/per-shard registry. Labels (shard, node) are fixed at
// construction; the encoder stamps them on every line. COUNTERS are monotonic event
// tallies bumped at the real event sites; GAUGES are refreshed on scrape from the
// consensus observables (refresh_gauges()).
struct ProdMetrics {
    // ---- labels (identity of this metrics stream) ----
    std::uint64_t shard = 0;  // shard index (0 for the single-shard daemon)
    std::uint64_t node = 0;   // consensus node id (Raft id)

    // ---- COUNTERS (monotonic) — bumped at the real event sites ----
    Counter submits_accepted;        // admin SUBMIT accepted by this node (it was leader)
    Counter submits_committed;       // accepted entries this node observed reach commit
    Counter client_requests;         // admin requests handled (any verb)
    Counter elections_started;       // times this node became a Candidate (term bump)
    Counter leader_changes;          // times this node became Leader
    Counter steps_down;              // times this node left Leader/Candidate -> Follower

    // ---- GAUGES (current state; snapshotted on scrape from consensus observables) ----
    Gauge role;          // 0=Follower 1=Candidate 2=Leader
    Gauge current_term;  // consensus current_term()
    Gauge commit_index;  // consensus commit_index()
    Gauge log_size;      // consensus log().size() (durable entries)
    Gauge in_flight;     // accepted-but-not-yet-committed entries on this node

    // ---- disk counters (passthrough of the existing ProdDisk introspection) ----
    // Set on scrape from ProdDisk's counters (fdatasync/append/bytes); these are NOT new
    // observables — the daemon already reports them on shutdown. Modeled as gauges since
    // we copy the disk's running total on each scrape (the disk owns the monotonicity).
    Gauge fdatasync_calls;
    Gauge bytes_appended;
    Gauge append_calls;

    ProdMetrics() = default;
    ProdMetrics(std::uint64_t shard_idx, std::uint64_t node_id)
        : shard(shard_idx), node(node_id) {}
};

namespace metrics_detail {

// Append one Prometheus exposition stanza for a counter/gauge: a HELP/TYPE header plus
// one labeled sample line. name is the metric name; type is "counter" or "gauge".
inline void emit(std::string& out, const char* name, const char* type, const char* help,
                 std::uint64_t shard, std::uint64_t node, std::uint64_t value) {
    out += "# HELP lockstep_";
    out += name;
    out += ' ';
    out += help;
    out += '\n';
    out += "# TYPE lockstep_";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
    out += "lockstep_";
    out += name;
    out += "{shard=\"";
    out += std::to_string(shard);
    out += "\",node=\"";
    out += std::to_string(node);
    out += "\"} ";
    out += std::to_string(value);
    out += '\n';
}

}  // namespace metrics_detail

// Encode ONE ProdMetrics as a Prometheus text-exposition block. O(#metrics) — a fixed
// small set; never O(log). The caller (the METRICS admin handler / a scrape endpoint)
// should refresh_gauges() first so the gauge snapshots are current.
[[nodiscard]] inline std::string encode_prometheus(const ProdMetrics& m) {
    std::string out;
    out.reserve(2048);
    const std::uint64_t s = m.shard;
    const std::uint64_t n = m.node;
    using metrics_detail::emit;
    // Counters.
    emit(out, "submits_accepted_total", "counter",
         "admin SUBMITs accepted by this node (it was leader)", s, n,
         m.submits_accepted.get());
    emit(out, "submits_committed_total", "counter",
         "accepted entries this node observed reach commit_index", s, n,
         m.submits_committed.get());
    emit(out, "client_requests_total", "counter", "admin requests handled (any verb)", s,
         n, m.client_requests.get());
    emit(out, "elections_started_total", "counter",
         "times this node became a Candidate", s, n, m.elections_started.get());
    emit(out, "leader_changes_total", "counter", "times this node became Leader", s, n,
         m.leader_changes.get());
    emit(out, "steps_down_total", "counter",
         "times this node stepped down to Follower", s, n, m.steps_down.get());
    // Gauges.
    emit(out, "role", "gauge", "current role (0=Follower 1=Candidate 2=Leader)", s, n,
         m.role.get());
    emit(out, "current_term", "gauge", "current Raft term", s, n, m.current_term.get());
    emit(out, "commit_index", "gauge", "committed log index (1-based, 0=none)", s, n,
         m.commit_index.get());
    emit(out, "log_size", "gauge", "durable log entry count", s, n, m.log_size.get());
    emit(out, "in_flight", "gauge", "accepted-but-not-yet-committed entries", s, n,
         m.in_flight.get());
    // Disk (passthrough of existing ProdDisk introspection).
    emit(out, "fdatasync_calls_total", "counter", "fdatasync calls on the consensus WAL",
         s, n, m.fdatasync_calls.get());
    emit(out, "bytes_appended_total", "counter", "bytes appended to the consensus WAL", s,
         n, m.bytes_appended.get());
    emit(out, "append_calls_total", "counter", "append() calls on the consensus WAL", s, n,
         m.append_calls.get());
    return out;
}

}  // namespace lockstep::prod
