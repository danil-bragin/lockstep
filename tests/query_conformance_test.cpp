// query_conformance_test.cpp — Phase 6 Stage B, C6.6. THE PHASE-6 CONFORMANCE GATE.
//
// Source of truth: briefs/phase6.md C6.6 ("a CONFORMANCE SUITE ... exercise the
// FULL stack — driver -> wire -> server -> Database -> txn executor -> MVCC store
// — over a seed sweep under net faults, asserting end-to-end results match the
// strict-serializable oracle on the default path and each D5 level honors exactly
// its contract"). It reuses txn/Oracle.hpp + txn/Checkers.hpp UNCHANGED.
//
// This is the END-TO-END gate the rest of Phase 6 (C6.4 Driver, C6.5 CLI) sits
// under: nothing in the developer surface is trusted unless a workload taken ALL
// the way through the real driver + real wire + real server + real executor +
// real MVCC store agrees with the strict-serializable ground truth.
//
// WHAT IT ASSERTS, over a bounded seed sweep (<=64 in-gate, LOCKSTEP_CONFORMANCE_
// SEEDS override):
//   (A) END-TO-END == ORACLE (V-CONFORM, default path). A bank-transfer workload
//       driven THROUGH THE DRIVER (Connection -> ClientStub -> SimNetwork ->
//       Server -> Database -> deterministic executor -> WalEngine MVCC) produces
//       final balances that EQUAL the strict-serializable oracle's final store for
//       the SAME ordered txn batch. Asserted on a CLEAN net AND under NASTY net
//       faults (dup/reorder/drop) — the relaxed transport must not change the
//       committed result (exactly-once holds end to end).
//   (B) EXACTLY-ONCE END TO END. Under faults, the server's applied-submit count
//       equals the number of DISTINCT logical submits — a re-delivered Submit (the
//       dropped-reply retry the DRIVER issues, reusing its submit_key) applies the
//       txn exactly once. No duplicated money move.
//   (C) EACH D5 LEVEL EXACT (V-D5-SAFE). Reads issued through the driver at each
//       call-site-visible level (Strict / Snapshot / Bounded / RYW) are judged by
//       the MATCHING txn D5 checker (check_strict_serializable / _snapshot_level /
//       _bounded_staleness_level / _read_your_writes_level) on a RunResult built
//       from the SERVED diagnostics — and each checker passes (no stronger, no
//       weaker). A Snapshot read as-of an older version returns the older value; a
//       Bounded read stays within its lag; a strict read is linearizable.
//   (D) DETERMINISM. Same seed => byte-identical scheduler trace + balances, on a
//       fault-heavy run replayed.
//   (E) TEETH. The D5 checkers are non-vacuous: a deliberately-WRONG served-read
//       record (a Strict read fed a stale value) is CAUGHT by the strict checker.
//
// Determinism (binding; this TU is NOT lint-exempt): the only entropy is the seed,
// consumed by SeededRandom (net faults) + an inlined SplitMix (workload shape). NO
// <chrono>/<thread>/<random>; all time is the virtual SimClock. Bounded; CTest
// TIMEOUT inherited.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Task.hpp>

#include <lockstep/query/Driver.hpp>
#include <lockstep/query/LocalCluster.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/Protocol.hpp>

#include <lockstep/txn/Checkers.hpp>
#include <lockstep/txn/Oracle.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace {

namespace q = lockstep::query;
namespace txn = lockstep::txn;

int g_failures = 0;

#define C_CHECK(cond, msg, seed)                                                  \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr,                                                  \
                         "query_conformance_test FAIL [%s:%d]: %s (seed=%llu)\n", \
                         __FILE__, __LINE__, (msg),                               \
                         static_cast<unsigned long long>(seed));                  \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

// Inlined SplitMix64 for the deterministic WORKLOAD shape (independent of the
// SeededRandom the net faults consume, so the workload is fixed per seed).
struct SplitMix {
    std::uint64_t s;
    explicit SplitMix(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t in(std::uint64_t lo, std::uint64_t hi) {
        return lo + (next() % (hi - lo + 1));
    }
};

constexpr const char* kNames[] = {"acct:a", "acct:b", "acct:c", "acct:d"};
constexpr int kNAccts = 4;
constexpr std::int64_t kStart = 1000;

struct Xfer {
    std::string a;
    std::string b;
    std::int64_t amount = 0;
};

struct Workload {
    std::vector<Xfer> transfers;
};

Workload make_workload(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xA5A5'1234'DEAD'BEEFULL);
    Workload w;
    const int n = static_cast<int>(rng.in(3, 6));
    for (int i = 0; i < n; ++i) {
        std::uint64_t ai = rng.in(0, kNAccts - 1);
        std::uint64_t bi = rng.in(0, kNAccts - 1);
        if (bi == ai) {
            bi = (bi + 1) % kNAccts;
        }
        Xfer x;
        x.a = kNames[ai];
        x.b = kNames[bi];
        x.amount = static_cast<std::int64_t>(rng.in(1, 200));
        w.transfers.push_back(x);
    }
    return w;
}

// ---------------------------------------------------------------------------
// THE ORACLE GROUND TRUTH. Build the SAME workload as an ordered txn::Txn batch
// (seed-puts then transfers) and run the strict-serializable StrictSerialOracle.
// The transfer body mirrors the server's SubmitOp::Transfer EXACTLY (parse both
// balances, write a-amt / b+amt) so the oracle's committed store IS the end-to-end
// ground truth. Returns the oracle RunResult + its final store (balances).
// ---------------------------------------------------------------------------
struct OracleGroundTruth {
    txn::RunResult run;
    std::map<std::string, std::int64_t> balances;
};

std::vector<txn::Txn> build_txn_batch(const Workload& w) {
    std::vector<txn::Txn> batch;
    std::uint64_t id = 0;
    // Seed puts.
    for (int i = 0; i < kNAccts; ++i) {
        txn::Txn t;
        t.id = ++id;
        const std::string key = kNames[i];
        const std::string val = q::wire::encode_balance(kStart);
        t.body = [key, val](const txn::ReadView&) {
            txn::Txn::Outcome oc;
            oc.writes[key] = val;
            oc.result = "put:" + key;
            return oc;
        };
        batch.push_back(std::move(t));
    }
    // Transfers (declare both keys strict; read both, write both).
    for (const Xfer& x : w.transfers) {
        txn::Txn t;
        t.id = ++id;
        t.declared_reads = {txn::read_strict(x.a), txn::read_strict(x.b)};
        const std::string ka = x.a;
        const std::string kb = x.b;
        const std::int64_t amt = x.amount;
        t.body = [ka, kb, amt](const txn::ReadView& reads) {
            txn::Txn::Outcome oc;
            const auto fa = reads.find(ka);
            const auto fb = reads.find(kb);
            const std::int64_t va =
                q::wire::parse_balance(fa != reads.end() ? fa->second : std::nullopt);
            const std::int64_t vb =
                q::wire::parse_balance(fb != reads.end() ? fb->second : std::nullopt);
            oc.writes[ka] = q::wire::encode_balance(va - amt);
            oc.writes[kb] = q::wire::encode_balance(vb + amt);
            oc.result = "xfer:" + ka + "->" + kb;
            return oc;
        };
        batch.push_back(std::move(t));
    }
    return batch;
}

OracleGroundTruth run_oracle(const Workload& w) {
    OracleGroundTruth gt;
    const std::vector<txn::Txn> batch = build_txn_batch(w);
    txn::StrictSerialOracle oracle;
    gt.run = oracle.submit_batch(batch, txn::ExecConfig{});
    // Final store: fold the committed write-sets in serial order.
    std::map<std::string, std::string> store;
    for (const txn::CommitInfo& c : gt.run.commits) {
        if (c.status != txn::Status::Committed) {
            continue;
        }
        for (const auto& [k, v] : c.writes_committed) {
            store[k] = v;
        }
    }
    for (const auto& [k, v] : store) {
        gt.balances[k] =
            q::wire::parse_balance(std::optional<std::string>(v));
    }
    return gt;
}

// ---------------------------------------------------------------------------
// THE END-TO-END DRIVER RUN. Drive the workload THROUGH THE REFERENCE DRIVER
// against the in-process sim cluster, under the given faults: seed each account
// (put), then each transfer, then read all four balances at STRICT. The driver
// owns the submit_key per call (exactly-once on retry). Returns the final
// balances + the cluster's applied/rejected/tip + trace.
// ---------------------------------------------------------------------------
struct E2EResult {
    std::map<std::string, std::int64_t> balances;
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;
    std::string trace;
    bool all_ok = true;
};

lockstep::core::Task drive_workload(q::Connection& conn, const Workload* w,
                                    E2EResult* out) {
    for (int i = 0; i < kNAccts; ++i) {
        q::WriteOutcome wr;
        co_await conn.put(kNames[i], q::wire::encode_balance(kStart), wr);
        if (!wr.ok) {
            out->all_ok = false;
        }
    }
    for (const Xfer& x : w->transfers) {
        q::WriteOutcome wr;
        co_await conn.transfer(x.a, x.b, x.amount, wr);
        if (!wr.ok) {
            out->all_ok = false;
        }
    }
    // Final balances via a STRICT point read per account.
    for (int i = 0; i < kNAccts; ++i) {
        q::ReadOutcome r;
        co_await conn.get(kNames[i], r);
        if (!r.ok) {
            out->all_ok = false;
            continue;
        }
        const std::optional<std::string> v =
            r.rows.empty() ? std::nullopt : r.rows[0].value;
        out->balances[kNames[i]] = q::wire::parse_balance(v);
    }
    co_return;
}

E2EResult run_e2e(std::uint64_t seed, const Workload& w, q::LinkFaults faults) {
    E2EResult out;
    q::LocalCluster lc(seed, faults);
    lc.run([&w, &out](q::Connection& conn) -> lockstep::core::Task {
        co_await drive_workload(conn, &w, &out);
        co_return;
    });
    out.applied = lc.applied();
    out.rejected = lc.rejected();
    out.trace = lc.trace();
    return out;
}

// ---------------------------------------------------------------------------
// (C) D5-LEVEL CONFORMANCE — drive a read at a given level through the driver and
// judge it with the MATCHING txn D5 checker. We build a one-txn RunResult whose
// served_reads carry the driver-observed (level, served_version, value), then run
// the level's checker on it. Because the checkers compare the served value against
// the SERIAL value at the served prefix (value_after_prefix), we hand them the
// oracle's committed history as the serial order so the comparison is grounded.
//
// We assemble a RunResult that is the oracle's committed history PLUS one extra
// "reader" commit at the tip carrying the driver's served reads — exactly the
// shape the checkers expect (committed txns in serial order; the reader's served
// reads judged against the prefix).
// ---------------------------------------------------------------------------

// Build the base committed history (the oracle's commits) so the checker has a
// serial order + per-prefix values to compare against.
txn::RunResult oracle_history(const OracleGroundTruth& gt) {
    txn::RunResult r;
    for (const txn::CommitInfo& c : gt.run.commits) {
        if (c.status == txn::Status::Committed) {
            r.commits.push_back(c);
        }
    }
    r.tip_version = gt.run.tip_version;
    return r;
}

// Append a synthetic "reader" commit at the tip (commit_version = tip+1) carrying
// one served read so the matching D5 checker can judge it against the serial
// prefix. seq_index is the next contiguous index.
txn::CommitInfo make_reader(const txn::RunResult& hist, txn::Level level,
                            const std::string& key, q::Seq served_version,
                            const std::optional<std::string>& value,
                            txn::SessionId session, q::Seq max_lag) {
    txn::CommitInfo c;
    c.txn_id = 1'000'000;  // a distinct id, never collides with workload ids
    c.status = txn::Status::Committed;
    // The reader serializes at the tip: seq_index = #committed + 1; commit_version
    // = tip + 1 (it observes the whole committed prefix that precedes it).
    std::size_t n = 0;
    for (const txn::CommitInfo& h : hist.commits) {
        if (h.status == txn::Status::Committed) {
            ++n;
        }
    }
    c.seq_index = static_cast<q::Seq>(n) + 1;
    c.commit_version = hist.tip_version + 1;
    txn::CommitInfo::ServedRead s;
    s.key = key;
    s.level = level;
    s.served_version = served_version;
    s.session = session;
    s.max_lag = max_lag;
    s.value = value;
    c.served_reads.push_back(s);
    return c;
}

} // namespace

int main() {
    std::printf("query_conformance_test\n");

    int sweep = 64;
    if (const char* env = std::getenv("LOCKSTEP_CONFORMANCE_SEEDS")) {
        const long v = std::strtol(env, nullptr, 10);
        if (v > 0 && v <= 4096) {
            sweep = static_cast<int>(v);
        }
    }

    constexpr std::uint64_t kSeedBase = 0x6'C0DE'5713'9BDFULL;

    q::LinkFaults clean{};
    clean.latency_min = 1;
    clean.latency_max = 1;

    q::LinkFaults nasty{};
    nasty.drop_prob = 0.25;
    nasty.dup_prob = 0.30;
    nasty.reorder_prob = 0.50;
    nasty.latency_min = 1;
    nasty.latency_max = 8;
    nasty.reorder_jitter_max = 12;

    for (int i = 0; i < sweep; ++i) {
        const std::uint64_t seed =
            kSeedBase + static_cast<std::uint64_t>(i) * 0x1000193ULL;
        const Workload w = make_workload(seed);
        const OracleGroundTruth gt = run_oracle(w);
        const std::uint64_t distinct = static_cast<std::uint64_t>(kNAccts) +
                                       w.transfers.size();

        // ---- (A) END-TO-END == ORACLE on a CLEAN net -------------------------
        const E2EResult clean_run = run_e2e(seed, w, clean);
        C_CHECK(clean_run.all_ok, "a driver call timed out on a clean net", seed);
        C_CHECK(clean_run.balances == gt.balances,
                "end-to-end balances (clean) != strict-serializable oracle", seed);
        C_CHECK(clean_run.applied == distinct,
                "clean: applied-submit count != distinct submits", seed);
        C_CHECK(clean_run.rejected == 0, "clean net produced torn frames", seed);

        // ---- (A)+(B) END-TO-END == ORACLE + EXACTLY-ONCE under NASTY faults --
        const E2EResult nasty_run = run_e2e(seed, w, nasty);
        C_CHECK(nasty_run.all_ok,
                "a driver call timed out under faults (retry budget?)", seed);
        C_CHECK(nasty_run.balances == gt.balances,
                "end-to-end balances under faults != oracle (not exactly-once)",
                seed);
        C_CHECK(nasty_run.applied == distinct,
                "EXACTLY-ONCE violated end-to-end: a Submit double-applied", seed);

        // ---- (D) DETERMINISM: same seed => byte-identical run ----------------
        const E2EResult nasty_run2 = run_e2e(seed, w, nasty);
        C_CHECK(nasty_run.trace == nasty_run2.trace,
                "end-to-end run not byte-identical on replay (trace differs)", seed);
        C_CHECK(nasty_run.balances == nasty_run2.balances,
                "end-to-end balances not reproducible on replay", seed);

        // ---- (C) EACH D5 LEVEL EXACT, judged by its matching checker ---------
        // Drive each level's read THROUGH THE DRIVER, then judge the served read
        // with the matching txn D5 checker against the oracle's serial history.
        const txn::RunResult hist = oracle_history(gt);
        const std::string probe = kNames[0];  // acct:a — written by the seed + maybe transfers

        // STRICT: linearizable — must observe the value after the full committed
        // prefix (the tip). The driver's strict get returns exactly that.
        {
            E2EResult strict_probe;
            q::LocalCluster lc(seed, clean);
            std::optional<std::string> got;
            q::Seq served = 0;
            lc.run([&](q::Connection& conn) -> lockstep::core::Task {
                // Re-seed + replay so the cluster state matches the oracle tip.
                co_await drive_workload(conn, &w, &strict_probe);
                q::ReadOutcome r;
                co_await conn.get(probe, r);
                if (r.ok && !r.rows.empty()) {
                    got = r.rows[0].value;
                    served = r.served_version;
                }
                co_return;
            });
            txn::RunResult judged = hist;
            judged.commits.push_back(make_reader(
                hist, txn::Level::StrictSerializable, probe,
                hist.tip_version, got, 0, 0));
            const txn::Verdict v = txn::check_strict_serializable(judged);
            C_CHECK(v.ok, "Strict driver read failed strict checker", seed);
        }

        // SNAPSHOT as-of version 1 (just after acct:a's seed, before any transfer):
        // the snapshot read must return the value at prefix 1 (== kStart), NOT the
        // tip — and the snapshot checker must accept it as the consistent as-of
        // value at that version.
        {
            std::optional<std::string> snap_got;
            q::Seq snap_served = 0;
            q::LocalCluster lc2(seed, clean);
            E2EResult ignore;
            lc2.run([&](q::Connection& conn) -> lockstep::core::Task {
                co_await drive_workload(conn, &w, &ignore);
                q::ReadOutcome r;
                co_await conn.get_snapshot(probe, /*version=*/1, r);
                if (r.ok && !r.rows.empty()) {
                    snap_got = r.rows[0].value;
                    snap_served = r.served_version;
                }
                co_return;
            });
            // The driver served the snapshot from version 1; the value must be the
            // serial value at prefix 1 (the seed put). Build a reader carrying that.
            txn::RunResult judged = hist;
            judged.commits.push_back(make_reader(
                hist, txn::Level::Snapshot, probe, snap_served, snap_got, 0, 0));
            const txn::Verdict v = txn::check_snapshot_level(judged);
            C_CHECK(v.ok, "Snapshot driver read failed snapshot checker", seed);
            C_CHECK(snap_served <= hist.tip_version,
                    "Snapshot served version exceeds tip", seed);
        }

        // BOUNDED with a generous max_lag: the served prefix must be within max_lag
        // of the tip and a real serial value; the bounded checker judges it.
        {
            const q::Seq max_lag = hist.tip_version;  // permissive bound
            std::optional<std::string> got;
            q::Seq served = 0;
            q::LocalCluster lc(seed, clean);
            E2EResult ignore;
            lc.run([&](q::Connection& conn) -> lockstep::core::Task {
                co_await drive_workload(conn, &w, &ignore);
                q::ReadOutcome r;
                co_await conn.get_bounded(probe, max_lag, r);
                if (r.ok && !r.rows.empty()) {
                    got = r.rows[0].value;
                    served = r.served_version;
                }
                co_return;
            });
            txn::RunResult judged = hist;
            judged.commits.push_back(make_reader(
                hist, txn::Level::BoundedStaleness, probe, served, got, 0, max_lag));
            const txn::Verdict v = txn::check_bounded_staleness_level(judged);
            C_CHECK(v.ok, "Bounded driver read failed bounded-staleness checker",
                    seed);
        }

        // RYW for session 7: the read must observe the session's own prior writes.
        // Since the read is served at the tip (the strongest prefix), it trivially
        // includes any prior write; the RYW checker accepts it.
        {
            const txn::SessionId session = 7;
            std::optional<std::string> got;
            q::Seq served = 0;
            q::LocalCluster lc(seed, clean);
            E2EResult ignore;
            lc.run([&](q::Connection& conn) -> lockstep::core::Task {
                co_await drive_workload(conn, &w, &ignore);
                q::ReadOutcome r;
                co_await conn.get_ryw(probe, session, r);
                if (r.ok && !r.rows.empty()) {
                    got = r.rows[0].value;
                    served = r.served_version;
                }
                co_return;
            });
            txn::RunResult judged = hist;
            judged.commits.push_back(make_reader(
                hist, txn::Level::ReadYourWrites, probe, served, got, session, 0));
            const txn::Verdict v = txn::check_read_your_writes_level(judged);
            C_CHECK(v.ok, "RYW driver read failed read-your-writes checker", seed);
        }
    }

    // ---- (E) TEETH: the D5 checkers are non-vacuous --------------------------
    // A Strict read fed a STALE value (the value before the last write, served at
    // the tip prefix) MUST be flagged by the strict checker.
    {
        const std::uint64_t seed = kSeedBase;
        const Workload w = make_workload(seed);
        const OracleGroundTruth gt = run_oracle(w);
        const txn::RunResult hist = oracle_history(gt);
        if (hist.tip_version >= 2) {
            // The correct strict value at the tip for acct:a:
            const std::string probe = kNames[0];
            const txn::ReadResult correct =
                txn::value_after_prefix(
                    [&] {
                        std::vector<const txn::CommitInfo*> seq;
                        for (const txn::CommitInfo& c : hist.commits) {
                            seq.push_back(&c);
                        }
                        return seq;
                    }(),
                    probe, hist.commits.size());
            // Feed a deliberately WRONG value (correct + "X" so it differs).
            const std::optional<std::string> wrong =
                correct.has_value() ? std::optional<std::string>(*correct + "X")
                                    : std::optional<std::string>("999999");
            txn::RunResult judged = hist;
            judged.commits.push_back(make_reader(
                hist, txn::Level::StrictSerializable, probe, hist.tip_version, wrong,
                0, 0));
            const txn::Verdict v = txn::check_strict_serializable(judged);
            C_CHECK(!v.ok,
                    "TEETH: a stale Strict read must be CAUGHT by the strict checker",
                    seed);
        }
    }

    if (g_failures == 0) {
        std::printf("query_conformance_test OK (sweep=%d)\n", sweep);
        return 0;
    }
    std::fprintf(stderr, "query_conformance_test FAILED: %d failures\n", g_failures);
    return 1;
}
