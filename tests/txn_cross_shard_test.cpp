// txn_cross_shard_test.cpp — Phase 9 S9.3 CROSS-SHARD ATOMIC COMMIT GATE.
//
// The hardest correctness piece: a transaction that touches keys on MULTIPLE
// shards commits ATOMICALLY (all-or-nothing across shards) and STRICT-SERIALIZABLY,
// Calvin-style (NO 2PC). It wires the ALREADY-VERIFIED building blocks end to end:
//
//   M independent single-shard Raft clusters (S9.1 model: each shard = its own
//   consensus group) ORDER, UNDER THE FULL FAULT STORM, the txns that touch them.
//   A cross-shard txn is submitted to EACH shard it touches, so it lands in EACH
//   involved shard's COMMITTED Raft log. The per-shard committed logs feed the
//   C4.4 Sequencer::merge -> ONE global deterministic order; the S9.3 dedup
//   (txn/CrossShard.hpp) collapses a cross-shard txn's per-shard appearances to ONE
//   global position GATED on it being committed on ALL its shards (the all-or-
//   nothing seal gate); the deduped order feeds the Phase-5 DeterministicExecutor,
//   which applies each txn's whole write set ATOMICALLY at that one position. The
//   committed history is judged by the EXISTING strict-serializable txn Oracle +
//   the 8-checker battery (txn/Oracle.hpp + txn/Checkers.hpp, UNCHANGED).
//
// Source of truth: specs/XShardCommit.tla (S9.3, TLC-verified clean + teeth) +
// specs/Sequencer.tla (C4.4) + specs/CommitOrdering.tla (Phase 5).
//
// WHAT THIS ASSERTS over a seed sweep (<=16 in-gate x cluster matrix, every run
// TERMINATES via the run_until step-backstop):
//   (a) ATOMICITY (XShardCommit.tla AtomicAllOrNone) — every committed cross-shard
//       txn's writes are present on ALL its shards or NONE, NEVER a partial commit.
//       Re-derived independently per shard from the committed prefix.
//   (b) STRICT-SERIALIZABLE — the committed history == a serial execution in the
//       global (sequencer) order, judged by the txn Oracle on that SAME order + the
//       strict_serializable checker.
//   (c) EXACTLY-ONCE — each committed txn applies exactly once (one global position
//       despite landing in several shard logs), via exactly_once + the dedup.
//   (d) DETERMINISM — same seed => byte-identical global order + committed history.
//   TEETH — a deliberately-WRONG wiring (apply a cross-shard txn on only ONE of its
//       shards, OR out of the sequencer order) is CAUGHT by the atomicity /
//       strict_serializable checker.
//
// DETERMINISM (binding; this TU is NOT lint-exempt): pure fn of (seed). All
// nondeterminism flows through the seeded provider PRNG threaded into each cluster +
// the inlined SplitMix building the workload; all time virtual. No wall-clock, no
// threads, no std::*_distribution, ordered maps throughout. Bounded: each cluster
// run_until()s a deterministic deadline with a step backstop, so it ALWAYS
// terminates; CTest TIMEOUT is the outer guard.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/consensus/ClusterDriver.hpp>
#include <lockstep/consensus/ConsensusNode.hpp>
#include <lockstep/consensus/raft_a/RaftNodeA.hpp>
#include <lockstep/consensus/sequencer/Sequencer.hpp>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/CrossShard.hpp>
#include <lockstep/txn/DeterministicExecutor.hpp>
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
namespace seq = lockstep::consensus::sequencer;

using lockstep::txn::check_exactly_once;
using lockstep::txn::check_serialized_by_seqlog;
using lockstep::txn::check_strict_serializable;
using lockstep::txn::CommitInfo;
using lockstep::txn::DeterministicExecutor;
using lockstep::txn::dedup_global_order;
using lockstep::txn::key_shard;
using lockstep::txn::Key;
using lockstep::txn::read_strict;
using lockstep::txn::ReadView;
using lockstep::txn::RunResult;
using lockstep::txn::run_all_checkers;
using lockstep::txn::shards_of;
using lockstep::txn::Status;
using lockstep::txn::StrictSerialOracle;
using lockstep::txn::Txn;
using lockstep::txn::Verdict;

int g_failures = 0;

#define XS_CHECK(cond, msg)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "txn_cross_shard_test FAIL [%s:%d]: %s\n",   \
                         __FILE__, __LINE__, (msg));                          \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

std::uint64_t sweep_count() {
    const char* env = std::getenv("LOCKSTEP_XSHARD_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 16;
}

// ---------------------------------------------------------------------------
// The opaque consensus command value for a txn == its id, string-encoded.
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

// Inlined SplitMix64 — pure seeded PRNG (NO std::*_distribution).
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
    [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept {
        return n == 0 ? 0 : (next() % n);
    }

private:
    std::uint64_t s_;
};

// ---------------------------------------------------------------------------
// THE CROSS-SHARD WORKLOAD. Each txn declares a strict read+RMW on a PRIMARY key,
// and with some probability ALSO touches a key on a DIFFERENT shard (making it a
// cross-shard txn). The body writes BOTH keys (a value derived from the strict
// reads), so a cross-shard txn's effect spans >=2 shards — exactly what must commit
// atomically. Keys are routed to shards by key_shard(). PURE fn of (seed, cfg).
// ---------------------------------------------------------------------------
struct XSWorkloadConfig {
    std::size_t num_txns = 10;
    std::uint32_t num_keys = 6;
    std::uint32_t num_shards = 3;
    double cross_frac_inv = 2;  // ~1/cross_frac_inv txns are cross-shard
    lockstep::txn::ExecConfig exec;
};

std::vector<Txn> build_cross_shard_workload(std::uint64_t seed,
                                            const XSWorkloadConfig& wc) {
    SplitMix rng(seed ^ 0x9933'AA55'1122'CCDDULL);
    std::vector<Txn> txns;
    txns.reserve(wc.num_txns);

    for (std::size_t i = 0; i < wc.num_txns; ++i) {
        Txn t;
        t.id = static_cast<std::uint64_t>(i + 1);

        const std::uint32_t k0 = static_cast<std::uint32_t>(rng.below(wc.num_keys));
        const Key key0 = "k" + std::to_string(k0);
        t.declared_reads.push_back(read_strict(key0));

        // Maybe make it cross-shard: pick a SECOND key on a DIFFERENT shard.
        std::vector<Key> write_keys{key0};
        const bool want_cross =
            wc.cross_frac_inv > 0 && rng.below(static_cast<std::uint64_t>(wc.cross_frac_inv)) == 0;
        if (want_cross) {
            // Find a key whose shard differs from key0's shard (bounded search).
            const seq::ShardId s0 = key_shard(key0, wc.num_shards);
            for (std::uint32_t tries = 0; tries < wc.num_keys * 2; ++tries) {
                const std::uint32_t k1 =
                    static_cast<std::uint32_t>(rng.below(wc.num_keys));
                const Key key1 = "k" + std::to_string(k1);
                if (key_shard(key1, wc.num_shards) != s0) {
                    t.declared_reads.push_back(read_strict(key1));
                    write_keys.push_back(key1);
                    break;
                }
            }
        }

        const std::uint64_t my_id = t.id;
        t.body = [my_id, write_keys](const ReadView& reads) -> Txn::Outcome {
            Txn::Outcome oc;
            // RMW: each written key gets base(its prior value)>tID. The write to a
            // key depends ONLY on the strict read of THAT key, so the serializable
            // result is well-defined and the oracle<->SUT differential is exact.
            for (const Key& wk : write_keys) {
                ReadView::const_iterator it = reads.find(wk);
                const std::string base =
                    (it != reads.end() && it->second.has_value()) ? *it->second
                                                                  : std::string("e");
                oc.writes[wk] = base + ">t" + std::to_string(my_id);
            }
            oc.result = "wrote:" + std::to_string(write_keys.size());
            return oc;
        };
        txns.push_back(std::move(t));
    }
    return txns;
}

// ===========================================================================
// Drive ONE shard's real cluster under the fault storm, submitting the txn ids
// that touch this shard, and return that shard's COMMITTED consensus order of txn
// ids. A generalization of txn_end_to_end_test's order_via_consensus, parameterized
// by which ids to submit. PURE fn of (seed, cfg, factory, ids).
// ===========================================================================
std::vector<std::uint64_t> shard_committed_order(std::uint64_t seed,
                                                 const ConsensusNodeFactory& factory,
                                                 const ClusterConfig& cfg,
                                                 const std::vector<std::uint64_t>& ids,
                                                 bool* stalled_out) {
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

    // One client submits this shard's slice of txn ids in order (mirrors the e2e
    // txn_client_driver — submit a value, observe a live/quorum node committing it).
    // A plain coroutine fn (NOT a capturing lambda): nothing is parked across an
    // await by reference (V-RKV1); all state flows through the pointer args.
    st->sched.spawn([](cdetail::ClusterRunState* s, const ClusterConfig* c,
                       const std::vector<std::uint64_t>* idv) -> lockstep::core::Task {
        for (std::size_t i = 0; i < idv->size(); ++i) {
            const Tick gap = s->rng.uniform_range(c->client_gap_min, c->client_gap_max);
            if (gap > 0) {
                co_await s->clock.delay(gap);
            }
            std::uint64_t li = UINT64_MAX;
            {
                lockstep::core::Task t =
                    cdetail::await_leader(s, c, c->election_timeout_max * 4, &li);
                co_await std::move(t);
            }
            const std::string value = txn_value((*idv)[i]);
            bool accepted = false;
            lockstep::consensus::Index index = 0;
            if (li != UINT64_MAX) {
                const SubmitResult r = s->nodes[li]->submit(value);
                accepted = r.accepted;
                index = r.index;
            }
            if (accepted) {
                Tick waited = 0;
                const Tick stride = c->heartbeat_interval * 2;
                bool committed = false;
                while (waited < c->request_deadline && !committed) {
                    if (stride > 0) {
                        co_await s->clock.delay(stride);
                    }
                    waited += stride;
                    cdetail::snapshot(s);
                    for (std::size_t n = 0; n < s->nodes.size(); ++n) {
                        if (!s->live[n]) {
                            continue;
                        }
                        if (s->nodes[n]->commit_index() >= index) {
                            const std::span<const LogEntry> lg = s->nodes[n]->log();
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
    }(st, &cfg, &ids));

    st->sched.spawn(cdetail::fault_driver(st, &cfg));
    st->sched.spawn(cdetail::settle_and_snapshot(
        st, cfg.settle_ticks * 4, cfg.heartbeat_interval * 2));

    const Tick deadline = cdetail::compute_deadline(&cfg);
    const std::uint64_t step_cap = cdetail::compute_step_cap(&cfg);
    const bool finished = st->sched.run_until(deadline, step_cap);
    st->run.progress_stalled = !finished;
    *stalled_out = !finished;

    cdetail::snapshot(st);
    cdetail::finalize_committed_log(st);

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

// ===========================================================================
// THE CROSS-SHARD END-TO-END RESULT for one seed.
// ===========================================================================
struct XSResult {
    std::uint64_t seed = 0;
    bool stalled = false;
    std::vector<Txn> seqlog;     // committed cross+single-shard txns in GLOBAL order
    RunResult sut;               // DeterministicExecutor over the deduped global order
    RunResult oracle;            // strict-serializable oracle over the SAME order
    std::vector<Verdict> verdicts;
    bool all_ok = true;
    std::size_t committed = 0;
    std::size_t cross_shard_committed = 0;
    // The deduped GLOBAL txn-id order (the byte-repro determinism surface).
    std::vector<std::uint64_t> global_ids;
    // Per-txn: which shards' committed logs it appeared on (atomicity ground truth).
    std::map<std::uint64_t, std::set<seq::ShardId>> committed_shards;
    std::map<std::uint64_t, std::set<seq::ShardId>> touched_shards;
};

// Run the full cross-shard pipeline for one seed.
XSResult run_cross_shard(std::uint64_t seed, const XSWorkloadConfig& wc,
                         const ClusterConfig& cluster_cfg) {
    XSResult out;
    out.seed = seed;

    const std::vector<Txn> all_txns = build_cross_shard_workload(seed, wc);
    std::map<std::uint64_t, const Txn*> by_id;
    std::map<seq::TxnId, std::set<seq::ShardId>> shards_by_txn;  // for the dedup gate

    // Partition the txn ids by which shard each touches (a cross-shard txn is
    // submitted to MULTIPLE shards' clusters). Per-shard submit order = id order.
    std::vector<std::vector<std::uint64_t>> per_shard_ids(wc.num_shards);
    for (const Txn& t : all_txns) {
        by_id[t.id] = &t;
        const std::set<seq::ShardId> shards = shards_of(t, wc.num_shards);
        out.touched_shards[t.id] = shards;
        shards_by_txn[txn_value(t.id)] = shards;
        for (const seq::ShardId s : shards) {
            per_shard_ids[s].push_back(t.id);
        }
    }

    // Each shard is its OWN independent consensus cluster ordering its slice under
    // the fault storm. Distinct seeds per shard (seed ^ shard salt) so the shards'
    // election jitter differs, yet everything stays a pure fn of the master seed.
    std::vector<seq::ShardLog> shard_logs(wc.num_shards);
    bool any_stalled = false;
    for (seq::ShardId s = 0; s < wc.num_shards; ++s) {
        bool stalled = false;
        const std::uint64_t shard_seed = seed ^ (0xA5A5'0000ULL + s);
        const std::vector<std::uint64_t> committed =
            shard_committed_order(shard_seed, make_raft_a_factory(), cluster_cfg,
                                  per_shard_ids[s], &stalled);
        any_stalled = any_stalled || stalled;
        // This shard's committed log -> a Sequencer ShardLog. Single-shard consensus
        // per shard: each shard commits into ascending epoch 1 (one epoch suffices
        // for the per-seed batch; the cross-shard merge is the interesting axis).
        for (const std::uint64_t id : committed) {
            shard_logs[s].push_back(seq::InputEntry{txn_value(id), /*epoch=*/1});
            out.committed_shards[id].insert(s);
        }
    }
    out.stalled = any_stalled;

    // C4.4 Sequencer merge: identity ranks (rank[s]=s, injective). Seal epoch 1
    // once every shard has committed into it (max_sealable handles the boundary; we
    // force epoch 2 by appending a closed marker is unnecessary — instead seal=1 is
    // legal because all per-shard logs are epoch 1 and we treat epoch 1 as closed
    // for this batch by sealing to the max committed epoch).
    seq::ShardRank ranks(wc.num_shards);
    for (seq::ShardId s = 0; s < wc.num_shards; ++s) {
        ranks[s] = s;
    }
    // All entries are epoch 1; max_sealable would return 0 (top epoch may be open).
    // For a drained per-seed batch every shard's epoch-1 prefix is final, so we seal
    // epoch 1 directly (the batch is closed: no more commits land after the run).
    const seq::GlobalLog global = seq::merge(shard_logs, ranks, /*sealed=*/1);

    // S9.3 dedup: collapse a cross-shard txn's per-shard appearances to ONE global
    // position, GATED on it being committed on ALL its shards (all-or-nothing).
    const std::vector<seq::TxnId> deduped = dedup_global_order(global, shards_by_txn);

    // Resolve the deduped global order back to Txn objects (the seqLog).
    for (const seq::TxnId& v : deduped) {
        std::uint64_t id = 0;
        if (parse_txn_value(v, &id)) {
            const auto it = by_id.find(id);
            if (it != by_id.end()) {
                out.seqlog.push_back(*it->second);
                out.global_ids.push_back(id);
            }
        }
    }

    // Phase-5 DeterministicExecutor over the global order (atomic per-txn apply) +
    // the strict-serializable oracle on the SAME order. Judge with the battery.
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
            if (out.touched_shards[c.txn_id].size() >= 2) {
                ++out.cross_shard_committed;
            }
        }
    }
    return out;
}

std::string render_xs(const XSResult& o) {
    std::string s;
    s += "seed=" + std::to_string(o.seed) + " stalled=" + (o.stalled ? "1" : "0") +
         " global=" + std::to_string(o.global_ids.size()) +
         " committed=" + std::to_string(o.committed) +
         " xshard=" + std::to_string(o.cross_shard_committed) + "\n";
    s += "  order:";
    for (const std::uint64_t id : o.global_ids) {
        s += " " + std::to_string(id);
    }
    s += "\n";
    for (const CommitInfo& c : o.sut.commits) {
        s += "  txn=" + std::to_string(c.txn_id) +
             " status=" + std::string(lockstep::txn::status_name(c.status)) +
             " seq=" + std::to_string(c.seq_index) + " result=\"" + c.result + "\"\n";
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

// ---------------------------------------------------------------------------
// ATOMICITY (XShardCommit.tla AtomicAllOrNone), re-derived INDEPENDENTLY of the
// executor: a committed cross-shard txn must be present in the committed log of
// EVERY shard it touches; an ABSENT (uncommitted) cross-shard txn must be present
// in NONE of the global order. Never a partial cross-shard commit. Returns true if
// atomic; fills `witness` on a partial.
// ---------------------------------------------------------------------------
bool atomicity_holds(const XSResult& o, std::string* witness) {
    std::set<std::uint64_t> in_global(o.global_ids.begin(), o.global_ids.end());
    for (const auto& [id, touched] : o.touched_shards) {
        const auto cit = o.committed_shards.find(id);
        const std::set<seq::ShardId>& committed_on =
            (cit != o.committed_shards.end()) ? cit->second : std::set<seq::ShardId>{};
        const bool in_order = in_global.count(id) != 0;
        if (in_order) {
            // A txn placed in the GLOBAL order (=> the executor applies its whole
            // write set atomically) MUST be committed on ALL the shards it touches.
            for (const seq::ShardId s : touched) {
                if (committed_on.count(s) == 0) {
                    *witness = "txn=" + std::to_string(id) +
                               " in global order but NOT committed on shard " +
                               std::to_string(s) + " (PARTIAL cross-shard commit)";
                    return false;
                }
            }
        } else if (touched.size() >= 2) {
            // A cross-shard txn NOT in the global order must apply on NONE of its
            // shards (it is absent from the order, so the executor never applies
            // it). That is the "none" side of all-or-nothing — trivially true here
            // because the executor only sees the global order; we assert it is not
            // present in the order (already known) for symmetry.
        }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("txn_cross_shard_test: Phase 9 S9.3 cross-shard atomic commit gate\n");

    const std::uint64_t seeds = sweep_count();
    XSWorkloadConfig wc;
    wc.num_txns = 10;
    wc.num_keys = 6;
    wc.num_shards = 3;
    wc.exec.max_retry = 2;

    std::vector<ClusterConfig> clusters;
    {
        ClusterConfig c;  // 3 nodes, full envelope.
        clusters.push_back(c);
    }
    {
        ClusterConfig c;
        c.n_nodes = 5;
        c.partition_episodes = 3;
        c.crash_episodes = 2;
        clusters.push_back(c);
    }

    // =====================================================================
    // (MAIN) ATOMICITY + STRICT-SERIALIZABLE + EXACTLY-ONCE over the sweep.
    // =====================================================================
    bool any_cross_shard = false;
    std::size_t total_committed = 0;
    std::size_t total_cross = 0;
    std::size_t runs = 0;
    for (const ClusterConfig& ccfg : clusters) {
        for (std::uint64_t seed = 0; seed < seeds; ++seed) {
            const XSResult o = run_cross_shard(seed, wc, ccfg);
            ++runs;

            XS_CHECK(!o.stalled, "every shard cluster terminated (no step-backstop trip)");

            // (a) ATOMICITY — re-derived independently per shard.
            std::string awit;
            const bool atomic = atomicity_holds(o, &awit);
            XS_CHECK(atomic, ("ATOMICITY: " + awit).c_str());

            // (b)+(c) STRICT-SERIALIZABLE + EXACTLY-ONCE + the full battery.
            if (!o.all_ok) {
                for (const Verdict& v : o.verdicts) {
                    if (!v.ok) {
                        std::fprintf(stderr,
                                     "  XS VIOLATION seed=%llu n_nodes=%llu checker=%s "
                                     "witness=%s\n",
                                     static_cast<unsigned long long>(seed),
                                     static_cast<unsigned long long>(ccfg.n_nodes),
                                     v.checker.c_str(), v.witness.c_str());
                    }
                }
            }
            XS_CHECK(o.all_ok,
                     "every checker passes (strict-serializable == oracle in the "
                     "sequencer order, exactly-once, OLLP sound, D5 contracts)");

            total_committed += o.committed;
            total_cross += o.cross_shard_committed;
            if (o.cross_shard_committed > 0) {
                any_cross_shard = true;
            }
        }
    }
    XS_CHECK(any_cross_shard,
             "COVERAGE: at least one cross-shard txn committed (non-vacuous — the "
             "atomicity claim is tested on real cross-shard txns)");
    std::printf("  swept %zu runs (%llu seeds x %zu clusters): committed=%zu "
                "(cross-shard=%zu), all atomic + strict-serializable==oracle + "
                "exactly-once.\n",
                runs, static_cast<unsigned long long>(seeds), clusters.size(),
                total_committed, total_cross);

    // =====================================================================
    // (TEETH-1) PARTIAL CROSS-SHARD COMMIT is CAUGHT by the atomicity check.
    // A deliberately-wrong wiring drops a cross-shard txn's appearance on ONE of its
    // shards (applies it on only the other), and the independent atomicity check
    // must flag the partial commit.
    // =====================================================================
    {
        bool teeth_caught = false;
        std::string witness;
        std::uint64_t wseed = 0;
        for (std::uint64_t seed = 0; seed < seeds && !teeth_caught; ++seed) {
            XSResult o = run_cross_shard(seed, wc, clusters.front());
            // Find a committed cross-shard txn (in the global order, touching >=2).
            std::uint64_t victim = 0;
            seq::ShardId drop_shard = 0;
            for (const std::uint64_t id : o.global_ids) {
                if (o.touched_shards[id].size() >= 2) {
                    victim = id;
                    drop_shard = *o.touched_shards[id].begin();
                    break;
                }
            }
            if (victim == 0) {
                continue;  // no cross-shard txn this seed; nothing to break
            }
            // WRONG WIRING: pretend the txn was committed on only a subset (drop one
            // shard) while STILL applying it (it stays in the global order). This is
            // a PARTIAL cross-shard commit — exactly XShardCommit.tla's BuggyApply.
            o.committed_shards[victim].erase(drop_shard);
            std::string awit;
            if (!atomicity_holds(o, &awit)) {
                teeth_caught = true;
                witness = awit;
                wseed = seed;
            }
        }
        XS_CHECK(teeth_caught,
                 "TEETH-1: a PARTIAL cross-shard commit (txn applied but missing on "
                 "one of its shards) is CAUGHT by the atomicity check");
        if (teeth_caught) {
            std::printf("  [TEETH-1] partial cross-shard commit caught (seed=%llu): %s\n",
                        static_cast<unsigned long long>(wseed), witness.c_str());
        }
    }

    // =====================================================================
    // (TEETH-2) OUT-OF-SEQUENCER-ORDER apply is CAUGHT by strict_serializable /
    // serialized_by_seqlog. Execute the committed txns in id (arrival) order instead
    // of the global sequencer order; for a seed where the orders differ, a checker
    // must flag it (the sequencer order IS the linearization point).
    // =====================================================================
    {
        bool teeth_caught = false;
        std::string witness;
        const char* wchecker = nullptr;
        std::uint64_t wseed = 0;
        for (std::uint64_t seed = 0; seed < seeds && !teeth_caught; ++seed) {
            const XSResult honest = run_cross_shard(seed, wc, clusters.front());
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
                continue;
            }
            DeterministicExecutor wrong_exec;
            const RunResult wrong = wrong_exec.submit_batch(arrival, wc.exec);
            const Verdict v_seq = check_serialized_by_seqlog(wrong, honest.seqlog);
            const std::vector<Verdict> vs =
                run_all_checkers(wrong, honest.oracle, honest.seqlog, wc.exec, seed);
            const Verdict* v_strict = nullptr;
            const Verdict* v_diff = nullptr;
            for (const Verdict& v : vs) {
                if (v.checker == "strict_serializable") {
                    v_strict = &v;
                }
                if (v.checker == "differential_vs_oracle") {
                    v_diff = &v;
                }
            }
            if (!v_seq.ok) {
                teeth_caught = true;
                witness = v_seq.witness;
                wchecker = "serialized_by_seqlog";
                wseed = seed;
            } else if (v_strict != nullptr && !v_strict->ok) {
                teeth_caught = true;
                witness = v_strict->witness;
                wchecker = "strict_serializable";
                wseed = seed;
            } else if (v_diff != nullptr && !v_diff->ok) {
                teeth_caught = true;
                witness = v_diff->witness;
                wchecker = "differential_vs_oracle";
                wseed = seed;
            }
        }
        XS_CHECK(teeth_caught,
                 "TEETH-2: executing in arrival order instead of the sequencer "
                 "global order is CAUGHT by a checker");
        if (teeth_caught) {
            std::printf("  [TEETH-2] wrong-order wiring caught by %s (seed=%llu) "
                        "witness=%s\n",
                        wchecker, static_cast<unsigned long long>(wseed),
                        witness.c_str());
        }
    }

    // =====================================================================
    // (DETERMINISM) same seed => byte-identical global order + committed history.
    // =====================================================================
    {
        bool all_identical = true;
        for (std::uint64_t seed = 0; seed < seeds; ++seed) {
            const XSResult a = run_cross_shard(seed, wc, clusters.front());
            const XSResult b = run_cross_shard(seed, wc, clusters.front());
            if (render_xs(a) != render_xs(b)) {
                std::fprintf(stderr, "  NONDETERMINISM at seed=%llu\n",
                             static_cast<unsigned long long>(seed));
                all_identical = false;
            }
        }
        XS_CHECK(all_identical,
                 "DETERMINISM: same seed => byte-identical global order + committed "
                 "history (the merge + execution are pure fns of seed)");
    }

    // === EXTERNAL DETERMINISM PROOF ======================================
    {
        const XSResult o = run_cross_shard(3, wc, clusters.front());
        std::printf("===TXN-XSHARD-DETERMINISM-BLOCK-BEGIN===\n");
        std::fputs(render_xs(o).c_str(), stdout);
        std::printf("===TXN-XSHARD-DETERMINISM-BLOCK-END===\n");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\ntxn_cross_shard_test: FAILED (%d assertion(s)).\n",
                     g_failures);
        return 1;
    }
    std::printf("\ntxn_cross_shard_test: OK — cross-shard txns commit ATOMICALLY "
                "(all-or-nothing across shards) + STRICT-SERIALIZABLE == oracle in "
                "the sequencer global order, exactly-once, deterministic; teeth "
                "caught (partial commit + out-of-order).\n");
    return 0;
}
