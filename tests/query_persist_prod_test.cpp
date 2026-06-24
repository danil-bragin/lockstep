// query_persist_prod_test.cpp — Phase 7 S5a closure, the PROD (real-file) half of
// the query-path persistence gate. [LINUX ONLY — uses prod::ProdDisk.]
//
// Proves the query-side durability seam on a REAL on-disk file (prod::ProdDisk over
// a scratch dir), NETWORKING-FREE (drives query::Database directly via the
// disk-injection seam — NO ProdReactor / TCP). This is the prod analogue of
// query_persist_test (which uses sim::SimDisk):
//
//   (A) write committed write-sets via the durable query path
//       (Database(disk_sched, prodDisk)::apply_committed — WAL'd + fdatasync'd to
//       the real file), read them back LIVE.
//   (B) "CRASH": drop the in-memory Database AND close the ProdDisk fd (the file
//       survives on disk). Reopen a FRESH ProdDisk over the SAME path + a FRESH
//       Database, recover(logical_len), and assert EVERY committed value is
//       queryable AFTER recovery — recovered from the real query-side FILE, NOT by
//       replaying any consensus log.
//
// The persistent engine runs on disk_sched (the ProdDisk's own scheduler), PUMPED
// INLINE by the query layer (apply_committed/run/recover call sched.run()) — it does
// NOT depend on the reactor. ProdDisk does synchronous inline IO, so the WAL
// append+sync completes within apply_committed. Bounded; RAII closes every fd.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>

#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/prod/ProdScratchDir.hpp>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/Query.hpp>

#include <lockstep/txn/Transaction.hpp>

namespace {

namespace q = lockstep::query;
namespace txn = lockstep::txn;
namespace prod = lockstep::prod;
namespace core = lockstep::core;

int g_failures = 0;

#define P_CHECK(cond, msg)                                                       \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "query_persist_prod_test FAIL [%s:%d]: %s\n",   \
                         __FILE__, __LINE__, (msg));                             \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

std::optional<std::string> read_key(q::Database& db, const std::string& key) {
    q::Query<q::Strict> query;
    query.get(key);
    const q::QueryResult r = db.run(query);
    if (r.points.empty()) {
        return std::nullopt;
    }
    return r.points[0].value;
}

}  // namespace

int main() {
    std::printf("[query_persist_prod_test] Phase 7 S5a — query state durable on a "
                "REAL ProdDisk file; recovers across a crash (networking-free)\n\n");

    prod::ProdScratchDir scratch("query_persist_prod");
    if (!scratch.ok()) {
        std::fprintf(stderr, "[query_persist_prod_test] FAILED to make scratch dir\n");
        return 1;
    }
    const std::string wal_path = scratch.path() + "/query.wal";

    std::vector<txn::WriteSet> history;
    history.push_back({{"acct:a", "100"}, {"acct:b", "50"}});
    history.push_back({{"acct:a", "90"}, {"acct:b", "60"}});  // a transfer of 10
    history.push_back({{"acct:c", "777"}});

    std::uint64_t durable_len = 0;

    // Phase 1: write the committed state durably to the real file + read it live.
    {
        core::Scheduler disk_sched;
        prod::ProdDisk disk(disk_sched, wal_path);
        P_CHECK(disk.valid(), "ProdDisk over the scratch file must open");

        q::Database db(disk_sched, disk);
        for (const txn::WriteSet& ws : history) {
            (void)db.apply_committed(ws);
        }
        P_CHECK(read_key(db, "acct:a") == std::optional<std::string>("90"),
                "live: acct:a should be 90");
        P_CHECK(read_key(db, "acct:b") == std::optional<std::string>("60"),
                "live: acct:b should be 60");
        P_CHECK(read_key(db, "acct:c") == std::optional<std::string>("777"),
                "live: acct:c should be 777");
        P_CHECK(db.tip() == 3, "live tip should be 3 committed write-sets");

        durable_len = disk.logical_len();
        P_CHECK(durable_len > 0, "the real file must hold durable WAL bytes");
    }  // db + disk destroyed: the in-memory state is GONE and the fd is CLOSED;
       // only the on-disk file survives — the "crash".

    // Phase 2: reopen a FRESH ProdDisk over the SAME real file + a FRESH Database,
    // and recover the committed query state from the durable WAL on disk.
    {
        core::Scheduler disk_sched2;
        prod::ProdDisk disk2(disk_sched2, wal_path);
        P_CHECK(disk2.valid(), "reopened ProdDisk over the same file must open");
        P_CHECK(disk2.logical_len() == durable_len,
                "reopened file length must equal the durable length written");

        q::Database db2(disk_sched2, disk2);
        P_CHECK(db2.tip() == 0, "a freshly reopened DB starts empty before recover");
        db2.recover(static_cast<std::size_t>(durable_len));

        // EVERY committed value is queryable after recovery from the real file.
        P_CHECK(db2.tip() == 1,
                "recovered tip must be a single non-empty committed prefix");
        P_CHECK(read_key(db2, "acct:a") == std::optional<std::string>("90"),
                "recovered: acct:a should be 90 (from the real file)");
        P_CHECK(read_key(db2, "acct:b") == std::optional<std::string>("60"),
                "recovered: acct:b should be 60 (from the real file)");
        P_CHECK(read_key(db2, "acct:c") == std::optional<std::string>("777"),
                "recovered: acct:c should be 777 (from the real file)");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "[query_persist_prod_test] %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("[query_persist_prod_test] PASS — query state recovered from the real "
                "ProdDisk file (durable_len=%llu)\n",
                static_cast<unsigned long long>(durable_len));
    return 0;
}
