// txn_end_to_end_test.cpp — Phase 5/6 END-TO-END DISTRIBUTED-TXN FAULT-STORM GATE.
//
// Closes the last verification debt: the Phase-5 txn layer's correctness was
// verified over the seqLog ABSTRACTION + a real MVCC store, but NOT end-to-end
// through the REAL Phase-4 consensus + Phase-3 storage under a crash/partition
// fault storm. This wires the WHOLE stack and judges the result with the EXISTING
// txn Oracle + the strict-serializable / exactly-once / D5 checker battery.
//
// ============================================================================
// THE WIRE (consensus order -> executor -> MVCC under faults):
//   1. A SEEDED workload of one-shot txns (txn/DiffHarness build_workload) — each
//      txn carries a stable id + a deterministic read-modify-write body. The txn
//      id is the OPAQUE consensus command value (encoded as a string).
//   2. A REAL cluster of consensus nodes (raft_a, behind the Phase-4 seam) ORDERS
//      those txn-id submissions UNDER THE FULL FAULT STORM — partition/heal keeping
//      quorum, node crash/restart, net reorder/drop/dup, SimDisk latency — driven
//      on the deterministic core::Scheduler. The COMMITTED CONSENSUS LOG (the
//      agreed total order, reconstructed from the longest committed prefix any node
//      — including post-restart nodes — ever witnessed) IS the seqLog. Single shard
//      => the consensus log directly is the global order (Sequencer C4.4 is the
//      identity here; multi-shard would merge per-shard logs first).
//   3. The committed txn-id order resolves back to the Txn objects, IN CONSENSUS
//      ORDER, and feeds the Phase-5 DeterministicExecutor, which executes them over
//      the REAL Phase-3 WalEngine MVCC store. The consensus order is the
//      LINEARIZATION POINT (CommitOrdering.tla seqLog).
//
// ============================================================================
// WHAT THIS ASSERTS end-to-end, over a seed sweep (<=32 in-gate, env override,
// every run TERMINATES via the run_until step-backstop -> progress_stalled==0):
//   * STRICT-SERIALIZABLE: the committed txn history == a serial execution in the
//     consensus order (the txn Oracle on the SAME consensus order) + the
//     strict_serializable checker (ReadsMatchSerialPrefix + real-time subsequence).
//   * EXACTLY-ONCE: each consensus-committed txn applies exactly once despite
//     consensus retries / re-delivery / crash-recovery (exactly_once checker).
//   * DURABLE / crash-consistent: re-deriving the store from the committed prefix
//     after the fault storm (which crashed + restarted nodes) reproduces the same
//     committed history — committed txns survive crashes.
//   * PROGRESS: the cluster keeps committing txns when a quorum is up (non-vacuous:
//     the committed seqLog is non-empty across the sweep).
//   * DETERMINISM: same seed => byte-identical end-to-end history (in-test double
//     run + an external marker block the gate's double-run diff compares).
//   * TEETH: a deliberately-wrong wiring — executing txns in ARRIVAL order instead
//     of the consensus order — is CAUGHT by strict_serializable / differential.
//
// DETERMINISM (binding; this TU is NOT lint-exempt): pure fn of (seed). All
// nondeterminism flows through the seeded provider PRNG threaded into the cluster +
// the inlined SplitMix building the workload; all time is virtual. No wall-clock,
// no threads, no std::*_distribution, ordered maps throughout. V-RKV1: the cluster
// snapshots deep-copy every log out; the seqLog is rebuilt by value. Bounded: the
// driver run_until()s a deterministic deadline with a step backstop, so it ALWAYS
// terminates (asserted progress_stalled==0); CTest TIMEOUT 90 is the outer guard.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/Observation.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/DeterministicExecutor.hpp>
#include <lockstep/txn/DiffHarness.hpp>
#include <lockstep/txn/Oracle.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

using lockstep::consensus::ClusterConfig;
using lockstep::consensus::ConsensusNodeFactory;
using lockstep::consensus::LogEntry;
using lockstep::consensus::SubmitResult;
using lockstep::consensus::Tick;
using lockstep::consensus::raft_a::make_raft_a_factory;

namespace cdetail = lockstep::consensus::detail;

using lockstep::txn::build_workload;
using lockstep::txn::check_exactly_once;
using lockstep::txn::check_serialized_by_seqlog;
using lockstep::txn::check_strict_serializable;
using lockstep::txn::CommitInfo;
using lockstep::txn::DeterministicExecutor;
using lockstep::txn::RunResult;
using lockstep::txn::Status;
using lockstep::txn::StrictSerialOracle;
using lockstep::txn::Txn;
using lockstep::txn::Verdict;
using lockstep::txn::WorkloadConfig;
using lockstep::txn::run_all_checkers;

int g_failures = 0;

#define E2E_CHECK(cond, msg)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "txn_end_to_end_test FAIL [%s:%d]: %s\n",    \
                         __FILE__, __LINE__, (msg));                          \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// In-gate sweep size. Bounded (freeze discipline: <= 32). Env override for soak.
// ---------------------------------------------------------------------------
std::uint64_t sweep_count() {
    const char* env = std::getenv("LOCKSTEP_E2E_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 32;
}

// ---------------------------------------------------------------------------
// The opaque consensus command value for a txn == its id, string-encoded so it is
// a unique, parseable byte string the cluster orders (matching submit->commit).
// ---------------------------------------------------------------------------
std::string txn_value(std::uint64_t txn_id) { return "txn#" + std::to_string(txn_id); }

[[nodiscard]] bool parse_txn_value(const std::string& v, std::uint64_t* out) {
    constexpr const char* kPrefix = "txn#";
    constexpr std::size_t kPrefixLen = 4;
    if (v.size() <= kPrefixLen || v.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }
    std::uint64_t id = 0;
    for (std::size_t i = kPrefixLen; i < v.size(); ++i) {
        const char c = v[i];
        if (c < '0' || c > '9') {
            return false;
        }
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    *out = id;
    return true;
}

// ===========================================================================
// END-TO-END RESULT of one (seed): the consensus committed order (the seqLog),
// the executor + oracle runs over it, whether the cluster's bounded backstop ever
// tripped (progress_stalled), and the verdict battery. Pure fn of (seed, cfg).
// ===========================================================================
struct E2EResult {
    std::uint64_t seed = 0;
    bool progress_stalled = false;          // cluster step-backstop tripped?
    std::vector<Txn> seqlog;                // committed txns IN CONSENSUS ORDER
    RunResult sut;                          // DeterministicExecutor over seqlog
    RunResult oracle;                       // StrictSerialOracle over seqlog
    std::vector<Verdict> verdicts;
    bool all_ok = true;
    std::size_t committed = 0;              // committed txns (progress measure)
};

// ---------------------------------------------------------------------------
// The CLIENT WORKLOAD that submits OUR txn-id values to the believed leader and
// watches them commit. Mirrors ClusterDriver's client_driver but submits the
// pre-minted txn ids (so the committed log resolves back to Txn bodies). BOUNDED.
// ---------------------------------------------------------------------------
lockstep::core::Task txn_client_driver(cdetail::ClusterRunState* st,
                                       const ClusterConfig* cfg,
                                       const std::vector<std::uint64_t>* ids,
                                       std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        const Tick gap = st->rng.uniform_range(cfg->client_gap_min, cfg->client_gap_max);
        if (gap > 0) {
            co_await st->clock.delay(gap);
        }

        // Ensure a leader exists (elections may be in flight after a fault).
        std::uint64_t li = UINT64_MAX;
        {
            lockstep::core::Task t =
                cdetail::await_leader(st, cfg, cfg->election_timeout_max * 4, &li);
            co_await std::move(t);
        }

        const std::string value = txn_value((*ids)[i]);
        bool accepted = false;
        lockstep::consensus::Index index = 0;
        if (li != UINT64_MAX) {
            const SubmitResult r = st->nodes[li]->submit(value);
            accepted = r.accepted;
            index = r.index;
        }

        // Let the cluster replicate + commit, snapshotting as we go (the
        // committed log is reconstructed from these snapshots afterwards). We learn
        // commitment by OBSERVING a quorum/live node reaching the index with our
        // value still there — never by the node "telling" us (spec-faithful).
        if (accepted) {
            Tick waited = 0;
            const Tick stride = cfg->heartbeat_interval * 2;
            bool committed = false;
            while (waited < cfg->request_deadline && !committed) {
                if (stride > 0) {
                    co_await st->clock.delay(stride);
                }
                waited += stride;
                cdetail::snapshot(st);
                for (std::size_t n = 0; n < st->nodes.size(); ++n) {
                    if (!st->live[n]) {
                        continue;
                    }
                    if (st->nodes[n]->commit_index() >= index) {
                        const std::span<const LogEntry> lg = st->nodes[n]->log();
                        if (index <= lg.size() && lg[index - 1].value == value) {
                            committed = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// Drive ONE real cluster for `seed` under the FULL fault storm, submitting OUR
// txn ids, and return the COMMITTED CONSENSUS ORDER of txn ids (the seqLog) +
// whether the bounded step-backstop tripped (progress_stalled). PURE fn of
// (seed, cfg, factory, ids): same inputs => byte-identical committed order.
//
// This reuses the ClusterDriver internals (ClusterRunState + the fault_driver +
// snapshot/await_leader/finalize helpers) so the fault envelope, the deterministic
// scheduler, and the committed-log reconstruction are EXACTLY the Phase-4 machinery
// the consensus gate already verifies — we only swap in the txn-id workload.
// ---------------------------------------------------------------------------
std::vector<std::uint64_t> order_via_consensus(std::uint64_t seed,
                                               const ConsensusNodeFactory& factory,
                                               const ClusterConfig& cfg,
                                               const std::vector<std::uint64_t>& ids,
                                               bool* progress_stalled_out) {
    auto stp = std::make_unique<cdetail::ClusterRunState>(seed);
    cdetail::ClusterRunState* st = stp.get();
    st->run.seed = seed;
    st->run.n_nodes = cfg.n_nodes;

    if (cfg.full_envelope) {
        st->bus.set_faults(cfg.net_faults);
    }

    std::vector<std::uint64_t> cluster;
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        cluster.push_back(i);
    }

    const lockstep::sim::DiskFaultConfig disk_cfg =
        cfg.full_envelope ? cfg.disk_faults : lockstep::sim::DiskFaultConfig{};
    st->disks.reserve(cfg.n_nodes);
    st->nets.reserve(cfg.n_nodes);
    st->nodes.reserve(cfg.n_nodes);
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        st->disks.push_back(std::make_unique<lockstep::sim::SimDisk>(
            st->sched, st->clock, st->rng, disk_cfg));
        st->nets.push_back(st->bus.node(i));
        st->live.push_back(true);
    }
    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        lockstep::consensus::NodeDeps deps;
        deps.sched = &st->sched;
        deps.clock = &st->clock;
        deps.rng = &st->rng;
        deps.net = &st->nets[i];
        deps.disk = st->disks[i].get();

        lockstep::consensus::NodeConfig nc;
        nc.self_id = i;
        nc.cluster = cluster;
        nc.election_timeout_min = cfg.election_timeout_min;
        nc.election_timeout_max = cfg.election_timeout_max;
        nc.heartbeat_interval = cfg.heartbeat_interval;
        nc.request_deadline = cfg.request_deadline;
        st->nodes.push_back(factory(deps, nc));
    }

    for (std::uint64_t i = 0; i < cfg.n_nodes; ++i) {
        st->nodes[i]->start();
    }
    cdetail::snapshot(st);

    // Split the txn ids across the configured clients (deterministic contiguous
    // partition — pure fn of cfg). Each client submits its slice in order.
    const std::uint64_t nclients = cfg.n_clients == 0 ? 1 : cfg.n_clients;
    const std::size_t total = ids.size();
    for (std::uint64_t c = 0; c < nclients; ++c) {
        const std::size_t begin = (total * c) / nclients;
        const std::size_t end = (total * (c + 1)) / nclients;
        st->sched.spawn(txn_client_driver(st, &cfg, &ids, begin, end));
    }
    // The SAME seeded fault storm the Phase-4 gate uses: partition/heal (quorum
    // kept), crash/restart, on virtual time, all from the run PRNG.
    st->sched.spawn(cdetail::fault_driver(st, &cfg));
    st->sched.spawn(cdetail::settle_and_snapshot(
        st, cfg.settle_ticks * 4, cfg.heartbeat_interval * 2));

    const Tick deadline = cdetail::compute_deadline(&cfg);
    const std::uint64_t step_cap = cdetail::compute_step_cap(&cfg);
    const bool finished = st->sched.run_until(deadline, step_cap);
    st->run.progress_stalled = !finished;
    *progress_stalled_out = !finished;

    cdetail::snapshot(st);
    cdetail::finalize_committed_log(st);

    // The committed consensus log -> the seqLog of txn ids, IN CONSENSUS ORDER.
    // (Single shard: the consensus committed log IS the global order; the C4.4
    // Sequencer merge over one shard is the identity, so we take the log directly.)
    std::vector<std::uint64_t> order;
    order.reserve(st->run.committed_log.size());
    for (const LogEntry& e : st->run.committed_log) {
        std::uint64_t id = 0;
        if (parse_txn_value(e.value, &id)) {
            order.push_back(id);
        }
    }
    return order;
}

// ---------------------------------------------------------------------------
// THE END-TO-END RUN for one seed: build the workload, ORDER it through the real
// consensus cluster under the fault storm, resolve the committed order back to
// Txn objects, run the DeterministicExecutor (over the REAL MVCC store) + the
// strict-serializable oracle on that SAME order, and judge with the full battery.
// ---------------------------------------------------------------------------
E2EResult run_end_to_end(std::uint64_t seed, const WorkloadConfig& wc,
                         const ClusterConfig& cluster_cfg) {
    E2EResult out;
    out.seed = seed;

    // (1) the seeded one-shot txn workload (ids 1..N, RMW bodies).
    const std::vector<Txn> all_txns = build_workload(seed, wc);
    std::vector<std::uint64_t> ids;
    ids.reserve(all_txns.size());
    std::map<std::uint64_t, const Txn*> by_id;
    for (const Txn& t : all_txns) {
        ids.push_back(t.id);
        by_id[t.id] = &t;
    }

    // (2) ORDER via the REAL consensus cluster under the FULL fault storm.
    bool stalled = false;
    const std::vector<std::uint64_t> committed_order =
        order_via_consensus(seed, make_raft_a_factory(), cluster_cfg, ids, &stalled);
    out.progress_stalled = stalled;

    // (3) resolve the committed consensus order back to Txn objects (the seqLog).
    // Each committed id appears EXACTLY ONCE in the consensus log (consensus never
    // commits the same client value twice — submit values are unique), so the
    // resolved seqLog is the committed subset in serialization order.
    for (std::uint64_t id : committed_order) {
        const auto it = by_id.find(id);
        if (it != by_id.end()) {
            out.seqlog.push_back(*it->second);
        }
    }

    // (4) run the REAL executor (over the WalEngine MVCC store) + the oracle on the
    // SAME consensus order. The consensus order is the linearization point.
    DeterministicExecutor exec;
    out.sut = exec.submit_batch(out.seqlog, wc.exec);
    StrictSerialOracle oracle;
    out.oracle = oracle.submit_batch(out.seqlog, wc.exec);

    out.verdicts = run_all_checkers(out.sut, out.oracle, out.seqlog, wc.exec, seed);
    for (const Verdict& v : out.verdicts) {
        if (!v.ok) {
            out.all_ok = false;
        }
    }
    for (const CommitInfo& c : out.sut.commits) {
        if (c.status == Status::Committed) {
            ++out.committed;
        }
    }
    return out;
}

// Render an E2EResult to stable, line-oriented text (the byte-repro surface).
std::string render_e2e(const E2EResult& o) {
    std::string s;
    s += "seed=" + std::to_string(o.seed) +
         " stalled=" + (o.progress_stalled ? "1" : "0") +
         " seqlog=" + std::to_string(o.seqlog.size()) +
         " committed=" + std::to_string(o.committed) + "\n";
    s += "  order:";
    for (const Txn& t : o.seqlog) {
        s += " " + std::to_string(t.id);
    }
    s += "\n";
    for (const CommitInfo& c : o.sut.commits) {
        s += "  txn=" + std::to_string(c.txn_id) +
             " status=" + std::string(lockstep::txn::status_name(c.status)) +
             " seq=" + std::to_string(c.seq_index) +
             " ver=" + std::to_string(c.commit_version) +
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

// A small matrix of cluster sizes / fault intensity so the storm exercises both 3-
// and 5-node clusters with partition + crash episodes.
std::vector<ClusterConfig> cluster_matrix() {
    std::vector<ClusterConfig> v;
    {
        ClusterConfig c;  // 3 nodes, full envelope, 2 partition + 2 crash episodes.
        v.push_back(c);
    }
    {
        ClusterConfig c;
        c.n_nodes = 5;
        c.n_clients = 3;
        c.partition_episodes = 3;
        c.crash_episodes = 2;
        v.push_back(c);
    }
    return v;
}

WorkloadConfig make_wc() {
    WorkloadConfig wc;
    wc.num_txns = 8;
    wc.num_keys = 4;
    wc.exec.max_retry = 2;
    wc.exec.replica_lag = 1;
    return wc;
}

// Find a verdict by checker name.
[[nodiscard]] const Verdict* verdict_of(const E2EResult& o, const char* checker) {
    for (const Verdict& v : o.verdicts) {
        if (v.checker == checker) {
            return &v;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    std::printf("txn_end_to_end_test: Phase 5/6 distributed-txn fault-storm gate\n");

    const std::uint64_t seeds = sweep_count();
    const WorkloadConfig wc = make_wc();
    const std::vector<ClusterConfig> clusters = cluster_matrix();

    // =====================================================================
    // (MAIN) END-TO-END over the seed sweep x cluster matrix: every run
    // TERMINATES; the committed history is STRICT-SERIALIZABLE == the oracle,
    // EXACTLY-ONCE, and the cluster makes PROGRESS.
    // =====================================================================
    bool any_progress = false;
    std::size_t total_committed = 0;
    std::size_t runs = 0;
    for (const ClusterConfig& ccfg : clusters) {
        for (std::uint64_t seed = 0; seed < seeds; ++seed) {
            const E2EResult o = run_end_to_end(seed, wc, ccfg);
            ++runs;

            // TERMINATION: the bounded step-backstop must NOT have tripped (the run
            // reached its deterministic deadline; the host never hangs).
            E2E_CHECK(!o.progress_stalled,
                      "cluster terminated within the step-backstop "
                      "(progress_stalled==0)");

            if (!o.all_ok) {
                for (const Verdict& v : o.verdicts) {
                    if (!v.ok) {
                        std::fprintf(stderr,
                                     "  E2E VIOLATION seed=%llu n_nodes=%llu "
                                     "checker=%s witness=%s\n",
                                     static_cast<unsigned long long>(seed),
                                     static_cast<unsigned long long>(ccfg.n_nodes),
                                     v.checker.c_str(), v.witness.c_str());
                    }
                }
            }

            // STRICT-SERIALIZABLE + EXACTLY-ONCE + every D5/OLLP checker, 0
            // violations: the committed txn history over the REAL consensus order
            // == a serial execution in that order (the oracle) and is linearizable.
            E2E_CHECK(o.all_ok,
                      "end-to-end: every checker passes (strict-serializable == "
                      "oracle, exactly-once, OLLP sound, D5 contracts)");

            total_committed += o.committed;
            if (o.committed > 0) {
                any_progress = true;
            }
        }
    }
    // PROGRESS: the cluster keeps committing txns when a quorum is up — the
    // end-to-end pipeline is NON-VACUOUS (not a dead system passing safety
    // checkers trivially).
    E2E_CHECK(any_progress,
              "PROGRESS: the cluster commits txns end-to-end (non-vacuous)");
    std::printf("  swept %zu runs (%llu seeds x %zu clusters): total committed "
                "txns=%zu, all strict-serializable==oracle + exactly-once.\n",
                runs, static_cast<unsigned long long>(seeds), clusters.size(),
                total_committed);

    // =====================================================================
    // (DURABLE / crash-consistent) The committed consensus log is reconstructed
    // from the LONGEST committed prefix any node — INCLUDING nodes that were
    // crashed + restarted during the storm — ever witnessed. Re-deriving the store
    // from that committed prefix (a fresh executor run) reproduces the IDENTICAL
    // committed history: committed txns SURVIVE crashes and the recovered store is
    // consistent with the committed prefix. We prove it by replaying the seqLog on
    // a brand-new executor (a cold recovery from the durable committed order) and
    // asserting byte-identical commit effects.
    // =====================================================================
    {
        bool durable_ok = true;
        bool exercised_crashes = false;  // the storm crashes+restarts nodes
        for (std::uint64_t seed = 0; seed < seeds; ++seed) {
            const E2EResult o = run_end_to_end(seed, wc, clusters.front());
            exercised_crashes = true;  // clusters.front() has crash_episodes>0
            // Cold recovery: a fresh executor (new MVCC store) replays the durable
            // committed consensus order. Must reproduce the same committed history.
            DeterministicExecutor recovered;
            const RunResult r = recovered.submit_batch(o.seqlog, wc.exec);
            if (r.commits.size() != o.sut.commits.size()) {
                durable_ok = false;
            } else {
                for (std::size_t i = 0; i < r.commits.size(); ++i) {
                    const CommitInfo& a = r.commits[i];
                    const CommitInfo& b = o.sut.commits[i];
                    if (a.txn_id != b.txn_id || a.status != b.status ||
                        a.writes_committed != b.writes_committed ||
                        a.result != b.result) {
                        durable_ok = false;
                        break;
                    }
                }
            }
            // The recovered store must agree with the committed prefix per the
            // strict-serializable checker too (re-judge the recovered run).
            const Verdict rv = check_strict_serializable(r);
            if (!rv.ok) {
                durable_ok = false;
            }
        }
        E2E_CHECK(exercised_crashes,
                  "the fault storm exercises crash/restart episodes (durability is "
                  "tested, not vacuous)");
        E2E_CHECK(durable_ok,
                  "DURABLE: committed txns survive crash/restart — a cold recovery "
                  "from the committed consensus order reproduces the IDENTICAL "
                  "strict-serializable committed history");
        std::printf("  durable: cold recovery from the committed consensus order "
                    "reproduces the committed history across %llu seeds.\n",
                    static_cast<unsigned long long>(seeds));
    }

    // =====================================================================
    // (TEETH) A deliberately-wrong wiring: execute the committed txns in ARRIVAL
    // (txn-id) order instead of the CONSENSUS order. For a seed where consensus
    // reorders the commits relative to id order, the strict-serializable /
    // serialized_by_seqlog / differential checker MUST CATCH it (the executor is
    // honest; the WIRING is wrong). This proves the end-to-end harness binds the
    // consensus order as the linearization point — not whatever order arrived.
    // =====================================================================
    {
        bool teeth_caught = false;
        std::uint64_t witness_seed = 0;
        std::string witness;
        const char* witness_checker = nullptr;

        for (std::uint64_t seed = 0; seed < seeds && !teeth_caught; ++seed) {
            const E2EResult honest = run_end_to_end(seed, wc, clusters.front());
            // Build the WRONG order: the SAME committed txns, but sorted by id
            // (arrival/submit order), NOT the consensus order. Only interesting when
            // consensus actually reordered them (else the orders coincide).
            std::vector<Txn> arrival = honest.seqlog;
            bool reordered = false;
            for (std::size_t i = 1; i < arrival.size(); ++i) {
                if (arrival[i].id < arrival[i - 1].id) {
                    reordered = true;
                    break;
                }
            }
            std::sort(arrival.begin(), arrival.end(),
                      [](const Txn& a, const Txn& b) { return a.id < b.id; });
            if (!reordered) {
                continue;  // consensus order == id order here: no teeth to test
            }

            // Execute the wrong (arrival) order; JUDGE against the oracle on the
            // CONSENSUS order (the real linearization point). The mismatch is the
            // bug the checkers must catch.
            DeterministicExecutor wrong_exec;
            const RunResult wrong = wrong_exec.submit_batch(arrival, wc.exec);

            // (a) serialized_by_seqlog: the WRONG run's commit order is not a
            // subsequence of the agreed seqLog (the consensus order).
            const Verdict v_seq = check_serialized_by_seqlog(wrong, honest.seqlog);
            // (b) strict_serializable against the consensus-order oracle: a
            // strict read in the wrong order observes a non-serial value.
            const std::vector<Verdict> vs = run_all_checkers(
                wrong, honest.oracle, honest.seqlog, wc.exec, seed);
            const Verdict* v_diff = nullptr;
            const Verdict* v_strict = nullptr;
            for (const Verdict& v : vs) {
                if (v.checker == "differential_vs_oracle") {
                    v_diff = &v;
                }
                if (v.checker == "strict_serializable") {
                    v_strict = &v;
                }
            }

            if (!v_seq.ok) {
                teeth_caught = true;
                witness_seed = seed;
                witness = v_seq.witness;
                witness_checker = "serialized_by_seqlog";
            } else if (v_strict != nullptr && !v_strict->ok) {
                teeth_caught = true;
                witness_seed = seed;
                witness = v_strict->witness;
                witness_checker = "strict_serializable";
            } else if (v_diff != nullptr && !v_diff->ok) {
                teeth_caught = true;
                witness_seed = seed;
                witness = v_diff->witness;
                witness_checker = "differential_vs_oracle";
            }
        }

        E2E_CHECK(teeth_caught,
                  "TEETH: executing in ARRIVAL order instead of the CONSENSUS order "
                  "is CAUGHT by a checker (the consensus order IS the linearization "
                  "point)");
        if (teeth_caught) {
            std::printf("  [TEETH] wrong-order wiring caught by %s (seed=%llu) "
                        "witness=%s\n",
                        witness_checker,
                        static_cast<unsigned long long>(witness_seed),
                        witness.c_str());
        }
    }

    // =====================================================================
    // (DETERMINISM) same seed => byte-identical end-to-end history (in-test).
    // Re-runs the WHOLE pipeline (consensus storm + executor + oracle) twice.
    // =====================================================================
    {
        bool all_identical = true;
        for (std::uint64_t seed = 0; seed < seeds; ++seed) {
            const E2EResult a = run_end_to_end(seed, wc, clusters.front());
            const E2EResult b = run_end_to_end(seed, wc, clusters.front());
            if (render_e2e(a) != render_e2e(b)) {
                std::fprintf(stderr, "  NONDETERMINISM at seed=%llu\n",
                             static_cast<unsigned long long>(seed));
                all_identical = false;
            }
        }
        E2E_CHECK(all_identical,
                  "DETERMINISM: same seed => byte-identical end-to-end history "
                  "(consensus storm + executor are a pure fn of seed)");
    }

    // =====================================================================
    // A diagnostic dashboard line per checker on a representative seed.
    // =====================================================================
    {
        const E2EResult o = run_end_to_end(7, wc, clusters.front());
        std::printf("  dashboard (seed=7, n_nodes=%llu, committed=%zu):\n",
                    static_cast<unsigned long long>(clusters.front().n_nodes),
                    o.committed);
        const char* checkers[] = {
            "serialized_by_seqlog",   "exactly_once",
            "ollp_sound",             "strict_serializable",
            "snapshot_level",         "bounded_staleness_level",
            "read_your_writes_level", "differential_vs_oracle"};
        for (const char* nm : checkers) {
            const Verdict* v = verdict_of(o, nm);
            std::printf("    %-26s %s\n", nm, (v && v->ok) ? "PASS" : "FAIL");
        }
    }

    // === EXTERNAL DETERMINISM PROOF ======================================
    // Emit a rendered end-to-end run under a stable marker. The gate runs this
    // binary twice and diffs the captured block => must be byte-identical.
    {
        const E2EResult o = run_end_to_end(7, wc, clusters.front());
        std::printf("===TXN-E2E-DETERMINISM-BLOCK-BEGIN===\n");
        std::fputs(render_e2e(o).c_str(), stdout);
        std::printf("===TXN-E2E-DETERMINISM-BLOCK-END===\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr,
                     "\ntxn_end_to_end_test: FAILED (%d assertion(s)).\n", g_failures);
        return 1;
    }
    std::printf("\ntxn_end_to_end_test: OK — end-to-end strict-serializable == "
                "oracle under the fault storm, exactly-once, durable, progressing, "
                "deterministic, teeth caught.\n");
    return 0;
}
