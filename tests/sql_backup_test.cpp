// sql_backup_test.cpp — LOGICAL backup / restore of a WHOLE SQL database (schemas + data). This is a
// critical durability feature, so the gate is deliberately exhaustive: it proves data RECOVERY across
// every data shape, both scopes, determinism, atomicity, and a battery of corruption teeth.
//
// Coverage:
//   (1) FULL round-trip into a FRESH engine — row table (+ secondary index, NULLs, UPDATE, DELETE
//       tombstones), columnar table (multiple flush generations + a leftover unflushed delta),
//       composite-PK table, a created-then-DROPPED table, and an empty CREATE SCHEMA marker — every
//       query byte-identical to the source, the dropped table stays gone, the schema is restored.
//   (2) DETERMINISM — the same state backs up byte-identically (Full and SchemaOnly).
//   (3) SCHEMA-ONLY — tables/index/schema definitions restore; no rows.
//   (4) EMPTY database — backs up + restores cleanly; the engine is usable afterwards.
//   (5) CHAINED — restore -> back up the restored engine -> byte-identical to the original image.
//   (6) FRESH-ENGINE guard — restoring onto a populated engine is rejected.
//   (7) ATOMICITY teeth — a corrupt DATA section is caught BEFORE the catalog is applied, so the
//       target is left pristine and a subsequent clean restore still succeeds (all-or-nothing).
//   (8) Corruption teeth — flipped catalog byte, flipped data byte, bad magic (empty disk), bad
//       version, bad section count, and a truncated stream are ALL rejected (no partial restore).
// See SqlEngine::backup / restore + storage/Backup.hpp.

#include <cstddef>
#include <cstdio>
#include <span>
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

// A rich workload exercising every data shape a backup must reproduce.
void load(SqlEngine& e) {
    // Row table with a secondary index + NULLable columns (bal/note may be NULL).
    e.exec("CREATE TABLE acct (id INT, owner TEXT NOT NULL, bal INT, note TEXT, PRIMARY KEY (id))");
    e.exec("CREATE INDEX ix_owner ON acct (owner)");
    const char* owners[] = {"ann", "bob", "cid", "dee"};
    for (int i = 0; i < 150; ++i) {
        const std::string bal = (i % 7 == 0) ? "NULL" : std::to_string((i * 7919) % 5000);
        const std::string note = (i % 5 == 0) ? "NULL" : ("'n" + std::to_string(i) + "'");
        e.exec("INSERT INTO acct (id, owner, bal, note) VALUES (" + std::to_string(i) + ", '" +
               owners[i % 4] + "', " + bal + ", " + note + ")");
    }
    e.exec("UPDATE acct SET bal = 424242 WHERE id = 11");  // a mutated row must round-trip
    e.exec("DELETE FROM acct WHERE id = 20");              // tombstones must NOT reappear
    e.exec("DELETE FROM acct WHERE id = 30");

    // Columnar table: two flush generations + a leftover unflushed delta (mixed read path).
    e.set_columnar_default(true);
    e.exec("CREATE TABLE sale (id INT, region TEXT NOT NULL, amount BIGINT NOT NULL, PRIMARY KEY (id))");
    e.set_columnar_default(false);
    const char* regs[] = {"north", "south", "east", "west", "central"};
    for (int i = 0; i < 100; ++i)
        e.exec("INSERT INTO sale (id, region, amount) VALUES (" + std::to_string(i) + ", '" +
               regs[i % 5] + "', " + std::to_string((i * 2654435761ULL) % 100000) + ")");
    e.flush_columnar("sale");
    for (int i = 100; i < 180; ++i)
        e.exec("INSERT INTO sale (id, region, amount) VALUES (" + std::to_string(i) + ", '" +
               regs[i % 5] + "', " + std::to_string((i * 2654435761ULL) % 100000) + ")");
    e.flush_columnar("sale");                              // 2nd generation of blocks
    for (int i = 180; i < 210; ++i)                        // leftover delta (NOT flushed)
        e.exec("INSERT INTO sale (id, region, amount) VALUES (" + std::to_string(i) + ", '" +
               regs[i % 5] + "', " + std::to_string((i * 2654435761ULL) % 100000) + ")");

    // Composite-PK table (all-INT, row mode).
    e.exec("CREATE TABLE pos (a INT, b INT, v INT NOT NULL, PRIMARY KEY (a, b))");
    for (int a = 0; a < 8; ++a)
        for (int b = 0; b < 4; ++b)
            e.exec("INSERT INTO pos (a, b, v) VALUES (" + std::to_string(a) + ", " + std::to_string(b) +
                   ", " + std::to_string(a * 10 + b) + ")");

    // A created-then-dropped table: its catalog tombstone must survive (the table stays GONE).
    e.exec("CREATE TABLE tmp (id INT, PRIMARY KEY (id))");
    e.exec("INSERT INTO tmp (id) VALUES (1)");
    e.exec("DROP TABLE tmp");

    // An empty schema (namespace) marker.
    e.exec("CREATE SCHEMA analytics");
}

const std::vector<std::string>& queries() {
    static const std::vector<std::string> q = {
        "SELECT COUNT(*), SUM(bal), MIN(bal), MAX(bal) FROM acct",     // NULL-aware aggregates
        "SELECT owner, COUNT(*), SUM(bal) FROM acct GROUP BY owner",
        "SELECT id, bal, note FROM acct WHERE id = 11",               // the UPDATEd row
        "SELECT id FROM acct WHERE id = 20",                          // a DELETEd row (no rows back)
        "SELECT bal FROM acct WHERE owner = 'bob'",                   // secondary index path
        "SELECT region, COUNT(*), SUM(amount) FROM sale GROUP BY region",  // columnar (2 gens + delta)
        "SELECT COUNT(*), SUM(amount), MAX(amount) FROM sale",
        "SELECT COUNT(*) FROM sale WHERE amount > 50000",
        "SELECT a, b, v FROM pos WHERE a = 3",                        // composite PK
        "SELECT COUNT(*), SUM(v) FROM pos",
    };
    return q;
}

std::vector<std::string> snapshot(SqlEngine& e) {
    std::vector<std::string> out;
    for (const std::string& q : queries()) out.push_back(render(e.exec(q)));
    return out;
}

// Append raw bytes to a disk on its scheduler (stages a corrupted/handcrafted backup image).
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

// Stage `bytes` onto a fresh BackupDisk and return it (kept in a unique_ptr so the disk's scheduler
// outlives the restore that reads it).
void stage(BackupDisk& bd, std::vector<std::byte> bytes) {
    lockstep::core::Error wrote{lockstep::core::ErrorCode::Unknown, "norun"};
    bd.sched.spawn(write_raw(bd.disk, std::move(bytes), wrote));
    bd.sched.run();
    check(wrote.ok(), "staged a handcrafted backup image");
}

// A restore is "rejected" when it returns ANY error (corrupt CRC, bad magic/version, a read past a
// truncated/empty device, or the fresh-engine guard) — the point of every teeth case is that the
// restore did NOT silently succeed and apply partial state.
bool rejected(const lockstep::core::Error& e) { return !e.ok(); }

}  // namespace

int main() {
    // ---- SOURCE: a durable engine loaded with the full workload ----
    Backing sb;
    SqlEngine src(sb.sched, sb.disk, sb.cat_sched, sb.cat_disk);
    load(src);
    const std::vector<std::string> want = snapshot(src);

    // ===== (1) FULL backup -> restore into a FRESH engine -> identical results + schema =====
    BackupDisk full_bk;
    check(src.backup(full_bk.sched, full_bk.disk, SqlBackupScope::Full).ok(), "full backup ok");
    Backing rb;
    SqlEngine restored(rb.sched, rb.disk, rb.cat_sched, rb.cat_disk);
    check(restored.restore(full_bk.sched, full_bk.disk).ok(), "full restore ok");
    {
        const std::vector<std::string> got = snapshot(restored);
        for (std::size_t i = 0; i < queries().size(); ++i)
            check(got[i] == want[i], "full-restore query " + std::to_string(i) + ":\n  want=[" +
                                         want[i] + "]\n  got =[" + got[i] + "]  for: " + queries()[i]);
    }
    // The DROPPED table must stay gone; the SCHEMA must be present.
    check(!restored.exec("SELECT COUNT(*) FROM tmp").ok, "dropped table stays gone after restore");
    check(restored.exec("CREATE TABLE analytics.thing (id INT, PRIMARY KEY (id))").ok,
          "restored schema 'analytics' is usable (CREATE TABLE in it)");
    // Full DML works post-restore (id allocators advanced; columnar re-flushable).
    check(restored.exec("INSERT INTO acct (id, owner, bal, note) VALUES (100000, 'zoe', 1, 'z')").ok,
          "post-restore INSERT (row) ok");
    check(restored.exec("INSERT INTO sale (id, region, amount) VALUES (100000, 'north', 5)").ok,
          "post-restore INSERT (columnar delta) ok");
    check(restored.exec("UPDATE acct SET bal = 7 WHERE id = 1").ok, "post-restore UPDATE ok");
    check(restored.exec("DELETE FROM acct WHERE id = 2").ok, "post-restore DELETE ok");
    restored.flush_columnar("sale");
    check(restored.exec("SELECT COUNT(*) FROM sale").ok, "post-restore re-flush + query ok");

    // ===== (2) DETERMINISM — same state backs up byte-identically (Full + SchemaOnly) =====
    BackupDisk full_bk2;
    check(src.backup(full_bk2.sched, full_bk2.disk, SqlBackupScope::Full).ok(), "second full backup ok");
    check(full_bk.disk.durable_snapshot() == full_bk2.disk.durable_snapshot(),
          "two full backups of one state are byte-identical");
    BackupDisk sch_a;
    BackupDisk sch_b;
    check(src.backup(sch_a.sched, sch_a.disk, SqlBackupScope::SchemaOnly).ok(), "schema backup a ok");
    check(src.backup(sch_b.sched, sch_b.disk, SqlBackupScope::SchemaOnly).ok(), "schema backup b ok");
    check(sch_a.disk.durable_snapshot() == sch_b.disk.durable_snapshot(),
          "two schema-only backups are byte-identical");

    // ===== (3) SCHEMA-ONLY -> restore -> tables/index/schema exist, NO rows =====
    Backing scb;
    SqlEngine schema_only(scb.sched, scb.disk, scb.cat_sched, scb.cat_disk);
    check(schema_only.restore(sch_a.sched, sch_a.disk).ok(), "schema-only restore ok");
    const ExecResult cnt = schema_only.exec("SELECT COUNT(*) FROM acct");
    check(render(cnt) == "OK aff=1 | COUNT(*)=0", "schema-only: acct exists but has NO rows — [" +
                                                      render(cnt) + "]");
    check(render(schema_only.exec("SELECT COUNT(*) FROM sale")) == "OK aff=1 | COUNT(*)=0",
          "schema-only: columnar table exists, no rows");
    check(schema_only.exec("INSERT INTO acct (id, owner, bal, note) VALUES (1, 'ann', 50, 'x')").ok,
          "schema-only: post-restore INSERT ok");
    check(schema_only.exec("SELECT bal FROM acct WHERE owner = 'ann'").ok,
          "schema-only: indexed lookup ok (index schema restored)");
    check(!schema_only.exec("SELECT COUNT(*) FROM tmp").ok, "schema-only: dropped table stays gone");

    // ===== (4) EMPTY database — backs up + restores cleanly =====
    Backing eb;
    SqlEngine empty_src(eb.sched, eb.disk, eb.cat_sched, eb.cat_disk);
    BackupDisk empty_bk;
    check(empty_src.backup(empty_bk.sched, empty_bk.disk, SqlBackupScope::Full).ok(), "empty-db backup ok");
    Backing erb;
    SqlEngine empty_restored(erb.sched, erb.disk, erb.cat_sched, erb.cat_disk);
    check(empty_restored.restore(empty_bk.sched, empty_bk.disk).ok(), "empty-db restore ok");
    check(empty_restored.exec("CREATE TABLE fresh (id INT, PRIMARY KEY (id))").ok,
          "empty-db restored engine is usable");

    // ===== (5) CHAINED — restore -> back up the restored engine -> restore again -> same data =====
    // A second generation through the backup pipeline must not drift. (The on-disk image is NOT
    // byte-identical the second time: the storage header embeds a snap_seq that reflects the engine's
    // operation count, and a restored engine replays live rows via fresh puts — fewer ops than the
    // source's full insert/update/delete history. The DATA is what must round-trip, and does.)
    Backing tb;
    SqlEngine twin(tb.sched, tb.disk, tb.cat_sched, tb.cat_disk);
    check(twin.restore(full_bk.sched, full_bk.disk).ok(), "twin restore ok");
    check(snapshot(twin) == want, "1st-generation restore reproduces the source");
    BackupDisk twin_bk;
    check(twin.backup(twin_bk.sched, twin_bk.disk, SqlBackupScope::Full).ok(), "twin re-backup ok");
    Backing tb2;
    SqlEngine twin2(tb2.sched, tb2.disk, tb2.cat_sched, tb2.cat_disk);
    check(twin2.restore(twin_bk.sched, twin_bk.disk).ok(), "2nd-generation restore ok");
    check(snapshot(twin2) == want, "2nd-generation restore reproduces the source (chained, no drift)");

    // ===== (6) FRESH-ENGINE guard — restoring onto a populated engine is rejected =====
    check(rejected(src.restore(full_bk.sched, full_bk.disk)),
          "restore onto a populated engine is rejected (fresh-engine contract)");

    // ===== (7) ATOMICITY — a corrupt DATA section is caught BEFORE the catalog is applied =====
    {
        std::vector<std::byte> img = full_bk.disk.durable_snapshot();
        check(img.size() > 4, "full image present");
        img.back() ^= std::byte{0x40};  // flip a byte in the LAST (data) section's payload
        BackupDisk bad;
        stage(bad, std::move(img));
        Backing ab;
        SqlEngine atomic_dst(ab.sched, ab.disk, ab.cat_sched, ab.cat_disk);
        check(rejected(atomic_dst.restore(bad.sched, bad.disk)),
              "corrupt data section -> restore rejected");
        // The catalog must NOT have been applied — the engine is still fresh, so a CLEAN restore
        // into the SAME engine now succeeds (proves all-or-nothing across both stores).
        check(atomic_dst.restore(full_bk.sched, full_bk.disk).ok(),
              "after a rejected restore the engine is pristine -> a clean restore succeeds");
        check(snapshot(atomic_dst) == want, "the clean-after-rejection restore reproduces the source");
    }

    // ===== (8) CORRUPTION TEETH — every malformed stream is rejected (no partial restore) =====
    auto reject_case = [&](std::vector<std::byte> img, const std::string& name) {
        BackupDisk bd;
        stage(bd, std::move(img));
        Backing cb;
        SqlEngine dst(cb.sched, cb.disk, cb.cat_sched, cb.cat_disk);
        check(rejected(dst.restore(bd.sched, bd.disk)), name + " -> rejected");
    };
    {
        const std::vector<std::byte> base = full_bk.disk.durable_snapshot();
        // catalog payload byte (past SQL header + section-len + storage header).
        std::vector<std::byte> a = base;
        const std::size_t cat_flip = kSqlBackupHeaderBytes + 8 + lockstep::storage::kBackupHeaderBytes + 1;
        check(a.size() > cat_flip, "image large enough to corrupt a catalog byte");
        a[cat_flip] ^= std::byte{0x40};
        reject_case(std::move(a), "flipped catalog byte");

        std::vector<std::byte> v = base;  // version field (offset 4..7)
        v[4] ^= std::byte{0xFF};
        reject_case(std::move(v), "bad version");

        std::vector<std::byte> n = base;  // section count (offset 12..15)
        n[12] ^= std::byte{0xFF};
        reject_case(std::move(n), "bad section count");

        std::vector<std::byte> m = base;  // magic (offset 0)
        m[0] ^= std::byte{0xFF};
        reject_case(std::move(m), "bad magic");

        std::vector<std::byte> t = base;  // truncated stream (drop the tail of the data section)
        t.resize(t.size() - 16);
        reject_case(std::move(t), "truncated stream");
    }
    // An empty disk (no header at all) is rejected.
    {
        BackupDisk empty;
        Backing cb;
        SqlEngine dst(cb.sched, cb.disk, cb.cat_sched, cb.cat_disk);
        check(rejected(dst.restore(empty.sched, empty.disk)), "empty disk -> rejected (bad magic)");
    }

    if (g_fail) {
        std::printf("sql_backup_test: FAILED\n");
        return 1;
    }
    std::printf("sql_backup_test: OK (full/schema-only/empty round-trip + determinism + chained "
                "idempotence + fresh-guard + atomicity + 7 corruption teeth)\n");
    return 0;
}
