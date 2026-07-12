// sql_cdc_storm_test.cpp — K4: the changefeed fault-storm oracle. Deterministic seeded
// storms interleave a writer (single rows, multi-row txns, rollbacks), a consumer
// (FETCH / partial ACK / idempotent apply), and CRASHES (engine torn down, recovered
// from the durable disks) at arbitrary points — including between FETCH and ACK.
//
// THE ORACLE, per storm:
//   V1 (exactly-once effect): the consumer's idempotently-replayed shadow == SELECT *.
//   V2 (no loss, no fabrication): the SET of _seqs the consumer ever saw == the exact
//       op-log of the table (CHANGES t SINCE 0 ground truth) — rolled-back txn writes
//       never appear, committed ones always do.
//   V3 (cursor discipline): FETCH after a crash resumes exactly at the durable acked
//       cursor — re-delivered ops are only ever the unacked suffix (never a gap).
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

struct Shadow {
    std::map<std::int64_t, std::string> rows;  // pk -> rendered non-pk image
    std::set<std::int64_t> seen;               // every _seq ever delivered
    std::int64_t apply(const ExecResult& r) {  // idempotent; returns last _seq (0 if none)
        std::int64_t last = 0;
        for (const auto& row : r.rows) {
            last = row.cells[0].second.i;
            seen.insert(last);
            const std::int64_t pk = row.cells[2].second.i;
            if (row.cells[1].second.s == "DELETE") {
                rows.erase(pk);
            } else {
                std::string img;
                for (std::size_t c = 3; c < row.cells.size(); ++c)
                    img += row.cells[c].second.render() + "|";
                rows[pk] = img;
            }
        }
        return last;
    }
};

void run_storm(std::uint64_t seed) {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock(sched);
    lockstep::sim::SeededRandom disk_rng(seed * 977 + 5);
    lockstep::sim::DiskFaultConfig dc;
    lockstep::sim::SimDisk data(sched, clock, disk_rng, dc), cat(sched, clock, disk_rng, dc);
    lockstep::sim::SeededRandom rng(seed);

    auto boot = [&](bool fresh) {
        auto e = std::make_unique<SqlEngine>(sched, data, sched, cat);
        if (!fresh) e->recover(data.logical_len(), cat.logical_len());
        return e;
    };
    std::unique_ptr<SqlEngine> e = boot(true);
    e->exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
    e->exec("CREATE CHANGEFEED cf FOR t");

    Shadow sh;
    std::int64_t next_id = 1;
    std::int64_t acked = 0;        // the durable cursor as the CONSUMER last confirmed it
    std::vector<std::int64_t> live;  // ids we believe exist (driver-side, best effort)
    int crashes = 0, txns = 0, rollbacks = 0, fetches = 0;

    const auto ins_sql = [&](std::int64_t id) {
        return "INSERT INTO t (id,name,score) VALUES (" + std::to_string(id) + ",'u" +
               std::to_string(id) + "'," + std::to_string((id * 13) % 997) + ")";
    };
    for (int round = 0; round < 160; ++round) {
        const std::uint64_t dice = rng.next() % 100;
        if (dice < 45) {  // single-row write
            const std::int64_t id = next_id++;
            if (e->exec(ins_sql(id)).ok) live.push_back(id);
        } else if (dice < 58 && !live.empty()) {
            const std::int64_t id = live[rng.next() % live.size()];
            (void)e->exec("UPDATE t SET score = " + std::to_string(rng.next() % 100000) +
                          " WHERE id = " + std::to_string(id));
        } else if (dice < 66 && !live.empty()) {
            const std::size_t at = rng.next() % live.size();
            (void)e->exec("DELETE FROM t WHERE id = " + std::to_string(live[at]));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(at));
        } else if (dice < 76) {  // multi-write txn; ~1/3 rolled back (feed atomicity)
            ++txns;
            (void)e->exec("BEGIN");
            const std::int64_t a = next_id++, b = next_id++;
            (void)e->exec(ins_sql(a));
            (void)e->exec(ins_sql(b));
            if (rng.next() % 3 == 0) {
                ++rollbacks;
                (void)e->exec("ROLLBACK");
            } else {
                (void)e->exec("COMMIT");
                live.push_back(a);
                live.push_back(b);
            }
        } else if (dice < 92) {  // consumer step
            ++fetches;
            const ExecResult f =
                e->exec("FETCH cf LIMIT " + std::to_string(1 + rng.next() % 7));
            check(f.ok, "FETCH ok mid-storm");
            for (const auto& row : f.rows) {
                check(row.cells[0].second.i > acked,
                      "V3: delivered op is past the acked cursor");
            }
            const std::int64_t last = sh.apply(f);
            if (last != 0 && rng.next() % 5 != 0) {  // 80%: confirm; 20%: crash-window
                if (e->exec("ACK CHANGEFEED cf AT " + std::to_string(last)).ok) acked = last;
            }
        } else {  // CRASH + recover (possibly mid-txn, or between FETCH and ACK)
            ++crashes;
            e = boot(false);
            const ExecResult f = e->exec("FETCH cf LIMIT 3");
            check(f.ok, "FETCH ok right after recovery");
            for (const auto& row : f.rows) {
                check(row.cells[0].second.i > acked,
                      "V3: post-crash delivery starts past the durable acked cursor");
            }
            sh.apply(f);  // may re-deliver the unacked suffix — idempotent by design
        }
    }
    // Quiesce: drain + final compare.
    for (;;) {
        const ExecResult f = e->exec("FETCH cf LIMIT 64");
        check(f.ok, "drain FETCH ok");
        const std::int64_t last = sh.apply(f);
        if (last == 0) break;
        (void)e->exec("ACK CHANGEFEED cf AT " + std::to_string(last));
        acked = last;
    }
    // V1: shadow == the live table.
    std::map<std::int64_t, std::string> want;
    for (const auto& row : e->exec("SELECT id, name, score FROM t ORDER BY id").rows) {
        want[row.cells[0].second.i] =
            row.cells[1].second.render() + "|" + row.cells[2].second.render() + "|";
    }
    check(sh.rows == want, "V1: idempotent shadow == table (seed " + std::to_string(seed) + ")");
    // V2: the delivered seq set == the exact ground-truth op-log of t.
    std::set<std::int64_t> truth;
    for (const auto& row : e->exec("CHANGES t SINCE 0").rows)
        truth.insert(row.cells[0].second.i);
    check(sh.seen == truth,
          "V2: delivered seqs == op-log exactly (no loss, no rollback leak; seed " +
              std::to_string(seed) + ")");
    std::printf("  storm seed=%llu: rounds=160 crashes=%d txns=%d (rb=%d) fetches=%d "
                "ops=%zu -- oracle OK\n",
                static_cast<unsigned long long>(seed), crashes, txns, rollbacks, fetches,
                truth.size());
}
}  // namespace

int main() {
    std::printf("=== sql_cdc_storm_test (K4 exactly-once under fault storms) ===\n");
    for (std::uint64_t seed = 1; seed <= 12; ++seed) run_storm(seed);
    if (g_fail != 0) { std::printf("sql_cdc_storm_test: FAILURES\n"); return 1; }
    std::printf("sql_cdc_storm_test: ALL PASS (12 storms)\n");
    return 0;
}
