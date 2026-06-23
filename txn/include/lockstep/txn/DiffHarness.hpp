#pragma once

// DiffHarness.hpp — Phase 5 Stage M. THE DIFFERENTIAL + LINEARIZABILITY HARNESS.
//
// Source of truth: briefs/phase5.md Stage M + specs/CommitOrdering.tla.
//
// WHAT IT DOES: from a SEED it builds a deterministic workload of ONE-SHOT txns
// (params in, deterministic body over the reads, writes out) ALREADY in a global
// seqLog order (the order consensus would have produced — here generated from the
// seed to stand in for the Phase-4 layer in Stage M). It runs that batch against
//   (a) the system-under-test executor (any ExecutorFactory), and
//   (b) the strict-serializable ORACLE,
// then runs the full checker battery (the differential + the per-D5-level + OLLP
// + linearizability checks). It records a HISTORY (the per-txn commit info) for
// the linearizability/serial-order check inside check_strict_serializable.
//
// The workload deliberately exercises:
//   * StrictSerializable reads (the default path — read-modify-write chains so a
//     reorder or stale read is observable),
//   * value-dependent footprints (OLLP trigger/extra: a read of a "trigger" key
//     that, when non-∅, expands the read set — drives the re-sequence path),
//   * relaxed-level reads (Snapshot / BoundedStaleness / ReadYourWrites) so the
//     D5 checkers have material to judge.
//
// DETERMINISM (binding): the ONLY entropy is the seed, consumed through a tiny
// inlined SplitMix64 (NO std::*_distribution, NO ambient randomness, NO clock).
// std::map / ordered iteration throughout. Same seed ⇒ byte-identical workload ⇒
// byte-identical RunResults ⇒ byte-identical verdicts. txn/ is NOT lint-exempt.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/Oracle.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace lockstep::txn {

// A tiny deterministic PRNG (SplitMix64). Header-inlined so the harness needs no
// provider; it is a PURE function of its seed state. NOT a std::*_engine /
// std::*_distribution (those are forbidden) — just integer mixing.
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : s_(seed) {}
    [[nodiscard]] std::uint64_t next() noexcept {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n) for n>0, via rejection-free modulo (bias negligible at our
    // tiny n; deterministic is all that matters here).
    [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept {
        return n == 0 ? 0 : (next() % n);
    }

private:
    std::uint64_t s_;
};

// Workload knobs. Small by default (in-gate sweeps are tiny + bounded).
struct WorkloadConfig {
    std::size_t num_txns = 8;     // txns in the batch
    std::size_t num_keys = 4;     // key space size (k0..k{num_keys-1})
    ExecConfig exec;              // MaxRetry + replica_lag passed to the executor
};

// Build a deterministic one-shot txn batch from a seed, ALREADY in seqLog order.
//
// SHAPE (deliberate, so each verification surface has teeth):
//   * The PRIMARY read is ALWAYS StrictSerializable on key `k`, and it drives a
//     read-modify-write on `k`. The WRITE depends ONLY on this strict read, so
//     the serializable result is well-defined and the oracle ↔ SUT differential
//     is exact. Any reorder / stale strict read changes this observable value.
//   * OLLP value-dependent footprint: with some probability the txn DECLARES only
//     {k} but its body, when the strict value of `k` is non-∅, ALSO needs an
//     undeclared `trig` key (extra_reads={trig}). That is a recon mismatch: the
//     honest executor RE-SEQUENCES (bounded, then aborts); a recon-skipping
//     executor commits on the stale footprint (the teeth). The trigger decision
//     uses only the DECLARED strict value of `k`, so no undeclared read leaks
//     into the body's logic (faithful to CommitOrdering.tla's Trigger/Extra).
//   * A SEPARATE relaxed-level observational read (Snapshot / BoundedStaleness /
//     ReadYourWrites) on another key, purely so the per-D5-level checkers have
//     material to judge. It is OBSERVATIONAL: it does NOT feed the write (relaxed
//     reads are not part of the strict-serializable result), so it cannot perturb
//     the differential. The RYW read targets the SESSION's own written key so the
//     read-your-writes relationship spans txns.
[[nodiscard]] inline std::vector<Txn> build_workload(std::uint64_t seed,
                                                     const WorkloadConfig& wc) {
    SplitMix rng(seed ^ 0xD1B54A32D192ED03ULL);
    std::vector<Txn> txns;
    txns.reserve(wc.num_txns);

    const SessionId session = 1;  // a single RYW session threads through the batch

    for (std::size_t i = 0; i < wc.num_txns; ++i) {
        Txn t;
        t.id = static_cast<std::uint64_t>(i + 1);

        const std::uint64_t k_idx = rng.below(wc.num_keys);
        const Key k = "k" + std::to_string(k_idx);
        const std::uint64_t trig_idx = rng.below(wc.num_keys);
        const Key trig = "k" + std::to_string(trig_idx);

        // PRIMARY strict read on k — the RMW driver.
        t.declared_reads.push_back(read_strict(k));

        // OLLP: should this txn have a value-dependent (undeclared) footprint? Only
        // when trig != k (else there is nothing extra to read).
        const bool ollp_expands = (trig != k) && (rng.below(2) == 0);

        // A separate OBSERVATIONAL relaxed read for the D5 checkers. Pick a level;
        // bias toward strict-only (no extra read) so the default path dominates.
        const std::uint64_t lvl_roll = rng.below(8);
        Read observ;
        bool has_observ = true;
        const Key obs_key = "k" + std::to_string(rng.below(wc.num_keys));
        if (lvl_roll < 4) {
            has_observ = false;  // strict-only txn (the common case)
        } else if (lvl_roll < 5) {
            observ = read_snapshot(obs_key, kNoSeq);
        } else if (lvl_roll < 6) {
            observ = read_bounded(obs_key, /*max_lag=*/wc.exec.replica_lag);
        } else {
            // RYW: read the SESSION's own key (k), so the session observes the
            // write it (and prior session txns) committed to k.
            observ = read_ryw(k, session);
        }
        if (has_observ) {
            t.declared_reads.push_back(observ);
        }

        const std::uint64_t my_id = t.id;
        const Key write_key = k;
        const Key trig_key = trig;

        t.body = [my_id, write_key, k, trig_key,
                  ollp_expands](const ReadView& reads) -> Txn::Outcome {
            Txn::Outcome oc;
            // Read-modify-write driven by the STRICT read of k.
            ReadResult cur;
            const auto it = reads.find(k);
            if (it != reads.end()) {
                cur = it->second;
            }
            const std::string base = cur.has_value() ? *cur : std::string("e");
            const std::string nv = base + ">t" + std::to_string(my_id);
            oc.writes[write_key] = nv;
            oc.result = "wrote:" + nv;

            // Value-dependent footprint: when the strict value of k is non-∅, the
            // body ALSO needs the (undeclared) trigger key. extra_reads={trig}
            // outside the declared footprint => a recon mismatch the honest
            // executor re-sequences on and a recon-skipper wrongly commits on.
            if (ollp_expands && cur.has_value()) {
                oc.extra_reads.push_back(trig_key);
            }
            return oc;
        };

        txns.push_back(std::move(t));
    }
    return txns;
}

// The result of running one seed: both runs + every verdict. A pure fn of seed.
struct DiffOutcome {
    std::uint64_t seed = 0;
    std::vector<Txn> submitted;
    RunResult sut;
    RunResult oracle;
    std::vector<Verdict> verdicts;
    bool all_ok = true;  // true iff every verdict passed

    // Whether the SUT made PROGRESS (at least one commit). A no-progress run is
    // vacuously "consistent" — the harness flags it so a dead executor is never
    // mistaken for correct.
    bool sut_made_progress = false;
};

// Run ONE seed through a SUT factory + the oracle + the full checker battery.
[[nodiscard]] inline DiffOutcome run_diff_seed(std::uint64_t seed,
                                               const ExecutorFactory& sut_factory,
                                               const WorkloadConfig& wc) {
    DiffOutcome out;
    out.seed = seed;
    out.submitted = build_workload(seed, wc);

    StrictSerialOracle oracle;
    out.oracle = oracle.submit_batch(out.submitted, wc.exec);

    auto sut = sut_factory();
    out.sut = sut->submit_batch(out.submitted, wc.exec);

    out.verdicts = run_all_checkers(out.sut, out.oracle, out.submitted, wc.exec, seed);
    for (const Verdict& v : out.verdicts) {
        if (!v.ok) {
            out.all_ok = false;
        }
    }
    for (const CommitInfo& c : out.sut.commits) {
        if (c.status == Status::Committed) {
            out.sut_made_progress = true;
            break;
        }
    }
    return out;
}

// Render one DiffOutcome to stable, line-oriented text (the byte-reproducibility
// surface for the determinism self-test). Pure function of the outcome.
[[nodiscard]] inline std::string render_outcome(const DiffOutcome& o) {
    std::string s;
    s += "seed=" + std::to_string(o.seed) + " progress=" +
         (o.sut_made_progress ? "1" : "0") + "\n";
    for (const CommitInfo& c : o.sut.commits) {
        s += "  txn=" + std::to_string(c.txn_id) + " status=" + status_name(c.status) +
             " seq=" + std::to_string(c.seq_index) + " ver=" +
             std::to_string(c.commit_version) + " retries=" + std::to_string(c.retries) +
             " result=\"" + c.result + "\"\n";
    }
    for (const Verdict& v : o.verdicts) {
        s += "  check[" + v.checker + "]=" + (v.ok ? "ok" : "VIOLATION");
        if (!v.ok) {
            s += " witness=" + v.witness;
        }
        s += "\n";
    }
    return s;
}

}  // namespace lockstep::txn
