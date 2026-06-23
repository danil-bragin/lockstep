// bank_example.cpp — Phase 6 Stage B, C6.6. A WORKED EXAMPLE using the DRIVER.
//
// Source of truth: briefs/phase6.md C6.6 ("a worked example — a small app using
// the driver"). This is a tiny, self-contained bank app written ENTIRELY against
// the C6.4 reference Driver (Driver.hpp) — the way an external developer would use
// Lockstep. It opens an in-process connection, seeds two accounts, moves money
// (exactly-once, automatically), and reads balances at several call-site-visible
// D5 levels — showing what each level returns and why.
//
// It demonstrates the WHOLE developer story end to end:
//   * connect()                 — open a connection (driver over the wire stub)
//   * conn.put / conn.transfer  — submit one-shot ops; exactly-once is automatic
//   * conn.get<Level>           — typed reads; the D5 level is call-site-visible
//   * the result/error values   — value-shaped outcomes, no protocol leakage
//
// DETERMINISM: a pure function of a fixed seed. Same build => byte-identical
// output. NO wall-clock, NO threads. This is an exe the gate BUILDS (proving the
// driver surface is usable + compiles clean); it also runs trivially fast.

#include <cstdint>
#include <cstdio>

#include <lockstep/core/Task.hpp>

#include <lockstep/query/Driver.hpp>
#include <lockstep/query/LocalCluster.hpp>

namespace {

namespace q = lockstep::query;

[[nodiscard]] std::string show(const std::optional<std::string>& v) {
    return v.has_value() ? *v : std::string("<nil>");
}

// The bank app, written as a driver program over one Connection.
lockstep::core::Task bank_app(q::Connection& conn) {
    std::printf("== Lockstep bank example (reference driver) ==\n");

    // 1) Seed two accounts. Each put is a one-shot submit; the driver owns the
    //    submit-key, so a dropped reply would retry the SAME key (exactly-once).
    q::WriteOutcome w;
    co_await conn.put("acct:alice", "100", w);
    std::printf("seed alice=100 -> committed=%d (v%llu)\n", w.committed,
                static_cast<unsigned long long>(w.commit_version));
    co_await conn.put("acct:bob", "100", w);
    std::printf("seed bob=100   -> committed=%d (v%llu)\n", w.committed,
                static_cast<unsigned long long>(w.commit_version));

    // 2) Move 30 from alice to bob. The server reads both balances at strict and
    //    writes alice-30 / bob+30 atomically, in the agreed serial order.
    co_await conn.transfer("acct:alice", "acct:bob", 30, w);
    std::printf("transfer 30 alice->bob -> committed=%d (v%llu, result=\"%s\")\n",
                w.committed, static_cast<unsigned long long>(w.commit_version),
                w.result.c_str());

    // 3) Read both balances at the STRICT (default) level — the strongest contract,
    //    so it reflects every committed write up to now.
    q::ReadOutcome r;
    co_await conn.get("acct:alice", r);
    std::printf("get alice [strict] = %s (served v%llu)\n",
                r.rows.empty() ? "<nil>" : show(r.rows[0].value).c_str(),
                static_cast<unsigned long long>(r.served_version));
    co_await conn.get("acct:bob", r);
    std::printf("get bob   [strict] = %s (served v%llu)\n",
                r.rows.empty() ? "<nil>" : show(r.rows[0].value).c_str(),
                static_cast<unsigned long long>(r.served_version));

    // 4) Read alice at a SNAPSHOT as-of version 1 (just after the alice seed, before
    //    the transfer) — the level is on the TYPE, so the contract is call-site-
    //    visible. A snapshot read can legitimately return an OLDER value.
    co_await conn.get_snapshot("acct:alice", /*version=*/1, r);
    std::printf("get alice [snapshot@v1] = %s (served v%llu) <- intentionally older\n",
                r.rows.empty() ? "<nil>" : show(r.rows[0].value).c_str(),
                static_cast<unsigned long long>(r.served_version));

    // 5) A scan over the account range, strict level.
    co_await conn.scan("acct:a", "acct:z", r);
    std::printf("scan [acct:a,acct:z) [strict] (served v%llu):\n",
                static_cast<unsigned long long>(r.served_version));
    if (!r.ranges.empty()) {
        for (const auto& [k, v] : r.ranges[0].rows) {
            std::printf("    %s = %s\n", k.c_str(), v.c_str());
        }
    }

    std::printf("== done ==\n");
    co_return;
}

}  // namespace

int main() {
    // A fixed seed: the whole example is a pure function of it (deterministic).
    q::LocalCluster lc(/*seed=*/42);
    lc.run([](q::Connection& conn) -> lockstep::core::Task {
        co_await bank_app(conn);
        co_return;
    });
    std::printf("(cluster: applied=%llu rejected=%llu tip=v%llu)\n",
                static_cast<unsigned long long>(lc.applied()),
                static_cast<unsigned long long>(lc.rejected()),
                static_cast<unsigned long long>(lc.tip()));
    return 0;
}
