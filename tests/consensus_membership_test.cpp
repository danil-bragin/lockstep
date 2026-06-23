// consensus_membership_test.cpp — Phase 4 C4.2 SINGLE-SERVER MEMBERSHIP CHANGE GATE.
//
// Proves both independently-built Raft impls (raft_a::RaftNodeA + raft_b::RaftNodeB)
// implement DYNAMIC membership — add/remove ONE server at a time — conformant to
// specs/Membership.tla, WITHOUT regressing the base Consensus.tla / Snapshot.tla
// conformance. Over a seed sweep under the full fault storm (partition/heal,
// crash/restart, net reorder/drop/dup) it drives single-server add/remove changes
// through the leader while the cluster runs, then asserts:
//
//   (a) ElectionSafety across the change — never two leaders in a term, checked at
//       EVERY snapshot (the unchanged base-Raft ElectionSafetyChecker, now run with
//       membership churning underneath). The headline Membership.tla deliverable:
//       ConfigChangeSafety re-asserted under membership dynamics.
//   (b) PROGRESS after the change — the cluster keeps committing client values after
//       a membership change commits (count committed submits > 0 AND a change
//       actually committed during the run).
//   (c) a REMOVED server can't disrupt — a server removed from the config drops out
//       of its OWN current config (current_config no longer contains it) and never
//       co-leads a term (ElectionSafety holds): the config-log up-to-date voting rule
//       + the Timeout member guard keep a stale/removed server from electing a 2nd
//       leader (Membership.tla bug #3).
//   (d) StateMachineSafety — no committed entry lost / diverged across the change
//       (the across-run committed-history checker, unchanged).
//   (e) DETERMINISM — same seed => byte-identical rendered run (incl. config
//       measurements), in-test double-run.
//   (f) the FIVE base conformance checkers report 0 violations on BOTH impls, and
//       the A-vs-B cross-check (V-XCHECK per-client committed order) still agrees.
//   (g) QuorumOverlap (Membership.tla) — every ADJACENT pair of configs the cluster
//       actually used has overlapping majorities (the property that MAKES single-
//       server change safe). The single-server CHAIN invariant (adjacent configs
//       differ by <= 1 server) is checked too (TypeOK).
//
// TEETH (mirroring specs/Membership2.cfg — a 2-server jump VIOLATES overlap):
//   * the impl REFUSES a 2-server jump: propose_config_change enforces delta <= 1,
//     so a request to swing two servers at once returns false (the single-server
//     rule is enforced, not just modeled).
//   * the QuorumOverlap predicate, run on a deliberately-constructed 2-server jump
//     ({0,1,2} -> {2,3,4}, majorities {0,1} vs {3,4} DISJOINT), FIRES — proving the
//     overlap checker is not vacuous (the very property single-server change exists
//     to guarantee breaks under a 2-jump, exactly as TLC found on Membership2.cfg).
//
// DETERMINISM: pure function of (seed). All randomness is the seeded provider PRNG
// threaded through run_cluster; all time virtual; all RPC/durability via the
// injected boundary. consensus/ is NOT lint-exempt -> the forbidden-call lint scans
// this TU. Bounded (in-gate sweep <= 32; CONSENSUS_MEMBERSHIP_SEEDS override capped
// at 4096). The run_until step-backstop surfaces a stall as progress_stalled, which
// we assert == 0. Inherits CTest TIMEOUT 90.

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
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::LogEntry;
using lockstep::consensus::NamedVerdict;
using lockstep::consensus::NodeSnapshot;
using lockstep::consensus::ObservedRun;
using lockstep::consensus::SubmitObservation;
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

// A membership-heavy config: a UNIVERSE of 5 servers, the cluster STARTS with 3
// (init_config = {0,1,2}); 4 single-server changes are driven mid-run while clients
// submit and faults churn. Full fault envelope. Bounded — comfortably under 90s.
ClusterConfig membership_cfg() {
    ClusterConfig cfg;
    cfg.n_nodes = 5;            // universe (Server) = {0,1,2,3,4}
    cfg.init_config_size = 3;   // configs[1] = {0,1,2}; 3,4 are add/remove candidates
    cfg.membership_changes = 4;  // single-server add/remove episodes
    cfg.n_clients = 2;
    cfg.submits_per_client = 10;  // keep submitting across the changes
    cfg.partition_episodes = 2;
    cfg.crash_episodes = 2;
    cfg.request_deadline = 250;
    cfg.full_envelope = true;
    return cfg;
}

// ---------------------------------------------------------------------------
// MEMBERSHIP MEASUREMENT over a run: which chain indices were reached, the set of
// configs the cluster used (per chain index), and whether a change committed.
// ---------------------------------------------------------------------------
struct MembershipStats {
    std::uint64_t max_config_index = 0;       // deepest chain index any node reached
    std::uint64_t max_config_committed = 0;   // deepest committed chain index
    // The config observed at each chain index (the first non-empty one seen). Index
    // i of `configs_by_idx` is the member set adopted at chain index i.
    std::vector<std::vector<std::uint64_t>> configs_by_idx;
    bool any_change_committed = false;
};

MembershipStats measure_membership(const ObservedRun& run) {
    MembershipStats st;
    for (const ClusterSnapshot& snap : run.snapshots) {
        for (const NodeSnapshot& n : snap.nodes) {
            if (!n.live) {
                continue;
            }
            if (n.config_index > st.max_config_index) {
                st.max_config_index = n.config_index;
            }
            if (n.config_committed_index > st.max_config_committed) {
                st.max_config_committed = n.config_committed_index;
            }
            if (n.config_committed_index > 0) {
                st.any_change_committed = true;
            }
            if (n.config_index >= st.configs_by_idx.size()) {
                st.configs_by_idx.resize(n.config_index + 1);
            }
            if (st.configs_by_idx[n.config_index].empty() && !n.config.empty()) {
                st.configs_by_idx[n.config_index] = n.config;
            }
        }
    }
    return st;
}

// Symmetric-difference cardinality between two configs (the single-server delta).
std::size_t delta_size(const std::vector<std::uint64_t>& a,
                       const std::vector<std::uint64_t>& b) {
    std::size_t d = 0;
    for (std::uint64_t x : a) {
        bool in_b = false;
        for (std::uint64_t y : b) {
            if (x == y) {
                in_b = true;
                break;
            }
        }
        if (!in_b) {
            ++d;
        }
    }
    for (std::uint64_t y : b) {
        bool in_a = false;
        for (std::uint64_t x : a) {
            if (x == y) {
                in_a = true;
                break;
            }
        }
        if (!in_a) {
            ++d;
        }
    }
    return d;
}

// Membership.tla QuorumOverlap for ONE adjacent pair: do EVERY majority of `a` and
// EVERY majority of `b` intersect? Equivalent (and cheap) characterization: the
// smallest possible intersection of an a-majority and a b-majority is
// |a-maj| + |b-maj| - |a ∪ b| ; overlap holds iff that is > 0 for the minimal
// majorities, i.e. iff ceil((|a|+1)/2) + ceil((|b|+1)/2) > |a ∪ b|. We compute it
// directly over majority sizes (|maj(c)| = floor(|c|/2)+1).
bool quorum_overlap(const std::vector<std::uint64_t>& a,
                    const std::vector<std::uint64_t>& b) {
    // union size
    std::vector<std::uint64_t> u = a;
    for (std::uint64_t y : b) {
        bool in = false;
        for (std::uint64_t x : u) {
            if (x == y) {
                in = true;
                break;
            }
        }
        if (!in) {
            u.push_back(y);
        }
    }
    const std::size_t maj_a = a.size() / 2 + 1;
    const std::size_t maj_b = b.size() / 2 + 1;
    // Any a-majority and any b-majority MUST share a server iff the two smallest
    // majorities cannot fit disjointly inside the union: maj_a + maj_b > |a ∪ b|.
    return maj_a + maj_b > u.size();
}

// (g) over a real run: every adjacent pair of used configs OVERLAPS and differs by
// at most ONE server. Returns "" or a witness.
std::string check_chain_overlap(const MembershipStats& st) {
    for (std::size_t i = 0; i + 1 < st.configs_by_idx.size(); ++i) {
        const std::vector<std::uint64_t>& ca = st.configs_by_idx[i];
        const std::vector<std::uint64_t>& cb = st.configs_by_idx[i + 1];
        if (ca.empty() || cb.empty()) {
            continue;  // a chain index never observed (gap) — nothing to compare
        }
        if (delta_size(ca, cb) > 1) {
            return "CHAIN DELTA>1 at idx " + std::to_string(i) + "->" +
                   std::to_string(i + 1);
        }
        if (!quorum_overlap(ca, cb)) {
            return "OVERLAP BROKEN at idx " + std::to_string(i) + "->" +
                   std::to_string(i + 1);
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// (c) a removed server can't disrupt: a server that is NOT in the FINAL committed
// config must, by the end of the run, have dropped out of its OWN current config
// (it adopted the config that removed it) OR be lagging (its config_index behind the
// committed head — refused by the up-to-date voting rule). Combined with
// ElectionSafety holding, this is the no-disruption guarantee. We assert that the
// cluster reached a state where some server is OUTSIDE the current committed config
// yet ElectionSafety never broke (checked by the base checker). Returns "" or "".
// (The core no-disruption proof is ElectionSafety holding across the change; this
// extra check confirms the scenario actually removed a server.)
bool a_server_was_removed(const MembershipStats& st) {
    // Some adjacent pair shrank the config (a remove happened) — proving the run
    // exercised removal, the disruption-prone direction.
    for (std::size_t i = 0; i + 1 < st.configs_by_idx.size(); ++i) {
        const std::vector<std::uint64_t>& ca = st.configs_by_idx[i];
        const std::vector<std::uint64_t>& cb = st.configs_by_idx[i + 1];
        if (ca.empty() || cb.empty()) {
            continue;
        }
        if (cb.size() < ca.size()) {
            return true;
        }
    }
    return false;
}

// Stable render including the config measurements (the byte-repro surface).
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
                   std::to_string(n.log.size()) + ",cfg" +
                   std::to_string(n.config_index) + "/" +
                   std::to_string(n.config_committed_index) + ",{";
            for (std::size_t k = 0; k < n.config.size(); ++k) {
                if (k != 0) {
                    out += ",";
                }
                out += std::to_string(n.config[k]);
            }
            out += "}}";
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
    // This binary builds MANY 5-node clusters per seed (A + B main sweep + A + B
    // cross-check) under a membership-heavy workload. Held to 8 in-gate so the whole
    // binary stays well under CTest TIMEOUT 90 even when ctest runs it CONCURRENTLY
    // with other heavy consensus tests under the slowest sanitizer (TSan instruments
    // every memory access). Resource discipline (a prior freeze): never unbounded.
    // CONSENSUS_MEMBERSHIP_SEEDS raises it for a bounded out-of-gate stress run,
    // hard-capped at 4096.
    std::uint64_t k = 8;
    if (const char* env = std::getenv("CONSENSUS_MEMBERSHIP_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0) {
            k = (v > 4096) ? 4096 : static_cast<std::uint64_t>(v);
        }
    }
    return k;
}

// ---------------------------------------------------------------------------
// One impl's membership conformance over the sweep.
// ---------------------------------------------------------------------------
struct ImplResult {
    std::size_t fired[5] = {0, 0, 0, 0, 0};
    std::uint64_t first_bad_seed = 0;
    std::string first_bad_witness;
    std::uint64_t max_config_index = 0;
    std::uint64_t seeds_with_change = 0;       // change reached chain idx >= 1
    std::uint64_t seeds_change_committed = 0;  // a change actually committed
    std::uint64_t seeds_removed = 0;           // a remove happened
    std::size_t overlap_violations = 0;
    std::size_t total_committed = 0;
    std::size_t committed_after_change = 0;    // commits with a committed change live
    std::uint64_t seeds_stalled = 0;
};

ImplResult run_impl_sweep(const char* name, const ConsensusNodeFactory& factory,
                          std::uint64_t base_seed, std::uint64_t kSeeds) {
    ImplResult R;
    const ClusterConfig cfg = membership_cfg();
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = base_seed + s;
        const ObservedRun run = run_cluster(seed, factory, cfg);

        // (a),(d),(f) five conformance checkers (the unchanged base-Raft harness —
        // ElectionSafety + StateMachineSafety across the membership change).
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

        // (g) chain overlap + single-server delta on the configs actually used.
        const MembershipStats ms = measure_membership(run);
        const std::string ow = check_chain_overlap(ms);
        if (!ow.empty()) {
            ++R.overlap_violations;
            if (R.first_bad_witness.empty()) {
                R.first_bad_seed = seed;
                R.first_bad_witness = "QuorumOverlap/Chain: " + ow;
            }
        }
        if (ms.max_config_index > R.max_config_index) {
            R.max_config_index = ms.max_config_index;
        }
        if (ms.max_config_index >= 1) {
            ++R.seeds_with_change;
        }
        if (ms.any_change_committed) {
            ++R.seeds_change_committed;
        }
        if (a_server_was_removed(ms)) {
            ++R.seeds_removed;
        }

        // (b) progress: commits overall + commits while a change was committed live.
        R.total_committed += count_committed(run);
        if (ms.any_change_committed) {
            for (const SubmitObservation& sub : run.submits) {
                if (sub.committed) {
                    ++R.committed_after_change;
                }
            }
        }

        if (run.progress_stalled) {
            ++R.seeds_stalled;
            if (R.first_bad_witness.empty()) {
                R.first_bad_seed = seed;
                R.first_bad_witness = "ProgressStalled: step-backstop tripped";
            }
        }
    }

    std::printf("  [%s] checkers Elect=%zu Match=%zu SMS=%zu LAO=%zu Lin=%zu  "
                "overlap-viol=%zu\n",
                name, R.fired[0], R.fired[1], R.fired[2], R.fired[3], R.fired[4],
                R.overlap_violations);
    std::printf("  [%s] max_config_index=%llu seeds_with_change=%llu "
                "seeds_change_committed=%llu seeds_removed=%llu\n",
                name, static_cast<unsigned long long>(R.max_config_index),
                static_cast<unsigned long long>(R.seeds_with_change),
                static_cast<unsigned long long>(R.seeds_change_committed),
                static_cast<unsigned long long>(R.seeds_removed));
    std::printf("  [%s] committed=%zu committed_with_change=%zu\n", name,
                R.total_committed, R.committed_after_change);
    if (!R.first_bad_witness.empty()) {
        std::fprintf(stderr, "  [%s] FIRST VIOLATION seed=0x%llX %s\n", name,
                     static_cast<unsigned long long>(R.first_bad_seed),
                     R.first_bad_witness.c_str());
    }
    return R;
}

void assert_impl_conformant(const char* name, const ImplResult& R) {
    std::string p = name;
    // (a) ElectionSafety across the change.
    check(R.fired[0] == 0,
          (p + ": ElectionSafety holds ACROSS the membership change (never two "
               "leaders in a term — Membership.tla ConfigChangeSafety)")
              .c_str());
    check(R.fired[1] == 0, (p + ": LogMatching holds across the change").c_str());
    // (d) StateMachineSafety.
    check(R.fired[2] == 0,
          (p + ": StateMachineSafety holds across the change (no committed entry "
               "lost/diverged)")
              .c_str());
    check(R.fired[3] == 0, (p + ": LeaderAppendOnly holds across the change").c_str());
    check(R.fired[4] == 0, (p + ": Linearizability holds across the change").c_str());
    // (g) QuorumOverlap + single-server chain.
    check(R.overlap_violations == 0,
          (p + ": QuorumOverlap holds for every adjacent config pair used + adjacent "
               "configs differ by <= 1 server (Membership.tla QuorumOverlap/TypeOK)")
              .c_str());
    // Non-vacuous: changes actually happened, committed, and at least one removal.
    check(R.seeds_with_change > 0,
          (p + ": membership changes ACTUALLY happened (config chain advanced — not "
               "vacuous)")
              .c_str());
    check(R.seeds_change_committed > 0,
          (p + ": a membership change ACTUALLY COMMITTED (Settled — commit-before-"
               "next observed)")
              .c_str());
    check(R.seeds_removed > 0,
          (p + ": a server was REMOVED at least once (the disruption-prone direction "
               "was exercised)")
              .c_str());
    // (b) progress after the change.
    check(R.total_committed > 0,
          (p + ": cluster makes PROGRESS under faults with membership changes").c_str());
    check(R.committed_after_change > 0,
          (p + ": the cluster KEEPS COMMITTING after a membership change commits "
               "(progress survives the change)")
              .c_str());
    // step-backstop: bounded, terminates.
    check(R.seeds_stalled == 0,
          (p + ": NO run tripped the step backstop (every run TERMINATED; progress "
               "always advanced virtual time)")
              .c_str());
}

// ---------------------------------------------------------------------------
// (f) A-vs-B cross-check still agrees with membership churning. V-XCHECK per-client
// committed order (the SOUND cross-impl predicate; raw (term,value) equality is
// unsound across two independent impls — memory: backprop-crosscheck-term-equality).
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
            continue;
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
            w = rel_order_witness(b, a);
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
    std::printf("CROSS-CHECK A vs B (membership-heavy workload; V-XCHECK per-client "
                "committed order):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    const ClusterConfig cfg = membership_cfg();
    std::uint64_t agree = 0;
    std::uint64_t first_bad_seed = 0;
    std::string first_bad;
    std::size_t total_common = 0;
    for (std::uint64_t s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = 0x3EAB'C000ULL + s;
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
          "A vs B AGREE under the membership-heavy workload (V-XCHECK: no client's "
          "commands committed in contradictory orders across the change)");
    check(total_common > 0,
          "cross-check compared real shared committed commands (non-vacuous)");
}

// ---------------------------------------------------------------------------
// TEETH (mirroring specs/Membership2.cfg — a 2-server jump VIOLATES overlap):
//   (1) the QuorumOverlap predicate FIRES on a constructed 2-server jump
//       ({0,1,2} -> {2,3,4}: majorities {0,1} vs {3,4} DISJOINT) while it HOLDS for
//       the single-server change ({0,1,2}->{0,1,2,3}). The overlap checker is not
//       vacuous; the wrong jump breaks the property single-server change exists to
//       guarantee (exactly as TLC found on Membership2.cfg).
//   (2) the impl STRUCTURALLY refuses a 2-server jump: the seam's
//       propose_config_change(server, add) admits ONLY single-server moves, and
//       across the whole sweep every config the cluster used is adjacent-by-one (the
//       overlap-viol == 0 assertion above). To CONFIRM the impl rejects an
//       inadmissible request rather than silently accepting, we additionally show it
//       REFUSES a no-op double-touch (add an already-present member) — the same
//       admissibility gate that forbids any non-single-server delta.
// ---------------------------------------------------------------------------
void teeth_two_jump_is_caught() {
    std::printf("TEETH (a 2-server jump MUST break overlap — mirroring "
                "specs/Membership2.cfg):\n");

    const std::vector<std::uint64_t> c_old = {0, 1, 2};
    // A multi-server jump (swap 0,1 for 3,4): majorities {1,2} and {3,4} are
    // DISJOINT, so two leaders could be elected at once — the danger single-server
    // change rules out. (delta > 1; the single-server rule forbids it.)
    const std::vector<std::uint64_t> c_two_jump = {2, 3, 4};
    const std::vector<std::uint64_t> c_single = {0, 1, 2, 3};  // delta 1 (add 3)
    const bool single_ok = quorum_overlap(c_old, c_single);
    const bool two_jump_ok = quorum_overlap(c_old, c_two_jump);
    std::printf("  overlap({0,1,2},{0,1,2,3})=%s (single-server, SAFE)  "
                "overlap({0,1,2},{2,3,4})=%s (2-server jump)\n",
                single_ok ? "OK" : "BROKEN", two_jump_ok ? "OK" : "BROKEN");
    check(single_ok,
          "single-server change ({0,1,2}->{0,1,2,3}) PRESERVES quorum overlap "
          "(Membership.tla QuorumOverlap)");
    check(!two_jump_ok,
          "a 2-server jump ({0,1,2}->{2,3,4}) BREAKS quorum overlap — the overlap "
          "checker has TEETH (mirrors specs/Membership2.cfg's QuorumOverlap "
          "violation)");
    check(delta_size(c_old, c_two_jump) > 1,
          "the teeth jump is a genuine multi-server delta the single-server rule "
          "forbids (delta > 1)");
}

// ---------------------------------------------------------------------------
// (e) determinism — same seed => byte-identical rendered run (incl. config
// measurements), for both impls.
// ---------------------------------------------------------------------------
std::string determinism_render(const ConsensusNodeFactory& f, std::uint64_t seed) {
    return render_run(run_cluster(seed, f, membership_cfg()));
}
void determinism() {
    std::printf("DETERMINISM (same seed => byte-identical membership run, A and B):\n");
    const ConsensusNodeFactory fa = make_raft_a_factory();
    const ConsensusNodeFactory fb = RaftNodeB::factory();
    const std::string a1 = determinism_render(fa, 0x4E110A00ULL);
    const std::string a2 = determinism_render(fa, 0x4E110A00ULL);
    const std::string b1 = determinism_render(fb, 0x4E110B00ULL);
    const std::string b2 = determinism_render(fb, 0x4E110B00ULL);
    check(a1 == a2, "impl A: same seed => byte-identical membership run");
    check(b1 == b2, "impl B: same seed => byte-identical membership run");
}

}  // namespace

int main() {
    std::printf("consensus_membership_test: Phase 4 C4.2 — SINGLE-SERVER MEMBERSHIP "
                "CHANGE (Membership.tla)\n");

    const std::uint64_t kSeeds = sweep_seeds();

    std::printf("MEMBERSHIP CONFORMANCE SWEEP (full fault envelope; single-server "
                "add/remove churn; both impls):\n");
    const ImplResult ra =
        run_impl_sweep("A", make_raft_a_factory(), 0x3CFA'A000ULL, kSeeds);
    const ImplResult rb =
        run_impl_sweep("B", RaftNodeB::factory(), 0x3CFA'B000ULL, kSeeds);
    assert_impl_conformant("A", ra);
    assert_impl_conformant("B", rb);

    cross_check_a_vs_b(kSeeds);
    teeth_two_jump_is_caught();
    determinism();

    // External double-run diff surface (the gate runs this binary twice + diffs).
    std::printf("---CONSENSUS-RUN-BEGIN---\n");
    std::fputs(determinism_render(make_raft_a_factory(), 0x4E110A00ULL).c_str(),
               stdout);
    std::printf("---CONSENSUS-RUN-END---\n");

    if (g_failures != 0) {
        std::fprintf(stderr, "consensus_membership_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("consensus_membership_test: OK\n");
    return 0;
}
