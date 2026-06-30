// sql_backup_test.cpp — LOGICAL backup / restore of a WHOLE SQL database (schemas + data). A backup
// taken from a live SqlEngine, restored into a FRESH SqlEngine, reproduces EXACTLY the source: every
// query returns byte-identical results and the schema (incl. a secondary index + a columnar table) is
// intact. Two scopes are covered: Full (catalog + data) and SchemaOnly (catalog only — tables exist,
// no rows). Integrity teeth: a single flipped byte in the stream is caught by the storage-layer CRC
// and the restore is REJECTED (no partial restore). See SqlEngine::backup / restore + storage/Backup.hpp.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/storage/Backup.hpp>

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

// A durable SQL backing: a data disk + a SEPARATE catalog disk (own WAL/Seq line), each on its own
// scheduler — mirrors the real 4-arg SqlEngine wiring.
struct Backing {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xBC2'9001ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
    lockstep::core::Scheduler cat_sched;
    lockstep::core::SimClock cat_clock{cat_sched};
    lockstep::sim::SeededRandom cat_rng{0xBC2'9002ULL};
    lockstep::sim::SimDisk cat_disk{cat_sched, cat_clock, cat_rng};
};

// A standalone disk to hold a backup image (its own scheduler).
struct BackupDisk {
    lockstep::core::Scheduler sched;
    lockstep::core::SimClock clock{sched};
    lockstep::sim::SeededRandom rng{0xBC2'9100ULL};
    lockstep::sim::SimDisk disk{sched, clock, rng};
};

void load(SqlEngine& e) {
    e.exec("CREATE TABLE acct (id INT, owner TEXT NOT NULL, bal INT NOT NULL, PRIMARY KEY (id))");
    e.exec("CREATE INDEX ix_owner ON acct (owner)");
    e.set_columnar_default(true);
    e.exec("CREATE TABLE sale (id INT, region TEXT NOT NULL, amount INT NOT NULL, PRIMARY KEY (id))");
    e.set_columnar_default(false);
    const char* owners[] = {"ann", "bob", "cid", "dee"};
    const char* regs[] = {"north", "south", "east", "west", "central"};
    for (int i = 0; i < 200; ++i) {
        e.exec("INSERT INTO acct (id, owner, bal) VALUES (" + std::to_string(i) + ", '" +
               owners[i % 4] + "', " + std::to_string((i * 7919) % 5000) + ")");
        e.exec("INSERT INTO sale (id, region, amount) VALUES (" + std::to_string(i) + ", '" +
               regs[i % 5] + "', " + std::to_string((i * 2654435761ULL) % 1000) + ")");
    }
    e.flush_columnar("sale");
}

const std::vector<std::string>& queries() {
    static const std::vector<std::string> q = {
        "SELECT COUNT(*), SUM(bal), MIN(bal), MAX(bal) FROM acct",
        "SELECT owner, COUNT(*), SUM(bal) FROM acct GROUP BY owner",
        "SELECT bal FROM acct WHERE owner = 'bob'",        // exercises the secondary index
        "SELECT region, COUNT(*), SUM(amount) FROM sale GROUP BY region",  // columnar table
        "SELECT COUNT(*), SUM(amount) FROM sale WHERE amount > 500",
    };
    return q;
}

// Append raw bytes to a disk on its scheduler (stages a corrupted backup image).
lockstep::core::Task write_raw(lockstep::core::IDisk& d, std::vector<std::byte> bytes,
                               lockstep::core::Error& result) {
    lockstep::core::Offset off = 0;
    const lockstep::core::Error ae =
        co_await d.append(std::span<const std::byte>(bytes.data(), bytes.size()), off);
    if (!ae.ok()) {
        result = ae;
        co_return;
    }
    result = co_await d.sync();
    co_return;
}

}  // namespace

int main() {
    // ---- SOURCE: a durable SQL engine with a row table (+ index) and a columnar table ----
    Backing sb;
    SqlEngine src(sb.sched, sb.disk, sb.cat_sched, sb.cat_disk);
    load(src);
    std::vector<std::string> want;
    for (const std::string& q : queries()) want.push_back(render(src.exec(q)));

    // ---- (1) FULL backup -> restore into a FRESH engine -> identical results + schema ----
    BackupDisk full_bk;
    check(src.backup(full_bk.sched, full_bk.disk, SqlBackupScope::Full).ok(), "full backup ok");

    Backing rb;
    SqlEngine restored(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
    check(restored.restore(full_bk.sched, full_bk.disk).ok(), "full restore ok");
    for (std::size_t i = 0; i < queries().size(); ++i) {
        const std::string got = render(restored.exec(queries()[i]));
        check(got == want[i], "full-restore query " + std::to_string(i) + ":\n  want=[" + want[i] +
                                  "]\n  got =[" + got + "]  for: " + queries()[i]);
    }
    // A post-restore INSERT works (id allocator advanced past the restored ids) + reads back.
    check(restored.exec("INSERT INTO acct (id, owner, bal) VALUES (100000, 'zoe', 1)").ok,
          "post-restore INSERT ok");

    // ---- (2) DETERMINISM: the same state backs up to a byte-identical image ----
    BackupDisk full_bk2;
    check(src.backup(full_bk2.sched, full_bk2.disk, SqlBackupScope::Full).ok(), "second full backup ok");
    check(full_bk.disk.durable_snapshot() == full_bk2.disk.durable_snapshot(),
          "two full backups of one state are byte-identical");

    // ---- (3) SCHEMA-ONLY backup -> restore -> tables exist, NO rows ----
    BackupDisk schema_bk;
    check(src.backup(schema_bk.sched, schema_bk.disk, SqlBackupScope::SchemaOnly).ok(), "schema-only backup ok");
    Backing scb;
    SqlEngine schema_only(scb.sched, scb.disk, scb.cat_sched, scb.cat_disk);
    check(schema_only.restore(schema_bk.sched, schema_bk.disk).ok(), "schema-only restore ok");
    const ExecResult cnt = schema_only.exec("SELECT COUNT(*) FROM acct");
    check(cnt.ok, "schema-only: table acct exists after restore — [" + render(cnt) + "]");
    check(render(cnt) == "OK aff=1 | COUNT(*)=0", "schema-only: acct has NO rows — [" + render(cnt) + "]");
    // The secondary index definition is restored too: an INSERT + indexed lookup works.
    check(schema_only.exec("INSERT INTO acct (id, owner, bal) VALUES (1, 'ann', 50)").ok,
          "schema-only: post-restore INSERT ok");
    check(schema_only.exec("SELECT bal FROM acct WHERE owner = 'ann'").ok,
          "schema-only: indexed lookup ok (index schema restored)");

    // ---- (4) TEETH: a single flipped byte -> storage CRC mismatch -> restore REJECTED ----
    std::vector<std::byte> img = full_bk.disk.durable_snapshot();
    // Flip a byte inside the catalog section's payload (past the SQL header + section-len + the
    // storage backup header), so the storage-layer CRC over that section catches it.
    const std::size_t flip = kSqlBackupHeaderBytes + 8 + lockstep::storage::kBackupHeaderBytes + 1;
    check(img.size() > flip, "backup image is large enough to corrupt a payload byte");
    img[flip] ^= std::byte{0x40};
    BackupDisk corrupt;
    lockstep::core::Error wrote{lockstep::core::ErrorCode::Unknown, "norun"};
    corrupt.sched.spawn(write_raw(corrupt.disk, std::move(img), wrote));
    corrupt.sched.run();
    check(wrote.ok(), "staged the corrupted image");
    Backing cb;
    SqlEngine corrupt_dst(cb.sched, cb.disk, cb.cat_sched, cb.cat_disk);
    const lockstep::core::Error ce = corrupt_dst.restore(corrupt.sched, corrupt.disk);
    check(!ce.ok() && ce.code == lockstep::core::ErrorCode::Corruption,
          "a single flipped byte is caught by the CRC and the restore is rejected — [" +
              std::string(ce.detail) + "]");

    if (g_fail) {
        std::printf("sql_backup_test: FAILED\n");
        return 1;
    }
    std::printf("sql_backup_test: OK (full + schema-only backup/restore round-trip + determinism + integrity teeth)\n");
    return 0;
}
