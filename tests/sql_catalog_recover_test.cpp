// sql_catalog_recover_test.cpp — DURABLE SQL CATALOG + recover gate (C7). The SQL catalog (table
// schemas from CREATE TABLE / CREATE INDEX) used to live ONLY in memory, so a restart lost it: the
// recovered ROW/BLOCK data was uninterpretable (the engine no longer knew the table existed). Now
// each CREATE/INDEX/DROP writes a durable schema record (reserved 0x01 key namespace) and recover()
// rebuilds the catalog from those records.
//
// Asserts (deterministic SimDisk so crash/recover is byte-stable): build a table (columnar) + rows
// through a durable SqlEngine, drop the engine (a "crash") keeping the disk image, reopen a FRESH
// SqlEngine over the SAME disk, recover() — then the schema AND the data are back: queries that
// would error "unknown table" without the catalog now return the EXACT pre-crash results. Covers a
// secondary index too (its schema record must round-trip). Result == a no-crash twin oracle.

#include <cstdint>
#include <cstdio>
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
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

std::string render(const ExecResult& r) {
    std::string out = r.ok ? "OK" : "ERR";
    if (!r.ok) return out + "(" + r.error + ")";
    out += " aff=" + std::to_string(r.affected);
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) out += " " + label + "=" + d.render();
    }
    return out;
}

struct Backing {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xC7C7'1234ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
    // The catalog now lives in its OWN durable store (separate WAL/Seq line — keeps DDL out
    // of the data MVCC version line). Recovering the SCHEMA needs this disk recovered too.
    lockstep::core::Scheduler cat_sched;
    lockstep::core::SimClock cat_clock{cat_sched};
    lockstep::sim::SeededRandom cat_rng{0xCA7A'5678ULL};
    lockstep::sim::SimDisk cat_disk{cat_sched, cat_clock, cat_rng};
};

// The DDL + data workload (identical for the recovered run and the oracle).
void load(SqlEngine& e) {
    e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, amount INT NOT NULL, region TEXT NOT NULL, PRIMARY KEY (id))");
    e.exec("CREATE INDEX ix_amount ON t (amount)");
    const char* regs[] = {"north", "south", "east", "west", "central"};
    for (int i = 0; i < 400; ++i) {
        e.exec("INSERT INTO t (id, amount, region) VALUES (" + std::to_string(i) + ", " +
               std::to_string((i * 2654435761ULL) % 1000) + ", '" + regs[i % 5] + "')");
    }
    e.flush_columnar("t");
}

const std::vector<std::string>& queries() {
    static const std::vector<std::string> q = {
        "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM t",
        "SELECT region, COUNT(*), SUM(amount) FROM t GROUP BY region",
        "SELECT COUNT(*), SUM(amount) FROM t WHERE amount > 500",
        "SELECT amount FROM t WHERE id = 123",
    };
    return q;
}

}  // namespace

int main() {
    // ---- ORACLE: a no-crash engine running the same workload ----
    Backing ob;
    SqlEngine oracle(ob.sched, ob.disk);
    load(oracle);
    std::vector<std::string> want;
    for (const std::string& q : queries()) want.push_back(render(oracle.exec(q)));

    // ---- CRASH + RECOVER: load, drop the engine (keep the disk), reopen + recover ----
    Backing b;
    std::size_t durable_len = 0;
    std::size_t cat_len = 0;
    {
        SqlEngine e(b.sched, b.disk, b.cat_sched, b.cat_disk);
        load(e);
        durable_len = b.disk.durable_len();      // durable DATA image after the workload
        cat_len = b.cat_disk.durable_len();      // durable CATALOG image (separate WAL)
        // Sanity: BEFORE recovery a fresh engine over the same disk does NOT know the table.
    }
    SqlEngine recovered(b.sched, b.disk, b.cat_sched, b.cat_disk);
    // Without recover() the catalog is empty => "unknown table".
    const ExecResult pre = recovered.exec("SELECT COUNT(*) FROM t");
    check(!pre.ok, "pre-recover: table unknown (catalog empty) — got [" + render(pre) + "]");

    recovered.recover(durable_len, cat_len);

    // After recover: schema + data are back; every query equals the oracle byte-for-byte.
    for (std::size_t i = 0; i < queries().size(); ++i) {
        const std::string got = render(recovered.exec(queries()[i]));
        check(got == want[i], "post-recover query " + std::to_string(i) + ":\n  want=[" + want[i] +
                                  "]\n  got =[" + got + "]  for: " + queries()[i]);
    }

    // The recovered catalog still knows the secondary index (an indexed lookup plans + runs).
    const ExecResult idx = recovered.exec("SELECT id FROM t WHERE amount = 0");
    check(idx.ok, "post-recover indexed column query ok — [" + render(idx) + "]");

    // A post-recovery INSERT works (id allocator advanced past the recovered table id) + persists.
    check(recovered.exec("INSERT INTO t (id, amount, region) VALUES (100000, 7, 'north')").ok,
          "post-recover INSERT ok");

    if (g_fail) {
        std::printf("sql_catalog_recover_test: FAILED\n");
        return 1;
    }
    std::printf("sql_catalog_recover_test: OK (schema + data survive a restart)\n");
    return 0;
}
