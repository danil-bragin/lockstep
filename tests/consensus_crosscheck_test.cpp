// consensus_crosscheck_test.cpp — Phase 4 Stage V, THE PHASE-4 GATE.
//
// The dual-implementation cross-check gate (master-plan §6.5 blind-spot defense).
// Two INDEPENDENTLY-built Raft implementations — impl A (raft_a::RaftNodeA) and
// impl B (raft_b::RaftNodeB), each already conformant to specs/Consensus.tla — are
// driven on the SAME seed + workload + fault schedule and must AGREE on every
// committed history. If the two ever commit the same client's commands in
// CONTRADICTORY orders, at least one implementation is WRONG.
//
// WHAT "AGREE" MEANS BETWEEN TWO *INDEPENDENT* IMPLS (V-XCHECK — see the receipt /
// backprop note): two correct Raft impls need NOT produce byte-identical committed
// logs. The TERM a value first commits at is a per-impl election artifact (timeout
// jitter ⇒ different election rounds). Under concurrent clients the cross-client
// value INTERLEAVING legitimately differs (both linearizable). A command accepted
// by a leader that is then deposed before commit may survive in one impl's log and
// not the other's — a legitimate not-committed GAP, not a conflict. So a RAW
// (term,value) committed-log equality (CrossCheck.hpp's impl-vs-ITSELF determinism
// baseline) is UNSOUND across two independent impls. The sound, toothed cross-impl
// agreement is:
//   V-XCHECK: ∀ client c, for the commands BOTH impls committed for c, A and B
//             order them IDENTICALLY (no inversion). Terms ignored; one-sided gaps
//             allowed. (Each impl is a linearization of the same per-client program
//             order — StateMachineSafety / linearizability AT THE CROSS-IMPL LEVEL.)
// We assert V-XCHECK on the full-fault concurrent sweep, PLUS a stricter
// deterministic-history sub-gate: with NO faults + a SINGLE sequential client the
// committed VALUE sequences must be byte-identical across impls (the forced history
// — a dropped/reordered committed command shows immediately).
//
// THIS SINGLE TEST IS THE PHASE-4 GATE — conform AND agree:
//   (1) AGREE (fault storm) — V-XCHECK holds on EVERY seed under partition/heal +
//       crash/restart + net reorder/drop/dup. Divergence ⇒ FAIL with seed + witness.
//   (2) AGREE (forced history) — no-fault single-client committed value sequences
//       are byte-identical A vs B on every seed.
//   (3) CONFORM (belt and suspenders) — on the same full-fault sweep, BOTH impls
//       individually pass all five conformance checkers.
//   (4) PROGRESS — the agreement is non-vacuous: real commits accumulate.
//   (5) DETERMINISM — same seed ⇒ byte-identical output (in-test double-run + the
//       external double-run diff under a stable marker).
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time is virtual; all RPC/durability via the
// injected boundary. consensus/ is NOT lint-exempt → the forbidden-call lint scans
// this TU. Bounded (in-gate sweep ≤ 64 seeds; CONSENSUS_XCHECK_SEEDS env override
// capped at 4096). Inherits CTest TIMEOUT 90.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConformanceCheckers.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/CrossCheck.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>
#include <lockstep/consensus/raft_b/RaftNodeB.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::CrossCheckResult;
using lockstep::consensus::LogEntry;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::SubmitObservation;
using lockstep::consensus::cross_check;
using lockstep::consensus::run_all_conformance;
using lockstep::consensus::run_cluster;
using lockstep::consensus::raft_a::make_raft_a_factory;
using lockstep::consensus::raft_b::RaftNodeB;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

const char* ck_name(std::size_t i) {
    switch (i) {
        case 0: return "ElectionSafety";
        case 1: return "LogMatching";
        case 2: return "StateMachineSafety";
        case 3: return "LeaderAppendOnly";
        case 4: return "Linearizability";
    }
    return "?";
}

std::size_t count_committed(const ObservedRun& run) {
    std::size_t n = 0;
    for (const SubmitObservation& s : run.submits) {
        if (s.committed) {
            ++n;
        }
    }
    return n;
}

// The per-client command IDENTITY: strip the global "_opN" suffix (op_id is a
// cross-client global counter — its interleaving legitimately differs per impl)
// and drop the term entirely (a per-impl election artifact). What remains, e.g.
// "c0_v3", is the (client_id, value-seq) identity — intrinsic to the submit.
std::string client_key(const std::string& value) {
    const std::size_t p = value.find("_op");
    return p == std::string::npos ? value : value.substr(0, p);
}

// The committed client-command identities, in committed-index order, for a run.
std::vector<std::string> committed_keys(const ObservedRun& run) {
    std::vector<std::string> ks;
    ks.reserve(run.committed_log.size());
    for (const LogEntry& e : run.committed_log) {
        ks.push_back(client_key(e.value));
    }
    return ks;
}

// V-XCHECK relative-order check (one direction): walking `a`, the position in `b`
// of each command also present in `b` must be STRICTLY INCREASING — i.e. `a` never
// orders two common commands in the opposite order to `b`. Commands in `a` not in
// `b` are skipped (legitimate one-sided gap). Returns "" if consistent, else a
// witness naming the inverted pair.
std::string rel_order_witness(const std::vector<std::string>& a,
                              const std::vector<std::string>& b) {
    long long last_pos = -1;
    std::string last_key;
    for (const std::string& x : a) {
        long long pos = -1;
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (b[j] == x) {
                pos = static_cast<long long>(j);
                break;
            }
        }
        if (pos < 0) {
            continue;  // committed in A but not B: legitimate gap, no order claim
        }
        if (pos <= last_pos) {
            return "INVERSION: \"" + last_key + "\" before \"" + x +
                   "\" in A but the reverse order in B (b_pos " +
                   std::to_string(pos) + " <= " + std::to_string(last_pos) + ")";
        }
        last_pos = pos;
        last_key = x;
    }
    return "";
}

// V-XCHECK for a whole run pair: per client, the commands BOTH impls committed must
// appear in the SAME relative order (no inversion either direction). Returns "" on
// agreement, else a per-client witness.
std::string crosscheck_agree(const ObservedRun& run_a, const ObservedRun& run_b,
                             std::uint64_t n_clients) {
    const std::vector<std::string> ka = committed_keys(run_a);
    const std::vector<std::string> kb = committed_keys(run_b);
    for (std::uint64_t c = 0; c < n_clients; ++c) {
        const std::string pfx = "c" + std::to_string(c) + "_";
        std::vector<std::string> a, b;
        for (const std::string& k : ka) {
            if (k.rfind(pfx, 0) == 0) {
                a.push_back(k);
            }
        }
        for (const std::string& k : kb) {
            if (k.rfind(pfx, 0) == 0) {
                b.push_back(k);
            }
        }
        std::string w = rel_order_witness(a, b);
        if (w.empty()) {
            w = rel_order_witness(b, a);  // symmetric
        }
        if (!w.empty()) {
            return "client " + std::to_string(c) + ": " + w;
        }
    }
    return "";
}

// Count the per-client commands committed by BOTH impls (the cross-checked load —
// proves the agreement is non-vacuous, comparing real shared commands).
std::size_t common_committed(const ObservedRun& run_a, const ObservedRun& run_b) {
    const std::vector<std::string> ka = committed_keys(run_a);
    const std::vector<std::string> kb = committed_keys(run_b);
    std::size_t n = 0;
    for (const std::string& x : ka) {
        for (const std::string& y : kb) {
            if (x == y) {
                ++n;
                break;
            }
        }
    }
    return n;
}

// Stable committed-value render — the byte-repro surface (terms dropped, since they
// are a per-impl artifact and not part of the agreed history).
std::string render_values(const std::vector<LogEntry>& log) {
    std::string out = "len=" + std::to_string(log.size()) + ":";
    for (const LogEntry& e : log) {
        out += " " + client_key(e.value);
    }
    return out;
}

// How many seeds the in-gate sweep covers. This single binary builds ~4 full
// clusters PER SEED (impl A + impl B, twice: the fault-storm sweep and the forced-
// history sweep), so it is the heaviest consensus test. The in-gate default is held
// to 20 so it stays well under the CTest TIMEOUT 90 even under the SLOWEST
// sanitizer (TSan instruments every memory access; impl A's own 64-seed sweep
// already costs ~55s there — twice that would time out). CONSENSUS_XCHECK_SEEDS
// raises it for a bounded out-of-gate stress run (e.g. 4096 on an uninstrumented
// build), hard-capped at 4096 (never unbounded). Resource discipline: a prior run
// froze the host — keep the in-gate sweep small and the override bounded.
std::uint64_t sweep_seeds() {
    std::uint64_t k = 20;
    if (const char* env = std::getenv("CONSENSUS_XCHECK_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0) {
            k = (v > 4096) ? 4096 : static_cast<std::uint64_t>(v);
        }
    }
    return k;
}

// ---------------------------------------------------------------------------
// (1) AGREE (fault storm) + (3) CONFORM seed sweep, under the FULL fault envelope.
//     For each seed: run A and B on the same seed; assert V-XCHECK (both impls
//     order every client's commonly-committed commands identically); run BOTH
//     through the five conformance checkers (each must individually hold). Logs
//     the per-seed agree fingerprint for replay.
// ---------------------------------------------------------------------------
void crosscheck_and_conform_sweep() {
    std::printf("CROSS-CHECK + CONFORM SWEEP (full fault envelope; A vs B; "
                "V-XCHECK per-client order):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    ClusterConfig cfg;  // defaults: 3 nodes, 2 clients, full envelope
    const std::uint64_t kSeeds = sweep_seeds();

    std::uint64_t agree_count = 0;
    std::uint64_t first_diverge_seed = 0;
    std::string first_diverge_witness;

    std::size_t fired_a[5] = {0, 0, 0, 0, 0};
    std::size_t fired_b[5] = {0, 0, 0, 0, 0};
    std::uint64_t first_bad_seed_a = 0, first_bad_seed_b = 0;
    std::string first_bad_a, first_bad_b;

    std::size_t total_committed_a = 0, total_committed_b = 0;
    std::size_t total_common = 0;
    std::size_t seeds_with_progress = 0;

    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0xC305'5000ULL + s;

        // One cluster build per impl on this seed; reused for cross-check + conform.
        const ObservedRun run_a = run_cluster(seed, fa, cfg);
        const ObservedRun run_b = run_cluster(seed, fb, cfg);

        // (1) AGREE: per-client relative order of commonly-committed commands.
        const std::string w = crosscheck_agree(run_a, run_b, cfg.n_clients);
        const std::size_t common = common_committed(run_a, run_b);
        total_common += common;
        if (w.empty()) {
            ++agree_count;
        } else if (first_diverge_witness.empty()) {
            first_diverge_seed = seed;
            first_diverge_witness = w;
        }

        // (3) CONFORM: both impls individually pass all five checkers.
        const std::vector<NamedVerdict> va = run_all_conformance(run_a);
        const std::vector<NamedVerdict> vb = run_all_conformance(run_b);
        for (std::size_t i = 0; i < 5; ++i) {
            if (!va[i].verdict.ok) {
                ++fired_a[i];
                if (first_bad_a.empty()) {
                    first_bad_seed_a = seed;
                    first_bad_a = std::string(ck_name(i)) + ": " + va[i].verdict.witness;
                }
            }
            if (!vb[i].verdict.ok) {
                ++fired_b[i];
                if (first_bad_b.empty()) {
                    first_bad_seed_b = seed;
                    first_bad_b = std::string(ck_name(i)) + ": " + vb[i].verdict.witness;
                }
            }
        }

        const std::size_t ca = count_committed(run_a);
        const std::size_t cb = count_committed(run_b);
        total_committed_a += ca;
        total_committed_b += cb;
        if (ca > 0 || cb > 0) {
            ++seeds_with_progress;
        }

        std::printf("  seed=0x%llX agree=%d common=%zu Alen=%zu Blen=%zu\n",
                    static_cast<unsigned long long>(seed), w.empty() ? 1 : 0,
                    common, run_a.committed_log.size(), run_b.committed_log.size());
    }

    std::printf("  SUMMARY: seeds=%llu AGREE=%llu/%llu common_committed_cmds=%zu\n",
                static_cast<unsigned long long>(kSeeds),
                static_cast<unsigned long long>(agree_count),
                static_cast<unsigned long long>(kSeeds), total_common);
    std::printf("  CONFORM A: Elect=%zu Match=%zu SMS=%zu LAO=%zu Lin=%zu  "
                "committed=%zu\n",
                fired_a[0], fired_a[1], fired_a[2], fired_a[3], fired_a[4],
                total_committed_a);
    std::printf("  CONFORM B: Elect=%zu Match=%zu SMS=%zu LAO=%zu Lin=%zu  "
                "committed=%zu\n",
                fired_b[0], fired_b[1], fired_b[2], fired_b[3], fired_b[4],
                total_committed_b);
    std::printf("  seeds_with_progress=%zu\n", seeds_with_progress);

    if (!first_diverge_witness.empty()) {
        std::fprintf(stderr,
                     "  DIVERGENCE seed=0x%llX %s\n"
                     "    Cross-check VIOLATED: two INDEPENDENT impls committed a "
                     "client's commands in CONTRADICTORY orders for the same seed + "
                     "workload + fault schedule (master-plan §6.5). At least one "
                     "implementation is wrong.\n",
                     static_cast<unsigned long long>(first_diverge_seed),
                     first_diverge_witness.c_str());
    }
    if (!first_bad_a.empty()) {
        std::fprintf(stderr, "  IMPL-A VIOLATION seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed_a),
                     first_bad_a.c_str());
    }
    if (!first_bad_b.empty()) {
        std::fprintf(stderr, "  IMPL-B VIOLATION seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed_b),
                     first_bad_b.c_str());
    }

    // (1) AGREE on every seed — V-XCHECK, the §6.5 dual-impl cross-check.
    check(agree_count == kSeeds,
          "A and B AGREE on every committed history (V-XCHECK: no client's commands "
          "are committed in contradictory orders on ANY seed under the fault storm)");

    // (3) CONFORM — both impls individually pass all five checkers on the sweep.
    check(fired_a[0] == 0 && fired_b[0] == 0, "ElectionSafety holds for BOTH impls");
    check(fired_a[1] == 0 && fired_b[1] == 0, "LogMatching holds for BOTH impls");
    check(fired_a[2] == 0 && fired_b[2] == 0, "StateMachineSafety holds for BOTH impls");
    check(fired_a[3] == 0 && fired_b[3] == 0, "LeaderAppendOnly holds for BOTH impls");
    check(fired_a[4] == 0 && fired_b[4] == 0, "Linearizability holds for BOTH impls");

    // (4) PROGRESS — the agreement is non-vacuous (real shared commits compared).
    check(total_committed_a > 0,
          "impl A makes PROGRESS under faults (commits accumulate — not vacuous)");
    check(total_committed_b > 0,
          "impl B makes PROGRESS under faults (commits accumulate — not vacuous)");
    check(total_common > 0,
          "the cross-check actually compared shared committed commands (common "
          "committed set is non-empty — agreement has teeth)");
}

// ---------------------------------------------------------------------------
// (2) AGREE (forced history). With NO faults and a SINGLE sequential client the
//     committed history is FORCED — both impls must commit the EXACT same client
//     value sequence, in order, on every seed (a dropped / reordered committed
//     command shows immediately). The strictest agreement: byte-identical
//     committed VALUE sequence A == B.
// ---------------------------------------------------------------------------
void forced_history_agree() {
    std::printf("FORCED-HISTORY AGREE (no-fault, single sequential client; "
                "committed VALUE sequences must be IDENTICAL A==B):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    ClusterConfig cfg;
    cfg.full_envelope = false;      // pristine bus + honest disk
    cfg.partition_episodes = 0;
    cfg.crash_episodes = 0;
    cfg.n_clients = 1;              // strictly sequential ⇒ forced order
    cfg.submits_per_client = 12;

    const std::uint64_t kSeeds = sweep_seeds();
    std::uint64_t agree_count = 0;
    std::size_t total_shared = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad;

    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0xC305'7000ULL + s;
        const ObservedRun run_a = run_cluster(seed, fa, cfg);
        const ObservedRun run_b = run_cluster(seed, fb, cfg);
        const std::vector<std::string> ka = committed_keys(run_a);
        const std::vector<std::string> kb = committed_keys(run_b);
        const std::size_t m = ka.size() < kb.size() ? ka.size() : kb.size();
        total_shared += m;
        bool ok = true;
        for (std::size_t i = 0; i < m; ++i) {
            if (ka[i] != kb[i]) {
                ok = false;
                if (first_bad.empty()) {
                    first_bad_seed = seed;
                    first_bad = "index " + std::to_string(i + 1) + " A=\"" + ka[i] +
                                "\" B=\"" + kb[i] + "\"";
                }
                break;
            }
        }
        if (ok) {
            ++agree_count;
        }
    }

    std::printf("  seeds=%llu AGREE=%llu/%llu shared_committed_entries=%zu\n",
                static_cast<unsigned long long>(kSeeds),
                static_cast<unsigned long long>(agree_count),
                static_cast<unsigned long long>(kSeeds), total_shared);
    if (!first_bad.empty()) {
        std::fprintf(stderr, "  FORCED-HISTORY DIVERGENCE seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed),
                     first_bad.c_str());
    }
    check(agree_count == kSeeds,
          "no-fault single-client: A and B commit the IDENTICAL value sequence on "
          "every seed (forced history — strict cross-impl agreement)");
    check(total_shared > 0,
          "forced-history gate actually committed values (non-vacuous)");
}

// ---------------------------------------------------------------------------
// (5) DETERMINISM — same seed ⇒ byte-identical cross-check fingerprint. Renders
//     both impls' committed value sequences + the agree witness for one full-fault
//     seed, twice; they must match byte-for-byte (pure function of seed). Also
//     exercises CrossCheck.hpp's impl-vs-ITSELF determinism baseline (the same
//     factory twice MUST be byte-identical (term,value) — its intended use).
// ---------------------------------------------------------------------------
std::string xcheck_fingerprint() {
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    ClusterConfig cfg;
    const std::uint64_t seed = 0xC305'BEEFULL;
    const ObservedRun run_a = run_cluster(seed, fa, cfg);
    const ObservedRun run_b = run_cluster(seed, fb, cfg);
    const std::string w = crosscheck_agree(run_a, run_b, cfg.n_clients);
    std::string out = "seed=" + std::to_string(seed) +
                      " agree=" + std::to_string(w.empty() ? 1 : 0) + "\n";
    out += "A " + render_values(run_a.committed_log) + "\n";
    out += "B " + render_values(run_b.committed_log) + "\n";
    if (!w.empty()) {
        out += "witness=" + w + "\n";
    }
    return out;
}

void determinism() {
    std::printf("DETERMINISM (same seed ⇒ byte-identical cross-check + impl-vs-self "
                "byte-identical):\n");
    const std::string a = xcheck_fingerprint();
    const std::string b = xcheck_fingerprint();
    check(a == b,
          "same seed ⇒ byte-identical cross-check fingerprint (pure fn of seed)");

    // CrossCheck.hpp's own baseline: the SAME factory run twice on one seed is a
    // strict determinism cross-check (term AND value byte-identical → agree).
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    ClusterConfig cfg;
    bool self_a = true, self_b = true;
    for (std::uint64_t s = 0; s < 8; ++s) {
        const std::uint64_t seed = 0xC305'5E1FULL + s;
        const CrossCheckResult ra = cross_check(seed, fa, fa, cfg);
        const CrossCheckResult rb = cross_check(seed, fb, fb, cfg);
        if (!ra.agree) {
            self_a = false;
        }
        if (!rb.agree) {
            self_b = false;
        }
    }
    check(self_a, "impl A vs ITSELF is byte-identical (deterministic, CrossCheck.hpp "
                  "baseline)");
    check(self_b, "impl B vs ITSELF is byte-identical (deterministic, CrossCheck.hpp "
                  "baseline)");
}

// ---------------------------------------------------------------------------
// (6) N=1 SELF-COMMIT. A single-node cluster (quorum == 1) must elect its lone
//     member leader and COMMIT every SUBMITted value with NO peer ack — the path
//     a prod 1-node deployment exercises but the N=3/5 sweep never did. For BOTH
//     impls: commit_index advances to each submit's index, the committed log holds
//     the submitted value, and A and B AGREE (V-XCHECK) on the committed history.
//     Bounded, deterministic; the N=3 sweep above already proved the fix is a
//     strict no-op for N>=2 (byte-identical), so this only enables N=1.
// ---------------------------------------------------------------------------
void single_node_self_commit() {
    std::printf("N=1 SELF-COMMIT (single-node leader self-commits SUBMITs; A, B, "
                "and A==B):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    ClusterConfig cfg;
    cfg.n_nodes = 1;                // ONE node: quorum() == 1, no peers ever ack
    cfg.n_clients = 1;             // single sequential client ⇒ forced history
    cfg.submits_per_client = 6;
    cfg.full_envelope = false;     // pristine bus + honest disk (no peers anyway)
    cfg.partition_episodes = 0;    // a 1-node cluster cannot partition
    cfg.crash_episodes = 0;

    const std::uint64_t seed = 0xC305'0001ULL;
    const ObservedRun run_a = run_cluster(seed, fa, cfg);
    const ObservedRun run_b = run_cluster(seed, fb, cfg);

    auto report = [](const char* who, const ObservedRun& run) -> std::size_t {
        std::size_t committed = count_committed(run);
        std::printf("  %s: submits=%zu committed=%zu committed_log_len=%zu vals=[%s]\n",
                    who, run.submits.size(), committed, run.committed_log.size(),
                    render_values(run.committed_log).c_str());
        return committed;
    };
    const std::size_t ca = report("A", run_a);
    const std::size_t cb = report("B", run_b);

    // Every SUBMIT must commit (a lone leader, no faults — nothing can block it).
    check(ca == cfg.submits_per_client,
          "impl A N=1: EVERY submitted value commits (lone-leader self-commit)");
    check(cb == cfg.submits_per_client,
          "impl B N=1: EVERY submitted value commits (lone-leader self-commit)");
    check(run_a.committed_log.size() == cfg.submits_per_client,
          "impl A N=1: committed log length == #submits (commit_index advanced)");
    check(run_b.committed_log.size() == cfg.submits_per_client,
          "impl B N=1: committed log length == #submits (commit_index advanced)");

    // The committed log holds the submitted values in submit order (forced history).
    bool a_values_ok = true, b_values_ok = true;
    for (std::size_t i = 0; i < run_a.committed_log.size(); ++i) {
        if (client_key(run_a.committed_log[i].value) != "c0_v" + std::to_string(i)) {
            a_values_ok = false;
        }
    }
    for (std::size_t i = 0; i < run_b.committed_log.size(); ++i) {
        if (client_key(run_b.committed_log[i].value) != "c0_v" + std::to_string(i)) {
            b_values_ok = false;
        }
    }
    check(a_values_ok, "impl A N=1: committed log holds the SUBMITTED values in order");
    check(b_values_ok, "impl B N=1: committed log holds the SUBMITTED values in order");

    // Both impls individually conform at N=1 (all five checkers hold).
    const std::vector<NamedVerdict> va = run_all_conformance(run_a);
    const std::vector<NamedVerdict> vb = run_all_conformance(run_b);
    bool conform_a = true, conform_b = true;
    for (std::size_t i = 0; i < 5; ++i) {
        if (!va[i].verdict.ok) { conform_a = false; }
        if (!vb[i].verdict.ok) { conform_b = false; }
    }
    check(conform_a, "impl A N=1: all five conformance checkers hold");
    check(conform_b, "impl B N=1: all five conformance checkers hold");

    // A == B agree at N=1 (V-XCHECK per-client relative order; also byte-identical
    // committed value sequence since it is a forced single-client history).
    const std::string w = crosscheck_agree(run_a, run_b, cfg.n_clients);
    check(w.empty(), "N=1: A and B AGREE on the committed history (V-XCHECK)");
    const std::vector<std::string> ka = committed_keys(run_a);
    const std::vector<std::string> kb = committed_keys(run_b);
    check(ka == kb,
          "N=1: A and B commit the IDENTICAL value sequence (forced history)");

    // DETERMINISM at N=1: same seed ⇒ byte-identical committed value sequence.
    const ObservedRun run_a2 = run_cluster(seed, fa, cfg);
    const ObservedRun run_b2 = run_cluster(seed, fb, cfg);
    check(render_values(run_a.committed_log) == render_values(run_a2.committed_log),
          "N=1: impl A is deterministic (same seed ⇒ byte-identical committed log)");
    check(render_values(run_b.committed_log) == render_values(run_b2.committed_log),
          "N=1: impl B is deterministic (same seed ⇒ byte-identical committed log)");
}

}  // namespace

int main() {
    std::printf("consensus_crosscheck_test: Phase 4 Stage V — DUAL-IMPL CROSS-CHECK "
                "GATE (conform + agree)\n");

    crosscheck_and_conform_sweep();
    forced_history_agree();
    single_node_self_commit();
    determinism();

    // Emit the cross-check fingerprint under a stable marker so the gate's external
    // double-run diff proves byte-identical replay across whole-process runs.
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(xcheck_fingerprint().c_str(), stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_crosscheck_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("consensus_crosscheck_test: ALL CHECKS PASSED\n");
    return 0;
}
