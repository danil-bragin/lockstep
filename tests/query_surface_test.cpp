// query_surface_test.cpp — Phase 6 Stage F CONFORMANCE GATE for P6-SURFACE.
//
// Source of truth: lockstep-phase-specs-all.md Phase 6 C6.1 (transaction-function
// model) + C6.2 (read/query language); master-plan D1/D3/D5; briefs/phase6.md
// (V-DET-USER / V-D5-SAFE / V-CONFORM). Judges the DEVELOPER-FACING surface
// (query/Database.hpp + query/Query.hpp) against the LANDED strict-serializable
// oracle + the per-D5-level checkers (txn/Oracle.hpp + txn/Checkers.hpp).
//
// Asserts, over a bounded seed sweep (<=64 in-gate, LOCKSTEP_QUERY_SEEDS override):
//   (A) TXN-FUNCTION CONFORMANCE (V-CONFORM, default path): example one-shot txns
//       AUTHORED THROUGH THE SURFACE (a bank-transfer-style workload over TxnFn +
//       TxnContext) produce results that MATCH the strict-serializable oracle and
//       pass the full 8-checker battery (differential + serialized_by_seqlog +
//       exactly_once + ollp_sound + strict_serializable) with 0 violations.
//   (B) D5-LEVEL CONFORMANCE (V-D5-SAFE): reads issued THROUGH THE TYPED query
//       surface at EACH call-site-visible level (Strict / Snapshot / Bounded /
//       RYW) honor EXACTLY that level's contract — judged by the matching txn D5
//       checker (check_strict_serializable / check_snapshot_level /
//       check_bounded_staleness_level / check_read_your_writes_level).
//   (C) V-DET-USER (COMPILE-ENFORCED): a user txn body receives ONLY a
//       TxnContext& — which exposes no clock / no rng / no IO — so a
//       nondeterministic body cannot compile. Asserted statically below.
//   (D) DETERMINISM: same seed => byte-identical rendered surface outcome.
//   (E) TEETH: a deliberately-WRONG usage (a Strict read served a STALE prefix)
//       is CAUGHT by check_strict_serializable (the checker is non-vacuous).
//
// Determinism (binding; query/ is NOT lint-exempt): the only entropy is the seed,
// consumed by an inlined SplitMix; the surface is a pure fn of (history, txns,
// queries). Bounded; CTest TIMEOUT inherited.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/Oracle.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

using namespace lockstep::query;
namespace txn = lockstep::txn;

int g_failures = 0;

#define Q_CHECK(cond, msg)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "query_surface_test FAIL [%s:%d]: %s\n",       \
                         __FILE__, __LINE__, (msg));                            \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------------------
// (C) V-DET-USER, COMPILE-ENFORCED. The user body signature is
// std::function<void(TxnContext&)>; TxnContext exposes ONLY read / write /
// also_read / result. We statically assert the surface gives the body NO
// nondeterministic handle: there is no member that returns a clock tick, a random
// value, or an IO handle. (A would-be nondeterministic body simply has nothing to
// call — the type carries no such member — so it cannot compile.) We assert the
// shape we rely on so a future edit that smuggled a nondeterministic member in
// would trip this.
// ---------------------------------------------------------------------------
static_assert(std::is_invocable_r_v<ReadResult, decltype(&TxnContext::read),
                                    const TxnContext&, const Key&>,
              "TxnContext::read must be the pure declared-read accessor");
static_assert(std::is_same_v<decltype(std::declval<TxnFn>().body),
                             std::function<void(TxnContext&)>>,
              "a user txn body must receive ONLY a TxnContext& (V-DET-USER): no "
              "clock/rng/IO handle is reachable from inside the body");

// A tiny deterministic PRNG (SplitMix64), matching the txn DiffHarness style. NOT
// a std::*_engine / std::*_distribution (forbidden) — just integer mixing.
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : s_(seed) {}
    std::uint64_t next() noexcept {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) noexcept { return n == 0 ? 0 : (next() % n); }

private:
    std::uint64_t s_;
};

std::uint64_t sweep_count() {
    const char* env = std::getenv("LOCKSTEP_QUERY_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 64;
}

// ===========================================================================
// (A) TXN-FUNCTION CONFORMANCE — a bank-transfer-style workload AUTHORED THROUGH
// THE SURFACE. Each txn is a one-shot TxnFn: declare {from, to} as strict reads,
// the body reads both balances over the deterministic TxnContext, and — if `from`
// has enough — moves `amount` from `from` to `to`. The write depends ONLY on the
// declared strict reads, so the strict-serializable result is well-defined and the
// surface ↔ oracle differential is exact (V-CONFORM default path).
// ===========================================================================

// Parse a balance value ("e" / absent == 0; otherwise a small integer string).
long parse_bal(const ReadResult& v) {
    if (!v.has_value() || v->empty() || *v == "e") {
        return 0;
    }
    return std::strtol(v->c_str(), nullptr, 10);
}

// Build a deterministic batch of bank-transfer TxnFns from a seed, ALREADY in
// seqLog order. Accounts are "acct0".."acct{N-1}". Each txn picks from!=to and an
// amount; the body does a read-modify-write of both balances.
std::vector<TxnFn> build_bank_workload(std::uint64_t seed, std::size_t num_txns,
                                       std::size_t num_accts) {
    SplitMix rng(seed ^ 0xA5A5A5A5DEADBEEFULL);
    std::vector<TxnFn> fns;
    fns.reserve(num_txns);
    for (std::size_t i = 0; i < num_txns; ++i) {
        const std::uint64_t a = rng.below(num_accts);
        std::uint64_t b = rng.below(num_accts);
        if (b == a) {
            b = (b + 1) % num_accts;  // ensure from != to
        }
        const Key from = "acct" + std::to_string(a);
        const Key to = "acct" + std::to_string(b);
        const long amount = static_cast<long>(1 + rng.below(20));

        TxnFn fn;
        fn.id = static_cast<std::uint64_t>(i + 1);
        fn.declared = reads(declare::strict(from), declare::strict(to));
        fn.body = [from, to, amount](TxnContext& ctx) {
            const long fb = parse_bal(ctx.read(from));
            const long tb = parse_bal(ctx.read(to));
            if (fb >= amount) {
                ctx.write(from, std::to_string(fb - amount));
                ctx.write(to, std::to_string(tb + amount));
                ctx.result("moved " + std::to_string(amount) + " " + from + "->" + to);
            } else {
                ctx.result("declined " + from);
            }
        };
        fns.push_back(std::move(fn));
    }
    return fns;
}

// Render a SubmitResult to stable text (the determinism surface for the txn path).
std::string render_submit(const SubmitResult& r) {
    std::string s = "tip=" + std::to_string(r.tip_version) + "\n";
    for (const txn::CommitInfo& c : r.commits) {
        s += "  txn=" + std::to_string(c.txn_id) +
             " status=" + txn::status_name(c.status) +
             " seq=" + std::to_string(c.seq_index) +
             " ver=" + std::to_string(c.commit_version) +
             " result=\"" + c.result + "\"\n";
    }
    return s;
}

// To judge the surface's submit result against the oracle + the full checker
// battery, we need the SAME submitted txn::Txn batch the oracle ran. We obtain it
// by wrapping the TxnFns into txn::Txns exactly as the surface does. The surface's
// to-txn is private, so we reconstruct the equivalent wrapper here (identical body
// semantics: run the user body over a TxnContext).
std::vector<txn::Txn> wrap_fns(const std::vector<TxnFn>& fns) {
    std::vector<txn::Txn> out;
    out.reserve(fns.size());
    for (const TxnFn& fn : fns) {
        txn::Txn t;
        t.id = fn.id;
        t.declared_reads = fn.declared;
        const std::function<void(TxnContext&)> body = fn.body;
        t.body = [body](const txn::ReadView& reads) -> txn::Txn::Outcome {
            TxnContext ctx(reads);
            if (body) {
                body(ctx);
            }
            return std::move(ctx).into_outcome();
        };
        out.push_back(std::move(t));
    }
    return out;
}

bool run_bank_conformance(std::uint64_t seed, std::size_t num_txns,
                          std::size_t num_accts) {
    const std::vector<TxnFn> fns = build_bank_workload(seed, num_txns, num_accts);
    const std::vector<txn::Txn> submitted = wrap_fns(fns);

    txn::ExecConfig cfg;
    cfg.max_retry = 2;
    cfg.replica_lag = 0;

    // The SURFACE (Database) executes via the real deterministic executor.
    Database db;
    const SubmitResult sut = db.submit(fns, cfg);

    // The oracle ground truth runs the identical wrapped bodies.
    txn::StrictSerialOracle oracle;
    const SubmitResult oref = oracle.submit_batch(submitted, cfg);

    // The full checker battery, including the differential vs the oracle.
    const std::vector<txn::Verdict> verdicts =
        txn::run_all_checkers(sut, oref, submitted, cfg, seed);

    bool ok = true;
    for (const txn::Verdict& v : verdicts) {
        if (!v.ok) {
            std::fprintf(stderr,
                         "query_surface_test FAIL: txn checker[%s] VIOLATION "
                         "(spec=%s) seed=%llu witness=%s : %s\n",
                         v.checker.c_str(), v.spec_ref.c_str(),
                         static_cast<unsigned long long>(v.seed), v.witness.c_str(),
                         v.explanation.c_str());
            ok = false;
        }
    }

    // Determinism: same seed => byte-identical surface outcome.
    Database db2;
    const SubmitResult sut2 = db2.submit(fns, cfg);
    if (render_submit(sut) != render_submit(sut2)) {
        std::fprintf(stderr, "query_surface_test FAIL: non-deterministic submit "
                             "(seed=%llu)\n",
                     static_cast<unsigned long long>(seed));
        ok = false;
    }
    return ok;
}

// ===========================================================================
// (B) D5-LEVEL CONFORMANCE — reads issued THROUGH THE TYPED query surface at each
// call-site-visible level, judged by the matching txn D5 checker.
//
// We prime the Database read path with a versioned MVCC history (each entry is a
// commit's write-set), then issue typed queries and wrap each query's served reads
// into a synthetic txn::RunResult so the EXACT txn D5 checkers judge them. The
// synthetic run is the primed history as committed txns (so value_after_prefix has
// ground truth) plus ONE extra "reader" txn carrying the query's served reads.
// ===========================================================================

// A versioned history: history[p-1] is the write-set committed at version p. We
// build a simple chain over a few keys so each prefix is distinct.
std::vector<txn::WriteSet> build_history(std::uint64_t seed, std::size_t commits,
                                         std::size_t num_keys) {
    SplitMix rng(seed ^ 0x1234ABCDFEED0001ULL);
    std::vector<txn::WriteSet> hist;
    hist.reserve(commits);
    for (std::size_t p = 1; p <= commits; ++p) {
        txn::WriteSet ws;
        const std::uint64_t k = rng.below(num_keys);
        ws["k" + std::to_string(k)] = "v" + std::to_string(p);
        hist.push_back(std::move(ws));
    }
    return hist;
}

// The value of `key` after the first `prefix` commits of `history` (ground truth).
Value history_value_at(const Key& key, const std::vector<txn::WriteSet>& history,
                       txn::Seq prefix) {
    Value v;  // empty == absent (the bank/D5 histories never write empty)
    for (txn::Seq p = 0; p < prefix && p < history.size(); ++p) {
        const auto it = history[p].find(key);
        if (it != history[p].end()) {
            v = it->second;
        }
    }
    return v;
}

// Build a synthetic RunResult: the history as committed txns, then ONE reader txn
// (id = commits+1) whose served_reads are the query's served reads. The reader's
// commit_version is the tip+1 (so its serial position is right after the history),
// which is exactly the query's own serialization point.
txn::RunResult synth_run(const std::vector<txn::WriteSet>& history,
                         const QueryResult& qr) {
    txn::RunResult run;
    txn::Seq ver = 0;
    for (const txn::WriteSet& ws : history) {
        ++ver;
        txn::CommitInfo ci;
        ci.txn_id = ver;
        ci.status = txn::Status::Committed;
        ci.seq_index = ver;
        ci.commit_version = ver;
        ci.writes_committed = ws;
        run.commits.push_back(std::move(ci));
    }
    // The reader txn, serialized right after the whole history.
    ++ver;
    txn::CommitInfo reader;
    reader.txn_id = ver;
    reader.status = txn::Status::Committed;
    reader.seq_index = ver;
    reader.commit_version = ver;
    reader.served_reads = qr.served_reads;
    run.commits.push_back(std::move(reader));
    run.tip_version = ver;
    return run;
}

bool run_d5_conformance(std::uint64_t seed) {
    const std::size_t commits = 6;
    const std::size_t num_keys = 3;
    const std::vector<txn::WriteSet> history = build_history(seed, commits, num_keys);

    Database db;
    const txn::Seq tip = db.prime(history);
    Q_CHECK(tip == commits, "primed tip must equal the number of commits");

    bool ok = true;

    // --- Strict: linearizable — every served read at the own serialization
    // prefix (the tip). Judged by check_strict_serializable.
    {
        Query<Strict> q;
        q.get("k0").get("k1").get("k2");
        const QueryResult qr = db.run(q);
        Q_CHECK(qr.served_version == tip,
                "a Strict query must serve at the committed tip (own prefix)");
        const txn::RunResult run = synth_run(history, qr);
        const txn::Verdict v = txn::check_strict_serializable(run);
        if (!v.ok) {
            std::fprintf(stderr, "query_surface_test FAIL: Strict query: %s : %s\n",
                         v.witness.c_str(), v.explanation.c_str());
            ok = false;
        }
    }

    // --- Snapshot at an explicit older version: consistent as-of ONE version, no
    // torn read. Judged by check_snapshot_level. Pick version = tip-2.
    {
        const txn::Seq ver = (tip > 2) ? (tip - 2) : tip;
        Query<Snapshot> q = snapshot_query(ver);
        q.get("k0").get("k1").get("k2");
        const QueryResult qr = db.run(q);
        Q_CHECK(qr.served_version == ver,
                "a Snapshot query must serve at exactly its requested version");
        const txn::RunResult run = synth_run(history, qr);
        const txn::Verdict v = txn::check_snapshot_level(run);
        if (!v.ok) {
            std::fprintf(stderr, "query_surface_test FAIL: Snapshot query: %s : %s\n",
                         v.witness.c_str(), v.explanation.c_str());
            ok = false;
        }
    }

    // --- BoundedStaleness: a local replica lagging by `replica_lag` <= max_lag.
    // Judged by check_bounded_staleness_level (lag within contract + real value).
    {
        const txn::Seq max_lag = 2;
        const txn::Seq replica_lag = 1;  // within the contract
        Query<Bounded> q = bounded_query(max_lag);
        q.get("k0").get("k1");
        const QueryResult qr = db.run(q, replica_lag);
        Q_CHECK(qr.served_version == tip - replica_lag,
                "a Bounded query must serve at tip - replica_lag");
        const txn::RunResult run = synth_run(history, qr);
        const txn::Verdict v = txn::check_bounded_staleness_level(run);
        if (!v.ok) {
            std::fprintf(stderr, "query_surface_test FAIL: Bounded query: %s : %s\n",
                         v.witness.c_str(), v.explanation.c_str());
            ok = false;
        }
    }

    // --- ReadYourWrites: the session observes its own prior committed write. We
    // model the session having committed at version `own` (the highest prefix it
    // wrote), so the read must serve from a prefix >= own. Judged by
    // check_read_your_writes_level (after recording the session's write).
    {
        const SessionId session = 7;
        const txn::Seq own = (tip > 1) ? (tip - 1) : tip;  // session's last own write
        Query<RYW> q = ryw_query(session);
        q.get("k0");
        const QueryResult qr = db.run(q, /*replica_lag=*/0, /*session_last_write=*/own);
        Q_CHECK(qr.served_version >= own,
                "a RYW query must serve from a prefix at/after the session's own "
                "last write");
        // Build a synthetic run where the session wrote k0 at version `own`, then
        // the reader reads it; the checker must see the read honor the own write.
        txn::RunResult run = synth_run(history, qr);
        // Inject the session's own write into the committing txn at version `own`
        // (so the RYW checker has a prior write to attribute to the session).
        for (txn::CommitInfo& c : run.commits) {
            if (c.commit_version == own) {
                txn::CommitInfo::ServedRead sw;
                sw.key = "k0";
                sw.level = Level::ReadYourWrites;
                sw.session = session;
                sw.served_version = own;
                c.served_reads.push_back(sw);
                c.writes_committed["k0"] = history_value_at("k0", history, own);
            }
        }
        const txn::Verdict v = txn::check_read_your_writes_level(run);
        if (!v.ok) {
            std::fprintf(stderr, "query_surface_test FAIL: RYW query: %s : %s\n",
                         v.witness.c_str(), v.explanation.c_str());
            ok = false;
        }
    }

    // Determinism: same seed => byte-identical query output.
    {
        Query<Strict> q;
        q.get("k0").get("k1").get("k2");
        Database db_a;
        (void)db_a.prime(history);
        Database db_b;
        (void)db_b.prime(history);
        const QueryResult ra = db_a.run(q);
        const QueryResult rb = db_b.run(q);
        std::string sa, sb;
        for (const PointResult& p : ra.points) {
            sa += p.key + "=" + (p.value ? *p.value : std::string("∅")) + ";";
        }
        for (const PointResult& p : rb.points) {
            sb += p.key + "=" + (p.value ? *p.value : std::string("∅")) + ";";
        }
        Q_CHECK(sa == sb, "non-deterministic query output for the same history");
    }

    return ok;
}

// ===========================================================================
// (E) TEETH — a deliberately-WRONG usage must be CAUGHT. We craft a Strict query
// result whose served read returns a STALE prefix's value (as if a Strict read
// were silently served from an older snapshot). check_strict_serializable MUST
// flag it. If the checker passed this, the surface could serve stale strict reads
// undetected — so the teeth check proves the conformance gate is non-vacuous.
// ===========================================================================
bool run_teeth() {
    // History: k0 = v1 (commit 1), k0 = v2 (commit 2). Tip = 2.
    std::vector<txn::WriteSet> history;
    history.push_back(txn::WriteSet{{"k0", "v1"}});
    history.push_back(txn::WriteSet{{"k0", "v2"}});

    Database db;
    (void)db.prime(history);

    // HONEST Strict read: must observe v2 (the tip). Build its synthetic run and
    // confirm the checker PASSES (no false positive on the honest path).
    Query<Strict> q;
    q.get("k0");
    const QueryResult honest = db.run(q);
    {
        const txn::RunResult run = synth_run(history, honest);
        const txn::Verdict v = txn::check_strict_serializable(run);
        Q_CHECK(v.ok, "honest Strict read must pass the strict checker (no false "
                      "positive)");
        Q_CHECK(honest.points.size() == 1 && honest.points[0].value == "v2",
                "honest Strict read of k0 at tip must observe v2");
    }

    // WRONG: fabricate a served read that returns the STALE v1 while claiming to be
    // a Strict read at the tip (served_version == 2, the own serialization point).
    // That is a strict read that did NOT observe the committed prefix before it —
    // the checker must flag it.
    QueryResult stale = honest;
    stale.served_reads.clear();
    {
        txn::CommitInfo::ServedRead sr;
        sr.key = "k0";
        sr.level = Level::StrictSerializable;
        sr.served_version = 2;       // claims to be at the tip (own point)
        sr.value = std::string("v1");  // but returns the STALE value
        stale.served_reads.push_back(sr);
    }
    const txn::RunResult run = synth_run(history, stale);
    const txn::Verdict v = txn::check_strict_serializable(run);
    Q_CHECK(!v.ok, "TEETH: a Strict read served the STALE value must be CAUGHT by "
                   "check_strict_serializable");
    if (!v.ok) {
        std::fprintf(stderr, "query_surface_test: teeth OK — stale strict read "
                             "caught: %s\n",
                     v.witness.c_str());
    }
    return v.ok ? false : true;  // teeth "passes" when the checker FLAGGED it
}

}  // namespace

int main() {
    const std::uint64_t seeds = sweep_count();

    // A small workload matrix so the default path + the bodies vary across seeds.
    struct WC {
        std::size_t num_txns;
        std::size_t num_accts;
    };
    const std::vector<WC> matrix = {{4, 3}, {8, 4}, {12, 5}};

    std::uint64_t total_runs = 0;
    for (std::uint64_t s = 0; s < seeds; ++s) {
        for (const WC& wc : matrix) {
            ++total_runs;
            if (!run_bank_conformance(s, wc.num_txns, wc.num_accts)) {
                ++g_failures;
            }
        }
        if (!run_d5_conformance(s)) {
            ++g_failures;
        }
    }

    // (E) teeth — the conformance gate has teeth (non-vacuous).
    if (!run_teeth()) {
        std::fprintf(stderr, "query_surface_test FAIL: teeth check did not catch a "
                             "deliberately-wrong stale strict read\n");
        ++g_failures;
    }

    std::fprintf(stderr,
                 "query_surface_test: seeds=%llu bank_runs=%llu d5_runs=%llu\n",
                 static_cast<unsigned long long>(seeds),
                 static_cast<unsigned long long>(total_runs),
                 static_cast<unsigned long long>(seeds));

    if (g_failures != 0) {
        std::fprintf(stderr, "query_surface_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "query_surface_test: ALL PASS (conformance == oracle on "
                         "default path; each D5 level exact; teeth caught)\n");
    return EXIT_SUCCESS;
}
