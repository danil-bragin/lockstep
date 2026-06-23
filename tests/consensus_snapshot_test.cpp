// consensus_snapshot_test.cpp — Phase 4 C4.3 LOG SNAPSHOTTING / COMPACTION GATE.
//
// Proves both independently-built Raft impls (raft_a::RaftNodeA + raft_b::RaftNodeB)
// implement log snapshotting + compaction + InstallSnapshot conformant to
// specs/Snapshot.tla, WITHOUT regressing the base Consensus.tla conformance. Over a
// seed sweep under the full fault storm (partition/heal, crash/restart, net
// reorder/drop/dup) it drives ENOUGH ops to (i) trigger TakeSnapshot on some nodes
// AND (ii) drive a lagging node that must be caught up by InstallSnapshot, then
// asserts:
//
//   (a) StateMachineSafety — no committed entry is lost / diverges after
//       compaction (the across-run committed-history checker; the SAME harness that
//       judges base Raft, unchanged, now run with snapshotting underneath).
//   (b) RecoveredEqualsFull — a node caught up via InstallSnapshot reaches the SAME
//       committed state as the others: the reconstructed full logical log
//       (snapshot.state ++ retained suffix, surfaced by log()) at any commonly-
//       committed index equals every other node's — Snapshot.tla RecoveredEqualsFull.
//   (c) COMPACTION ACTUALLY HAPPENS + the log is BOUNDED (measured): snapshots are
//       taken, the discarded prefix (snapshot_index) advances, and the PHYSICAL
//       (in-memory) retained log stays bounded by the threshold + a quorum's worth
//       of in-flight tail while the LOGICAL log keeps growing past it.
//   (d) DETERMINISM — same seed ⇒ byte-identical rendered run (incl. the snapshot
//       measurements), in-test double-run + external double-run diff.
//   (e) the FIVE conformance checkers still report 0 violations on BOTH impls.
//   (f) the A-vs-B cross-check (V-XCHECK per-client committed order) still agrees.
//
// TEETH (the gate has them): a deliberately-WRONG snapshotting node is CAUGHT —
//   * TeethDiscardUnappliedNode: snapshots through commit_index but DISCARDS its
//     reconstructed prefix as EMPTY state (an InstallSnapshot adopting empty
//     state / a TakeSnapshot that drops un-folded entries). Its log() then loses
//     the committed prefix → StateMachineSafety / RecoveredEqualsFull FIRE.
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time virtual; all RPC/durability via the
// injected boundary. consensus/ is NOT lint-exempt → the forbidden-call lint scans
// this TU. Bounded (in-gate sweep ≤ 64; CONSENSUS_SNAPSHOT_SEEDS override capped at
// 4096). Inherits CTest TIMEOUT 90.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConformanceCheckers.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>
#include <lockstep/consensus/raft_b/RaftNodeB.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ClusterSnapshot;
using lockstep::consensus::ConsensusNode;
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::Index;
using lockstep::consensus::LogEntry;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::NodeDeps;
using lockstep::consensus::NodeConfig;
using lockstep::consensus::NodeSnapshot;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::Role;
using lockstep::consensus::SubmitObservation;
using lockstep::consensus::Term;
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

// A snapshot-heavy config: many submits per client (so logs grow well past the
// impls' kSnapshotThreshold ⇒ TakeSnapshot fires) and crash episodes (so a node
// falls behind the leader's discarded prefix ⇒ InstallSnapshot fires to catch it
// up). Full fault envelope. Still bounded — comfortably under CTest TIMEOUT 90.
ClusterConfig snapshot_cfg() {
    ClusterConfig cfg;
    cfg.n_nodes = 3;
    cfg.n_clients = 2;
    cfg.submits_per_client = 16;  // 32 submits ≫ threshold(8) ⇒ real compaction
    cfg.partition_episodes = 2;
    cfg.crash_episodes = 3;       // crash/restart ⇒ a lagging follower needs install
    cfg.request_deadline = 250;   // bounded per-submit wait (keeps the sweep fast)
    cfg.full_envelope = true;
    return cfg;
}

// ---------------------------------------------------------------------------
// (b) RecoveredEqualsFull, applied to the OBSERVED run. For every snapshot and
// every pair of LIVE nodes, the reconstructed FULL logical logs (log() already
// surfaces snapshot.state ++ retained suffix) must agree on every commonly-
// committed index — i.e. a node caught up via InstallSnapshot holds the same
// committed state as the others. (This is the pointwise face of Snapshot.tla
// RecoveredEqualsFull: ReconstructUpTo(a, n) == ReconstructUpTo(b, n) for
// n <= min(commit) ⇒ both equal the full-log fold.) Returns "" or a witness.
std::string recovered_equals_full(const ObservedRun& run) {
    for (const ClusterSnapshot& snap : run.snapshots) {
        for (std::size_t a = 0; a < snap.nodes.size(); ++a) {
            const NodeSnapshot& na = snap.nodes[a];
            if (!na.live) {
                continue;
            }
            for (std::size_t b = a + 1; b < snap.nodes.size(); ++b) {
                const NodeSnapshot& nb = snap.nodes[b];
                if (!nb.live) {
                    continue;
                }
                const Index common = na.commit_index < nb.commit_index
                                         ? na.commit_index
                                         : nb.commit_index;
                for (Index i = 1; i <= common; ++i) {
                    if (i > na.log.size() || i > nb.log.size()) {
                        return "RECOVER GAP step=" + std::to_string(snap.step) +
                               " index=" + std::to_string(i) + " a_len=" +
                               std::to_string(na.log.size()) + " b_len=" +
                               std::to_string(nb.log.size());
                    }
                    if (!(na.log[i - 1] == nb.log[i - 1])) {
                        return "RECOVERED DIVERGE step=" + std::to_string(snap.step) +
                               " index=" + std::to_string(i) + " nodes={" +
                               std::to_string(na.node_id) + "," +
                               std::to_string(nb.node_id) + "} a=[t" +
                               std::to_string(na.log[i - 1].term) + ",\"" +
                               na.log[i - 1].value + "\"] b=[t" +
                               std::to_string(nb.log[i - 1].term) + ",\"" +
                               nb.log[i - 1].value + "\"]";
                    }
                }
            }
        }
    }
    return "";
}

// Compaction measurement over a run: did any node TakeSnapshot? Adopt an
// InstallSnapshot? What is the max snapshot_index (discarded prefix) and the max
// physical retained-log size ever seen?
struct CompactionStats {
    std::uint64_t total_taken = 0;
    std::uint64_t total_installed = 0;
    Index max_snapshot_index = 0;
    std::size_t max_physical_log = 0;
    Index max_logical_log = 0;
};

CompactionStats measure_compaction(const ObservedRun& run) {
    CompactionStats st;
    // Per-node final counters (monotone within a generation; sum the per-node max).
    std::vector<std::uint64_t> taken(run.n_nodes, 0), installed(run.n_nodes, 0);
    for (const ClusterSnapshot& snap : run.snapshots) {
        for (const NodeSnapshot& n : snap.nodes) {
            if (!n.live) {
                continue;
            }
            if (n.node_id < taken.size()) {
                if (n.snapshots_taken > taken[n.node_id]) {
                    taken[n.node_id] = n.snapshots_taken;
                }
                if (n.snapshots_installed > installed[n.node_id]) {
                    installed[n.node_id] = n.snapshots_installed;
                }
            }
            if (n.snapshot_index > st.max_snapshot_index) {
                st.max_snapshot_index = n.snapshot_index;
            }
            if (n.physical_log_size > st.max_physical_log) {
                st.max_physical_log = n.physical_log_size;
            }
            if (static_cast<Index>(n.log.size()) > st.max_logical_log) {
                st.max_logical_log = static_cast<Index>(n.log.size());
            }
        }
    }
    for (std::uint64_t t : taken) {
        st.total_taken += t;
    }
    for (std::uint64_t i : installed) {
        st.total_installed += i;
    }
    return st;
}

// Stable render including the snapshot measurements (the byte-repro surface).
std::string render_run(const ObservedRun& run) {
    std::string out;
    out += "seed=" + std::to_string(run.seed) +
           " snapshots=" + std::to_string(run.snapshots.size()) +
           " submits=" + std::to_string(run.submits.size()) + "\n";
    for (const ClusterSnapshot& snap : run.snapshots) {
        out += "S" + std::to_string(snap.step) + ":";
        for (const NodeSnapshot& n : snap.nodes) {
            out += " n" + std::to_string(n.node_id) + "{" + (n.live ? "L" : "D") +
                   ",t" + std::to_string(n.term) + ",ci" +
                   std::to_string(n.commit_index) + ",log" +
                   std::to_string(n.log.size()) + ",sb" +
                   std::to_string(n.snapshot_index) + ",ps" +
                   std::to_string(n.physical_log_size) + ",st" +
                   std::to_string(n.snapshots_taken) + ",si" +
                   std::to_string(n.snapshots_installed) + "}";
        }
        out += "\n";
    }
    out += "committed_log=" + std::to_string(run.committed_log.size()) + ":";
    for (const LogEntry& e : run.committed_log) {
        out += " [t" + std::to_string(e.term) + ",\"" + e.value + "\"]";
    }
    out += "\n";
    return out;
}

std::uint64_t sweep_seeds() {
    // This binary builds MANY clusters per seed (A + B main sweep, A + B
    // cross-check, plus the teeth sweep) under a snapshot-HEAVY workload (48
    // submits/run ≫ threshold ⇒ real compaction). Held to 10 in-gate so the whole
    // binary stays well under CTest TIMEOUT 90 even when ctest runs it CONCURRENTLY
    // with other heavy consensus tests and under the slowest sanitizer (TSan
    // instruments every memory access). Resource discipline: a prior run froze the
    // host; a 32-seed default timed out at 90s in the parallel ctest gate.
    // CONSENSUS_SNAPSHOT_SEEDS raises it for a bounded out-of-gate stress run,
    // hard-capped at 4096 (never unbounded).
    std::uint64_t k = 8;
    if (const char* env = std::getenv("CONSENSUS_SNAPSHOT_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0) {
            k = (v > 4096) ? 4096 : static_cast<std::uint64_t>(v);
        }
    }
    return k;
}

// ---------------------------------------------------------------------------
// One impl's snapshot conformance over the sweep.
// ---------------------------------------------------------------------------
struct ImplResult {
    std::size_t fired[5] = {0, 0, 0, 0, 0};
    std::size_t recover_violations = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad_witness;
    std::uint64_t total_taken = 0;
    std::uint64_t total_installed = 0;
    Index max_snapshot_index = 0;
    std::size_t max_physical_log = 0;
    Index max_logical_log = 0;
    std::size_t total_committed = 0;
    std::uint64_t seeds_compacted = 0;
    std::uint64_t seeds_installed = 0;
    std::uint64_t seeds_stalled = 0;  // driver step-backstop tripped (zero-vtime storm)
};

ImplResult run_impl_sweep(const char* name, const ConsensusNodeFactory& factory,
                          std::uint64_t base_seed, std::uint64_t kSeeds) {
    ImplResult R;
    const ClusterConfig cfg = snapshot_cfg();
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = base_seed + s;
        const ObservedRun run = run_cluster(seed, factory, cfg);

        // (e) five conformance checkers (the unchanged base-Raft harness).
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < 5; ++i) {
            if (!vs[i].verdict.ok) {
                ++R.fired[i];
                if (R.first_bad_witness.empty()) {
                    R.first_bad_seed = seed;
                    R.first_bad_witness =
                        std::string(ck_name(i)) + ": " + vs[i].verdict.witness;
                }
            }
        }

        // (b) RecoveredEqualsFull.
        const std::string rw = recovered_equals_full(run);
        if (!rw.empty()) {
            ++R.recover_violations;
            if (R.first_bad_witness.empty()) {
                R.first_bad_seed = seed;
                R.first_bad_witness = "RecoveredEqualsFull: " + rw;
            }
        }

        // (c) compaction measurement.
        const CompactionStats cs = measure_compaction(run);
        R.total_taken += cs.total_taken;
        R.total_installed += cs.total_installed;
        if (cs.max_snapshot_index > R.max_snapshot_index) {
            R.max_snapshot_index = cs.max_snapshot_index;
        }
        if (cs.max_physical_log > R.max_physical_log) {
            R.max_physical_log = cs.max_physical_log;
        }
        if (cs.max_logical_log > R.max_logical_log) {
            R.max_logical_log = cs.max_logical_log;
        }
        if (cs.total_taken > 0) {
            ++R.seeds_compacted;
        }
        if (cs.total_installed > 0) {
            ++R.seeds_installed;
        }
        if (run.progress_stalled) {
            ++R.seeds_stalled;
            if (R.first_bad_witness.empty()) {
                R.first_bad_seed = seed;
                R.first_bad_witness =
                    "ProgressStalled: driver step-backstop tripped (zero-virtual-"
                    "time message storm — InstallSnapshot livelock class)";
            }
        }
        R.total_committed += count_committed(run);
    }

    std::printf("  [%s] checkers Elect=%zu Match=%zu SMS=%zu LAO=%zu Lin=%zu  "
                "RecoveredEqualsFull-viol=%zu\n",
                name, R.fired[0], R.fired[1], R.fired[2], R.fired[3], R.fired[4],
                R.recover_violations);
    std::printf("  [%s] compaction: snapshots_taken=%llu installs=%llu  "
                "max_discarded_prefix=%llu  max_physical_log=%zu  "
                "max_logical_log=%llu\n",
                name, static_cast<unsigned long long>(R.total_taken),
                static_cast<unsigned long long>(R.total_installed),
                static_cast<unsigned long long>(R.max_snapshot_index),
                R.max_physical_log,
                static_cast<unsigned long long>(R.max_logical_log));
    std::printf("  [%s] seeds_compacted=%llu seeds_with_install=%llu  "
                "committed=%zu\n",
                name, static_cast<unsigned long long>(R.seeds_compacted),
                static_cast<unsigned long long>(R.seeds_installed),
                R.total_committed);
    if (!R.first_bad_witness.empty()) {
        std::fprintf(stderr, "  [%s] FIRST VIOLATION seed=0x%llX %s\n", name,
                     static_cast<unsigned long long>(R.first_bad_seed),
                     R.first_bad_witness.c_str());
    }
    return R;
}

void assert_impl_conformant(const char* name, const ImplResult& R,
                            std::uint64_t kSeeds) {
    std::string p = name;
    check(R.fired[0] == 0, (p + ": ElectionSafety holds with snapshotting").c_str());
    check(R.fired[1] == 0, (p + ": LogMatching holds with snapshotting").c_str());
    check(R.fired[2] == 0,
          (p + ": StateMachineSafety holds with snapshotting (no committed entry "
               "lost/diverged after compaction)")
              .c_str());
    check(R.fired[3] == 0, (p + ": LeaderAppendOnly holds with snapshotting").c_str());
    check(R.fired[4] == 0, (p + ": Linearizability holds with snapshotting").c_str());
    check(R.recover_violations == 0,
          (p + ": RecoveredEqualsFull (a node caught up via InstallSnapshot has the "
               "same committed state as the others)")
              .c_str());
    // (c) compaction is NON-VACUOUS and BOUNDED.
    check(R.total_taken > 0,
          (p + ": COMPACTION actually fired (snapshots were taken — not vacuous)")
              .c_str());
    check(R.max_snapshot_index > 0,
          (p + ": the log prefix was actually DISCARDED (snapshot_index advanced)")
              .c_str());
    check(R.total_installed > 0,
          (p + ": InstallSnapshot actually caught up a lagging follower (non-vacuous)")
              .c_str());
    // The PHYSICAL retained log is BOUNDED while the LOGICAL log grew strictly past
    // it — the measurable point of compaction. A regression that stops discarding
    // (physical == logical, unbounded) is caught here. The retained suffix is
    // bounded by ~threshold(8) + the in-flight tail to a quorum; we assert it stays
    // BELOW the logical max AND under a generous absolute cap (4× threshold) so an
    // accidental "never compact" never slips through as merely "logical happened to
    // be larger this seed".
    check(R.max_logical_log > static_cast<Index>(R.max_physical_log),
          (p + ": the LOGICAL log grew strictly past the PHYSICAL retained log — "
               "compaction bounds in-memory growth (measured)")
              .c_str());
    check(R.max_physical_log <= 4 * 8,
          (p + ": the physical retained log stays within a small absolute bound "
               "(compaction keeps the in-memory log bounded regardless of how far "
               "the logical log grows)")
              .c_str());
    check(R.total_committed > 0,
          (p + ": cluster makes PROGRESS under faults with snapshotting on").c_str());
    // DEFENSE-IN-DEPTH: no run hit the driver's bounded step backstop. A trip means
    // forward progress stalled at one virtual time (the InstallSnapshot livelock
    // class) — the run was cut short as a DETECTABLE failure rather than hanging.
    check(R.seeds_stalled == 0,
          (p + ": NO run tripped the step backstop (no zero-virtual-time message "
               "storm / InstallSnapshot livelock — progress always advanced vtime)")
              .c_str());
    (void)kSeeds;
}

// ---------------------------------------------------------------------------
// (f) A-vs-B cross-check still agrees with snapshotting on. We use the SOUND
// cross-impl predicate V-XCHECK (memory: backprop-crosscheck-term-equality) — NOT
// raw (term,value) committed-log equality, which is UNSOUND across two independent
// impls (the TERM a value first commits at is a per-impl election artifact; under
// concurrent clients the cross-client value interleaving legitimately differs).
// V-XCHECK: ∀ client c, the commands BOTH impls committed for c appear in the SAME
// relative order (no inversion), terms ignored, one-sided gaps allowed. A
// compaction bug that lost/reordered a committed command would surface as an
// inversion. (This is the same predicate consensus_crosscheck_test.cpp gates on.)
// ---------------------------------------------------------------------------
std::string client_key(const std::string& value) {
    const std::size_t p = value.find("_op");
    return p == std::string::npos ? value : value.substr(0, p);
}

std::vector<std::string> committed_keys(const ObservedRun& run) {
    std::vector<std::string> ks;
    ks.reserve(run.committed_log.size());
    for (const LogEntry& e : run.committed_log) {
        ks.push_back(client_key(e.value));
    }
    return ks;
}

// One-direction relative-order check: walking `a`, the position in `b` of each
// command also present in `b` must be strictly increasing. "" if consistent.
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
            continue;  // committed in a but not b: legitimate one-sided gap
        }
        if (pos <= last_pos) {
            return "INVERSION: \"" + last_key + "\" before \"" + x +
                   "\" in A but the reverse in B";
        }
        last_pos = pos;
        last_key = x;
    }
    return "";
}

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

void cross_check_a_vs_b(std::uint64_t kSeeds) {
    std::printf("CROSS-CHECK A vs B (snapshot-heavy workload; V-XCHECK per-client "
                "committed order):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    const ClusterConfig cfg = snapshot_cfg();
    std::uint64_t agree = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad;
    std::size_t total_common = 0;
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0x5A99'A000ULL + s;
        const ObservedRun ra = run_cluster(seed, fa, cfg);
        const ObservedRun rb = run_cluster(seed, fb, cfg);
        const std::string w = crosscheck_agree(ra, rb, cfg.n_clients);
        total_common += common_committed(ra, rb);
        if (w.empty()) {
            ++agree;
        } else if (first_bad.empty()) {
            first_bad_seed = seed;
            first_bad = w;
        }
    }
    std::printf("  seeds=%llu AGREE=%llu/%llu common_committed_cmds=%zu\n",
                static_cast<unsigned long long>(kSeeds),
                static_cast<unsigned long long>(agree),
                static_cast<unsigned long long>(kSeeds), total_common);
    if (!first_bad.empty()) {
        std::fprintf(stderr, "  CROSS-CHECK DIVERGE seed=0x%llX %s\n",
                     static_cast<unsigned long long>(first_bad_seed),
                     first_bad.c_str());
    }
    check(agree == kSeeds,
          "A vs B AGREE under the snapshot-heavy workload (V-XCHECK: no client's "
          "commands committed in contradictory orders — compaction preserved "
          "cross-impl agreement)");
    check(total_common > 0,
          "cross-check compared real shared committed commands (non-vacuous)");
}

// ---------------------------------------------------------------------------
// TEETH. A deliberately-WRONG snapshotting node: it wraps a real RaftNodeA but,
// once it has committed past a small index, REPORTS an empty committed prefix —
// modelling a TakeSnapshot that discards un-folded entries / an InstallSnapshot
// that adopts EMPTY state (Snapshot.check.md mutant #1 + #2). Concretely: log()
// drops the committed prefix below its (fake) snapshot point while commit_index()
// still claims it committed — so the reconstructed state at a committed index is
// WRONG. StateMachineSafety / RecoveredEqualsFull MUST fire.
// ---------------------------------------------------------------------------
class TeethEmptySnapshotNode final : public ConsensusNode {
public:
    TeethEmptySnapshotNode(const NodeDeps& d, const NodeConfig& c)
        : inner_(std::make_unique<lockstep::consensus::raft_a::RaftNodeA>(d, c)) {}

    [[nodiscard]] lockstep::consensus::SubmitResult submit(
        const std::string& v) override {
        return inner_->submit(v);
    }
    [[nodiscard]] Role role() const noexcept override { return inner_->role(); }
    [[nodiscard]] Term current_term() const noexcept override {
        return inner_->current_term();
    }
    // The BUG: once enough entries have committed, the FIRST few committed entries'
    // values are blanked — as if a snapshot folded the committed prefix into EMPTY
    // state (InstallSnapshot adopting empty state / a TakeSnapshot that dropped the
    // entries' content without recording it). The log keeps its LENGTH (so the wrong
    // compaction is observable at the SAME committed indices the real value belongs
    // to — and `commit_index <= log.size()` still holds, exercising the checker's
    // value-comparison path rather than an out-of-range index), but a committed
    // index now reconstructs the WRONG (empty) value — exactly the corruption
    // Snapshot.tla rules out (CompactionPreservesState / RecoveredEqualsFull).
    [[nodiscard]] std::span<const LogEntry> log() const noexcept override {
        const std::span<const LogEntry> full = inner_->log();
        const std::size_t corrupt = 4;  // pretend [1..4] were folded into the void
        if (full.size() > corrupt && inner_->commit_index() > corrupt) {
            view_.assign(full.begin(), full.end());
            for (std::size_t k = 0; k < corrupt; ++k) {
                view_[k].value.clear();  // committed value lost to empty snapshot state
            }
            return {view_.data(), view_.size()};
        }
        return full;
    }
    [[nodiscard]] Index commit_index() const noexcept override {
        return inner_->commit_index();
    }
    void start() override { inner_->start(); }
    void crash() override { inner_->crash(); }
    void restart() override { inner_->restart(); }
    [[nodiscard]] std::uint64_t id() const noexcept override { return inner_->id(); }

private:
    std::unique_ptr<lockstep::consensus::raft_a::RaftNodeA> inner_;
    mutable std::vector<LogEntry> view_;
};

void teeth_wrong_compaction_is_caught() {
    std::printf("TEETH (deliberately-wrong compaction — empty/dropped snapshot "
                "state MUST be caught):\n");
    ConsensusNodeFactory teeth =
        [](const NodeDeps& d, const NodeConfig& c)
        -> std::unique_ptr<ConsensusNode> {
        return std::make_unique<TeethEmptySnapshotNode>(d, c);
    };
    const ClusterConfig cfg = snapshot_cfg();
    bool caught = false;
    std::uint64_t caught_seed = 0;
    std::string witness;
    for (std::uint64_t s = 0; s < 16 && !caught; ++s) {
        const std::uint64_t seed = 0x7EE7'0000ULL + s;
        const ObservedRun run = run_cluster(seed, teeth, cfg);
        const std::vector<NamedVerdict> vs = run_all_conformance(run);
        for (std::size_t i = 0; i < 5; ++i) {
            if (!vs[i].verdict.ok) {
                caught = true;
                caught_seed = seed;
                witness = std::string(ck_name(i)) + ": " + vs[i].verdict.witness;
                break;
            }
        }
        if (!caught && !recovered_equals_full(run).empty()) {
            caught = true;
            caught_seed = seed;
            witness = "RecoveredEqualsFull: " + recovered_equals_full(run);
        }
    }
    if (caught) {
        std::printf("  CAUGHT seed=0x%llX %s\n",
                    static_cast<unsigned long long>(caught_seed), witness.c_str());
    }
    check(caught,
          "a deliberately-WRONG compaction (snapshot adopting empty/dropped state) "
          "is CAUGHT by StateMachineSafety / RecoveredEqualsFull (the gate has "
          "teeth)");
}

// ---------------------------------------------------------------------------
// (d) determinism — same seed ⇒ byte-identical rendered run (incl. snapshot
// measurements), for both impls.
// ---------------------------------------------------------------------------
std::string determinism_render(const ConsensusNodeFactory& f, std::uint64_t seed) {
    return render_run(run_cluster(seed, f, snapshot_cfg()));
}

void determinism() {
    std::printf("DETERMINISM (same seed ⇒ byte-identical snapshot run, A and B):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    const std::string a1 = determinism_render(fa, 0xD37E'A000ULL);
    const std::string a2 = determinism_render(fa, 0xD37E'A000ULL);
    const std::string b1 = determinism_render(fb, 0xD37E'B000ULL);
    const std::string b2 = determinism_render(fb, 0xD37E'B000ULL);
    check(a1 == a2, "impl A: same seed ⇒ byte-identical snapshot run");
    check(b1 == b2, "impl B: same seed ⇒ byte-identical snapshot run");
}

}  // namespace

int main() {
    std::printf("consensus_snapshot_test: Phase 4 C4.3 — LOG SNAPSHOTTING / "
                "COMPACTION (Snapshot.tla)\n");

    const std::uint64_t kSeeds = sweep_seeds();

    std::printf("SNAPSHOT CONFORMANCE SWEEP (full fault envelope; snapshot-heavy "
                "workload; both impls):\n");
    const ImplResult ra =
        run_impl_sweep("A", make_raft_a_factory(), 0x5A00'A000ULL, kSeeds);
    const ImplResult rb =
        run_impl_sweep("B", RaftNodeB::factory(), 0x5A00'B000ULL, kSeeds);
    assert_impl_conformant("A", ra, kSeeds);
    assert_impl_conformant("B", rb, kSeeds);

    cross_check_a_vs_b(kSeeds);
    teeth_wrong_compaction_is_caught();
    determinism();

    // External double-run diff surface (the gate runs this binary twice + diffs).
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(determinism_render(make_raft_a_factory(), 0xD37E'A000ULL).c_str(),
               stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_snapshot_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("consensus_snapshot_test: OK\n");
    return 0;
}
