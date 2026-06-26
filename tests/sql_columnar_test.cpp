// sql_columnar_test.cpp — COLUMNAR layout conformance gate (the columnar rollout's
// end-to-end gate). A columnar table (each (row,column) is its own KV under the 'c'
// namespace, scanned per-family) is a STORAGE LAYOUT change, NOT a semantic one: it
// must return the EXACT SAME rows/order/errors as the row-mode layout. This gate runs
// an IDENTICAL seeded workload on two engines — one columnar (set_columnar_default),
// one row-mode — and asserts byte-identical rendered output for every statement. Since
// the row-mode engine is already proven == the independent reference model
// (sql_conformance_test), columnar == row-mode == reference.
//
// Covers: CREATE, INSERT (named/omitted cols, NULLs), UPDATE, DELETE, SELECT * and
// projections, WHERE pk Eq / pk BETWEEN (the columnar PK-fast paths) / non-pk filter
// (full-family scan + residual), aggregates + GROUP BY, ORDER BY/LIMIT, CREATE INDEX +
// indexed WHERE (columnar index fetch assembles rows from families), dup-PK + absent-
// row errors. (E) DETERMINISM: same seed => identical output both layouts.
//
// Determinism: only entropy is the seed (inlined SplitMix). No clock/rng/threads.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

class SplitMix {
public:
    explicit SplitMix(std::uint64_t s) : s_(s) {}
    std::uint64_t next() {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
private:
    std::uint64_t s_;
};

// Render an ExecResult to a stable string (ok flag, affected, error, then each row's
// labelled cells) so two engines' outputs compare byte-for-byte.
std::string render(const ExecResult& r) {
    std::string out;
    out += r.ok ? "OK" : "ERR";
    if (!r.ok) {
        out += "(" + r.error + ")";
        return out;
    }
    out += " aff=" + std::to_string(r.affected);
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) {
            out += " " + label + "=" + d.render();
        }
    }
    return out;
}

// Run the SAME statement on both engines; assert identical rendered output.
void both(SqlEngine& col, SqlEngine& row, const std::string& sql, const std::string& tag) {
    const std::string rc = render(col.exec(sql));
    const std::string rr = render(row.exec(sql));
    check(rc == rr, tag + ": columnar=[" + rc + "] row=[" + rr + "] for: " + sql);
}

// JOIN over COLUMNAR tables must equal JOIN over row-mode (regression: a columnar table's rows live
// in blocks + the 'd' delta, NOT the row-mode 't' prefix — a 't'-scan JOIN returned EMPTY).
void run_join() {
    SqlEngine col;
    col.set_columnar_default(true);
    SqlEngine row;
    for (SqlEngine* e : {&col, &row}) {
        e->exec("CREATE TABLE ord (id INT, cust INT NOT NULL, amt INT NOT NULL, PRIMARY KEY (id))");
        e->exec("CREATE TABLE cust (cid INT, name TEXT NOT NULL, PRIMARY KEY (cid))");
        for (int i = 0; i < 60; ++i) {
            e->exec("INSERT INTO ord (id, cust, amt) VALUES (" + std::to_string(i) + ", " +
                    std::to_string(i % 6) + ", " + std::to_string((i * 17) % 100) + ")");
        }
        for (int c = 0; c < 6; ++c) {
            e->exec("INSERT INTO cust (cid, name) VALUES (" + std::to_string(c) + ", 'c" +
                    std::to_string(c) + "')");
        }
    }
    // Flush ONE columnar table (mix of flushed + unflushed — the case that surfaced the bug).
    col.flush_columnar("ord");
    both(col, row, "SELECT ord.id, cust.name FROM ord JOIN cust ON ord.cust = cust.cid", "join-proj");
    both(col, row,
         "SELECT cust.name, COUNT(*), SUM(ord.amt) FROM ord JOIN cust ON ord.cust = cust.cid "
         "GROUP BY cust.name",
         "join-groupby");
    col.flush_columnar("cust");  // both flushed
    both(col, row,
         "SELECT cust.name, SUM(ord.amt) FROM ord JOIN cust ON ord.cust = cust.cid "
         "WHERE ord.amt > 40 GROUP BY cust.name",
         "join-filtered");
}

void run_seed(std::uint64_t seed) {
    SqlEngine col;
    col.set_columnar_default(true);
    SqlEngine row;  // row-mode (default)

    const std::string ddl =
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, bio TEXT, PRIMARY KEY (id))";
    both(col, row, ddl, "create");

    SplitMix rng(seed);
    const char* depts[] = {"eng", "sales", "ops", "hr", "legal"};
    const std::size_t N = 40;

    // INSERTs — mix of full rows, omitted (NULL) columns, and explicit NULLs.
    for (std::size_t i = 0; i < N; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(rng.below(60));  // collisions => dup-PK errors
        const std::string dept = depts[rng.below(5)];
        const std::int64_t sal = static_cast<std::int64_t>(rng.below(1000));
        const std::int64_t age = static_cast<std::int64_t>(rng.below(50)) + 18;
        const std::uint64_t shape = rng.below(4);
        std::string sql;
        if (shape == 0) {
            sql = "INSERT INTO emp (id, dept, sal, age, bio) VALUES (" + std::to_string(id) +
                  ", '" + dept + "', " + std::to_string(sal) + ", " + std::to_string(age) +
                  ", 'bio-" + std::to_string(id) + "')";
        } else if (shape == 1) {
            // omit nullable bio + age => NULL
            sql = "INSERT INTO emp (id, dept, sal) VALUES (" + std::to_string(id) + ", '" +
                  dept + "', " + std::to_string(sal) + ")";
        } else if (shape == 2) {
            // explicit NULL dept
            sql = "INSERT INTO emp (id, dept, sal, age) VALUES (" + std::to_string(id) +
                  ", NULL, " + std::to_string(sal) + ", " + std::to_string(age) + ")";
        } else {
            sql = "INSERT INTO emp (id, dept, sal, age, bio) VALUES (" + std::to_string(id) +
                  ", '" + dept + "', " + std::to_string(sal) + ", " + std::to_string(age) +
                  ", 'x')";
        }
        both(col, row, sql, "insert");
    }

    // FLUSH the columnar table: live rows move INSERTed-so-far into column blocks; the
    // row engine is unaffected. Subsequent reads must merge blocks + (new) delta and stay
    // byte-identical to row-mode. (A flush mid-workload exercises block+delta merge,
    // post-flush UPDATE/DELETE shadowing a block row, and re-INSERT of a flushed pk.)
    check(!col.flush_columnar("emp").has_value(), "flush #1 ok");

    // UPDATEs + DELETEs (PK-targeted, incl. absent rows => affected=0).
    for (std::size_t k = 0; k < 20; ++k) {
        const std::int64_t id = static_cast<std::int64_t>(rng.below(60));
        if (rng.below(2) == 0) {
            both(col, row,
                 "UPDATE emp SET sal = " + std::to_string(rng.below(1000)) +
                     " WHERE id = " + std::to_string(id),
                 "update");
        } else {
            both(col, row, "DELETE FROM emp WHERE id = " + std::to_string(id), "delete");
        }
    }

    // READ shapes — projections, PK-fast Eq/BETWEEN, non-pk filter, aggregates, order.
    both(col, row, "SELECT id FROM emp", "select-pk-only");
    both(col, row, "SELECT id, sal FROM emp", "select-proj");
    both(col, row, "SELECT id, dept, sal, age, bio FROM emp", "select-star");
    both(col, row, "SELECT id, sal FROM emp WHERE id = 30", "pk-eq");
    both(col, row, "SELECT id, dept FROM emp WHERE id BETWEEN 10 AND 40", "pk-between");
    both(col, row, "SELECT id, sal FROM emp WHERE sal > 500", "filter-nonpk");
    both(col, row, "SELECT id, sal, dept FROM emp WHERE sal > 300 AND dept = 'eng'", "filter-and");
    both(col, row, "SELECT id, dept FROM emp WHERE dept IS NULL", "filter-null");
    both(col, row, "SELECT dept, COUNT(*), SUM(sal), MIN(sal), MAX(sal) FROM emp GROUP BY dept",
         "groupby");
    both(col, row, "SELECT id, sal FROM emp ORDER BY sal DESC LIMIT 5", "order-limit");

    // Secondary index — create on both, then an indexed WHERE (columnar assembles the
    // matched rows from their families) must equal the row-mode (full-scan) answer.
    // Second flush AFTER updates/deletes: blocks now rebuilt from the merged set (the
    // delta — incl. post-flush deletes that shadowed block rows — is compacted away).
    check(!col.flush_columnar("emp").has_value(), "flush #2 ok");
    both(col, row, "SELECT id, sal FROM emp WHERE sal > 500", "filter-postflush2");
    both(col, row, "SELECT id, dept FROM emp WHERE id BETWEEN 10 AND 40", "pk-between-postflush2");
    // A4 vectorized scalar aggregates over SoA blocks (delta empty post-flush) — must
    // equal row-mode (generic AoS aggregate). Covers COUNT(*)/COUNT(col w/ NULLs)/SUM/MIN/
    // MAX/AVG, INT + TEXT, with and without a vectorized filter, and an empty selection.
    both(col, row, "SELECT COUNT(*), SUM(sal), MIN(sal), MAX(sal), AVG(sal) FROM emp", "agg-all");
    both(col, row, "SELECT COUNT(*), SUM(sal) FROM emp WHERE sal > 500", "agg-filtered");
    both(col, row, "SELECT COUNT(dept), MIN(dept), MAX(dept) FROM emp", "agg-text-nullable");
    both(col, row, "SELECT COUNT(*), SUM(sal) FROM emp WHERE sal > 999999", "agg-empty-sel");
    both(col, row, "SELECT AVG(age), MIN(age), MAX(age) FROM emp WHERE age > 30", "agg-age");
    // A4 vectorized GROUP BY over SoA blocks (delta clean) — hash-aggregate per group,
    // must equal row-mode. Covers grouped aggregates, a filtered GROUP BY, and ORDER BY.
    both(col, row,
         "SELECT dept, COUNT(*), SUM(sal), MIN(sal), MAX(sal), AVG(sal) FROM emp GROUP BY dept",
         "groupby-postflush");
    both(col, row, "SELECT dept, COUNT(*), SUM(sal) FROM emp WHERE sal > 300 GROUP BY dept",
         "groupby-filtered");
    both(col, row, "SELECT dept, SUM(sal) FROM emp GROUP BY dept ORDER BY dept DESC",
         "groupby-order");
    // A4 vectorized HAVING over SoA groups (delta clean) — must equal row-mode.
    both(col, row, "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 2",
         "having-count");
    both(col, row, "SELECT dept, SUM(sal) FROM emp GROUP BY dept HAVING SUM(sal) > 100",
         "having-sum");
    both(col, row,
         "SELECT dept, COUNT(*), SUM(sal) FROM emp GROUP BY dept "
         "HAVING COUNT(*) > 1 AND SUM(sal) > 50",
         "having-and");
    both(col, row, "SELECT COUNT(*) FROM emp HAVING COUNT(*) > 0", "having-scalar");

    both(col, row, "CREATE INDEX idx_sal ON emp (sal)", "create-index");
    both(col, row, "SELECT id, sal FROM emp WHERE sal = 250", "indexed-eq");
    both(col, row, "SELECT id, sal, dept FROM emp WHERE sal BETWEEN 100 AND 300", "indexed-range");
    both(col, row, "SELECT id, sal FROM emp WHERE sal = 250 AND dept = 'ops'", "indexed-residual");
}

// MULTI-CHUNK gate: a table larger than kChunkRows (1024) so flushed columns span several
// chunks (block_no 0..K-1). Exercises the chunked read paths (block_base_rows per-chunk + pk-
// range data skipping, read_block_row chunk search, vectorized agg concat), and a re-flush
// after DELETEs that SHRINKS the table (stale trailing chunks must be tombstoned). All must
// stay byte-identical to row-mode.
void run_large() {
    SqlEngine col;
    col.set_columnar_default(true);
    SqlEngine row;
    const std::string ddl =
        "CREATE TABLE big (id INT, grp INT, seq INT, val INT, PRIMARY KEY (id))";
    both(col, row, ddl, "large-create");
    const std::size_t N = 2600;  // > 2*kChunkRows => 3 chunks/column
    for (std::size_t i = 0; i < N; ++i) {
        // seq is MONOTONIC (== insert order) so chunks have DISJOINT seq ranges => a
        // WHERE seq <op> X exercises ZONE-MAP chunk skipping; grp/val are low-card/random
        // (no skip) so the skip path is checked against both skippable + non-skippable.
        const std::string sql = "INSERT INTO big (id, grp, seq, val) VALUES (" +
                                std::to_string(i) + ", " + std::to_string(i % 7) + ", " +
                                std::to_string(i) + ", " + std::to_string((i * 13) % 500) + ")";
        both(col, row, sql, "large-insert");
    }
    check(!col.flush_columnar("big").has_value(), "large flush #1");  // -> 3 chunks/column
    // Multi-chunk reads: full scan, projection, pk BETWEEN (data skipping across chunks),
    // scalar aggregate, GROUP BY — all spanning chunk boundaries.
    both(col, row, "SELECT COUNT(*), SUM(val), MIN(val), MAX(val) FROM big", "large-agg");
    both(col, row, "SELECT grp, COUNT(*), SUM(val) FROM big GROUP BY grp", "large-groupby");
    both(col, row, "SELECT id, val FROM big WHERE id BETWEEN 1500 AND 1520", "large-pk-skip");
    both(col, row, "SELECT id, val FROM big WHERE id BETWEEN 1020 AND 1030", "large-pk-skip2");
    both(col, row, "SELECT id, val FROM big WHERE val > 400", "large-filter");
    both(col, row, "SELECT id FROM big WHERE id = 2049", "large-pk-eq");
    // ZONE-MAP skipping on the monotonic non-pk `seq` column (chunks skippable) — result
    // must match row-mode whether chunks are skipped or not.
    both(col, row, "SELECT COUNT(*), SUM(val) FROM big WHERE seq > 2000", "large-zone-agg");
    both(col, row, "SELECT COUNT(*), SUM(val) FROM big WHERE seq > 1000 AND seq < 1100", "large-zone-band");
    both(col, row, "SELECT id, seq FROM big WHERE seq >= 2050 AND seq <= 2060", "large-zone-proj");
    both(col, row, "SELECT seq, COUNT(*) FROM big WHERE seq > 1500 GROUP BY seq", "large-zone-groupby");
    both(col, row, "SELECT COUNT(*) FROM big WHERE seq > 99999", "large-zone-allskip");
    both(col, row, "SELECT grp, COUNT(*), SUM(val) FROM big WHERE val > 250 GROUP BY grp", "large-zone-noskip");
    // INCREMENTAL APPEND: insert ids ABOVE the current max (monotonic), flush -> the
    // append fast path adds NEW chunks without rewriting existing blocks. Result must match
    // row-mode (the differential catches a wrong append / zone / order).
    for (std::size_t i = N; i < N + 1500; ++i) {
        const std::string sql = "INSERT INTO big (id, grp, seq, val) VALUES (" +
                                std::to_string(i) + ", " + std::to_string(i % 7) + ", " +
                                std::to_string(i) + ", " + std::to_string((i * 13) % 500) + ")";
        both(col, row, sql, "large-append-insert");
    }
    check(!col.flush_columnar("big").has_value(), "large incremental append flush");
    both(col, row, "SELECT COUNT(*), SUM(val), MIN(seq), MAX(seq) FROM big", "large-append-agg");
    both(col, row, "SELECT id, seq FROM big WHERE seq > 3500", "large-append-zone");
    both(col, row, "SELECT id, val FROM big WHERE id BETWEEN 3000 AND 3010", "large-append-pk");
    both(col, row, "SELECT grp, COUNT(*) FROM big GROUP BY grp", "large-append-groupby");

    // DELETE a big swathe, re-flush -> the table shrinks; full flush retires stale chunks.
    for (std::size_t i = 1000; i < N; ++i) {
        both(col, row, "DELETE FROM big WHERE id = " + std::to_string(i), "large-delete");
    }
    check(!col.flush_columnar("big").has_value(), "large flush #2 (shrink)");
    both(col, row, "SELECT COUNT(*), SUM(val) FROM big", "large-agg-after-shrink");
    both(col, row, "SELECT id, val FROM big WHERE id BETWEEN 1500 AND 1520", "large-skip-after-shrink");
    both(col, row, "SELECT id, grp, val FROM big", "large-scan-after-shrink");
}

// AUTO-FLUSH gate: a columnar engine with set_auto_flush(threshold) self-flushes during the
// write stream (no manual flush_columnar). Must stay byte-identical to a row-mode engine. The
// monotonic inserts hit the incremental-append fast path; updates/deletes trigger a full flush.
void run_autoflush() {
    SqlEngine col;
    col.set_columnar_default(true);
    col.set_auto_flush(500);  // flush every ~500 delta writes
    SqlEngine row;
    const std::string ddl = "CREATE TABLE s (id INT, grp INT, val INT, PRIMARY KEY (id))";
    both(col, row, ddl, "af-create");
    for (std::size_t i = 0; i < 3000; ++i) {  // monotonic => incremental-append auto-flushes
        both(col, row,
             "INSERT INTO s (id, grp, val) VALUES (" + std::to_string(i) + ", " +
                 std::to_string(i % 5) + ", " + std::to_string((i * 7) % 400) + ")",
             "af-insert");
    }
    both(col, row, "SELECT COUNT(*), SUM(val) FROM s", "af-agg");
    both(col, row, "SELECT grp, COUNT(*), SUM(val) FROM s GROUP BY grp", "af-groupby");
    both(col, row, "SELECT id, val FROM s WHERE id BETWEEN 1500 AND 1510", "af-pk");
    both(col, row, "SELECT id, val FROM s WHERE val > 390", "af-filter");
    // Updates + deletes (trigger full auto-flushes) — still byte-identical to row-mode.
    for (std::size_t i = 0; i < 600; ++i) {
        both(col, row, "UPDATE s SET val = 999 WHERE id = " + std::to_string(i * 3), "af-update");
    }
    both(col, row, "SELECT COUNT(*), SUM(val) FROM s WHERE val = 999", "af-after-update");
    both(col, row, "SELECT id, val FROM s WHERE id = 1500", "af-point");
}

// MULTI-RUN OVERLAY gate: repeated non-append flushes build overlay runs; once kMaxOverlays(4)
// runs accumulate the next flush COMPACTS them into the base. Reads must equal row-mode at every
// stage (overlapping runs, tombstones from deletes, re-inserts, compaction).
void run_overlays() {
    SqlEngine col;
    col.set_columnar_default(true);
    SqlEngine row;
    both(col, row, "CREATE TABLE o (id INT, a INT, b TEXT, PRIMARY KEY (id))", "ov-create");
    for (int i = 0; i < 200; ++i) {
        both(col, row, "INSERT INTO o (id, a, b) VALUES (" + std::to_string(i) + ", " +
                           std::to_string(i) + ", 'x" + std::to_string(i % 10) + "')", "ov-ins");
    }
    check(!col.flush_columnar("o").has_value(), "ov base flush");  // base run 0
    // 6 rounds of update+delete+insert, each flushed -> overlay runs; round 5+ compacts.
    for (int rnd = 0; rnd < 6; ++rnd) {
        for (int k = 0; k < 15; ++k) {
            const int id = (rnd * 7 + k * 11) % 200;
            both(col, row, "UPDATE o SET a = " + std::to_string(1000 + rnd * 100 + k) +
                               " WHERE id = " + std::to_string(id), "ov-upd");
        }
        both(col, row, "DELETE FROM o WHERE id = " + std::to_string(rnd * 13), "ov-del");
        both(col, row, "INSERT INTO o (id, a, b) VALUES (" + std::to_string(300 + rnd) + ", " +
                           std::to_string(rnd) + ", 're')", "ov-reins");
        check(!col.flush_columnar("o").has_value(), "ov flush round");
        // Read after each flush — base + overlays merge (or post-compaction base).
        both(col, row, "SELECT COUNT(*), SUM(a) FROM o", "ov-agg");
        both(col, row, "SELECT id, a, b FROM o WHERE id BETWEEN 50 AND 60", "ov-range");
        both(col, row, "SELECT b, COUNT(*) FROM o GROUP BY b", "ov-groupby");
        both(col, row, "SELECT id, a FROM o WHERE a > 1000", "ov-filter");
    }
    both(col, row, "SELECT id, a, b FROM o", "ov-final-scan");
}

// TEXT ZONE-MAP gate: a monotonic zero-padded TEXT column => chunks have disjoint lexicographic
// ranges => a WHERE tag <op> 'literal' exercises TEXT zone skipping. Must equal row-mode whether
// chunks are skipped or not.
void run_text_zones() {
    SqlEngine col;
    col.set_columnar_default(true);
    SqlEngine row;
    both(col, row,
         "CREATE TABLE tz (id INT, tag TEXT NOT NULL, bkt TEXT NOT NULL, v INT, PRIMARY KEY (id))",
         "tz-create");
    auto pad = [](int i) {
        std::string s = std::to_string(i);
        return std::string(6 - s.size(), '0') + s;  // zero-padded => lexicographic == numeric
    };
    const char* bkts[] = {"north", "south", "east", "west", "central"};
    for (int i = 0; i < 3000; ++i) {
        both(col, row,
             "INSERT INTO tz (id, tag, bkt, v) VALUES (" + std::to_string(i) + ", 't" + pad(i) +
                 "', '" + bkts[i % 5] + "', " + std::to_string(i % 400) + ")",
             "tz-ins");
    }
    check(!col.flush_columnar("tz").has_value(), "tz flush");
    both(col, row, "SELECT COUNT(*), SUM(v) FROM tz WHERE tag > 't002500'", "tz-gt");
    both(col, row, "SELECT id, v FROM tz WHERE tag = 't001234'", "tz-eq");
    both(col, row, "SELECT COUNT(*) FROM tz WHERE tag < 't000500'", "tz-lt");
    both(col, row, "SELECT id, tag FROM tz WHERE tag >= 't002990'", "tz-ge");
    both(col, row, "SELECT COUNT(*) FROM tz WHERE tag = 'zzz_absent'", "tz-allskip");
    both(col, row, "SELECT tag, COUNT(*) FROM tz WHERE tag > 't002000' GROUP BY tag", "tz-groupby");
    // NOT NULL TEXT group column => the raw-string-key GROUP BY fast path.
    both(col, row, "SELECT bkt, COUNT(*), SUM(v), MIN(v), MAX(v) FROM tz GROUP BY bkt", "tz-gb-bkt");
    both(col, row, "SELECT bkt, COUNT(*) FROM tz WHERE v > 200 GROUP BY bkt", "tz-gb-bkt-filt");
    both(col, row, "SELECT tag, SUM(v) FROM tz GROUP BY tag ORDER BY tag DESC LIMIT 5", "tz-gb-tag");
}

}  // namespace

int main() {
    std::uint64_t seeds = 60;
    if (const char* e = std::getenv("LOCKSTEP_SQL_SEEDS")) {
        seeds = std::strtoull(e, nullptr, 10);
    }
    for (std::uint64_t s = 0; s < seeds; ++s) {
        run_seed(s * 0x9E37ULL + 1);
    }
    run_large();
    run_autoflush();
    run_overlays();
    run_text_zones();
    run_join();
    if (g_fail == 0) {
        std::printf("sql_columnar_test: ALL PASS (columnar == row-mode across the workload)\n");
    }
    return g_fail;
}
