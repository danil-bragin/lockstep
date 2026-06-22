#pragma once

// DiffHarness.hpp — Phase 3 §4. THE differential gate (master-plan §6.4): drive
// one deterministic, seeded op stream against BOTH a system-under-test Engine and
// the trivial Oracle, and assert every observable result MATCHES the oracle under
// the version mapping. A mismatch is reported as a replayable WITNESS (the op,
// key, snapshot, expected-vs-got) plus the seed.
//
// HOW IT IS THE GATE (storage-engine.md §4 / §5 step 1):
//   * The harness is a PURE FUNCTION of (seed). One SeededRandom drives the whole
//     op stream; given the same seed it emits a byte-identical op trace and the
//     same verdict. No wall-clock, no std::*_distribution, no unordered iteration.
//   * It speaks only the Engine interface, so the SUT and the oracle are swapped
//     behind the SAME driver. Step 1 drives oracle-vs-oracle (always-match
//     baseline, proves the comparator compares) and a deliberately-WRONG engine
//     (proves the harness has TEETH — a harness that passes a known-wrong engine
//     is itself the bug).
//
// THE VERSION MAPPING (the crux): put/del return a commit Seq. The oracle and a
// correct SUT assign Seqs by the SAME rule (monotonic, one per commit, in op
// order), so for an identical op stream the Nth commit gets the same Seq in both
// (V-MONO). The harness asserts that EQUALITY directly (commit-seq agreement),
// then issues reads at snapshots EXPRESSED in that shared Seq space and asserts
// the values agree. Because both engines map ops→Seqs identically, a snapshot Seq
// means the same MVCC version to both — that is the mapping under which get
// results must match.
//
// WHAT IS COMPARED each step:
//   put/del : the returned commit Seq (V-MONO agreement).
//   get     : the optional<Value> at a chosen snapshot (V-SNAP agreement).
//   snapshot: the current committed Seq (latest-version agreement).
//   sync    : the Error ok-ness (oracle always ok; a real engine asserts durability
//             — crash/recover hooks plug in at step 2).
//
// AWAIT SAFETY (V-RKV1, carried from Phase 2): the driver holds NO pointer into
// any engine-internal container across a co_await. Everything awaited resolves to
// a value type (Seq / optional<Value> / Snapshot / Error) copied out before the
// next step. The Witness stores owned copies, never references.
//
// DETERMINISM: the op kind, key, value, and snapshot choice are ALL drawn from
// the single injected SeededRandom; the key/value alphabets are tiny + fixed so
// collisions (overwrite, delete-then-read) happen often. Same seed ⇒ identical
// run, identical witness.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/sim/SeededRandom.hpp>

#include <lockstep/storage/Engine.hpp>

namespace lockstep::storage {

using core::Scheduler;
using core::Task;
using sim::SeededRandom;

// The kind of op emitted at a step. Stable, append-only.
enum class DiffOpKind : std::uint8_t {
    Put,
    Del,
    Get,
    Snapshot,
    Sync,
};

[[nodiscard]] inline const char* diff_op_name(DiffOpKind k) noexcept {
    switch (k) {
        case DiffOpKind::Put:      return "put";
        case DiffOpKind::Del:      return "del";
        case DiffOpKind::Get:      return "get";
        case DiffOpKind::Snapshot: return "snapshot";
        case DiffOpKind::Sync:     return "sync";
    }
    return "?";
}

// A single mismatch witness: enough to REPLAY and to read the disagreement at a
// glance. All fields are owned copies (no dangling refs across awaits / runs).
struct DiffWitness {
    std::uint64_t seed = 0;     // the run seed — replay with this
    std::uint64_t step = 0;     // the op index at which the SUT diverged
    DiffOpKind kind = DiffOpKind::Get;
    Key key;                    // the key involved (empty for snapshot/sync)
    Seq snap_at = kNoSeq;       // the snapshot version the read observed (get)
    std::string expected;       // oracle's answer, rendered
    std::string got;            // SUT's answer, rendered
    std::string note;           // which assertion failed (human-readable)
};

// The verdict of a differential run. `ok` true ⇒ the SUT matched the oracle at
// every step. On a mismatch, `ok` is false and `witness` holds the FIRST
// divergence (the run stops there — the first witness is the minimal evidence).
struct DiffVerdict {
    bool ok = true;
    std::uint64_t steps = 0;     // ops actually executed before stop
    DiffWitness witness;         // meaningful only when !ok
};

// Tunables for the generated op stream. All small + fixed so the workload is
// dense (lots of overwrites / delete-then-read / snapshot-straddling reads). A
// DIFFERENT (seed) is the only thing that changes a run; cfg shapes the shape.
struct DiffConfig {
    std::uint64_t steps = 200;   // number of ops in the stream
    std::uint64_t n_keys = 5;    // key alphabet size (small ⇒ frequent collisions)
    std::uint64_t n_values = 4;  // value alphabet size
};

// Render an optional<Value> for a witness ("∅" for nullopt; the raw bytes else).
[[nodiscard]] inline std::string render_opt(const std::optional<Value>& v) {
    return v.has_value() ? ("\"" + *v + "\"") : std::string("nil");
}

// ---------------------------------------------------------------------------
// run_diff — drive `sut` and `reference` through the SAME seeded op stream and
// return the verdict. Pure function of (seed, cfg): both engines see byte-for-
// byte the same ops in the same order; the only difference allowed is the engine
// under test. Stops at the FIRST mismatch with a witness.
//
// The two engines are constructed by the CALLER (so the caller picks the SUT and
// the reference, e.g. Oracle-vs-Oracle or WrongOracle-vs-Oracle) and share the
// scheduler. We never reach into either engine's internals — only the public
// async Engine surface is used, exactly as a real engine would be driven.
// ---------------------------------------------------------------------------
[[nodiscard]] inline DiffVerdict run_diff(Scheduler& sched, Engine& sut, Engine& reference,
                                          std::uint64_t seed, DiffConfig cfg = {}) {
    DiffVerdict verdict;
    // One PRNG drives the whole op stream (the single source of randomness). Both
    // engines receive the identical generated ops — randomness chooses the OP, not
    // the engine's behavior.
    SeededRandom rng(seed);

    auto key_of = [&](std::uint64_t i) -> Key {
        return std::string("k") + std::to_string(i);
    };
    auto value_of = [&](std::uint64_t i) -> Value {
        return std::string("v") + std::to_string(i);
    };

    // The driver coroutine. It owns the verdict via reference; on the first
    // disagreement it fills the witness, flips ok=false, and co_returns early.
    Task driver = [](Scheduler& /*s*/, Engine& sut_e, Engine& ref_e, SeededRandom& r,
                     const DiffConfig& c, std::uint64_t the_seed, DiffVerdict& out,
                     auto mk_key, auto mk_value) -> Task {
        // The highest commit Seq we have seen agree so far — the live frontier of
        // the shared Seq space, used to pick in-range snapshot reads.
        Seq frontier = kNoSeq;

        for (std::uint64_t step = 0; step < c.steps; ++step) {
            out.steps = step + 1;
            // Draw the op kind with a mutation-heavy mix so versions pile up and
            // reads straddle snapshots. Weights are fixed (deterministic).
            const std::uint64_t roll = r.uniform(10);
            DiffOpKind kind;
            if (roll < 4) {
                kind = DiffOpKind::Put;       // 40%
            } else if (roll < 6) {
                kind = DiffOpKind::Del;       // 20%
            } else if (roll < 9) {
                kind = DiffOpKind::Get;       // 30%
            } else {
                // 10% split between snapshot + sync.
                kind = (r.uniform(2) == 0) ? DiffOpKind::Snapshot : DiffOpKind::Sync;
            }

            const Key key = mk_key(r.uniform(c.n_keys));

            switch (kind) {
                case DiffOpKind::Put: {
                    const Value val = mk_value(r.uniform(c.n_values));
                    // Issue to BOTH; copy out the Seqs before any further await.
                    const Seq sref = co_await ref_e.put(key, val);
                    const Seq ssut = co_await sut_e.put(key, val);
                    if (ssut != sref) {
                        out.ok = false;
                        out.witness = DiffWitness{
                            the_seed, step, kind, key, kNoSeq,
                            std::to_string(sref), std::to_string(ssut),
                            "put commit-seq disagreement (V-MONO)"};
                        co_return;
                    }
                    if (sref > frontier) {
                        frontier = sref;
                    }
                    break;
                }
                case DiffOpKind::Del: {
                    const Seq sref = co_await ref_e.del(key);
                    const Seq ssut = co_await sut_e.del(key);
                    if (ssut != sref) {
                        out.ok = false;
                        out.witness = DiffWitness{
                            the_seed, step, kind, key, kNoSeq,
                            std::to_string(sref), std::to_string(ssut),
                            "del commit-seq disagreement (V-MONO)"};
                        co_return;
                    }
                    if (sref > frontier) {
                        frontier = sref;
                    }
                    break;
                }
                case DiffOpKind::Get: {
                    // Pick a snapshot version somewhere in [0, frontier] so reads
                    // straddle historical versions (true MVCC reads), not just the
                    // tip. uniform(frontier+1) ∈ [0, frontier].
                    const Seq at = static_cast<Seq>(r.uniform(frontier + 1));
                    const Snapshot snap{at};
                    const std::optional<Value> vref = co_await ref_e.get(key, snap);
                    const std::optional<Value> vsut = co_await sut_e.get(key, snap);
                    if (vref != vsut) {
                        out.ok = false;
                        out.witness = DiffWitness{
                            the_seed, step, kind, key, at,
                            render_opt(vref), render_opt(vsut),
                            "get value disagreement (V-SNAP)"};
                        co_return;
                    }
                    break;
                }
                case DiffOpKind::Snapshot: {
                    const Snapshot sref = co_await ref_e.snapshot();
                    const Snapshot ssut = co_await sut_e.snapshot();
                    if (ssut.at != sref.at) {
                        out.ok = false;
                        out.witness = DiffWitness{
                            the_seed, step, kind, Key{}, kNoSeq,
                            std::to_string(sref.at), std::to_string(ssut.at),
                            "snapshot version disagreement"};
                        co_return;
                    }
                    break;
                }
                case DiffOpKind::Sync: {
                    const Error eref = co_await ref_e.sync();
                    const Error esut = co_await sut_e.sync();
                    if (eref.ok() != esut.ok()) {
                        out.ok = false;
                        out.witness = DiffWitness{
                            the_seed, step, kind, Key{}, kNoSeq,
                            eref.ok() ? "ok" : "err", esut.ok() ? "ok" : "err",
                            "sync durability-barrier disagreement (V-DUR)"};
                        co_return;
                    }
                    break;
                }
            }
        }
        co_return;
    }(sched, sut, reference, rng, cfg, seed, verdict, key_of, value_of);

    sched.spawn(std::move(driver));
    sched.run();
    return verdict;
}

}  // namespace lockstep::storage
