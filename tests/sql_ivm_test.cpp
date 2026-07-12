// sql_ivm_test.cpp — K5: INCREMENTAL MATERIALIZED VIEWS. THE ORACLE: after ANY
// seeded workload prefix, reading the incremental view returns EXACTLY what a full
// recompute of its source SELECT returns (the definition of correct maintenance) —
// across inserts, updates, deletes, group birth/death, a WHERE filter, txn batches,
// restarts, and REFRESH re-basing. Plus creation teeth for unmaintainable shapes.
#include <cstdio>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::string canon(const ExecResult& r) {
    std::string out = r.ok ? "ok:" : ("err:" + r.error);
    for (const auto& row : r.rows) {
        for (const auto& c : row.cells) out += c.first + "=" + c.second.render() + "|";
        out += "\n";
    }
    return out;
}
const char* kSrc =
    "SELECT cat, COUNT(*) AS n, SUM(amount) AS s FROM t WHERE amount > 10 GROUP BY cat";
void oracle(SqlEngine& e, const std::string& tag) {
    const std::string inc = canon(e.exec("SELECT cat, n, s FROM mv ORDER BY cat"));
    const std::string full = canon(e.exec(
        "SELECT cat, COUNT(*) AS n, SUM(amount) AS s FROM t WHERE amount > 10 "
        "GROUP BY cat ORDER BY cat"));
    check(inc == full, "ORACLE incremental == full recompute (" + tag + ")\n  inc=[" + inc +
                           "]\n  full=[" + full + "]");
}
}  // namespace

int main() {
    std::printf("=== sql_ivm_test (K5 incremental matviews) ===\n");
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock(sched);
    lockstep::sim::SeededRandom rng(0x5AA5ull);
    lockstep::sim::DiskFaultConfig dc;
    lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
    lockstep::sim::SeededRandom wr(7ull);
    {
        SqlEngine e(sched, data, sched, cat);
        e.exec("CREATE TABLE t (id INT, cat INT, amount INT, PRIMARY KEY (id))");

        // Teeth first: unmaintainable shapes are clean CREATE-time errors.
        check(!e.exec("CREATE INCREMENTAL MATERIALIZED VIEW bad1 AS SELECT cat, AVG(amount) "
                      "FROM t GROUP BY cat").ok,
              "AVG rejected with teaching error");
        check(!e.exec("CREATE INCREMENTAL MATERIALIZED VIEW bad2 AS SELECT cat, MIN(amount), "
                      "COUNT(*) FROM t GROUP BY cat").ok,
              "MIN rejected");
        check(!e.exec("CREATE INCREMENTAL MATERIALIZED VIEW bad3 AS SELECT cat, SUM(amount) "
                      "FROM t GROUP BY cat").ok,
              "missing COUNT(*) rejected");

        check(e.exec(std::string("CREATE INCREMENTAL MATERIALIZED VIEW mv AS ") + kSrc).ok,
              "CREATE INCREMENTAL MATERIALIZED VIEW");
        oracle(e, "empty");

        // Seeded workload: inserts / updates / deletes, group birth + death, filter edge.
        std::int64_t next = 1;
        std::vector<std::int64_t> live;
        for (int round = 0; round < 220; ++round) {
            const std::uint64_t dice = wr.next() % 100;
            if (dice < 55) {
                const std::int64_t id = next++;
                const std::int64_t c = static_cast<std::int64_t>(wr.next() % 5);
                const std::int64_t a = static_cast<std::int64_t>(wr.next() % 40);  // straddles >10
                if (e.exec("INSERT INTO t (id,cat,amount) VALUES (" + std::to_string(id) + "," +
                           std::to_string(c) + "," + std::to_string(a) + ")").ok) {
                    live.push_back(id);
                }
            } else if (dice < 75 && !live.empty()) {
                const std::int64_t id = live[wr.next() % live.size()];
                (void)e.exec("UPDATE t SET amount = " + std::to_string(wr.next() % 40) +
                             " WHERE id = " + std::to_string(id));
            } else if (dice < 90 && !live.empty()) {
                const std::size_t at = wr.next() % live.size();
                (void)e.exec("DELETE FROM t WHERE id = " + std::to_string(live[at]));
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(at));
            } else {  // transactional batch (atomic delta)
                (void)e.exec("BEGIN");
                const std::int64_t a1 = next++, a2 = next++;
                (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (" + std::to_string(a1) +
                             ",7,25)");
                (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (" + std::to_string(a2) +
                             ",7,5)");
                if (wr.next() % 2 == 0) {
                    (void)e.exec("COMMIT");
                    live.push_back(a1);
                    live.push_back(a2);
                } else {
                    (void)e.exec("ROLLBACK");
                    next -= 0;  // ids burned; harmless
                }
            }
            if (round % 17 == 0) oracle(e, "round " + std::to_string(round));
        }
        oracle(e, "storm end");

        // REFRESH still works on an incremental view and re-bases the cursor.
        check(e.exec("REFRESH MATERIALIZED VIEW mv").ok, "REFRESH on incremental view");
        oracle(e, "post-refresh");
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (99001, 3, 30)");
        oracle(e, "post-refresh + write");
    }

    // Restart: registry + cursor + backing rows are durable; maintenance continues.
    {
        SqlEngine e(sched, data, sched, cat);
        e.recover(data.logical_len(), cat.logical_len());
        oracle(e, "after restart");
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (99002, 9, 50)");   // new group
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (99003, 9, 11)");
        oracle(e, "after restart + new group");
        (void)e.exec("DELETE FROM t WHERE id = 99002");
        (void)e.exec("DELETE FROM t WHERE id = 99003");
        oracle(e, "after restart + group death");
    }

    // (K6 composition) LIVE FEED: a changefeed over the view delivers view DELTAS on
    // base-table commits — with NOBODY ever SELECTing the view (FETCH drives the
    // catch-up). This is the push-dashboard loop: LISTEN 'A' -> FETCH -> apply -> ACK.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, cat INT, amount INT, PRIMARY KEY (id))");
        check(e.exec(std::string("CREATE INCREMENTAL MATERIALIZED VIEW mv AS ") + kSrc).ok,
              "live: create view");
        check(e.exec("CREATE CHANGEFEED dash FOR mv").ok, "live: changefeed over the view");
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (1, 4, 20)");
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (2, 4, 30)");
        const ExecResult f1 = e.exec("FETCH dash");
        check(f1.ok && !f1.rows.empty(), "live: FETCH alone surfaces the view delta");
        bool saw = false;
        for (const auto& row : f1.rows) {
            std::string flat;
            for (const auto& c : row.cells) flat += c.first + "=" + c.second.render() + "|";
            if (flat.find("cat=4") != std::string::npos &&
                flat.find("n=2") != std::string::npos &&
                flat.find("s=50") != std::string::npos) {
                saw = true;
            }
        }
        check(saw, "live: the delta row is the maintained aggregate (cat=4, n=2, s=50)");
        const std::int64_t last = f1.rows.back().cells[0].second.i;
        check(e.exec("ACK CHANGEFEED dash AT " + std::to_string(last)).ok, "live: ack");
        check(e.exec("FETCH dash").rows.empty(), "live: quiet after ack");
        (void)e.exec("DELETE FROM t WHERE id = 1");
        (void)e.exec("DELETE FROM t WHERE id = 2");
        const ExecResult f2 = e.exec("FETCH dash");
        bool saw_death = false;
        for (const auto& row : f2.rows) {
            saw_death = saw_death || row.cells[1].second.s == "DELETE";
        }
        check(f2.ok && saw_death, "live: group death arrives as a DELETE delta");
        // DROP cleans the registry + cursor row.
        check(e.exec("DROP TABLE mv").ok, "live: drop view");
        check(e.exec("SELECT cur FROM __ivm WHERE name = 'mv'").rows.empty(),
              "live: durable cursor retired on drop");
    }

    // (K5.3) EAGER mode: after SET ivm.eager = 1 a base write maintains the view
    // immediately — the feed sees the delta with NO read and NO fetch in between.
    {
        SqlEngine e;
        e.exec("CREATE TABLE t (id INT, cat INT, amount INT, PRIMARY KEY (id))");
        (void)e.exec(std::string("CREATE INCREMENTAL MATERIALIZED VIEW mv AS ") + kSrc);
        check(e.exec("SET ivm.eager = 1").ok, "eager knob");
        (void)e.exec("INSERT INTO t (id,cat,amount) VALUES (1, 6, 20)");
        // Read the BACKING TABLE's feed directly — maintenance already happened.
        const ExecResult raw = e.exec("CHANGES mv SINCE 0");
        check(raw.ok && !raw.rows.empty(), "eager: view rows exist before any read");
        oracle(e, "eager");
        check(!e.exec("SET ivm.eager = 2").ok, "eager knob teeth");
    }

    // (K5.4 decision) SUM over REAL: refused BY DESIGN with the reason named — float
    // addition is order-dependent, so an incremental REAL sum cannot honor the
    // byte-exact oracle. The error must say so.
    {
        SqlEngine e;
        e.exec("CREATE TABLE r (id INT, cat INT, price REAL, PRIMARY KEY (id))");
        const ExecResult r = e.exec(
            "CREATE INCREMENTAL MATERIALIZED VIEW rv AS SELECT cat, COUNT(*) AS n, "
            "SUM(price) AS s FROM r GROUP BY cat");
        check(!r.ok && r.error.find("order-dependent") != std::string::npos,
              "SUM(REAL): deliberate refusal names the float-exactness reason");
    }

    if (g_fail != 0) { std::printf("sql_ivm_test: FAILURES\n"); return 1; }
    std::printf("sql_ivm_test: ALL PASS\n");
    return 0;
}
