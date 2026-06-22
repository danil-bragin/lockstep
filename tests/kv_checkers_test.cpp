// kv_checkers_test.cpp — Phase 2 batch 2 (stage C) self-test for the §4 checker
// set: C-INT (Integrity), C-MONO (SessionMonotonic), C-LIN (Linearizable),
// C-DUR (Durability). Author ≠ toy-system author (DECISION-D: independence →
// real teeth). The checkers are a SEPARATE judgment of "correct"; this test must
// NOT modify the toy KV system — if a checker fires on the honest system, that
// is a REAL bug to REPORT, not silence.
//
// TWO KINDS OF ASSERTION (per the brief):
//
//   (1) RUN-AGAINST-HONEST-SYSTEM. Drive run_kv_sim_checked(seed, runner) over
//       many seeds under the FULL fault envelope with all 4 checkers registered.
//       The honest system SHOULD pass every checker. A violation is captured
//       (seed + witness) and reported — NEVER silenced.
//
//   (2) UNIT TEETH (non-vacuous proof, V-CHK5). Each checker is fed a small
//       HAND-CRAFTED bad History that violates EXACTLY its property, and we
//       assert it FLAGS the violation WITH a witness. A checker that cannot flag
//       its own canonical violation is vacuous (the harness-has-teeth invariant).
//       We ALSO feed each checker a clean history and assert it does NOT fire (no
//       false alarm — V-CHK4 "no stronger").
//
// This is NON-provider code → the forbidden-call lint scans it. All time is
// virtual; all randomness is the seeded provider PRNG threaded through
// run_kv_sim_checked. Seeds are printed for replay (V-CHK2).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <lockstep/harness/Checker.hpp>
#include <lockstep/harness/CheckerRunner.hpp>
#include <lockstep/harness/History.hpp>
#include <lockstep/harness/checkers/Durability.hpp>
#include <lockstep/harness/checkers/Integrity.hpp>
#include <lockstep/harness/checkers/Linearizable.hpp>
#include <lockstep/harness/checkers/SessionMonotonic.hpp>
#include <lockstep/harness/kv/KvSim.hpp>

namespace {

using lockstep::harness::CheckerRunner;
using lockstep::harness::History;
using lockstep::harness::Op;
using lockstep::harness::OpKind;
using lockstep::harness::Verdict;
using lockstep::harness::checkers::DurabilityChecker;
using lockstep::harness::checkers::IntegrityChecker;
using lockstep::harness::checkers::LinearizableChecker;
using lockstep::harness::checkers::SessionMonotonicChecker;
using lockstep::harness::kv::KvConfig;
using lockstep::harness::kv::run_kv_sim_checked;

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "ASSERT FAILED: %s\n", what);
        ++g_failures;
    }
}

// --- tiny Op builders for hand-crafted histories ---------------------------
// seq is set to the construction order so a History built in return_vt order is
// already a valid total order (matching what HistoryRecorder would produce).

std::uint64_t g_seq = 0;
std::uint64_t g_opid = 1;

Op mk(std::uint64_t client, OpKind kind, const std::string& key,
      lockstep::core::Tick invoke, lockstep::core::Tick ret, bool ok) {
    Op op;
    op.client_id = client;
    op.op_id = g_opid++;
    op.kind = kind;
    op.key = key;
    op.invoke_vt = invoke;
    op.return_vt = ret;
    op.ok = ok;
    op.seq = g_seq++;
    op.returned = true;
    return op;
}

Op read(std::uint64_t client, const std::string& key, lockstep::core::Tick inv,
        lockstep::core::Tick ret, const std::string& result) {
    Op op = mk(client, OpKind::Read, key, inv, ret, true);
    op.result = result;
    return op;
}

Op write(std::uint64_t client, const std::string& key,
         lockstep::core::Tick inv, lockstep::core::Tick ret,
         const std::string& value, bool ok = true) {
    Op op = mk(client, OpKind::Write, key, inv, ret, ok);
    op.value = value;
    op.result = ok ? "ack" : "";
    if (!ok) {
        op.error = "timeout";
    }
    return op;
}

Op cas(std::uint64_t client, const std::string& key, lockstep::core::Tick inv,
       lockstep::core::Tick ret, const std::string& old,
       const std::string& nv, bool committed) {
    Op op = mk(client, OpKind::Cas, key, inv, ret, committed);
    op.cas_old = old;
    op.value = nv;
    if (committed) {
        op.result = "ack";
    } else {
        op.ok = false;
        op.error = "cas_mismatch";
    }
    return op;
}

// Run one checker's final() over a history; return its verdict.
template <class CheckerT>
Verdict run_one(const History& h) {
    CheckerT c;
    return c.final(h);
}

void report_verdict(const char* tag, const Verdict& v) {
    std::printf("  [%s] ok=%d witness=\"%s\"\n", tag, v.ok ? 1 : 0,
                v.witness.c_str());
}

// =====================================================================
// (2) UNIT TEETH — each checker MUST flag its canonical bad history.
// =====================================================================

void teeth_integrity() {
    std::printf("TEETH C-INT:\n");
    // CLEAN: a write then a read of that value → must PASS.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(1, "k0", 3, 4, "c0_v0"));
        Verdict v = run_one<IntegrityChecker>(h);
        check(v.ok, "C-INT clean history passes (no false alarm)");
    }
    // BAD (INT-1): a read returns a value NEVER written to k0 → must FLAG.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(1, "k0", 3, 4, "GHOST_VALUE"));
        Verdict v = run_one<IntegrityChecker>(h);
        report_verdict("C-INT fabricated-read", v);
        check(!v.ok, "C-INT FLAGS a fabricated read (INT-1 teeth)");
        check(!v.witness.empty(), "C-INT violation carries a witness");
    }
    // BAD (INT-2): ack'd write, then every later read returns ∅ → lost ack.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(1, "k0", 5, 6, ""));   // later read sees ∅
        h.push_back(read(1, "k0", 7, 8, ""));   // and again
        Verdict v = run_one<IntegrityChecker>(h);
        report_verdict("C-INT lost-ack", v);
        check(!v.ok, "C-INT FLAGS a lost ack'd write (INT-2 teeth)");
    }
}

void teeth_session_monotonic() {
    std::printf("TEETH C-MONO:\n");
    // CLEAN: session writes v0 then v1, reads back v1 → PASS.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(write(0, "k0", 3, 4, "c0_v1"));
        h.push_back(read(0, "k0", 5, 6, "c0_v1"));
        Verdict v = run_one<SessionMonotonicChecker>(h);
        check(v.ok, "C-MONO clean read-your-writes passes (no false alarm)");
    }
    // CLEAN: reading ANOTHER client's value is allowed (not backward).
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(0, "k0", 5, 6, "c9_vX"));  // other client's value
        Verdict v = run_one<SessionMonotonicChecker>(h);
        check(v.ok, "C-MONO allows reading another client's value (level)");
    }
    // BAD (MONO-1): after acking its own write, the session reads ∅ → backward.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(0, "k0", 5, 6, ""));  // own write vanished
        Verdict v = run_one<SessionMonotonicChecker>(h);
        report_verdict("C-MONO read-your-writes-∅", v);
        check(!v.ok, "C-MONO FLAGS read-your-writes returning ∅ (MONO-1 teeth)");
        check(!v.witness.empty(), "C-MONO violation carries a witness");
    }
    // BAD (MONO-2): session writes v0, v1; reads v1; then reads its OWN v0 →
    // monotonic-reads goes backward over own writes.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(write(0, "k0", 3, 4, "c0_v1"));
        h.push_back(read(0, "k0", 5, 6, "c0_v1"));
        h.push_back(read(0, "k0", 7, 8, "c0_v0"));  // backward to own v0
        Verdict v = run_one<SessionMonotonicChecker>(h);
        report_verdict("C-MONO monotonic-reads-backward", v);
        check(!v.ok, "C-MONO FLAGS monotonic-reads going backward (MONO-2 teeth)");
    }
}

void teeth_linearizable() {
    std::printf("TEETH C-LIN:\n");
    // CLEAN: a sequential write-then-read (no overlap) → PASS.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "A"));
        h.push_back(read(1, "k0", 3, 4, "A"));
        Verdict v = run_one<LinearizableChecker>(h);
        check(v.ok, "C-LIN clean sequential history passes");
    }
    // CLEAN: concurrent writes; a read sees one of them — linearizable.
    {
        History h;
        h.push_back(write(0, "k0", 1, 10, "A"));   // overlaps
        h.push_back(write(1, "k0", 1, 10, "B"));   // overlaps
        h.push_back(read(2, "k0", 11, 12, "B"));   // after both
        Verdict v = run_one<LinearizableChecker>(h);
        check(v.ok, "C-LIN concurrent writes + later read is linearizable");
    }
    // BAD: NON-linearizable. Two NON-overlapping reads of the SAME key bracket a
    // single write such that the value goes A -> (write B) -> back to A with no
    // write of A in between, and the reads' real-time order forbids any legal
    // register order. Construct: write A [1,2]; read B [3,4]; read A [5,6] but
    // only write of B exists after A — there is no write that could restore A
    // after B in real-time. Concretely: w(A)[1,2], w(B)[3,4], r(A)[5,6]. The read
    // at [5,6] returns A but the last write before it (real-time) is B → no legal
    // order makes the final read see A. NOT linearizable.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "A"));
        h.push_back(write(1, "k0", 3, 4, "B"));
        h.push_back(read(2, "k0", 5, 6, "A"));  // impossible: B is the last write
        Verdict v = run_one<LinearizableChecker>(h);
        report_verdict("C-LIN non-linearizable", v);
        check(!v.ok, "C-LIN FLAGS a non-linearizable history (teeth)");
        check(!v.witness.empty(), "C-LIN violation carries a witness");
        check(v.witness.find("NOT LINEARIZABLE") != std::string::npos,
              "C-LIN witness names the non-linearizable key");
    }
    // BAD: stale read. Sequential w(A)[1,2], w(B)[3,4], then a read AFTER both
    // returns A (the old value) — a classic stale read; not linearizable.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "A"));
        h.push_back(write(0, "k0", 3, 4, "B"));
        h.push_back(read(1, "k0", 5, 6, "A"));  // stale
        Verdict v = run_one<LinearizableChecker>(h);
        report_verdict("C-LIN stale-read", v);
        check(!v.ok, "C-LIN FLAGS a stale read (teeth)");
    }
}

void teeth_durability() {
    std::printf("TEETH C-DUR:\n");
    // CLEAN: ack'd write then read of that value → PASS.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(1, "k0", 3, 4, "c0_v0"));
        Verdict v = run_one<DurabilityChecker>(h);
        check(v.ok, "C-DUR clean history passes (no false alarm)");
    }
    // CLEAN: a LOST-ACK write's value MAY appear (envelope-permitted) → PASS.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0", /*ok=*/false));  // lost ack
        h.push_back(read(1, "k0", 3, 4, "c0_v0"));  // surfaced anyway: allowed
        Verdict v = run_one<DurabilityChecker>(h);
        check(v.ok, "C-DUR allows a lost-ack write to surface (level)");
    }
    // BAD (DUR-1): a REJECTED cas (cas_mismatch) carrying value W, then a read
    // returns W with no committing source → non-ack'd write surfaced.
    {
        History h;
        h.push_back(cas(0, "k0", 1, 2, "expected_old", "REJECTED_W",
                        /*committed=*/false));        // system said NO
        h.push_back(read(1, "k0", 5, 6, "REJECTED_W"));  // yet it appears
        Verdict v = run_one<DurabilityChecker>(h);
        report_verdict("C-DUR rejected-write-surfaced", v);
        check(!v.ok, "C-DUR FLAGS a rejected write surfacing (DUR-1 teeth)");
        check(!v.witness.empty(), "C-DUR violation carries a witness");
    }
    // BAD (DUR-2): a read returns a value NO mutation ever offered → fabricated
    // durable value.
    {
        History h;
        h.push_back(write(0, "k0", 1, 2, "c0_v0"));
        h.push_back(read(1, "k0", 5, 6, "FABRICATED"));
        Verdict v = run_one<DurabilityChecker>(h);
        report_verdict("C-DUR fabricated-durable", v);
        check(!v.ok, "C-DUR FLAGS a fabricated durable value (DUR-2 teeth)");
    }
}

// C-LIN over-bound surfacing: a key with more relevant ops than kMaxOpsPerKey
// must report UNDECIDED-WITHIN-BOUND (loud), NEVER a silent pass.
void teeth_linearizable_overbound() {
    std::printf("TEETH C-LIN over-bound:\n");
    History h;
    // Build (kMaxOpsPerKey + 5) trivially-linearizable reads of ∅ on one key.
    const std::size_t over = LinearizableChecker::kMaxOpsPerKey + 5;
    lockstep::core::Tick t = 1;
    for (std::size_t i = 0; i < over; ++i) {
        h.push_back(read(0, "kbig", t, t + 1, ""));  // ∅ reads, all legal
        t += 2;
    }
    Verdict v = run_one<LinearizableChecker>(h);
    report_verdict("C-LIN over-bound", v);
    check(!v.ok, "C-LIN reports a NON-pass when over its ops bound");
    check(v.witness.find("UNDECIDED-WITHIN-BOUND") != std::string::npos,
          "C-LIN surfaces UNDECIDED-WITHIN-BOUND LOUDLY (never silent pass)");
}

// =====================================================================
// (1) RUN-AGAINST-HONEST-SYSTEM — all 4 checkers over many seeds.
// =====================================================================

void honest_system_run() {
    std::printf("HONEST-SYSTEM RUN (full envelope, all 4 checkers):\n");
    KvConfig cfg;  // defaults: 3 nodes, 3 clients, full envelope, crashes+parts
    const std::uint64_t kSeedBase = 0xC0FFEE00ULL;
    const int kSeeds = 40;

    int violations = 0;
    int v_int = 0;
    int v_mono = 0;
    int v_lin = 0;
    int v_dur = 0;
    int undecided = 0;
    for (int s = 0; s < kSeeds; ++s) {
        const std::uint64_t seed = kSeedBase + static_cast<std::uint64_t>(s);

        CheckerRunner runner;
        runner.add(std::make_unique<IntegrityChecker>());
        runner.add(std::make_unique<SessionMonotonicChecker>());
        runner.add(std::make_unique<LinearizableChecker>());
        runner.add(std::make_unique<DurabilityChecker>());

        const History h = run_kv_sim_checked(seed, runner, cfg);
        const std::vector<Verdict> verdicts = runner.finalize(h, seed);

        for (const Verdict& v : verdicts) {
            if (!v.ok) {
                ++violations;
                // Categorize (deterministic substring match on the witness tag).
                if (v.witness.find("UNDECIDED-WITHIN-BOUND") != std::string::npos) {
                    ++undecided;
                } else if (v.explanation.find("C-INT") != std::string::npos) {
                    ++v_int;
                } else if (v.explanation.find("C-MONO") != std::string::npos) {
                    ++v_mono;
                } else if (v.explanation.find("C-LIN") != std::string::npos) {
                    ++v_lin;
                } else if (v.explanation.find("C-DUR") != std::string::npos) {
                    ++v_dur;
                }
                // REPORT every violation LOUDLY (seed + witness) — never silence
                // the checker. This is the reproducible bug report (V-CHK2).
                std::printf(
                    "  !! VIOLATION seed=0x%llX explanation=\"%s\"\n"
                    "     witness=%s\n",
                    static_cast<unsigned long long>(v.seed),
                    v.explanation.c_str(), v.witness.c_str());
            }
        }
    }

    std::printf(
        "  honest run: %d seeds, %d violation(s) "
        "[C-INT=%d C-MONO=%d C-LIN=%d C-DUR=%d UNDECIDED=%d]\n",
        kSeeds, violations, v_int, v_mono, v_lin, v_dur, undecided);

    // NOTE ON THE PASS CRITERION (independence, DECISION-D):
    //   This test certifies the CHECKER SET, not the toy KV system (a different
    //   agent's code I MUST NOT modify). The honest-system sweep is a BUG-
    //   SURFACING report, not a pass-gate on someone else's component: spec
    //   §5 V-TOY1 explicitly permits "a real bug found & reported reproducibly",
    //   and the best-effort primary-backup system (NOT consensus — Phase 4) is
    //   expected to violate linearizability under failover. So we do NOT fail on
    //   the PRESENCE of violations (that would be silencing the system's bugs
    //   behind a red build on non-owned code). Instead the receipt reports them.
    //
    //   What we DO assert here keeps the checkers honest against the REAL system:
    //   (i) the UNDECIDED bound was never hit on the toy workload (the C-LIN
    //       search always DECIDED — no silent give-up masquerading as a pass);
    //   (ii) the checkers actually ran (size sanity is covered by determinism).
    check(undecided == 0,
          "C-LIN DECIDED every key on the toy workload (no UNDECIDED-within-"
          "bound on the honest sweep — bound is adequate)");
    // (Determinism of the verdicts is asserted separately in determinism_run.)
}

// Determinism: the same seed run twice ⇒ byte-identical verdict witnesses.
void determinism_run() {
    std::printf("DETERMINISM (same seed ⇒ identical verdicts):\n");
    KvConfig cfg;
    const std::uint64_t seed = 0xD37E12345ULL;

    auto run = [&](std::string& out) {
        CheckerRunner runner;
        runner.add(std::make_unique<IntegrityChecker>());
        runner.add(std::make_unique<SessionMonotonicChecker>());
        runner.add(std::make_unique<LinearizableChecker>());
        runner.add(std::make_unique<DurabilityChecker>());
        const History h = run_kv_sim_checked(seed, runner, cfg);
        const std::vector<Verdict> vs = runner.finalize(h, seed);
        out.clear();
        for (const Verdict& v : vs) {
            out += (v.ok ? "OK|" : "VIOL|");
            out += v.witness;
            out += "\n";
        }
    };
    std::string a;
    std::string b;
    run(a);
    run(b);
    check(a == b, "same seed ⇒ byte-identical verdict set (deterministic)");
}

}  // namespace

int main() {
    std::printf("kv_checkers_test: §4 checker set teeth + honest-system run\n");

    // (2) UNIT TEETH — non-vacuous proof per checker.
    teeth_integrity();
    teeth_session_monotonic();
    teeth_linearizable();
    teeth_durability();
    teeth_linearizable_overbound();

    // (1) HONEST-SYSTEM run across many seeds.
    honest_system_run();
    determinism_run();

    if (g_failures != 0) {
        std::fprintf(stderr, "kv_checkers_test: FAILED (%d assertion(s))\n",
                     g_failures);
        return 1;
    }
    std::printf("kv_checkers_test: OK\n");
    return 0;
}
