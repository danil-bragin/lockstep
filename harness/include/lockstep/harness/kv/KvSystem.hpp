#pragma once

// KvSystem.hpp — Phase 2 batch 2 (stage B, spec §5). The toy replicated KV
// register system AND the abstract interface the run harness drives it through.
//
// WHAT THIS IS (spec §5, §9 DECISION-C): a replicated set of key→value
// REGISTERS. Ops: write(k,v) (exactly-once if ack'd), read(k), cas(k,old,new)
// (atomic compare-and-set). N logical nodes, each an actor on the deterministic
// Scheduler, talking over SimNetwork, persisting via SimDisk. The replication
// scheme is the SIMPLEST CORRECT-ish thing that survives the fault envelope —
// NOT consensus (that is Phase 4). Its job is to be a real, honestly-built
// system that the checkers + faults stress.
//
// REPLICATION SCHEME (single-leader primary-backup, crude failover):
//   * The leader is the live node with the LOWEST id (a fixed, deterministic
//     rule — no election protocol). Every mutating op is serialized THROUGH the
//     leader: it is the single linearization point.
//   * A client request reaches some node (the "router"); the router forwards it
//     to the believed leader. The leader applies the op to its in-memory map,
//     appends the resulting committed record to its durable WAL (SimDisk
//     append + sync), best-effort replicates the record to the backups, and
//     only THEN acks the client. So an ack'd write is leader-durable.
//   * Backups apply replicated records to their own map + WAL. On leader crash,
//     the next-lowest live node becomes leader and serves from its (replicated)
//     durable state. Replication is best-effort over the lossy bus, so a fresh
//     leader may be behind — this is where the envelope can expose real bugs;
//     that is the POINT (checkers judge it; we do not pretend it is perfect).
//
// THE INTERFACE (IKvSystem) is what the run harness calls. It is deliberately
// small and synchronous-looking from the CLIENT's view: submit an op, get back
// a Future that completes (ack or error) in bounded virtual time. Keeping the
// driver coded against IKvSystem (not the concrete class) is the PLUGGABILITY
// requirement: a later agent swaps in a known-buggy KV system behind the SAME
// run_kv_sim driver without rewriting the workload / lifecycle / fault code.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock, std::thread/atomics,
// std::*_distribution, unordered iteration affecting order, any nondeterminism.
// All time is core::Tick (virtual); all randomness is the injected core::IRandom.

#include <cstdint>
#include <string>
#include <utility>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/harness/History.hpp>

namespace lockstep::harness::kv {

using core::Error;
using core::Future;

// A client request handed to the system. Mirrors OpKind from History so the
// recorder and the system speak the same vocabulary. `cas_old` is meaningful
// only for Cas. The request is value-typed and copyable.
struct KvRequest {
    OpKind kind{OpKind::Read};
    std::string key;
    std::string value;    // Write value / Cas new-value
    std::string cas_old;  // Cas expected-old
};

// The outcome of a client request. `ok` distinguishes a completed op from an
// errored one (timeout / unavailable / cas-mismatch). For a Read, `result` is
// the observed value (empty ⇒ the ∅ / never-written register). For a Write/Cas
// it is an ack token. On !ok, `error` carries the reason. This is exactly the
// shape the HistoryRecorder needs for on_return.
struct KvResult {
    bool ok{};
    std::string result;
    std::string error;
};

// IKvSystem — the abstract replicated-register system the run harness drives.
// One instance owns all N nodes for a run. The driver:
//   * calls start()/tick lifecycle through the concrete setup (below) — but the
//     CLIENT surface is just submit().
//   * submit(client_id, req) returns a Future<KvResult> that completes in
//     BOUNDED virtual time (the system enforces a client-side deadline so the
//     harness always reaches quiescence — no livelock).
class IKvSystem {
public:
    virtual ~IKvSystem() = default;

    // Submit a client op. Returns a Future that completes (ok or error) within a
    // bounded number of virtual ticks. The implementation runs the request
    // through its replication scheme on the scheduler. `client_id` lets the
    // system attribute the op (e.g. for write dedup / read-your-writes).
    [[nodiscard]] virtual Future<KvResult> submit(std::uint64_t client_id,
                                                  const KvRequest& req) = 0;

    // Crash a node by id (node lifecycle C2.3): discard in-flight non-durable
    // state, consistent with SimDisk crash semantics (staged + lying bytes
    // lost; durable WAL survives). A crashed node serves nothing until recover.
    virtual void crash_node(std::uint64_t node_id) = 0;

    // Recover a previously-crashed node: reopen its durable WAL and rebuild its
    // in-memory map from the surviving durable records. The node rejoins as a
    // backup (and may become leader again if it is now the lowest live id).
    virtual void recover_node(std::uint64_t node_id) = 0;

    // Permanently kill a node (lifecycle C2.3 "kill"): like crash but it never
    // recovers in this run. Used to model a node that leaves the cluster.
    virtual void kill_node(std::uint64_t node_id) = 0;

    // Number of nodes the system was built with (for the lifecycle driver).
    [[nodiscard]] virtual std::uint64_t node_count() const = 0;
};

}  // namespace lockstep::harness::kv
