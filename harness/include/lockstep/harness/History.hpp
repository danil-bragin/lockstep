#pragma once

// History.hpp — observable history of client operations against a
// system-under-test (spec: specs/checker-framework.md §2; LOCKED API CONTRACT
// in briefs/phase2-batch2.md). This is the offline-checking substrate: a
// totally-ordered (by virtual time, tie-broken by a deterministic seq) log of
// OBSERVABLE client operations that the checker set (Phase 2) and later
// linearizability / Elle reuse (Phases 4/5) judge.
//
// DESIGN INVARIANTS (binding):
//   V-HIST1: every client op records BOTH invoke_vt and return_vt. The
//            recorder enforces this structurally: an op enters the history at
//            on_invoke() with invoke_vt set, and is only completed by
//            on_return() which sets return_vt. A render that exposes an op
//            whose return_vt was never set is a malformed history.
//   V-HIST2: history is a PURE FUNCTION of (seed/inputs). No wall-clock, no
//            ambient randomness, no unordered iteration affecting output: op_id
//            is a monotonic counter, ordering is by (return_vt, seq), and seq
//            is the deterministic invoke order. Same inputs ⇒ byte-identical
//            render.
//   V-HIST3: the recorder is PASSIVE. It only appends/patches plain records and
//            never schedules work, advances the clock, or otherwise touches the
//            system-under-test. on_invoke/on_return take the caller's `now`
//            virtual time; they do not read a clock themselves. Recording can
//            be removed without changing the run.
//
// FORBIDDEN here (harness/ is NOT lint-exempt): wall-clock (std::chrono),
// std::thread/atomics, std::*_distribution, unordered iteration affecting
// order, any nondeterminism. All time is core::Tick (virtual).

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/IClock.hpp>  // core::Tick (virtual time)

namespace lockstep::harness {

// The KV-register operation kinds the toy gate system (spec §4/§5) exercises.
// Kept deliberately small and decoupled from any concrete system-under-test.
enum class OpKind : std::uint8_t {
    Read,   // read(key)            -> result is the observed value (∅ ⇒ empty)
    Write,  // write(key, value)    -> result is an ack token (or empty)
    Cas,    // cas(key, cas_old, value) atomic compare-and-set
};

// A single observed client operation. Carries BOTH the invoke and return
// virtual timestamps (V-HIST1) so real-time order is available for the
// linearizability checker later. Fields mirror the LOCKED API CONTRACT exactly;
// later stages (toy system, checkers) code against this shape.
struct Op {
    std::uint64_t client_id{};  // which client session issued the op
    std::uint64_t op_id{};      // unique, monotonic; the recorder's handle
    OpKind kind{OpKind::Read};
    std::string key;
    std::string value;    // Write value / Cas new-value (empty ⇒ ∅)
    std::string cas_old;  // Cas expected-old (Cas only)

    core::Tick invoke_vt{};  // virtual time at invoke (set by on_invoke)
    core::Tick return_vt{};  // virtual time at return (set by on_return)

    bool ok{};           // true ⇒ completed; false ⇒ errored
    std::string result;  // Read result / ack token (valid when ok)
    std::string error;   // error detail (valid when !ok)

    // Deterministic tie-break for equal return_vt (the invoke sequence number).
    // NOT part of the LOCKED CONTRACT's render fields but needed to make the
    // total order total; render() below stamps it into the output.
    std::uint64_t seq{};

    // True once on_return() has stamped return_vt. A pending op (invoked but not
    // yet returned) is excluded from the checkable, totally-ordered history.
    bool returned{};
};

// The observable history: total order by (return_vt, seq). `seq` is the
// deterministic invoke order, so ties in return_vt resolve identically on
// every replay (V-HIST2). Per the LOCKED CONTRACT this is a plain vector<Op>.
using History = std::vector<Op>;

// HistoryRecorder — passive observer that builds a History (spec §2).
//
// Lifecycle per op: on_invoke() mints an op_id and appends a PENDING record
// stamped with invoke_vt; on_return() finds that op_id and patches in the
// outcome + return_vt. history() returns the COMPLETED ops in (return_vt, seq)
// order. Pending (un-returned) ops are not surfaced to checkers — a checker
// judges only fully observed operations.
//
// The recorder holds no clock and no scheduler reference (V-HIST3): callers
// pass `now` from the system-under-test's virtual clock. It performs no
// nondeterministic work; given the same call sequence + timestamps it produces
// a byte-identical history (V-HIST2).
class HistoryRecorder {
public:
    // Record an invocation. Returns the op_id the caller must pass to
    // on_return(). invoke_vt is stamped to `now`; seq is the monotonic invoke
    // order. Does NOT touch the clock or scheduler (passive).
    [[nodiscard]] std::uint64_t on_invoke(std::uint64_t client_id, OpKind kind,
                                          std::string key, std::string value,
                                          std::string cas_old, core::Tick now) {
        const std::uint64_t id = next_op_id_++;
        Op op;
        op.client_id = client_id;
        op.op_id = id;
        op.kind = kind;
        op.key = std::move(key);
        op.value = std::move(value);
        op.cas_old = std::move(cas_old);
        op.invoke_vt = now;
        op.seq = next_seq_++;
        op.returned = false;
        ops_.push_back(std::move(op));
        return id;
    }

    // Record the return of a previously-invoked op. Stamps return_vt to `now`
    // and the outcome (ok/result/error). No-op if op_id is unknown (defensive;
    // a well-behaved caller always pairs invoke+return). Does NOT touch the
    // clock or scheduler (passive).
    void on_return(std::uint64_t op_id, bool ok, std::string result,
                   std::string error, core::Tick now) {
        for (Op& op : ops_) {
            if (op.op_id == op_id) {
                op.return_vt = now;
                op.ok = ok;
                op.result = std::move(result);
                op.error = std::move(error);
                op.returned = true;
                return;
            }
        }
    }

    // The completed, totally-ordered history (by return_vt, then seq). Computed
    // lazily and cached; recomputed only after a mutation. Pending ops are
    // excluded. Stable + deterministic (V-HIST2). The reference is valid until
    // the next on_invoke/on_return.
    [[nodiscard]] const History& history() const {
        rebuild_view();
        return view_;
    }

    // Whether any op was invoked but never returned. A checker harness may
    // assert this is false for a clean run (every invoke paired with a return).
    [[nodiscard]] bool has_pending() const {
        for (const Op& op : ops_) {
            if (!op.returned) {
                return true;
            }
        }
        return false;
    }

private:
    // Build the (return_vt, seq)-ordered view of completed ops. Pure function of
    // the recorded calls: a stable sort over a deterministic key.
    void rebuild_view() const {
        view_.clear();
        for (const Op& op : ops_) {
            if (op.returned) {
                view_.push_back(op);
            }
        }
        // Total order: primary return_vt, deterministic tie-break seq. seq is
        // unique per op, so the order is total and replay-stable. std::sort is
        // fine (no equal keys after the seq tie-break) but we keep it explicit.
        std::sort(view_.begin(), view_.end(), [](const Op& a, const Op& b) {
            if (a.return_vt != b.return_vt) {
                return a.return_vt < b.return_vt;
            }
            return a.seq < b.seq;
        });
    }

    std::vector<Op> ops_;       // insertion-ordered backing store (by invoke)
    mutable History view_;      // cached (return_vt, seq)-ordered completed ops
    std::uint64_t next_op_id_{1};
    std::uint64_t next_seq_{0};
};

// Render a History to a stable, line-oriented text form. This is the
// byte-reproducibility surface used by determinism self-tests (V-HIST2): same
// inputs ⇒ byte-identical string. Pure function of the History; no clock, no
// randomness, no unordered iteration.
[[nodiscard]] inline std::string render_history(const History& h) {
    std::string out;
    out.reserve(h.size() * 64);
    for (const Op& op : h) {
        out += "op_id=";
        out += std::to_string(op.op_id);
        out += " client=";
        out += std::to_string(op.client_id);
        out += " kind=";
        switch (op.kind) {
            case OpKind::Read:
                out += "read";
                break;
            case OpKind::Write:
                out += "write";
                break;
            case OpKind::Cas:
                out += "cas";
                break;
        }
        out += " key=";
        out += op.key;
        out += " value=";
        out += op.value;
        out += " cas_old=";
        out += op.cas_old;
        out += " invoke_vt=";
        out += std::to_string(static_cast<long long>(op.invoke_vt));
        out += " return_vt=";
        out += std::to_string(static_cast<long long>(op.return_vt));
        out += " ok=";
        out += op.ok ? "1" : "0";
        out += " result=";
        out += op.result;
        out += " error=";
        out += op.error;
        out += '\n';
    }
    return out;
}

}  // namespace lockstep::harness
