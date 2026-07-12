#pragma once

// Engine.hpp — SQL SURFACE: the PLANNER/EXECUTOR that LOWERS a parsed Statement
// onto the EXISTING verified surface (query::Database + Query<L> + the txn submit
// path). This is the whole point: SQL is SUGAR. Nothing here re-implements storage,
// MVCC, or the txn executor — every read goes through Database::run(Query<L>) and
// every write through Database::submit(TxnFn) over the verified deterministic
// executor. core/sim/consensus/txn/storage are UNCHANGED.
//
// THE LOWERING (the one place SQL semantics map to the verified surface):
//   CREATE  -> a Catalog entry (in-memory schema; no storage write).
//   INSERT  -> a one-shot TxnFn: read the row KEY (to detect a duplicate PK), and
//              if absent write encode_value(row); the committed write-set is then
//              applied to the read-path store. A duplicate PK is an ERROR.
//   UPDATE  -> a one-shot TxnFn: read the row KEY; if present, decode, set one
//              column, re-encode, write. A missing row is reported (0 rows).
//   DELETE  -> a one-shot TxnFn: read the row KEY; if present, write a tombstone
//              (encode a DELETED marker) so the read path treats it as absent.
//   SELECT  -> Database::run(Query<L>) at the call-site-visible D5 level:
//                WHERE pk = v        -> Query.get(encode_key)            (point)
//                WHERE pk BETWEEN ab -> Query.scan(encode_key(a), encode_key(b)+1) (range)
//                no WHERE            -> Query.scan(table_prefix, table_prefix_end) (full scan)
//              then DECODE each returned KV back to a row + PROJECT the columns.
//
// v2 SELECT PIPELINE (the new layer; STILL sugar over the same scan/get/range read).
// The PLANNER picks the storage read, then runs an in-memory pipeline over the rows:
//   1. READ — the PK fast path (point/range) is taken iff the WHERE is EXACTLY a PK
//      equality / PK BETWEEN over the real PK column (the parser records a candidate;
//      the Engine validates it is the PK). Otherwise the planner does a FULL SCAN of
//      the table's contiguous key namespace and applies the general predicate as a
//      ROW-LEVEL FILTER. (Planner choice documented at run_select_at_level.)
//   2. WHERE — evaluate the general boolean predicate tree on each decoded row.
//   3. GROUP/AGGREGATE — if the SELECT has aggregates or GROUP BY, fold rows into
//      DETERMINISTIC groups (sorted group keys) and compute COUNT/SUM/MIN/MAX/AVG.
//      AVG over INT TRUNCATES toward zero (SUM/COUNT integer division — documented
//      below). A SELECT list mixing a non-grouped, non-aggregate column is REJECTED.
//   4. HAVING — filter groups by a predicate over the aggregate results.
//   5. DISTINCT — drop duplicate output rows (stable, first occurrence kept).
//   6. ORDER BY — a STABLE TOTAL order over the requested keys, TIE-BROKEN BY PK so
//      the byte image is deterministic (V-RKV1). Applied AFTER where/group/having.
//   7. LIMIT/OFFSET — slice the ordered result.
// The whole pipeline is a pure function of (catalog, committed history, statement):
// no clock, no rng, ordered containers / std::stable_sort only.
//
// TYPE SEMANTICS (documented, mirrored by the reference model in the conformance gate):
//   * SUM(INT) / MIN / MAX / AVG over INT -> INT. SUM(TEXT) is a clean error.
//   * MIN/MAX work for INT (numeric order) and TEXT (lexicographic byte order).
//   * COUNT(*) counts rows; COUNT(col) counts rows where col is present (every column
//     is non-NULL in this subset, so COUNT(col) == COUNT(*) — kept distinct for the
//     grammar + future NULLs; the reference model uses the same definition).
//   * AVG(INT) = SUM/COUNT with INTEGER TRUNCATION TOWARD ZERO (e.g. AVG of {1,2} = 1,
//     AVG of {-3,-3,-2} = -2). An empty group's AVG/SUM/MIN/MAX is omitted (the group
//     does not exist); COUNT of an empty group is 0 but empty groups never arise (a
//     group exists iff >=1 row maps to it).
//
// TOMBSTONE NOTE: the txn WriteSet models writes as key->value puts (no native del
// at this seam), so DELETE writes a reserved one-byte sentinel value the SELECT
// decode treats as "row absent" (it is filtered out of results). This keeps storage
// UNCHANGED while giving SQL DELETE the right observable semantics.
//
// v3 JOIN PIPELINE (the new layer; STILL sugar over the same per-table scan reads).
// FROM produces a row stream per base table (each a Database scan, decoded); JOIN
// COMBINES the streams BEFORE WHERE in the logical plan, then WHERE/GROUP/HAVING/
// ORDER/LIMIT apply over the JOINED row. The planner is:
//   * each base table  -> a FULL SCAN of its key namespace (decode rows). (A JOIN
//     never uses the PK point/range fast path: the parser suppresses it when there
//     is >1 FROM entry or an alias != table.)
//   * INNER JOIN ON p  -> a HASH JOIN when p has a top-level equi-join key
//     `left.col = right.col` (build a hash table on the right's key, probe with the
//     left); else a NESTED-LOOP join evaluating the full ON predicate per pair.
//   * LEFT [OUTER] JOIN -> the same, but every left row with NO matching right row
//     emits ONE output row whose right-side columns are NULL.
//   * CROSS join (comma FROM a,b or CROSS JOIN) -> the cartesian product.
//   * multiple joins are LEFT-DEEP: ((a join b) join c) ...; the accumulated left
//     side is itself a joined-row stream.
//
// QUALIFIED-COLUMN RESOLUTION (the joined schema). Each base table binds under an
// ALIAS (its AS name, else the table name; aliases MUST be unique — a self-join uses
// distinct aliases). A column reference resolves as:
//   * `alias.col`  -> the column `col` of the table bound to `alias` (unknown alias /
//     unknown column => a clean error).
//   * bare `col`   -> the unique table that has a column named `col`. If MORE THAN ONE
//     joined table has `col`, it is AMBIGUOUS => a clean error (fail-closed). If none,
//     unknown column => a clean error.
//
// v4 EXPLICIT NULL + SUBQUERIES (documented + EXACTLY mirrored by the reference model in
// sql_subquery_test.cpp — a divergence is a BUG the conformance gate catches):
//
//   NULLABLE COLUMNS. A column is NULLABLE unless declared NOT NULL; the PRIMARY KEY is
//   ALWAYS NOT NULL (forced in exec_create). A NULL is stored as a single reserved tag
//   byte (Catalog::kNullTag) in the value tuple — a row with NO nulls encodes byte-
//   identically to pre-v4, so the existing conformance/order gates still hold. INSERT
//   may OMIT a nullable column (=> NULL) or write the `NULL` literal; omitting a NOT NULL
//   column, or writing NULL into one, is a RAISED error (fail-closed). UPDATE ... SET
//   col = NULL stores a NULL into a nullable column. A NULL column value gets NO
//   secondary-index entry (a NULL is never matched by an indexed `= v` / BETWEEN lookup).
//
//   IS NULL / IS NOT NULL. `col IS NULL` is TRUE iff the value is NULL; `col IS NOT NULL`
//   is its negation. These are the ONLY predicates that can be TRUE for a NULL operand.
//
//   THREE-VALUED LOGIC (collapsed to two-valued AT THE FILTER: a row is kept iff the
//   predicate is TRUE; UNKNOWN => dropped). A comparison (=,!=,<,<=,>,>=) with a NULL
//   operand is UNKNOWN => false: BOTH `val = 5` and `val != 5` DROP a NULL-val row.
//
//   AGGREGATES SKIP NULL. COUNT(*) counts EVERY row; COUNT(col)/SUM/MIN/MAX/AVG aggregate
//   the PRESENT (non-NULL) values only. A NON-empty group whose aggregated column is ALL
//   NULL yields COUNT=0, SUM=0, MIN/MAX/AVG=NULL. (The synthetic ungrouped-over-EMPTY-
//   table group keeps the pinned pre-v4 rendering: COUNT=0, SUM=0, MIN/MAX/AVG=0.)
//   GROUP BY treats NULL as ONE distinct group (all NULLs of a column group together);
//   a NULL group key sorts FIRST under ORDER BY (cmp_datum: NULLs-first).
//
//   SUBQUERIES (UNCORRELATED only; lowered by RUNNING the inner SELECT through the same
//   exec_select pipeline, ONCE, then applying the predicate — FLAG: a correlated subquery
//   referencing an outer column resolves it as unknown => a clean error, never a wrong
//   answer):
//     * SCALAR `col <op> (SELECT agg/single-col)`: the subquery MUST return at most ONE
//       row / exactly one column. >1 row is a RAISED error (cardinality, like real SQL);
//       0 rows => the scalar is NULL => the comparison is UNKNOWN => the row is dropped.
//     * `col [NOT] IN (SELECT col)`: membership under three-valued logic. IN is TRUE iff
//       the probe equals some PRESENT subquery value. NOT IN is TRUE iff the probe equals
//       NO present value AND the subquery had NO NULL; if the probe matches no present
//       value BUT the subquery contained a NULL, NOT IN is UNKNOWN => the row is dropped
//       (the load-bearing NULL-in-NOT-IN rule). A NULL probe is UNKNOWN for both.
//     * `[NOT] EXISTS (SELECT ...)`: TRUE iff the subquery returns >=1 row (any shape);
//       never UNKNOWN. NOT EXISTS is its negation (arrives as a prefix-NOT wrapping it).
//   Subqueries are OUT of HAVING (a clean error).
//
// NULL SEMANTICS (introduced ONLY by LEFT JOIN; documented + mirrored in the model):
//   * A comparison with a NULL operand is UNKNOWN, treated as FALSE in WHERE/ON/HAVING
//     (SQL three-valued logic collapsed to two-valued at the filter: a row is kept iff
//     the predicate is TRUE). NOT UNKNOWN is still UNKNOWN (== false). So `WHERE
//     r.col = 5` and `WHERE r.col != 5` both DROP a NULL r.col row.
//   * COUNT(*) counts rows (incl. NULL-filled ones). COUNT(col) SKIPS NULLs (counts
//     only present values). SUM/MIN/MAX/AVG SKIP NULLs (aggregate over present values
//     only); a group with ALL-NULL values for the aggregated column yields SUM=0 and
//     MIN/MAX/AVG NULL (no present value). GROUP BY treats NULL as one distinct group
//     (all NULLs of a column group together).
//   * DISTINCT treats two same-type NULLs as the same output cell (one NULL row).
//
// DEFAULT OUTPUT ORDER (no ORDER BY) for a JOIN: LEFT-table scan order (PK-ascending),
// then for each left row the RIGHT-table scan order (PK-ascending) of its matches —
// i.e. the deterministic nested iteration order (left-major, right-minor), extended
// left-deep across multiple joins. With ORDER BY, the requested keys apply, TIE-BROKEN
// by this default join order (a stable_sort over the default-ordered stream) so the
// byte image is always deterministic (V-RKV1). The reference model pins the SAME order.
//
// DETERMINISM: pure function of (catalog, committed history, statement). No clock,
// no rng, no threads. The committed write-set history is replayed into the read
// store via the verified Database::prime — so a SELECT reads EXACTLY what the typed
// query surface would for the same KV writes (the conformance gate proves it).

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>  // K1 perf: std::endian for the LE payload fast path
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lockstep/storage/Backup.hpp>
#include <lockstep/storage/Codec.hpp>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/ParallelExecutor.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/sql/Ast.hpp>
#include <lockstep/query/sql/Catalog.hpp>
#include <lockstep/query/sql/ColumnBlock.hpp>
#include <lockstep/query/sql/Json.hpp>
#include <lockstep/query/sql/Parser.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace lockstep::query::sql {

// The reserved tombstone value a DELETE writes (an opaque byte unlikely to be a real
// encoded value: a 0-length encoded tuple has length 0, so a 1-byte 0xFF marker is
// distinct and decode treats it as "row deleted / absent").
inline const std::string& tombstone_marker() {
    static const std::string kTomb(1, static_cast<char>(0xFF));
    return kTomb;
}
[[nodiscard]] inline bool is_tombstone(const Value& v) {
    return v == tombstone_marker();
}

// One result row of a SELECT: the projected columns (name -> rendered datum), kept
// as an ordered name+datum vector so rendering is deterministic + projection-stable.
struct ResultRow {
    std::vector<std::pair<std::string, Datum>> cells;
};

// The outcome of executing one SQL statement.
struct ExecResult {
    bool ok = true;
    std::string error;        // set iff !ok (unknown table/column, dup PK, type ...)
    std::uint64_t affected = 0;  // rows inserted/updated/deleted
    std::vector<ResultRow> rows;  // SELECT output, in PK-ascending (scan) order

    static ExecResult failure(std::string msg) {
        ExecResult r;
        r.ok = false;
        r.error = std::move(msg);
        return r;
    }
};

// ---- Logical SQL backup (whole DB = catalog + data) ------------------------------------------
// A SqlEngine has TWO independent stores on TWO schedulers: the catalog store (schemas) and the
// data store (rows/blocks). A single IDisk cannot be driven by both schedulers, so each store backs
// up to a self-contained storage image (storage::backup_engine_bytes) on its OWN scheduler and an
// outer SQL wrapper concatenates the section(s) into one stream written to the caller's `out`.
//
// SCOPE: SchemaOnly backs up the catalog only (table/index/type definitions, no rows); Full backs up
// catalog + data. restore() reads the scope from the stream header, so a SchemaOnly image can never
// silently restore stale data, and a Full image always restores both.
//
// STREAM: header(16B) = magic "LSQB"(4) · version u32 · scope u32 · nsec u32 ; then `nsec` sections,
// each = len u64 · storage-backup-image[len]. Section 0 = catalog; section 1 (Full only) = data.
enum class SqlBackupScope : std::uint32_t { SchemaOnly = 0, Full = 1 };

inline constexpr std::uint32_t kSqlBackupVersion = 1;
inline constexpr std::size_t kSqlBackupHeaderBytes = 16;

namespace backup_detail {
[[nodiscard]] inline bool sql_magic_ok(const std::byte* p) noexcept {
    return std::to_integer<char>(p[0]) == 'L' && std::to_integer<char>(p[1]) == 'S' &&
           std::to_integer<char>(p[2]) == 'Q' && std::to_integer<char>(p[3]) == 'B';
}

// Append the assembled stream to `out` + sync (runs on the caller's out scheduler). `image` is moved
// into the coroutine frame (stable for the suspend), so this is NOT a dangling-capture lambda.
inline core::Task write_stream(core::IDisk& out, std::vector<std::byte> image, core::Error& result) {
    core::Offset off = 0;
    if (const core::Error e = co_await out.append(std::span<const std::byte>(image.data(), image.size()), off);
        !e.ok()) {
        result = e;
        co_return;
    }
    result = co_await out.sync();
    co_return;
}

// Read + parse the SQL backup stream from `in` into per-section byte blobs (runs on the caller's in
// scheduler). Bounds + magic + version checked; a short/garbage stream → Corruption (no partial).
inline core::Task read_stream(core::IDisk& in, std::uint32_t& scope_out,
                              std::vector<std::vector<std::byte>>& sections_out, core::Error& result) {
    std::array<std::byte, kSqlBackupHeaderBytes> hdr{};
    if (const core::Error e = co_await in.read(0, std::span<std::byte>(hdr.data(), hdr.size())); !e.ok()) {
        result = e;
        co_return;
    }
    if (!sql_magic_ok(hdr.data())) {
        result = core::Error{core::ErrorCode::Corruption, "sql backup: bad magic"};
        co_return;
    }
    if (storage::get_u32(hdr.data() + 4) != kSqlBackupVersion) {
        result = core::Error{core::ErrorCode::InvalidArgument, "sql backup: unsupported version"};
        co_return;
    }
    scope_out = storage::get_u32(hdr.data() + 8);
    const std::uint32_t nsec = storage::get_u32(hdr.data() + 12);
    if (nsec == 0 || nsec > 2) {
        result = core::Error{core::ErrorCode::Corruption, "sql backup: bad section count"};
        co_return;
    }
    core::Offset off = static_cast<core::Offset>(kSqlBackupHeaderBytes);
    for (std::uint32_t i = 0; i < nsec; ++i) {
        std::array<std::byte, 8> lb{};
        if (const core::Error e = co_await in.read(off, std::span<std::byte>(lb.data(), lb.size())); !e.ok()) {
            result = e;
            co_return;
        }
        off += 8;
        const std::uint64_t len = storage::get_u64(lb.data());
        // Read in bounded chunks rather than pre-allocating `len` bytes: a CORRUPT length field
        // (e.g. a flipped bit inflating it to gigabytes) then fails on the first read past the
        // device end instead of attempting one enormous allocation. The buffer only ever grows to
        // the bytes actually read.
        constexpr std::size_t kChunk = std::size_t{1} << 16;
        std::vector<std::byte> sec;
        std::uint64_t remaining = len;
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kChunk));
            const std::size_t base = sec.size();
            sec.resize(base + want);
            if (const core::Error e = co_await in.read(off, std::span<std::byte>(sec.data() + base, want)); !e.ok()) {
                result = e;
                co_return;
            }
            off += static_cast<core::Offset>(want);
            remaining -= want;
        }
        sections_out.push_back(std::move(sec));
    }
    result = core::Error{};
    co_return;
}

// Read a framed SQL-TEXT dump (magic "LSQL" + version u32 + len u64 + utf8 text) from `in` into
// `out`. Magic/version checked; the text is read in bounded chunks (a corrupt length fails on a read
// past the device end rather than a giant allocation). A non-dump / short stream → Corruption.
inline core::Task read_text_stream(core::IDisk& in, std::string& out, core::Error& result) {
    std::array<std::byte, 16> hdr{};
    if (const core::Error e = co_await in.read(0, std::span<std::byte>(hdr.data(), hdr.size())); !e.ok()) {
        result = e;
        co_return;
    }
    if (!(std::to_integer<char>(hdr[0]) == 'L' && std::to_integer<char>(hdr[1]) == 'S' &&
          std::to_integer<char>(hdr[2]) == 'Q' && std::to_integer<char>(hdr[3]) == 'L')) {
        result = core::Error{core::ErrorCode::Corruption, "sql dump: bad magic"};
        co_return;
    }
    if (storage::get_u32(hdr.data() + 4) != kSqlBackupVersion) {
        result = core::Error{core::ErrorCode::InvalidArgument, "sql dump: unsupported version"};
        co_return;
    }
    const std::uint64_t len = storage::get_u64(hdr.data() + 8);
    constexpr std::size_t kChunk = std::size_t{1} << 16;
    core::Offset off = 16;
    std::uint64_t remaining = len;
    out.clear();
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kChunk));
        std::vector<std::byte> buf(want);
        if (const core::Error e = co_await in.read(off, std::span<std::byte>(buf.data(), buf.size())); !e.ok()) {
            result = e;
            co_return;
        }
        for (std::byte b : buf) out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
        off += static_cast<core::Offset>(want);
        remaining -= want;
    }
    result = core::Error{};
    co_return;
}
}  // namespace backup_detail

// ----------------------------------------------------------------------------
// THE SQL ENGINE. Owns a Catalog + a query::Database (the verified surface) + the
// committed write-set HISTORY (so the read path can be primed). Each write statement
// submits ONE txn (one statement == one txn in v1; BEGIN/COMMIT multi-statement is
// OUT — FLAG) and, on commit, appends its write-set to the history + re-primes the
// read store. A SELECT runs through Database::run(Query<L>).
// ----------------------------------------------------------------------------
class SqlEngine {
public:
    SqlEngine() = default;

    // DURABLE DATA backing (crash/recovery testing + a real durable server): the committed
    // query state is a WalEngine over the injected IDisk (a SimDisk for a deterministic
    // crash test, a ProdDisk for real on-disk recovery). The CATALOG (schemas) is held in a
    // SEPARATE in-memory store here (its own Seq line — so DDL never shifts data versions);
    // the catalog is NOT durable in this 2-arg form — the caller re-establishes DDL. Use the
    // 4-arg ctor below for a durably-recoverable catalog (C7).
    SqlEngine(core::Scheduler& sched, core::IDisk& disk) : db_(sched, disk) {}

    // DURABLE DATA + DURABLE CATALOG (C7): the data store (db_) and the catalog store
    // (catalog_db_) each get their OWN scheduler + IDisk, hence their OWN WAL and OWN Seq
    // line. Keeping the catalog out of db_'s Seq line is what preserves the data-MVCC version
    // numbering (`AT SNAPSHOT N` counts DATA writes only), while still surviving a restart
    // (recover() replays both). Two WalEngines cannot share one WAL — exactly the keyed-vs-SQL
    // split, applied again to data-vs-catalog.
    SqlEngine(core::Scheduler& sched, core::IDisk& disk, core::Scheduler& cat_sched,
              core::IDisk& cat_disk)
        : db_(sched, disk), catalog_db_(cat_sched, cat_disk), catalog_durable_(true) {}

    // Recover the committed DATA query state from the durable disk image (after a crash). The
    // catalog is recovered separately (recover(data_len, catalog_len)); this 1-arg form recovers
    // data only and rebuilds the catalog from whatever catalog_db_ holds (durable when the 4-arg
    // ctor was used, else empty — the caller replays DDL).
    void recover(std::size_t durable_len) { recover(durable_len, 0); }

    // Recover DATA (db_, data_len bytes) AND CATALOG (catalog_db_, catalog_len bytes) from their
    // OWN durable images, then rebuild the in-memory Catalog from the catalog store's schema
    // records. Two stores ⇒ two byte lengths. catalog_len is ignored when the catalog is not
    // disk-backed (the 2-arg ctor); the scan then sees the in-memory catalog_db_.
    void recover(std::size_t data_len, std::size_t catalog_len) {
        db_.recover(data_len);
        tip_ = db_.tip();
        chunk_cache_.clear();  // recovered blocks may differ from any cached decode
        concat_cache_.clear();
        text_dict_cache_.clear();
        zone_cache_.clear();
        if (catalog_durable_) {
            catalog_db_.recover(catalog_len);
        }
        reprime_catalog_from_store();
    }

    // Rebuild the in-memory Catalog from the catalog store's durable schema records (reserved 0x01
    // namespace + 0x02 empty-schema markers). Without this the recovered ROW/BLOCK data is
    // uninterpretable (the schema lived only in memory). Shared by recover() and restore() — each
    // serialized Table is re-registered with its PERSISTED id (the data keys are namespaced by it).
    void reprime_catalog_from_store() {
        catalog_ = Catalog{};  // restore() may run on a fresh engine; recover() starts empty too
        std::vector<storage::KeyValue> kvs;
        {
            Query<Strict> q;
            q.scan(std::string(1, '\x01'), std::string(1, '\x02'));
            collect(catalog_db_.run(q), kvs);
        }
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            Table t = deserialize_schema(kv.second);
            t.delta_dirty = t.columnar;  // force a delta merge for columnar (safe); stats stay 0
            const std::string nm = t.name;
            (void)catalog_.insert_recovered(std::move(t));
            const std::size_t dot = nm.find('.');  // E4: re-register the table's schema
            if (dot != std::string::npos) (void)catalog_.create_schema(nm.substr(0, dot), true);
        }
        // E4: recover empty-schema markers ('\x02' + name).
        std::vector<storage::KeyValue> smarks;
        {
            Query<Strict> q;
            q.scan(std::string(1, '\x02'), std::string(1, '\x03'));
            collect(catalog_db_.run(q), smarks);
        }
        for (const storage::KeyValue& kv : smarks) {
            if (is_tombstone(kv.second)) continue;
            (void)catalog_.create_schema(kv.first.substr(1), true);
        }
        // H1: recover view records ('\x04' + name -> serialized View).
        std::vector<storage::KeyValue> vrecs;
        {
            Query<Strict> q;
            q.scan(std::string(1, '\x04'), std::string(1, '\x05'));
            collect(catalog_db_.run(q), vrecs);
        }
        for (const storage::KeyValue& kv : vrecs) {
            if (is_tombstone(kv.second)) continue;
            (void)catalog_.insert_recovered_view(deserialize_view(kv.second));
        }
        // MATERIALIZED VIEW sources ('\x06' + qualified name -> raw SELECT text). The backing
        // table recovered above via the normal 0x01 table records; this restores REFRESH-ability.
        matviews_.clear();
        std::vector<storage::KeyValue> mvrecs;
        {
            Query<Strict> q;
            q.scan(std::string(1, '\x06'), std::string(1, '\x07'));
            collect(catalog_db_.run(q), mvrecs);
        }
        for (const storage::KeyValue& kv : mvrecs) {
            if (is_tombstone(kv.second)) continue;
            matviews_[kv.first.substr(1)] = kv.second;
        }
    }

    // True iff the catalog store is disk-backed (4-arg ctor) — the caller then persists the
    // catalog disk's length alongside the data disk's and passes both to recover().
    [[nodiscard]] bool catalog_durable() const noexcept { return catalog_durable_; }

    // ---- Logical backup / restore of the WHOLE SQL database (schemas + optionally data) ----
    // Write a self-contained, CRC-protected logical backup of this engine to `out` (driven on
    // `out_sched`, the scheduler that runs `out`). SchemaOnly captures the catalog (every table /
    // index / type definition); Full also captures all row + columnar data as-of the live tip. The
    // image is portable + point-in-time (see storage::backup_engine — same record stream, here for
    // BOTH the catalog and the data store). Determinism: identical state ⇒ byte-identical stream.
    [[nodiscard]] core::Error backup(core::Scheduler& out_sched, core::IDisk& out,
                                     SqlBackupScope scope = SqlBackupScope::Full) {
        std::vector<std::byte> cat_img;
        if (const core::Error e = storage::backup_engine_bytes(
                catalog_db_.scheduler(), catalog_db_.engine(),
                storage::Snapshot{catalog_db_.live_snap_seq()}, cat_img);
            !e.ok()) {
            return e;
        }
        std::vector<std::byte> data_img;
        if (scope == SqlBackupScope::Full) {
            if (const core::Error e = storage::backup_engine_bytes(
                    db_.scheduler(), db_.engine(), storage::Snapshot{db_.live_snap_seq()}, data_img);
                !e.ok()) {
                return e;
            }
        }
        const std::uint32_t nsec = (scope == SqlBackupScope::Full) ? 2U : 1U;
        std::vector<std::byte> image;
        for (char c : std::string_view("LSQB")) image.push_back(static_cast<std::byte>(c));
        storage::put_u32(image, kSqlBackupVersion);
        storage::put_u32(image, static_cast<std::uint32_t>(scope));
        storage::put_u32(image, nsec);
        storage::put_u64(image, static_cast<std::uint64_t>(cat_img.size()));
        image.insert(image.end(), cat_img.begin(), cat_img.end());
        if (scope == SqlBackupScope::Full) {
            storage::put_u64(image, static_cast<std::uint64_t>(data_img.size()));
            image.insert(image.end(), data_img.begin(), data_img.end());
        }
        core::Error result = core::Error{core::ErrorCode::Unknown, "sql backup: did not run"};
        out_sched.spawn(backup_detail::write_stream(out, std::move(image), result));
        out_sched.run();
        return result;
    }

    // Restore a logical backup from `in` (driven on `in_sched`) into THIS (fresh) engine. The whole
    // stream is CRC-verified before anything is applied — a torn/corrupt/short backup is REJECTED
    // and the engine is left untouched (V-NOTORN). The scope is read from the stream: a SchemaOnly
    // backup restores schemas only (the data store stays empty); a Full backup restores both. The
    // in-memory catalog is rebuilt from the restored catalog store (reprime_catalog_from_store).
    [[nodiscard]] core::Error restore(core::Scheduler& in_sched, core::IDisk& in) {
        // FRESH-ENGINE contract: restoring onto a populated engine would overlay records and leave
        // stale tables/rows the backup never had (the catalog reprime sees BOTH). Require an empty
        // target so the restored state is EXACTLY the backup, with nothing left over.
        if (!catalog_.all().empty() || db_.tip() != 0) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "sql restore: target engine must be fresh (empty)"};
        }
        std::uint32_t scope_raw = 0;
        std::vector<std::vector<std::byte>> sections;
        core::Error r = core::Error{core::ErrorCode::Unknown, "sql restore: did not run"};
        in_sched.spawn(backup_detail::read_stream(in, scope_raw, sections, r));
        in_sched.run();
        if (!r.ok()) {
            return r;
        }
        if (sections.empty()) {
            return core::Error{core::ErrorCode::Corruption, "sql backup: missing catalog section"};
        }
        const bool full = scope_raw == static_cast<std::uint32_t>(SqlBackupScope::Full);
        if (full && sections.size() < 2) {
            return core::Error{core::ErrorCode::Corruption, "sql backup: missing data section"};
        }
        // ATOMIC: CRC-verify EVERY section BEFORE applying ANY, so a corrupt DATA section cannot
        // leave the catalog half-restored (all-or-nothing across both stores, V-NOTORN).
        if (const core::Error e = storage::validate_backup_image(sections[0]); !e.ok()) {
            return e;
        }
        if (full) {
            if (const core::Error e = storage::validate_backup_image(sections[1]); !e.ok()) {
                return e;
            }
        }
        if (const core::Error e = catalog_db_.restore_image(sections[0]); !e.ok()) {
            return e;
        }
        if (full) {
            if (const core::Error e = db_.restore_image(sections[1]); !e.ok()) {
                return e;
            }
        }
        tip_ = db_.tip();
        chunk_cache_.clear();
        concat_cache_.clear();
        text_dict_cache_.clear();
        zone_cache_.clear();
        reprime_catalog_from_store();
        return core::Error{};
    }

    // ============================================================================================
    // SQL-TEXT backup (pg_dump style): emit a portable .sql script of CREATE SCHEMA / CREATE TABLE /
    // CREATE INDEX (+ INSERTs for Full) that, replayed through the verified exec() path, rebuilds the
    // database. Human-readable + portable to any SQL-ish tool; the engine-specific columnar attribute
    // rides as a `-- lockstep:columnar` directive (a comment our restore_sql honours, ignored by other
    // tools). The binary backup() above stays the FULL-fidelity path; this text path covers the core
    // relational surface and REFUSES (clear error) a column whose type it cannot yet round-trip.
    // ============================================================================================

    // Build the .sql script in `out`. scope: SchemaOnly = DDL only; Full = DDL + one INSERT per row.
    [[nodiscard]] core::Error dump_sql_string(SqlBackupScope scope, std::string& out) {
        out.clear();
        for (const std::string& s : catalog_.schemas()) {
            out += "CREATE SCHEMA IF NOT EXISTS " + s + ";\n";
        }
        const std::vector<const Table*> order = sql_table_order();
        for (const Table* tp : order) {
            if (tp->columnar) out += "-- lockstep:columnar\n";
            std::string ddl;
            if (const core::Error e = sql_render_create_table(*tp, ddl); !e.ok()) return e;
            out += ddl + "\n";
            for (const Index& ix : tp->indexes) out += sql_render_create_index(*tp, ix) + "\n";
        }
        if (scope == SqlBackupScope::Full) {
            for (const Table* tp : order) {
                if (const core::Error e = sql_render_inserts(*tp, out); !e.ok()) return e;
            }
        }
        return core::Error{};
    }

    // Write the .sql script to `out` (framed: magic "LSQL" + version u32 + len u64 + utf8 text), so a
    // restore can read it back without a separate length channel and reject a non-dump stream.
    [[nodiscard]] core::Error dump_sql(core::Scheduler& out_sched, core::IDisk& out,
                                       SqlBackupScope scope = SqlBackupScope::Full) {
        std::string text;
        if (const core::Error e = dump_sql_string(scope, text); !e.ok()) return e;
        std::vector<std::byte> image;
        for (char c : std::string_view("LSQL")) image.push_back(static_cast<std::byte>(c));
        storage::put_u32(image, kSqlBackupVersion);
        storage::put_u64(image, static_cast<std::uint64_t>(text.size()));
        for (char c : text) image.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        core::Error result = core::Error{core::ErrorCode::Unknown, "sql dump: did not run"};
        out_sched.spawn(backup_detail::write_stream(out, std::move(image), result));
        out_sched.run();
        return result;
    }

    // Recover the WHOLE database by replaying a .sql script into THIS (fresh) engine: split into
    // single statements (the parser takes one at a time), honour the `-- lockstep:columnar` directive,
    // and exec() each. Aborts on the first failing statement (returns its error). Fresh-engine guarded.
    [[nodiscard]] core::Error restore_sql_string(const std::string& script) {
        if (!catalog_.all().empty() || db_.tip() != 0) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "sql restore: target engine must be fresh (empty)"};
        }
        bool columnar_pending = false;
        auto run_stmt = [&](const std::string& raw) -> core::Error {
            std::size_t b = raw.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return core::Error{};  // blank
            const std::size_t e = raw.find_last_not_of(" \t\r\n");
            const std::string stmt = raw.substr(b, e - b + 1);
            const bool col = columnar_pending;
            columnar_pending = false;
            if (col) set_columnar_default(true);
            const ExecResult r = exec(stmt);
            if (col) set_columnar_default(false);
            if (!r.ok) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "sql restore: a statement in the script failed to execute"};
            }
            return core::Error{};
        };
        std::string cur;
        bool in_str = false;
        bool at_start = true;  // at the start of a statement (between ';' and the next real token)
        const std::size_t n = script.size();
        for (std::size_t i = 0; i < n;) {
            const char ch = script[i];
            if (!in_str && at_start && ch == '-' && i + 1 < n && script[i + 1] == '-') {
                std::size_t j = i + 2;  // a directive / comment line — consumed, never parsed
                std::string line;
                while (j < n && script[j] != '\n') line.push_back(script[j++]);
                if (line.find("lockstep:columnar") != std::string::npos) columnar_pending = true;
                i = (j < n) ? j + 1 : j;
                continue;
            }
            if (ch == '\'') {
                if (in_str && i + 1 < n && script[i + 1] == '\'') {  // '' escaped quote inside a string
                    cur += "''";
                    i += 2;
                    continue;
                }
                in_str = !in_str;
                cur.push_back(ch);
                ++i;
                continue;
            }
            if (!in_str && ch == ';') {
                if (const core::Error e = run_stmt(cur); !e.ok()) return e;
                cur.clear();
                at_start = true;
                ++i;
                continue;
            }
            if (!in_str && (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')) {
                if (!cur.empty()) cur.push_back(ch);  // keep interior whitespace; ignore leading
                ++i;
                continue;
            }
            cur.push_back(ch);
            at_start = false;
            ++i;
        }
        return run_stmt(cur);  // a trailing statement with no terminating ';'
    }

    // Read a framed .sql dump from `in` and replay it (see restore_sql_string).
    [[nodiscard]] core::Error restore_sql(core::Scheduler& in_sched, core::IDisk& in) {
        std::string text;
        core::Error r = core::Error{core::ErrorCode::Unknown, "sql restore: did not run"};
        in_sched.spawn(backup_detail::read_text_stream(in, text, r));
        in_sched.run();
        if (!r.ok()) return r;
        return restore_sql_string(text);
    }

private:
    // ---- SQL-text renderers (pure, from the catalog) -------------------------------------------
    // Quote + escape a TEXT value as a SQL string literal ('' for an embedded single quote).
    static std::string sql_quote(const std::string& s) {
        std::string o = "'";
        for (char c : s) {
            if (c == '\'') o += "''";
            else o.push_back(c);
        }
        o.push_back('\'');
        return o;
    }

    // Render a column's TYPE token. Returns false for a type the text dump cannot yet round-trip
    // (every logical/exotic type — DECIMAL/DATE/TIMESTAMP/UUID/ENUM/ARRAY/JSON/INT128/…); the caller
    // turns that into a clear error pointing at the binary backup.
    static bool sql_column_type(const Column& c, std::string& out) {
        if (c.type == Type::Int && c.logical == 0) {
            switch (c.int_bits) {
                case 8: out = "TINYINT"; break;
                case 16: out = "SMALLINT"; break;
                case 32: out = "INT32"; break;
                default: out = "BIGINT"; break;
            }
            if (c.is_unsigned) out += " UNSIGNED";
            return true;
        }
        if (c.type == Type::Text && c.logical == 0) {
            if (c.max_len > 0) {
                out = (c.fixed_char ? "CHAR(" : "VARCHAR(") + std::to_string(c.max_len) + ")";
            } else {
                out = "TEXT";
            }
            return true;
        }
        return false;
    }

    // A SQL literal for one stored cell (only reached for supported, logical-0 columns).
    static std::string sql_literal(const Datum& d) {
        if (d.is_null) return "NULL";
        if (d.type == Type::Int) return std::to_string(d.i);
        return sql_quote(d.s);
    }

    [[nodiscard]] core::Error sql_render_create_table(const Table& t, std::string& out) {
        out = "CREATE TABLE " + t.name + " (";
        bool first = true;
        for (const Column& c : t.columns) {
            if (c.dropped) continue;  // a logically-dropped column is gone after a fresh rebuild
            std::string ty;
            if (!sql_column_type(c, ty)) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "sql dump: a column type cannot be round-tripped by the SQL-text "
                                   "dump yet; use the binary backup() for this database"};
            }
            if (!first) out += ", ";
            first = false;
            out += c.name + " " + ty;
            if (!c.nullable) out += " NOT NULL";
            if (c.auto_increment) out += " AUTO_INCREMENT";
            if (c.unique) out += " UNIQUE";
            if (c.has_default) {
                out += " DEFAULT " + (c.type == Type::Int ? std::to_string(c.default_i)
                                                          : sql_quote(c.default_s));
            }
            if (!c.fk_table.empty()) {
                out += " REFERENCES " + c.fk_table;
                if (!c.fk_column.empty()) out += "(" + c.fk_column + ")";
            }
        }
        for (const std::string& chk : t.checks) out += ", CHECK (" + chk + ")";
        out += ", PRIMARY KEY (";
        if (t.pk_columns.empty()) {
            out += t.columns[t.pk_index].name;  // back-compat (recovered single-col PK)
        } else {
            for (std::size_t k = 0; k < t.pk_columns.size(); ++k) {
                if (k != 0) out += ", ";
                out += t.columns[t.pk_columns[k]].name;
            }
        }
        out += "));";
        return core::Error{};
    }

    [[nodiscard]] std::string sql_render_create_index(const Table& t, const Index& ix) {
        std::string o = "CREATE ";
        if (ix.unique) o += "UNIQUE ";
        o += "INDEX " + ix.name + " ON " + t.name + " (";
        if (!ix.expr_src.empty()) {
            o += "(" + ix.expr_src + ")";  // expression index: ON t ((expr))
        } else if (!ix.columns.empty()) {
            for (std::size_t k = 0; k < ix.columns.size(); ++k) {
                if (k != 0) o += ", ";
                o += t.columns[ix.columns[k]].name;
            }
        } else {
            o += t.columns[ix.column].name;
        }
        o += ")";
        if (ix.gin) o += " USING GIN";
        else if (ix.ivfflat) o += " USING IVFFLAT";  // K1.3 (WITH knobs are rebuild-time defaults)
        else if (ix.hnsw) o += " USING HNSW";        // K1.4
        else if (ix.bm25) o += " USING BM25";        // K2
        else if (ix.hash) o += " USING HASH";
        if (!ix.partial_src.empty()) o += " WHERE " + ix.partial_src;
        o += ";";
        return o;
    }

    [[nodiscard]] core::Error sql_render_inserts(const Table& t, std::string& out) {
        std::string collist;
        std::string sel = "SELECT ";
        bool first = true;
        for (const Column& c : t.columns) {
            if (c.dropped) continue;
            std::string ty;
            if (!sql_column_type(c, ty)) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "sql dump: a column type cannot be round-tripped by the SQL-text "
                                   "dump yet; use the binary backup() for this database"};
            }
            if (!first) {
                collist += ", ";
                sel += ", ";
            }
            first = false;
            collist += c.name;
            sel += c.name;
        }
        sel += " FROM " + t.name;
        const ExecResult r = exec(sel);
        if (!r.ok) {
            return core::Error{core::ErrorCode::Unknown, "sql dump: reading a table's rows failed"};
        }
        for (const ResultRow& row : r.rows) {
            out += "INSERT INTO " + t.name + " (" + collist + ") VALUES (";
            for (std::size_t k = 0; k < row.cells.size(); ++k) {
                if (k != 0) out += ", ";
                out += sql_literal(row.cells[k].second);
            }
            out += ");\n";
        }
        return core::Error{};
    }

    // Tables in FOREIGN-KEY dependency order (a referenced parent before its children), so the
    // replayed CREATE/INSERT never references a table/row that does not exist yet. A self-reference or
    // a cycle (rare) falls back to catalog order for the unresolved remainder.
    [[nodiscard]] std::vector<const Table*> sql_table_order() {
        const std::map<std::string, Table>& tabs = catalog_.all();
        std::map<std::string, int> indeg;
        for (const auto& [nm, t] : tabs) indeg[nm] = 0;
        for (const auto& [nm, t] : tabs) {
            for (const Column& c : t.columns) {
                if (!c.fk_table.empty() && c.fk_table != nm && tabs.count(c.fk_table) != 0) {
                    indeg[nm] += 1;  // nm depends on its parent c.fk_table
                }
            }
        }
        std::vector<const Table*> order;
        std::set<std::string> done;
        bool progress = true;
        while (order.size() < tabs.size() && progress) {
            progress = false;
            for (const auto& [nm, t] : tabs) {
                if (done.count(nm) != 0 || indeg[nm] != 0) continue;
                order.push_back(&t);
                done.insert(nm);
                progress = true;
                for (const auto& [cnm, ct] : tabs) {  // relax children of nm
                    if (done.count(cnm) != 0) continue;
                    for (const Column& c : ct.columns) {
                        if (c.fk_table == nm && cnm != nm) indeg[cnm] -= 1;
                    }
                }
            }
        }
        for (const auto& [nm, t] : tabs) {  // any cyclic remainder, catalog order
            if (done.count(nm) == 0) order.push_back(&t);
        }
        return order;
    }

public:
    // ---- Catalog persistence (C7) — schema records under the reserved 0x01 key namespace ----
    static void cat_put_u32(std::string& o, std::uint32_t v) {
        o.push_back(static_cast<char>(v >> 24));
        o.push_back(static_cast<char>(v >> 16));
        o.push_back(static_cast<char>(v >> 8));
        o.push_back(static_cast<char>(v));
    }
    static std::uint32_t cat_get_u32(const std::string& s, std::size_t& p) {
        const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p])) << 24) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 1])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 2])) << 8) |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 3]));
        p += 4;
        return v;
    }
    static void cat_put_s(std::string& o, const std::string& v) {
        cat_put_u32(o, static_cast<std::uint32_t>(v.size()));
        o += v;
    }
    static std::string cat_get_s(const std::string& s, std::size_t& p) {
        const std::uint32_t n = cat_get_u32(s, p);
        std::string v = s.substr(p, n);
        p += n;
        return v;
    }
    static Key catalog_key(const std::string& name) { return std::string(1, '\x01') + name; }
    // H1: view records live in their OWN durable namespace (0x04), clear of the table (0x01) and
    // empty-schema-marker (0x02) key ranges the recovery scans use.
    static Key view_key(const std::string& name) { return std::string(1, '\x04') + name; }

    static std::string serialize_view(const View& v) {
        std::string o;
        cat_put_s(o, v.name);
        cat_put_s(o, v.select_src);
        cat_put_u32(o, static_cast<std::uint32_t>(v.columns.size()));
        for (const std::string& c : v.columns) cat_put_s(o, c);
        return o;
    }
    static View deserialize_view(const std::string& s) {
        View v;
        std::size_t p = 0;
        v.name = cat_get_s(s, p);
        v.select_src = cat_get_s(s, p);
        const std::uint32_t n = cat_get_u32(s, p);
        for (std::uint32_t i = 0; i < n; ++i) v.columns.push_back(cat_get_s(s, p));
        return v;
    }

    static std::string serialize_schema(const Table& t) {
        std::string o;
        cat_put_s(o, t.name);
        cat_put_u32(o, t.id);
        cat_put_u32(o, static_cast<std::uint32_t>(t.pk_index));
        o.push_back(t.columnar ? 1 : 0);
        cat_put_u32(o, t.next_index_id);
        cat_put_s(o, std::to_string(t.next_auto_id));  // F6: persist the AUTO_INCREMENT counter
        cat_put_s(o, std::to_string(t.next_uuid));     // F9c: persist the gen_uuid() counter
        cat_put_u32(o, static_cast<std::uint32_t>(t.columns.size()));
        for (const Column& c : t.columns) {
            cat_put_s(o, c.name);
            o.push_back(static_cast<char>(c.type == Type::Int ? 0 : 1));
            o.push_back(c.nullable ? 1 : 0);
            // F4: persist the DEFAULT (has-default byte, then the value rendered as a string —
            // decimal for INT, raw for TEXT).
            o.push_back(c.has_default ? 1 : 0);
            if (c.has_default) {
                cat_put_s(o, c.type == Type::Int ? std::to_string(c.default_i) : c.default_s);
            }
            o.push_back(c.auto_increment ? 1 : 0);  // F6
            o.push_back(c.unique ? 1 : 0);           // F2
            cat_put_s(o, c.fk_table);                // F3
            cat_put_s(o, c.fk_column);
            o.push_back(static_cast<char>(c.logical));  // F9b: DECIMAL/DATE/TIMESTAMP/UUID logical tag
            o.push_back(static_cast<char>(c.scale));
            o.push_back(c.uuid_default ? 1 : 0);        // F9c
            cat_put_u32(o, c.max_len);                  // F10 domain constraints
            o.push_back(c.fixed_char ? 1 : 0);
            o.push_back(c.is_unsigned ? 1 : 0);
            o.push_back(static_cast<char>(c.precision));
            o.push_back(static_cast<char>(c.int_bits));
            o.push_back(static_cast<char>(c.elem_type == Type::Int ? 0 : 1));  // F12
            o.push_back(static_cast<char>(c.elem_logical));
            o.push_back(static_cast<char>(c.elem_scale));
            cat_put_u32(o, static_cast<std::uint32_t>(c.enum_labels.size()));  // F13
            for (const std::string& lbl : c.enum_labels) cat_put_s(o, lbl);
            o.push_back(c.dropped ? 1 : 0);  // E1
        }
        cat_put_u32(o, static_cast<std::uint32_t>(t.indexes.size()));
        for (const Index& ix : t.indexes) {
            cat_put_s(o, ix.name);
            cat_put_u32(o, ix.id);
            cat_put_u32(o, static_cast<std::uint32_t>(ix.column));
            o.push_back(ix.unique ? 1 : 0);  // E5
            o.push_back(ix.hash ? 1 : 0);    // I7
            cat_put_s(o, ix.partial_src);    // I5
            cat_put_u32(o, static_cast<std::uint32_t>(ix.columns.size()));
            for (const std::size_t c : ix.columns) cat_put_u32(o, static_cast<std::uint32_t>(c));
            cat_put_s(o, ix.expr_src);  // J2: expression index source
            o.push_back(ix.expr_type == Type::Int ? 0 : 1);
            o.push_back(ix.gin ? 1 : 0);  // J3: GIN (array-element) index
            o.push_back(ix.ivfflat ? 1 : 0);  // K1.3: IVFFLAT (approximate k-NN)
            cat_put_u32(o, ix.lists);
            cat_put_u32(o, ix.probes);
            cat_put_s(o, ix.centroids);
            o.push_back(static_cast<char>(ix.vec_op));  // K1.3c: operator class
            o.push_back(ix.hnsw ? 1 : 0);               // K1.4: HNSW (graph k-NN)
            cat_put_u32(o, ix.hnsw_m);
            cat_put_u32(o, ix.hnsw_efc);
            o.push_back(ix.bm25 ? 1 : 0);  // K2: BM25 full-text
        }
        cat_put_u32(o, static_cast<std::uint32_t>(t.checks.size()));  // F5
        for (const std::string& chk : t.checks) {
            cat_put_s(o, chk);
        }
        cat_put_u32(o, static_cast<std::uint32_t>(t.pk_columns.size()));  // F1 (0 => single-col)
        for (const std::size_t c : t.pk_columns) {
            cat_put_u32(o, static_cast<std::uint32_t>(c));
        }
        cat_put_u32(o, static_cast<std::uint32_t>(t.constraints.size()));  // named-constraint registry
        for (const Table::NamedConstraint& nc : t.constraints) {
            cat_put_s(o, nc.name);
            o.push_back(static_cast<char>(nc.kind));
            cat_put_u32(o, static_cast<std::uint32_t>(nc.column));
            cat_put_s(o, nc.check_src);
        }
        return o;
    }
    static Table deserialize_schema(const std::string& s) {
        std::size_t p = 0;
        Table t;
        t.name = cat_get_s(s, p);
        t.id = cat_get_u32(s, p);
        t.pk_index = cat_get_u32(s, p);
        t.columnar = s[p++] != 0;
        t.next_index_id = cat_get_u32(s, p);
        t.next_auto_id = std::strtoll(cat_get_s(s, p).c_str(), nullptr, 10);  // F6
        t.next_uuid = std::strtoull(cat_get_s(s, p).c_str(), nullptr, 10);    // F9c
        const std::uint32_t nc = cat_get_u32(s, p);
        for (std::uint32_t i = 0; i < nc; ++i) {
            Column c;
            c.name = cat_get_s(s, p);
            c.type = (s[p++] == 0) ? Type::Int : Type::Text;
            c.nullable = s[p++] != 0;
            c.has_default = s[p++] != 0;  // F4
            if (c.has_default) {
                const std::string dv = cat_get_s(s, p);
                if (c.type == Type::Int) {
                    c.default_i = std::strtoll(dv.c_str(), nullptr, 10);
                } else {
                    c.default_s = dv;
                }
            }
            c.auto_increment = s[p++] != 0;  // F6
            c.unique = s[p++] != 0;          // F2
            c.fk_table = cat_get_s(s, p);    // F3
            c.fk_column = cat_get_s(s, p);
            c.logical = static_cast<std::uint8_t>(s[p++]);  // F9b
            c.scale = static_cast<std::uint8_t>(s[p++]);
            c.uuid_default = s[p++] != 0;  // F9c
            c.max_len = cat_get_u32(s, p);  // F10
            c.fixed_char = s[p++] != 0;
            c.is_unsigned = s[p++] != 0;
            c.precision = static_cast<std::uint8_t>(s[p++]);
            c.int_bits = static_cast<std::uint8_t>(s[p++]);
            c.elem_type = (s[p++] == 0) ? Type::Int : Type::Text;  // F12
            c.elem_logical = static_cast<std::uint8_t>(s[p++]);
            c.elem_scale = static_cast<std::uint8_t>(s[p++]);
            const std::uint32_t nlbl = cat_get_u32(s, p);  // F13 ENUM labels
            for (std::uint32_t li = 0; li < nlbl; ++li) c.enum_labels.push_back(cat_get_s(s, p));
            c.dropped = s[p++] != 0;  // E1
            t.columns.push_back(std::move(c));
        }
        const std::uint32_t ni = cat_get_u32(s, p);
        for (std::uint32_t i = 0; i < ni; ++i) {
            Index ix;
            ix.name = cat_get_s(s, p);
            ix.id = cat_get_u32(s, p);
            ix.column = cat_get_u32(s, p);
            ix.unique = s[p++] != 0;  // E5
            ix.hash = s[p++] != 0;    // I7
            ix.partial_src = cat_get_s(s, p);  // I5
            const std::uint32_t nic = cat_get_u32(s, p);
            for (std::uint32_t k = 0; k < nic; ++k) ix.columns.push_back(cat_get_u32(s, p));
            ix.expr_src = cat_get_s(s, p);  // J2: expression index source
            ix.expr_type = (s[p++] == 0) ? Type::Int : Type::Text;
            ix.gin = s[p++] != 0;  // J3: GIN (array-element) index
            ix.ivfflat = s[p++] != 0;  // K1.3: IVFFLAT (approximate k-NN)
            ix.lists = cat_get_u32(s, p);
            ix.probes = cat_get_u32(s, p);
            ix.centroids = cat_get_s(s, p);
            ix.vec_op = static_cast<std::uint8_t>(s[p++]);  // K1.3c: operator class
            ix.hnsw = s[p++] != 0;                          // K1.4: HNSW (graph k-NN)
            ix.hnsw_m = cat_get_u32(s, p);
            ix.hnsw_efc = cat_get_u32(s, p);
            ix.bm25 = s[p++] != 0;  // K2: BM25 full-text
            t.indexes.push_back(std::move(ix));
        }
        const std::uint32_t ncheck = cat_get_u32(s, p);  // F5
        for (std::uint32_t i = 0; i < ncheck; ++i) {
            t.checks.push_back(cat_get_s(s, p));
        }
        const std::uint32_t npk = cat_get_u32(s, p);  // F1
        for (std::uint32_t i = 0; i < npk; ++i) {
            t.pk_columns.push_back(cat_get_u32(s, p));
        }
        const std::uint32_t ncons = cat_get_u32(s, p);  // named-constraint registry
        for (std::uint32_t i = 0; i < ncons; ++i) {
            Table::NamedConstraint nc;
            nc.name = cat_get_s(s, p);
            nc.kind = static_cast<std::uint8_t>(s[p++]);
            nc.column = cat_get_u32(s, p);
            nc.check_src = cat_get_s(s, p);
            t.constraints.push_back(std::move(nc));
        }
        t.col_stats.assign(t.columns.size(), Table::ColStat{});
        return t;
    }
    // Durably (re)write the table's schema record. Called after CREATE / CREATE INDEX / DROP INDEX.
    // The schema record goes to the SEPARATE catalog store (catalog_db_), NOT the data store, so
    // DDL never consumes a data MVCC Seq — `AT SNAPSHOT N` keeps counting DATA writes only (the
    // pre-C7 invariant the conformance gate encodes; C7 had funneled the catalog through db_ and
    // shifted every data version by the DDL count). Catalog reads are tip-only (recover scans the
    // strict tip), so it needs no group commit; write it durably (immediate sync) — DDL is rare.
    void persist_schema(const std::string& name) {
        const Table* t = catalog_.find(name);
        if (t != nullptr) {
            // E4: key by the table's QUALIFIED name (== t->name) so create/drop/rename match keys.
            (void)commit_batch(catalog_db_, {{catalog_key(t->name), serialize_schema(*t)}},
                               /*nosync=*/false);
        }
    }
    // E4: durably mark a (possibly empty) schema so CREATE SCHEMA survives a restart.
    void persist_schema_marker(const std::string& schema) {
        (void)commit_batch(catalog_db_, {{std::string(1, '\x02') + schema, std::string("1")}},
                           /*nosync=*/false);
    }
    // E1: durably retire a schema record (a tombstone), e.g. the old name after RENAME / a DROP.
    void retire_schema(const std::string& name) {
        (void)commit_batch(catalog_db_, {{catalog_key(catalog_.qualify(name)), tombstone_marker()}},
                           /*nosync=*/false);
    }
    // H1: durably (re)write a view's record (raw SELECT text + optional column renames). Keyed by the
    // QUALIFIED name (== v->name) so CREATE/DROP match keys. Catalog store, own Seq line (like tables).
    void persist_view(const std::string& name) {
        const View* v = catalog_.find_view(name);
        if (v != nullptr) {
            (void)commit_batch(catalog_db_, {{view_key(v->name), serialize_view(*v)}},
                               /*nosync=*/false);
        }
    }
    // H1: durably tombstone a view record (DROP VIEW) so a restart does not resurrect it.
    void retire_view(const std::string& name) {
        (void)commit_batch(catalog_db_, {{view_key(catalog_.qualify(name)), tombstone_marker()}},
                           /*nosync=*/false);
    }
    // MATERIALIZED VIEW: the refreshable source, keyed at catalog namespace 0x06 (clear of the
    // table 0x01 / marker 0x02 / view 0x04 recovery scans). The backing table persists normally.
    static Key matview_key(const std::string& name) { return std::string(1, '\x06') + name; }
    void persist_matview(const std::string& name) {
        const std::string qn = catalog_.qualify(name);
        const auto it = matviews_.find(qn);
        if (it != matviews_.end()) {
            (void)commit_batch(catalog_db_, {{matview_key(qn), it->second}}, /*nosync=*/false);
        }
    }
    void retire_matview(const std::string& name) {
        (void)commit_batch(catalog_db_, {{matview_key(catalog_.qualify(name)), tombstone_marker()}},
                           /*nosync=*/false);
    }

    // W3.1: set the per-statement query-memory cap in bytes (0 = unlimited, the default).
    // Deterministic (byte-counted, not allocator-based), so a replicated statement hits the
    // limit identically on every replica.
    void set_max_query_memory(std::size_t bytes) noexcept { max_query_mem_ = bytes; }
    [[nodiscard]] std::size_t query_memory_used() const noexcept { return query_mem_used_; }

    // W3.2: install (or clear, with nullptr) the cooperative cancellation flag. The engine polls
    // it during execution; when it reads true, the current statement aborts with "query canceled".
    void set_cancel_flag(const std::atomic<bool>* flag) noexcept { cancel_ = flag; }
    [[nodiscard]] bool canceled() const noexcept { return cancel_ != nullptr && cancel_->load(); }

    // Parse + execute one SQL string. Parse errors surface as ExecResult::failure.
    ExecResult exec(const std::string& sql) {
        // W9.2 PARSE CACHE: parsing is a pure function of the SQL text (the parser has no
        // catalog), so a cached AST is ALWAYS valid — no invalidation on DDL (execution
        // re-resolves against the live catalog). Caching skips re-parsing repeated queries
        // (the prepared-statement / ORM / wire hot path — parsing measured ~2.8us for a
        // complex query, which dominates a us-scale point query). Byte-identical: the same
        // SQL yields the same AST yields the same execution.
        const auto it = parse_cache_.find(sql);
        if (it != parse_cache_.end()) {
            return exec(it->second);
        }
        ParseResult pr = parse_sql(sql);
        if (!pr.ok()) {
            return ExecResult::failure(pr.error().render());  // parse errors are NOT cached
        }
        if (parse_cache_.size() >= kParseCacheCap) {
            parse_cache_.clear();  // simple bounded cap (avoids unbounded growth)
        }
        const auto ins = parse_cache_.emplace(sql, pr.stmt()).first;
        return exec(ins->second);  // run over the cached AST (no extra copy)
    }

    // Execute an already-parsed statement.
    ExecResult exec(const Statement& st) {
        // W3.1: reset the per-statement memory counter at the OUTERMOST exec() only (nested
        // materialization calls exec_select, not exec(Statement) — but guard by depth so a
        // future nested exec() can never clear a parent's accounting mid-flight).
        StmtMemGuard mem_guard(*this);
        if (canceled()) {  // W3.2: an already-canceled session aborts before doing any work.
            return ExecResult::failure("query canceled");
        }
        switch (st.kind) {
            case StmtKind::Create:
                return exec_create(st.create);
            case StmtKind::Insert:
                return exec_insert(st.insert);
            case StmtKind::Update:
                return exec_update(st.update);
            case StmtKind::Delete:
                return exec_delete(st.del);
            case StmtKind::Select: {
                ExecResult r = exec_select(st.select);
                // W3.1: charge the returned result set against the per-statement budget
                // (bounds a runaway result before it is handed back / serialized to the
                // client). Deterministic; inert when max_query_mem_ == 0 (the default).
                if (r.ok && max_query_mem_ != 0) {
                    std::size_t sz = 0;
                    for (const ResultRow& row : r.rows) sz += result_row_bytes(row);
                    if (auto e = charge_query_mem(sz)) return ExecResult::failure(*e);
                }
                return r;
            }
            case StmtKind::CreateIndex:
                return exec_create_index(st.create_index);
            case StmtKind::DropIndex:
                return exec_drop_index(st.drop_index);
            case StmtKind::DropTable:
                return exec_drop_table(st.drop_table);
            case StmtKind::CreateView:
                return exec_create_view(st.create_view);
            case StmtKind::DropView:
                return exec_drop_view(st.drop_view);
            case StmtKind::CreateQueue:
                return exec_create_queue(st.queue);
            case StmtKind::DropQueue:
                return exec_drop_queue(st.queue);
            case StmtKind::Send:
                return exec_send(st.queue);
            case StmtKind::Receive:
                return exec_receive(st.queue);
            case StmtKind::Ack:
                return exec_ack(st.queue);
            case StmtKind::SetParam:
                return exec_set_param(st.set_param_name, st.set_param_value);
            case StmtKind::RefreshMatView:
                return exec_refresh_matview(st.truncate.table);
            case StmtKind::Truncate:
                return exec_truncate(st.truncate);
            case StmtKind::Alter:
                return exec_alter(st.alter);
            case StmtKind::Begin:
                if (in_txn_) return ExecResult::failure("already in a transaction");
                in_txn_ = true;
                txn_writes_.clear();
                txn_overlay_.clear();
                savepoints_.clear();
                return ExecResult{};
            case StmtKind::Commit: {
                if (!in_txn_) return ExecResult::failure("COMMIT with no active transaction");
                in_txn_ = false;
                savepoints_.clear();
                std::vector<std::pair<Key, Value>> flush;
                flush.swap(txn_writes_);
                txn_overlay_.clear();
                if (!flush.empty()) commit_writes(flush);  // now in_txn_ is false -> actually commits
                return ExecResult{};
            }
            case StmtKind::Rollback:
                if (!in_txn_) return ExecResult::failure("ROLLBACK with no active transaction");
                in_txn_ = false;
                txn_writes_.clear();  // discard the buffered writes (nothing was committed)
                txn_overlay_.clear();
                savepoints_.clear();
                return ExecResult{};
            case StmtKind::Savepoint:
                if (!in_txn_) return ExecResult::failure("SAVEPOINT can only be used in a transaction");
                savepoints_.emplace_back(st.savepoint_name, txn_writes_.size());
                return ExecResult{};
            case StmtKind::RollbackToSavepoint: {
                if (!in_txn_)
                    return ExecResult::failure("ROLLBACK TO SAVEPOINT with no active transaction");
                std::size_t at = savepoints_.size();
                for (std::size_t i = savepoints_.size(); i-- > 0;) {
                    if (savepoints_[i].first == st.savepoint_name) { at = i; break; }
                }
                if (at == savepoints_.size())
                    return ExecResult::failure("no such savepoint: " + st.savepoint_name);
                if (savepoints_[at].second < txn_writes_.size()) {
                    txn_writes_.resize(savepoints_[at].second);  // undo writes since the savepoint
                    txn_overlay_.clear();  // rebuild the RYW index from the kept prefix
                    for (const auto& kv : txn_writes_) txn_overlay_[kv.first] = kv.second;
                }
                savepoints_.resize(at + 1);  // drop later savepoints; keep the target (re-rollback ok)
                return ExecResult{};
            }
            case StmtKind::ReleaseSavepoint: {
                if (!in_txn_)
                    return ExecResult::failure("RELEASE SAVEPOINT with no active transaction");
                std::size_t at = savepoints_.size();
                for (std::size_t i = savepoints_.size(); i-- > 0;) {
                    if (savepoints_[i].first == st.savepoint_name) { at = i; break; }
                }
                if (at == savepoints_.size())
                    return ExecResult::failure("no such savepoint: " + st.savepoint_name);
                savepoints_.resize(at);  // forget the savepoint (+ later ones); writes are kept
                return ExecResult{};
            }
            case StmtKind::CreateSchema:
                if (!catalog_.create_schema(st.schema_arg, st.schema_if_not_exists))
                    return ExecResult::failure("schema '" + st.schema_arg + "' already exists");
                persist_schema_marker(st.schema_arg);  // E4: durable empty-schema marker
                return ExecResult{};
            case StmtKind::DropSchema:
                if (!catalog_.drop_schema(st.schema_arg) && !st.schema_if_exists)
                    return ExecResult::failure("unknown schema '" + st.schema_arg + "'");
                return ExecResult{};
            case StmtKind::SetSearchPath:
                if (!catalog_.set_search_path(st.schema_arg))
                    return ExecResult::failure("unknown schema '" + st.schema_arg + "'");
                return ExecResult{};
            case StmtKind::ShowTables:
                return exec_show_tables();
            case StmtKind::Describe:
                return exec_describe(st.truncate.table);
            case StmtKind::Analyze:
                return exec_analyze(st.truncate.table);
        }
        return ExecResult::failure("unknown statement kind");
    }

    [[nodiscard]] const Catalog& catalog() const { return catalog_; }

    // TEST/BENCH-ONLY: toggle the vectorized filter fast path (for A/B measurement). A pure
    // config flag (deterministic per setting); the default is on.
    void set_vectorize(bool v) { vectorize_ = v; }
    // When true, every subsequently CREATEd table uses the COLUMNAR layout (col_key per
    // (row,column) in the 'c' namespace). Existing tables are unaffected. A/B + the
    // columnar conformance gate run the SAME workload with this on vs off.
    void set_columnar_default(bool v) { columnar_default_ = v; }
    // I4 TEST: did the most recent join take the index nested-loop fast path?
    [[nodiscard]] bool last_join_used_index_nl() const { return last_join_index_nl_; }

    // Auto-flush a columnar table once its delta exceeds `rows` live writes (0 = off,
    // manual flush only). Makes columnar self-managing — no explicit flush_columnar needed.
    // Deterministic (count-based, not time). The append fast path keeps a monotonic-pk
    // stream's auto-flushes O(delta).
    void set_auto_flush(std::uint64_t rows) { auto_flush_rows_ = rows; }

    // MORSEL PARALLELISM seam. Inject a providers/prod thread-pool executor to fold a columnar
    // aggregate's chunks across cores; null (default) = serial. The RESULT is byte-identical
    // either way (partials merged in a FIXED chunk order), so determinism + the sim are
    // unaffected — only wall-clock changes. Threads live in the injected impl, never here.
    void set_parallel_executor(IParallelExecutor* ex) { parallel_executor_ = ex; }

    // GROUP COMMIT (write durability): when set, INSERT/UPDATE/DELETE defer their fsync; the
    // caller MUST call sync() before acking any of the deferred writes (acked==durable). Lets the
    // wire server amortize ONE fsync over a burst of SQL writes (the SQL analogue of the keyed
    // path's 3.3k -> 59k lift). Off by default (every statement fsyncs inline).
    void set_group_commit(bool v) { group_commit_ = v; }

    // Flush the deferred SQL-write fsync once — after this, all group-committed writes are durable.
    void sync() { db_.sync(); }

    // FLUSH a columnar table's row 'd' delta into column blocks (LSM compaction): read the
    // merged live rows, pack each column into a block (block_no 0, overwrite), and clear
    // the delta — ALL in ONE atomic commit (the durable batch), so a crash leaves either
    // the pre-flush or the post-flush state, never a torn mix. After flush, reads take the
    // SoA block path (projection pushdown); writes repopulate the delta. No-op for a
    // row-mode table. Public so the bench/tests + a future auto-flush trigger can call it.
    std::optional<std::string> flush_columnar(const std::string& table) {
        Table* t = catalog_.find_mut(table);
        if (t == nullptr) {
            return std::string("unknown table '" + table + "'");
        }
        if (!t->columnar) {
            return std::nullopt;  // nothing to flush
        }
        if (!t->delta_dirty) {
            return std::nullopt;  // no writes since the last flush — nothing to compact
        }
        // INCREMENTAL APPEND fast path: if the delta is purely NEW rows whose pks are all
        // GREATER than every flushed pk (the streaming / monotonic-id case — no
        // update/delete/mid-insert), APPEND them as new chunks instead of rewriting the
        // whole table. O(delta) not O(rows); the existing column blocks are untouched.
        if (try_incremental_flush(*t)) {
            return std::nullopt;
        }
        // NON-APPEND: write the delta as an OVERLAY RUN (no base rewrite) unless there are
        // already too many runs — then fall through to a full COMPACTING flush.
        if (create_overlay_run(*t)) {
            return std::nullopt;
        }
        // FULL COMPACTION: read the merged live rows (base + overlays + delta) and rewrite
        // the base (run 0), then retire all overlay runs + their tombstones + the manifest.
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> old_overlays =
            load_manifest(*t);
        SelectStmt full;
        full.table = table;
        full.level = Level::StrictSerializable;
        const std::vector<bool> all(t->columns.size(), true);
        std::vector<std::vector<Datum>> rows;  // merged live rows, pk-ascending
        if (auto e = columnar_build_rows(*t, full, all, rows)) {
            return e;
        }
        // Pack each column into ceil(N/kChunkRows) pk-ascending chunks (block_no 0..K-1).
        // Tombstone any STALE chunk a prior flush left beyond the new chunk count (the table
        // shrank). All in one atomic commit, so a crash leaves the pre- or post-flush state.
        const std::size_t nrows = rows.size();
        const std::uint64_t nchunks =
            (nrows + kChunkRows - 1) / kChunkRows;  // 0 rows => 0 chunks
        const std::uint64_t old_nchunks =
            read_col_chunks(*t, static_cast<std::uint32_t>(t->pk_index)).size();
        std::vector<std::pair<Key, Value>> writes;
        for (std::size_t c = 0; c < t->columns.size(); ++c) {
            for (std::uint64_t j = 0; j < nchunks; ++j) {
                const std::size_t lo = static_cast<std::size_t>(j) * kChunkRows;
                const std::size_t hi = std::min(lo + kChunkRows, nrows);
                std::vector<Datum> cells;
                cells.reserve(hi - lo);
                for (std::size_t r = lo; r < hi; ++r) {
                    cells.push_back(rows[r][c]);
                }
                writes.emplace_back(
                    block_key(t->id, static_cast<std::uint32_t>(c), j),
                    encode_column_block(t->columns[c].type, cells));
            }
            for (std::uint64_t j = nchunks; j < old_nchunks; ++j) {
                writes.emplace_back(block_key(t->id, static_cast<std::uint32_t>(c), j),
                                    tombstone_marker());  // retire a stale chunk
            }
        }
        // ZONE MAP (Phase 4): per-chunk per-INT-column [min,max] for data skipping.
        std::vector<std::vector<ColZone>> zones(
            static_cast<std::size_t>(nchunks), std::vector<ColZone>(t->columns.size()));
        for (std::uint64_t j = 0; j < nchunks; ++j) {
            const std::size_t lo = static_cast<std::size_t>(j) * kChunkRows;
            const std::size_t hi = std::min(lo + kChunkRows, nrows);
            for (std::size_t c = 0; c < t->columns.size(); ++c) {
                ColZone& cz = zones[static_cast<std::size_t>(j)][c];
                for (std::size_t r = lo; r < hi; ++r) {
                    zone_add(cz, rows[r][c]);
                }
            }
        }
        writes.emplace_back(zone_key(t->id), encode_zone_map(zones, t->columns.size()));
        std::vector<storage::KeyValue> delta;  // clear the delta: tombstone every 'd' key
        {
            Query<Strict> q;
            q.scan(row_delta_prefix(t->id), row_delta_prefix_end(t->id));
            collect(db_.run(q), delta);
        }
        for (const storage::KeyValue& kv : delta) {
            writes.emplace_back(kv.first, tombstone_marker());
        }
        // COMPACTION cleanup: the merged rows are now in the base, so retire every overlay
        // run (all its column chunks), its tombstone list, and the manifest — all in this
        // atomic commit, so a crash leaves either the pre- or the post-compaction state.
        for (const auto& [run, nch] : old_overlays) {
            for (std::size_t c = 0; c < t->columns.size(); ++c) {
                for (std::uint64_t j = 0; j < nch; ++j) {
                    writes.emplace_back(
                        overlay_key(t->id, run, static_cast<std::uint32_t>(c), j),
                        tombstone_marker());
                }
            }
            writes.emplace_back(overlay_tomb_key(t->id, run), tombstone_marker());
        }
        if (!old_overlays.empty()) {
            writes.emplace_back(overlay_manifest_key(t->id), tombstone_marker());
        }
        commit_writes(writes, /*blind=*/true);  // ATOMIC blind overwrite: base + delta clear + overlay retire
        t->delta_dirty = false;  // delta is now empty (compacted into blocks)
        t->delta_count = 0;
        ++t->flush_gen;          // invalidate the decoded-block + zone cache for this table
        return std::nullopt;
    }

    // Incremental APPEND flush (true if handled). Pure-append case only: the table already
    // has blocks, the delta is non-empty + ALL live (no del-markers), and EVERY delta pk is
    // strictly greater than the max flushed pk (no overwrite / mid-insert / delete). Packs
    // the delta into NEW chunks appended after the existing ones — the existing column
    // blocks are UNTOUCHED (O(delta), not O(rows)); only the small zone map is rewritten +
    // the delta cleared, in ONE atomic commit. The chunks stay pk-disjoint + ascending, so
    // every read path is unchanged. Anything not pure-append => false => the full flush runs.
    [[nodiscard]] bool try_incremental_flush(Table& t) {
        if (has_overlays(t)) {
            return false;  // base is the OLDEST run — an append must become a NEW overlay run
        }
        const std::uint32_t pkc_id = static_cast<std::uint32_t>(t.pk_index);
        // The chunk count comes from the (cached, small) zone map; the max flushed pk from
        // decoding ONLY the last pk chunk. No full-column decode => the flush stays O(delta).
        const std::vector<std::vector<ColZone>>& zmap = load_zones(t);
        const std::uint64_t old_nchunks = zmap.size();
        if (old_nchunks == 0) {
            return false;  // no blocks yet — let the full flush build the initial blocks
        }
        const ReadResult lastb = read_committed(block_key(t.id, pkc_id, old_nchunks - 1));
        if (!lastb.has_value() || is_tombstone(*lastb)) {
            return false;
        }
        const ColumnChunk last_pkc = decode_column_block(*lastb);
        if (last_pkc.count == 0) {
            return false;
        }
        const Datum max_pk = last_pkc.at(last_pkc.count - 1);

        std::vector<storage::KeyValue> delta;
        {
            Query<Strict> q;
            q.scan(row_delta_prefix(t.id), row_delta_prefix_end(t.id));
            collect(db_.run(q), delta);
        }
        if (delta.empty()) {
            return false;
        }
        std::vector<std::vector<Datum>> newrows;  // pk-ascending (delta keys are pk-ordered)
        newrows.reserve(delta.size());
        const std::vector<bool> all(t.columns.size(), true);
        bool any_live = false;
        for (const storage::KeyValue& kv : delta) {
            if (is_tombstone(kv.second)) {
                continue;  // a cleared-delta marker from a prior flush — skip
            }
            if (is_row_del_marker(kv.second)) {
                return false;  // a delete — not a pure append
            }
            any_live = true;
            const Datum pk = decode_pk_from_delta_key(t, kv.first);
            if (!max_pk.less_than(pk)) {
                return false;  // pk <= max flushed pk — would overwrite / mis-order
            }
            newrows.push_back(decode_row_projected(t, encode_key(t, pk), kv.second, all));
        }
        if (!any_live) {
            return false;  // only cleared markers — nothing to append (let flush no-op/full)
        }
        const std::size_t nnew = newrows.size();
        const std::uint64_t nnewchunks = (nnew + kChunkRows - 1) / kChunkRows;
        std::vector<std::pair<Key, Value>> writes;
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            for (std::uint64_t j = 0; j < nnewchunks; ++j) {
                const std::size_t lo = static_cast<std::size_t>(j) * kChunkRows;
                const std::size_t hi = std::min(lo + kChunkRows, nnew);
                std::vector<Datum> cells;
                cells.reserve(hi - lo);
                for (std::size_t rr = lo; rr < hi; ++rr) {
                    cells.push_back(newrows[rr][c]);
                }
                writes.emplace_back(
                    block_key(t.id, static_cast<std::uint32_t>(c), old_nchunks + j),
                    encode_column_block(t.columns[c].type, cells));
            }
        }
        std::vector<std::vector<ColZone>> zones = zmap;  // existing (copy) + append new chunks
        if (zones.size() != old_nchunks) {
            return false;  // zone map out of sync — fall back to a full flush
        }
        for (std::uint64_t j = 0; j < nnewchunks; ++j) {
            const std::size_t lo = static_cast<std::size_t>(j) * kChunkRows;
            const std::size_t hi = std::min(lo + kChunkRows, nnew);
            std::vector<ColZone> zr(t.columns.size());
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                for (std::size_t rr = lo; rr < hi; ++rr) {
                    zone_add(zr[c], newrows[rr][c]);
                }
            }
            zones.push_back(std::move(zr));
        }
        writes.emplace_back(zone_key(t.id), encode_zone_map(zones, t.columns.size()));
        for (const storage::KeyValue& kv : delta) {
            writes.emplace_back(kv.first, tombstone_marker());  // clear the delta
        }
        commit_writes(writes, /*blind=*/true);  // blind bulk overwrite (no read-conflict footprint)
        t.delta_dirty = false;
        t.delta_count = 0;
        ++t.flush_gen;
        return true;
    }

    static constexpr std::size_t kMaxOverlays = 4;  // compact (full flush) past this many runs

    // Non-append flush WITHOUT rewriting the base: write the delta as a new OVERLAY RUN
    // (live rows as chunks + deleted pks as a tombstone list) + extend the manifest, in one
    // atomic commit. O(delta). Returns false (=> the caller does a full compacting flush) if
    // there is no base yet, or there are already kMaxOverlays runs (time to compact).
    [[nodiscard]] bool create_overlay_run(Table& t) {
        if (load_zones(t).empty()) {
            return false;  // no base (run 0) — let the full flush build it
        }
        std::vector<std::pair<std::uint32_t, std::uint64_t>> manifest = load_manifest(t);
        if (manifest.size() >= kMaxOverlays) {
            return false;  // too many runs — compact instead
        }
        std::uint32_t run = 1;
        for (const auto& [r, n] : manifest) {
            (void)n;
            if (r >= run) {
                run = r + 1;
            }
        }
        std::vector<storage::KeyValue> delta;
        {
            Query<Strict> q;
            q.scan(row_delta_prefix(t.id), row_delta_prefix_end(t.id));
            collect(db_.run(q), delta);
        }
        if (delta.empty()) {
            return false;
        }
        std::vector<std::vector<Datum>> liverows;  // pk-ascending (delta keys are pk-ordered)
        std::vector<Key> tombs;
        const std::vector<bool> all(t.columns.size(), true);
        for (const storage::KeyValue& kv : delta) {
            if (is_tombstone(kv.second)) {
                continue;  // a cleared-delta marker (a prior flush wrote it) — not a row
            }
            const Datum pk = decode_pk_from_delta_key(t, kv.first);
            if (is_row_del_marker(kv.second)) {
                tombs.push_back(encode_pk(pk));
            } else {
                std::vector<Datum> row =
                    decode_row_projected(t, encode_key(t, pk), kv.second, all);
                row[t.pk_index] = pk;
                liverows.push_back(std::move(row));
            }
        }
        if (liverows.empty() && tombs.empty()) {
            return false;  // only cleared markers — nothing to overlay
        }
        const std::size_t nlive = liverows.size();
        const std::uint64_t nchunks = (nlive + kChunkRows - 1) / kChunkRows;
        std::vector<std::pair<Key, Value>> writes;
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            for (std::uint64_t j = 0; j < nchunks; ++j) {
                const std::size_t lo = static_cast<std::size_t>(j) * kChunkRows;
                const std::size_t hi = std::min(lo + kChunkRows, nlive);
                std::vector<Datum> cells;
                cells.reserve(hi - lo);
                for (std::size_t rr = lo; rr < hi; ++rr) {
                    cells.push_back(liverows[rr][c]);
                }
                writes.emplace_back(
                    overlay_key(t.id, run, static_cast<std::uint32_t>(c), j),
                    encode_column_block(t.columns[c].type, cells));
            }
        }
        writes.emplace_back(overlay_tomb_key(t.id, run), encode_tombs(tombs));
        manifest.emplace_back(run, nchunks);
        writes.emplace_back(overlay_manifest_key(t.id), encode_manifest(manifest));
        for (const storage::KeyValue& kv : delta) {
            writes.emplace_back(kv.first, tombstone_marker());  // clear the delta
        }
        commit_writes(writes, /*blind=*/true);  // blind bulk overwrite (no read-conflict footprint)
        t.delta_dirty = false;
        t.delta_count = 0;
        ++t.flush_gen;
        return true;
    }

    // Auto-flush a columnar table once its delta passes the threshold (self-managing). Called
    // after each columnar write; a no-op when auto-flush is off or the delta is small.
    void maybe_auto_flush(const std::string& table) {
        if (auto_flush_rows_ == 0) {
            return;
        }
        Table* mt = catalog_.find_mut(table);
        if (mt != nullptr && mt->columnar && mt->delta_count >= auto_flush_rows_) {
            (void)flush_columnar(table);
        }
    }

private:
    // Append ONE named constraint to a table's registry. An explicit name is used as-is (deduped if it
    // somehow collides); an empty name is auto-generated Postgres-style (`<t>_check`, `<t>_<col>_key`,
    // `<t>_<col>_fkey`) with a numeric suffix on collision. Returns the assigned name.
    static std::string add_named_constraint(Table& t, std::uint8_t kind, std::size_t column,
                                            const std::string& check_src,
                                            const std::string& explicit_name) {
        std::set<std::string> used;
        for (const auto& c : t.constraints) used.insert(c.name);
        std::string base = explicit_name;
        if (base.empty()) {
            base = kind == 1   ? t.name + "_" + t.columns[column].name + "_key"
                   : kind == 2 ? t.name + "_" + t.columns[column].name + "_fkey"
                               : t.name + "_check";
        }
        std::string nm = base;
        int k = 1;
        while (used.count(nm) != 0) nm = base + std::to_string(++k);
        t.constraints.push_back(Table::NamedConstraint{nm, kind, column, check_src});
        return nm;
    }

    // Build the named-constraint registry from a freshly created table's columns + checks. Explicit
    // CHECK names come from `check_names` (parallel to t.checks; "" => auto). Column UNIQUE / FOREIGN
    // KEY constraints are auto-named. Deterministic (a pure function of the schema).
    static void build_constraints(Table& t, const std::vector<std::string>& check_names) {
        t.constraints.clear();
        for (std::size_t i = 0; i < t.checks.size(); ++i) {
            const std::string nm = (i < check_names.size()) ? check_names[i] : std::string();
            add_named_constraint(t, 0, 0, t.checks[i], nm);  // kind 0 = Check
        }
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            if (t.columns[c].unique) add_named_constraint(t, 1, c, "", "");  // kind 1 = Unique
            if (!t.columns[c].fk_table.empty()) add_named_constraint(t, 2, c, "", "");  // kind 2 = FK
        }
    }

    // --- CREATE TABLE ---------------------------------------------------------
    ExecResult exec_create(const CreateStmt& c) {
        if (catalog_.has(c.table)) {
            if (c.if_not_exists) return ExecResult{};  // E2: no-op
            return ExecResult::failure("table '" + c.table + "' already exists");
        }
        if (catalog_.find_view(c.table) != nullptr) {  // H1: shared namespace (a view owns the name)
            return ExecResult::failure("'" + c.table + "' is a view, not a table");
        }
        if (!c.like_table.empty()) return exec_create_like(c);  // E2
        if (c.as_select) return exec_create_as_select(c);       // E3
        Table t;
        t.name = c.table;
        t.columns = c.columns;
        t.checks = c.checks;  // F5: CHECK predicate texts (re-parsed + evaluated on write)
        bool found = false;
        for (std::size_t i = 0; i < t.columns.size(); ++i) {
            if (t.columns[i].name == c.pk_column) {
                t.pk_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return ExecResult::failure("PRIMARY KEY column '" + c.pk_column +
                                       "' is not a declared column");
        }
        // F1: resolve the (possibly composite) PK column list. A composite PK is row-mode + all-INT
        // (fixed-width components keep the key order-preserving + self-delimiting); reject otherwise.
        const std::vector<std::string>& pkc =
            c.pk_columns.empty() ? std::vector<std::string>{c.pk_column} : c.pk_columns;
        t.columnar = columnar_default_;  // columnar layout opt-in (engine default at CREATE)
        if (pkc.size() > 1) {
            if (t.columnar) {
                return ExecResult::failure("a composite PRIMARY KEY is row-mode only (columnar OUT)");
            }
            for (const std::string& name : pkc) {
                const auto ci = t.column_index(name);
                if (!ci) return ExecResult::failure("PRIMARY KEY column '" + name + "' is not declared");
                if (t.columns[*ci].type != Type::Int) {
                    return ExecResult::failure(
                        "a composite PRIMARY KEY column must be INT ('" + name + "' is not)");
                }
                t.pk_columns.push_back(*ci);
                t.columns[*ci].nullable = false;
            }
        }
        // v4: the PRIMARY KEY column is ALWAYS NOT NULL (a NULL PK is meaningless and
        // could never be addressed by the order-preserving key encoding). Force it
        // regardless of any NOT NULL spelling in the DDL.
        t.columns[t.pk_index].nullable = false;
        t.col_stats.assign(t.columns.size(), Table::ColStat{});  // per-column stats (Phase 2)
        build_constraints(t, c.check_names);  // name the CHECK / UNIQUE / FK constraints (droppable)
        (void)catalog_.create(std::move(t));
        persist_schema(c.table);  // C7: durable schema record (survives a restart)
        return ExecResult{};
    }

    // E2: CREATE TABLE t LIKE other — a fresh, EMPTY table with `other`'s columns + PK + checks.
    ExecResult exec_create_like(const CreateStmt& c) {
        const Table* src = catalog_.find(c.like_table);
        if (src == nullptr) return ExecResult::failure("unknown source table '" + c.like_table + "'");
        Table t;
        t.name = c.table;
        t.columns = src->columns;
        t.pk_index = src->pk_index;
        t.pk_columns = src->pk_columns;
        t.checks = src->checks;
        t.next_auto_id = 1;  // a copy of the SCHEMA only — counters reset, no data, no indexes
        t.col_stats.assign(t.columns.size(), Table::ColStat{});
        build_constraints(t, {});  // re-derive names for the copied schema (no explicit names)
        (void)catalog_.create(std::move(t));
        persist_schema(c.table);
        return ExecResult{};
    }

    // E3: CREATE TABLE t AS SELECT ... — run the query, infer the column types from the result, create
    // the table (with a HIDDEN auto-increment PK so the model's PK requirement is met without polluting
    // SELECT *), and INSERT every result row.
    ExecResult exec_create_as_select(const CreateStmt& c) {
        if (auto e = materialize_select(c.table, *c.as_select)) return ExecResult::failure(*e);
        if (c.materialized) {  // record the refreshable source, durably.
            matviews_[catalog_.qualify(c.table)] = c.source_sql;
            persist_matview(c.table);
        }
        return ExecResult{};
    }

    // REFRESH MATERIALIZED VIEW name — recompute the stored source and replace the table's rows.
    // Implemented as drop + re-materialize (non-incremental), so the result is exactly the query
    // over the current base data. Deterministic (same source + same data -> same rows).
    ExecResult exec_refresh_matview(const std::string& name) {
        const std::string qn = catalog_.qualify(name);
        const auto it = matviews_.find(qn);
        if (it == matviews_.end()) {
            return ExecResult::failure("'" + name + "' is not a materialized view");
        }
        const std::string src = it->second;  // copy (drop below mutates the catalog)
        ParseResult pr = parse_sql(src);
        if (!pr.ok() || pr.stmt().kind != StmtKind::Select) {
            return ExecResult::failure("materialized view '" + name + "': stored source is not a SELECT");
        }
        // Drop the current backing table, then re-materialize under the same name.
        DropTableStmt d;
        d.table = name;
        d.if_exists = true;
        (void)exec_drop_table(d, /*keep_matview=*/true);  // keep the matview registration
        if (auto e = materialize_select(name, pr.stmt().select)) return ExecResult::failure(*e);
        persist_matview(name);  // re-assert the source (drop tombstoned the schema record)
        return ExecResult{};
    }

    // Run a SELECT and materialize its result into a NEW table `name` — the schema is inferred from
    // the result (column names from the output labels; each type from the first non-NULL value,
    // default INT; plus a hidden synthetic `_ctid` identity PK). Reused by CREATE TABLE AS SELECT
    // (E3) and by the WITH-CTE / FROM-subquery materialization (D3/D4). Returns an error string on
    // failure, std::nullopt on success.
    // W3.1: RAII reset of the per-statement memory counter at the outermost exec() only.
    // Increments stmt_depth_ on entry; when it transitions 0->1 the counter is cleared, so a
    // (hypothetical) nested exec(Statement) never clears a parent's accounting mid-statement.
    struct StmtMemGuard {
        SqlEngine& e;
        explicit StmtMemGuard(SqlEngine& eng) : e(eng) {
            if (e.stmt_depth_++ == 0) e.query_mem_used_ = 0;
        }
        ~StmtMemGuard() { --e.stmt_depth_; }
        StmtMemGuard(const StmtMemGuard&) = delete;
        StmtMemGuard& operator=(const StmtMemGuard&) = delete;
    };

    // W3.1: deterministic byte size of one result row (cell count overhead + value bytes).
    // Counts the logical payload, not the allocator footprint, so it is identical on every
    // replica regardless of platform/allocator.
    [[nodiscard]] static std::size_t result_row_bytes(const ResultRow& r) {
        std::size_t n = 0;
        for (const auto& cell : r.cells) {
            // column label + a fixed Datum footprint (type/int/flags) + the TEXT payload.
            n += cell.first.size() + 24 + cell.second.s.size();
        }
        return n;
    }

    // W3.1: charge `bytes` to the current statement's memory budget. Returns a deterministic
    // error string if the cap (max_query_mem_, when non-zero) is exceeded; std::nullopt
    // otherwise. The cap is off (0) by default → this is inert and results are unchanged.
    [[nodiscard]] std::optional<std::string> charge_query_mem(std::size_t bytes) {
        query_mem_used_ += bytes;
        if (max_query_mem_ != 0 && query_mem_used_ > max_query_mem_) {
            return std::string("query memory limit exceeded (") +
                   std::to_string(query_mem_used_) + " > " + std::to_string(max_query_mem_) +
                   " bytes); raise lockstep.max_query_memory or narrow the query";
        }
        return std::nullopt;
    }

    // W3.1: SET <name> = <value>. Known session parameters take effect; unknown ones are
    // accepted as a no-op (PostgreSQL-compatible: clients SET client_encoding / datestyle /
    // etc. which the engine need not model). Knobs acted on today:
    // lockstep.max_query_memory (bytes; 0 = unlimited) and ivfflat.probes (K1.3; pgvector's
    // query-time recall knob — 0 restores each index's own default).
    ExecResult exec_set_param(const std::string& name, const std::string& value) {
        if (name == "lockstep.max_query_memory") {
            std::size_t bytes = 0;
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    return ExecResult::failure(
                        "SET lockstep.max_query_memory expects a non-negative integer (bytes)");
                }
                bytes = bytes * 10 + static_cast<std::size_t>(c - '0');
            }
            if (value.empty()) {
                return ExecResult::failure(
                    "SET lockstep.max_query_memory expects a non-negative integer (bytes)");
            }
            set_max_query_memory(bytes);
        }
        if (name == "ivfflat.probes") {  // K1.3: session override of the per-index probes default
            std::uint64_t n = 0;
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    return ExecResult::failure(
                        "SET ivfflat.probes expects an integer in 0..32768 (0 = index default)");
                }
                n = n * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (value.empty() || n > 32768) {
                return ExecResult::failure(
                    "SET ivfflat.probes expects an integer in 0..32768 (0 = index default)");
            }
            ivfflat_probes_ = static_cast<std::uint32_t>(n);
        }
        if (name == "hnsw.ef_search") {  // K1.4: HNSW search beam width (pgvector's knob)
            std::uint64_t n = 0;
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    return ExecResult::failure(
                        "SET hnsw.ef_search expects an integer in 0..32768 (0 = default 40)");
                }
                n = n * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (value.empty() || n > 32768) {
                return ExecResult::failure(
                    "SET hnsw.ef_search expects an integer in 0..32768 (0 = default 40)");
            }
            hnsw_ef_search_ = n == 0 ? 40 : static_cast<std::uint32_t>(n);
        }
        // Unknown parameter → accepted no-op (client-compat). Return an empty OK result.
        return ExecResult{};
    }

    std::optional<std::string> materialize_select(const std::string& name, const SelectStmt& sel) {
        if (canceled()) return std::string("query canceled");  // W3.2: before a nested materialize
        const ExecResult q = exec_select(sel);
        if (!q.ok) return q.error;
        // W3.1: charge the materialized intermediate against the per-statement memory budget
        // (derived tables / CTEs / views build this fully before it is consumed — the common
        // unbounded-intermediate OOM vector). Deterministic: same rows → same charge → same
        // verdict on every replica. Inert when max_query_mem_ == 0 (the default).
        {
            std::size_t sz = 0;
            for (const ResultRow& r : q.rows) sz += result_row_bytes(r);
            if (auto e = charge_query_mem(sz)) return e;
        }
        // Determine the output column names + types. Names from the result labels; a type from the
        // first non-NULL value in each column (default INT if a column is entirely NULL/empty).
        std::size_t ncol = q.rows.empty() ? 0 : q.rows[0].cells.size();
        if (ncol == 0) return std::string("subquery / CREATE TABLE AS SELECT needs a non-empty result shape");
        Table t;
        t.name = name;
        Column pk;  // a hidden synthetic identity
        pk.name = "_ctid";
        pk.type = Type::Int;
        pk.auto_increment = true;
        pk.nullable = false;
        pk.dropped = true;  // hidden from SELECT * / name lookup; still the key identity
        t.columns.push_back(pk);
        t.pk_index = 0;
        for (std::size_t k = 0; k < ncol; ++k) {
            Column col;
            col.name = q.rows.empty() ? ("c" + std::to_string(k)) : q.rows[0].cells[k].first;
            col.nullable = true;
            col.type = Type::Int;
            for (const ResultRow& r : q.rows) {
                if (!r.cells[k].second.is_null) {
                    col.type = r.cells[k].second.type;
                    col.logical = r.cells[k].second.logical;
                    col.scale = r.cells[k].second.scale;
                    break;
                }
            }
            t.columns.push_back(std::move(col));
        }
        t.col_stats.assign(t.columns.size(), Table::ColStat{});
        (void)catalog_.create(std::move(t));
        persist_schema(name);
        // Insert the rows over the verified write path.
        Table* mt = catalog_.find_mut(name);
        std::int64_t auto_id = 1;
        std::vector<std::pair<Key, Value>> writes;
        for (const ResultRow& r : q.rows) {
            std::vector<Datum> row(mt->columns.size());
            row[0] = Datum::make_int(auto_id++);
            for (std::size_t k = 0; k < ncol; ++k) {
                Datum d;
                if (auto e = coerce(mt->columns[k + 1], r.cells[k].second, d)) return e;
                row[k + 1] = d;
            }
            emit_row_writes(*mt, row, writes);
        }
        commit_writes(writes);
        mt->next_auto_id = auto_id;
        persist_schema(name);
        return std::nullopt;
    }

    // W9 (information_schema): materialize an ephemeral table with an EXPLICIT typed schema
    // and pre-built rows (unlike materialize_select, which infers the schema from result
    // rows and cannot represent an empty relation). Used to synthesize system catalogs so a
    // plain SELECT (with its WHERE / projection / joins) runs over them via the normal path.
    // A canonical dedup key for a result row (type + null + value per cell). Used by
    // UNION-based recursive CTEs to detect when the fixpoint stops producing new rows.
    [[nodiscard]] static std::string row_key(const ResultRow& r) {
        std::string k;
        for (const auto& cell : r.cells) {
            k.push_back(cell.second.is_null ? 'N' : (cell.second.type == Type::Text ? 'T' : 'I'));
            if (!cell.second.is_null) {
                k += (cell.second.type == Type::Text) ? cell.second.s : std::to_string(cell.second.i);
            }
            k.push_back('\x1f');
        }
        return k;
    }

    // Materialize a NON-EMPTY result row set into a fresh table `name`, inferring the schema from
    // the rows (labels + first non-NULL type per column). Used by recursive-CTE fixpoint iteration.
    std::optional<std::string> materialize_rows(const std::string& name,
                                                const std::vector<ResultRow>& rows,
                                                const std::vector<std::string>& col_names = {}) {
        if (rows.empty()) return std::string("internal: materialize_rows over an empty set");
        const std::size_t ncol = rows[0].cells.size();
        std::vector<std::pair<std::string, Type>> cols(ncol);
        for (std::size_t k = 0; k < ncol; ++k) {
            // Column names come from the override (recursive CTE: fixed by the base term,
            // positional) or the rows' own labels.
            cols[k].first = (k < col_names.size()) ? col_names[k] : rows[0].cells[k].first;
            cols[k].second = Type::Int;
            for (const ResultRow& r : rows)
                if (!r.cells[k].second.is_null) { cols[k].second = r.cells[k].second.type; break; }
        }
        std::vector<std::vector<Datum>> drows;
        drows.reserve(rows.size());
        for (const ResultRow& r : rows) {
            std::vector<Datum> row(ncol);
            for (std::size_t k = 0; k < ncol; ++k) row[k] = r.cells[k].second;
            drows.push_back(std::move(row));
        }
        return materialize_typed(name, cols, drows);
    }

    // WITH RECURSIVE: fixpoint-materialize `name` from `<base> UNION [ALL] <recursive>`. The base
    // seeds the working set; the recursive term (which reads `name`) runs against the working set,
    // its fresh rows (deduped for UNION) accumulate, and iteration stops when no new rows appear
    // (bounded to avoid a non-terminating recursion). Finally `name` = the full accumulated set.
    std::optional<std::string> materialize_recursive_cte(const std::string& name,
                                                         const SelectStmt& sub) {
        if (sub.set_op != SetOp::Union || !sub.set_op_rhs) {
            return std::string("a recursive CTE must be `<base> UNION [ALL] <recursive>`");
        }
        const bool all = sub.set_op_all;
        SelectStmt base = sub;  // the base term = sub without its set-op tail
        base.set_op = SetOp::None;
        base.set_op_rhs.reset();
        const ExecResult b = exec_select(base);
        if (!b.ok) return b.error;
        if (b.rows.empty()) return std::string("recursive CTE '" + name + "' has an empty base term");
        // The CTE's column names come from an explicit WITH cte(cols) list if given, else the base
        // term (the recursive term maps by position either way).
        std::vector<std::string> col_names = sub.cte_columns;
        if (col_names.empty())
            for (const auto& cell : b.rows[0].cells) col_names.push_back(cell.first);
        else if (col_names.size() != b.rows[0].cells.size())
            return std::string("recursive CTE '" + name + "' column list arity mismatch");
        std::vector<ResultRow> accumulated = b.rows, working = b.rows;
        std::set<std::string> seen;
        if (!all) for (const ResultRow& r : accumulated) seen.insert(row_key(r));
        const SelectStmt& rec = *sub.set_op_rhs;
        for (int iter = 0; iter < 100000 && !working.empty(); ++iter) {
            if (catalog_.find(name) != nullptr) {
                DropTableStmt d; d.table = name; d.if_exists = true;
                (void)exec_drop_table(d);
            }
            if (auto e = materialize_rows(name, working, col_names)) return e;  // name := working set
            const ExecResult step = exec_select(rec);                // recursive term reads `name`
            if (!step.ok) return step.error;
            std::vector<ResultRow> fresh;
            for (const ResultRow& r : step.rows) {
                if (all || seen.insert(row_key(r)).second) fresh.push_back(r);
            }
            if (fresh.empty()) break;
            for (const ResultRow& r : fresh) accumulated.push_back(r);
            working = std::move(fresh);
        }
        if (catalog_.find(name) != nullptr) {
            DropTableStmt d; d.table = name; d.if_exists = true;
            (void)exec_drop_table(d);
        }
        return materialize_rows(name, accumulated, col_names);  // name := the full result
    }

    std::optional<std::string> materialize_typed(
        const std::string& name,
        const std::vector<std::pair<std::string, Type>>& cols,
        const std::vector<std::vector<Datum>>& rows) {
        if (catalog_.find(name) != nullptr) {
            return std::string("system relation name '" + name + "' clashes with a table");
        }
        Table t;
        t.name = name;
        Column pk;  // hidden synthetic identity (as in materialize_select)
        pk.name = "_ctid";
        pk.type = Type::Int;
        pk.auto_increment = true;
        pk.nullable = false;
        pk.dropped = true;
        t.columns.push_back(pk);
        t.pk_index = 0;
        for (const auto& c : cols) {
            Column col;
            col.name = c.first;
            col.type = c.second;
            col.nullable = true;
            t.columns.push_back(std::move(col));
        }
        t.col_stats.assign(t.columns.size(), Table::ColStat{});
        (void)catalog_.create(std::move(t));
        persist_schema(name);
        Table* mt = catalog_.find_mut(name);
        std::int64_t auto_id = 1;
        std::vector<std::pair<Key, Value>> writes;
        for (const auto& r : rows) {
            std::vector<Datum> row(mt->columns.size());
            row[0] = Datum::make_int(auto_id++);
            for (std::size_t k = 0; k < cols.size(); ++k) row[k + 1] = r[k];
            emit_row_writes(*mt, row, writes);
        }
        commit_writes(writes);
        mt->next_auto_id = auto_id;
        persist_schema(name);
        return std::nullopt;
    }

    // W9: SQL data-type name for a column (information_schema.columns.data_type).
    [[nodiscard]] static std::string sql_data_type_name(const Column& c) {
        switch (c.logical) {
            case 1: return "numeric";
            case 2: return "date";
            case 3: return "timestamp";
            case 14: return "double precision";  // F14
            case 15: return "vector";            // K1
            default: return c.type == Type::Text ? "text" : "integer";
        }
    }

    // W9: split a (possibly schema-qualified) catalog name into (schema, table). A bare name
    // defaults to the "public" schema, matching the PostgreSQL default search path.
    [[nodiscard]] static std::pair<std::string, std::string> split_schema(const std::string& qn) {
        const auto dot = qn.find('.');
        if (dot == std::string::npos) return {"public", qn};
        return {qn.substr(0, dot), qn.substr(dot + 1)};
    }

    // W9: recognise a system relation name (lower-cased) that SELECT synthesises on the fly.
    [[nodiscard]] static bool is_system_relation(const std::string& lower_name) {
        return lower_name == "information_schema.tables" ||
               lower_name == "information_schema.columns" ||
               lower_name == "information_schema.views" ||
               lower_name == "information_schema.schemata" ||
               lower_name == "information_schema.table_constraints" ||
               lower_name == "information_schema.key_column_usage" ||
               lower_name == "information_schema.constraint_column_usage" ||
               lower_name == "pg_catalog.pg_tables" ||
               lower_name == "pg_tables" ||
               lower_name == "pg_catalog.pg_namespace" ||
               lower_name == "pg_namespace" ||
               lower_name == "pg_catalog.pg_class" ||
               lower_name == "pg_class" ||
               lower_name == "pg_catalog.pg_type" ||  // K1: vector-OID discovery
               lower_name == "pg_type";
    }

    // W9: build the (columns, rows) of a synthesised system relation from the live catalog.
    // Both relations are all-TEXT except columns.ordinal_position (INT). The ephemeral table
    // itself (materialised under an alias for the current query) is skipped so it never lists
    // itself. Deterministic: catalog_.all() / all_views() iterate in name order (std::map).
    [[nodiscard]] std::pair<std::vector<std::pair<std::string, Type>>,
                            std::vector<std::vector<Datum>>>
    build_system_relation(const std::string& lower_name) {
        std::vector<std::pair<std::string, Type>> cols;
        std::vector<std::vector<Datum>> rows;
        auto is_ephemeral = [](const std::string& n) {
            // Hide the transient system/materialisation relations so they never list themselves.
            return n.rfind("information_schema.", 0) == 0 || n.rfind("pg_catalog.", 0) == 0 ||
                   n.rfind("pg_", 0) == 0;
        };
        if (lower_name == "information_schema.schemata") {
            cols = {{"catalog_name", Type::Text}, {"schema_name", Type::Text}};
            std::set<std::string> schemas{"public", "information_schema", "pg_catalog"};
            for (const auto& [qn, t] : catalog_.all()) {
                (void)t;
                if (is_ephemeral(qn)) continue;
                schemas.insert(split_schema(qn).first);
            }
            for (const std::string& s : schemas) {
                rows.push_back({Datum::make_text("lockstep"), Datum::make_text(s)});
            }
        } else if (lower_name == "information_schema.views") {
            cols = {{"table_catalog", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"view_definition", Type::Text}};
            for (const auto& [qn, v] : catalog_.all_views()) {
                const auto [sch, nm] = split_schema(qn);
                rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                Datum::make_text(nm), Datum::make_text(v.select_src)});
            }
        } else if (lower_name == "pg_catalog.pg_tables" || lower_name == "pg_tables") {
            cols = {{"schemaname", Type::Text}, {"tablename", Type::Text},
                    {"tableowner", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                (void)t;
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                rows.push_back({Datum::make_text(sch), Datum::make_text(nm),
                                Datum::make_text("lockstep")});
            }
        } else if (lower_name == "pg_catalog.pg_namespace" || lower_name == "pg_namespace") {
            cols = {{"nspname", Type::Text}};
            std::set<std::string> schemas{"public", "information_schema", "pg_catalog"};
            for (const auto& [qn, t] : catalog_.all()) {
                (void)t;
                if (is_ephemeral(qn)) continue;
                schemas.insert(split_schema(qn).first);
            }
            for (const std::string& s : schemas) rows.push_back({Datum::make_text(s)});
        } else if (lower_name == "pg_catalog.pg_class" || lower_name == "pg_class") {
            // relkind: 'r' ordinary table, 'v' view (the two we synthesise).
            cols = {{"relname", Type::Text}, {"relkind", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                (void)t;
                if (is_ephemeral(qn)) continue;
                rows.push_back({Datum::make_text(split_schema(qn).second), Datum::make_text("r")});
            }
            for (const auto& [qn, v] : catalog_.all_views()) {
                (void)v;
                rows.push_back({Datum::make_text(split_schema(qn).second), Datum::make_text("v")});
            }
        } else if (lower_name == "pg_catalog.pg_type" || lower_name == "pg_type") {
            // K1: the type OIDs the PG wire shim advertises — enough for client-side type
            // discovery, notably `SELECT oid FROM pg_type WHERE typname = 'vector'` (the
            // pgvector adapters' registration query). OIDs match PgWire.hpp's mapping.
            cols = {{"oid", Type::Int}, {"typname", Type::Text}};
            const std::pair<std::int64_t, const char*> kTypes[] = {
                {16, "bool"},     {20, "int8"},      {21, "int2"},      {23, "int4"},
                {25, "text"},     {114, "json"},     {700, "float4"},   {701, "float8"},
                {1043, "varchar"}, {1082, "date"},   {1083, "time"},    {1114, "timestamp"},
                {1700, "numeric"}, {2950, "uuid"},   {16388, "vector"},
            };
            for (const auto& [oid, nm] : kTypes) {
                rows.push_back({Datum::make_int(oid), Datum::make_text(nm)});
            }
        } else if (lower_name == "information_schema.table_constraints") {
            cols = {{"constraint_catalog", Type::Text}, {"constraint_schema", Type::Text},
                    {"constraint_name", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"constraint_type", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                auto emit = [&](const std::string& cname, const std::string& ctype) {
                    rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                    Datum::make_text(cname), Datum::make_text(sch),
                                    Datum::make_text(nm), Datum::make_text(ctype)});
                };
                if (!t.columns[t.pk_index].dropped) emit(nm + "_pkey", "PRIMARY KEY");
                for (const Column& c : t.columns) {
                    if (c.dropped) continue;
                    if (c.unique) emit(nm + "_" + c.name + "_key", "UNIQUE");
                    if (!c.fk_table.empty()) emit(nm + "_" + c.name + "_fkey", "FOREIGN KEY");
                }
            }
        } else if (lower_name == "information_schema.constraint_column_usage") {
            // For a FOREIGN KEY, names the REFERENCED table + column (what the FK points at) —
            // ORMs read this to resolve relationships. Keyed by the child's FK constraint name.
            cols = {{"table_catalog", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"column_name", Type::Text},
                    {"constraint_name", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                for (const Column& c : t.columns) {
                    if (c.dropped || c.fk_table.empty()) continue;
                    const auto [rsch, rnm] = split_schema(c.fk_table);
                    const std::string refcol = c.fk_column.empty() ? std::string("id") : c.fk_column;
                    rows.push_back({Datum::make_text("lockstep"), Datum::make_text(rsch),
                                    Datum::make_text(rnm), Datum::make_text(refcol),
                                    Datum::make_text(nm + "_" + c.name + "_fkey")});
                }
            }
        } else if (lower_name == "information_schema.key_column_usage") {
            cols = {{"constraint_catalog", Type::Text}, {"constraint_schema", Type::Text},
                    {"constraint_name", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"column_name", Type::Text},
                    {"ordinal_position", Type::Int}};
            for (const auto& [qn, t] : catalog_.all()) {
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                auto emit = [&](const std::string& cname, const std::string& col, std::int64_t ord) {
                    rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                    Datum::make_text(cname), Datum::make_text(sch),
                                    Datum::make_text(nm), Datum::make_text(col), Datum::make_int(ord)});
                };
                if (!t.columns[t.pk_index].dropped) {
                    const std::vector<std::size_t>& pkc =
                        t.pk_columns.empty() ? std::vector<std::size_t>{t.pk_index} : t.pk_columns;
                    std::int64_t ord = 0;
                    for (std::size_t ci : pkc) emit(nm + "_pkey", t.columns[ci].name, ++ord);
                }
                for (const Column& c : t.columns) {
                    if (c.dropped) continue;
                    if (c.unique) emit(nm + "_" + c.name + "_key", c.name, 1);
                    if (!c.fk_table.empty()) emit(nm + "_" + c.name + "_fkey", c.name, 1);
                }
            }
        } else if (lower_name == "information_schema.tables") {
            cols = {{"table_catalog", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"table_type", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                (void)t;
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                Datum::make_text(nm), Datum::make_text("BASE TABLE")});
            }
            for (const auto& [qn, v] : catalog_.all_views()) {
                (void)v;
                const auto [sch, nm] = split_schema(qn);
                rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                Datum::make_text(nm), Datum::make_text("VIEW")});
            }
        } else {  // information_schema.columns
            cols = {{"table_catalog", Type::Text}, {"table_schema", Type::Text},
                    {"table_name", Type::Text}, {"column_name", Type::Text},
                    {"ordinal_position", Type::Int}, {"data_type", Type::Text},
                    {"is_nullable", Type::Text}};
            for (const auto& [qn, t] : catalog_.all()) {
                if (is_ephemeral(qn)) continue;
                const auto [sch, nm] = split_schema(qn);
                std::int64_t ord = 0;
                for (const Column& c : t.columns) {
                    if (c.dropped) continue;  // hidden synthetic PK / dropped columns
                    ++ord;
                    rows.push_back({Datum::make_text("lockstep"), Datum::make_text(sch),
                                    Datum::make_text(nm), Datum::make_text(c.name),
                                    Datum::make_int(ord), Datum::make_text(sql_data_type_name(c)),
                                    Datum::make_text(c.nullable ? "YES" : "NO")});
                }
            }
        }
        return {std::move(cols), std::move(rows)};
    }

    // H1: apply a view's explicit output-column list to its just-materialized ephemeral table. Renames
    // the VISIBLE columns (the hidden synthetic `_ctid` PK is skipped) in order; the name count must
    // match the query's output arity. In-memory only (the table is dropped after the query). Returns
    // an error string on an arity mismatch, std::nullopt on success.
    std::optional<std::string> rename_materialized_columns(const std::string& table,
                                                           const std::vector<std::string>& names) {
        Table* t = catalog_.find_mut(table);
        if (t == nullptr) return std::string("internal: materialized view table '" + table + "' missing");
        std::vector<std::size_t> visible;
        for (std::size_t i = 0; i < t->columns.size(); ++i)
            if (!t->columns[i].dropped) visible.push_back(i);
        if (visible.size() != names.size())
            return "view column list has " + std::to_string(names.size()) +
                   " name(s) but the view query yields " + std::to_string(visible.size());
        for (std::size_t k = 0; k < visible.size(); ++k) t->columns[visible[k]].name = names[k];
        return std::nullopt;
    }

    // I6: ANALYZE t — scan the table once and recompute per-column stats: n_distinct (exact distinct
    // non-NULL value count) + INT min/max + row_count. Used by the cost model for eq selectivity
    // (matches ~= n / n_distinct). In-memory (re-run after large changes; not persisted).
    ExecResult exec_analyze(const std::string& table) {
        Table* t = catalog_.find_mut(table);
        if (t == nullptr) return ExecResult::failure("unknown table '" + table + "'");
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(*t, rows)) return ExecResult::failure(*e);
        const std::size_t nc = t->columns.size();
        std::vector<std::set<std::string>> distinct(nc);
        std::vector<Table::ColStat> cs(nc);
        for (const auto& [pk, r] : rows) {
            for (std::size_t c = 0; c < nc; ++c) {
                const Datum& d = r[c];
                if (d.is_null) continue;
                distinct[c].insert(group_key_field(d));
                if (d.type == Type::Int) {
                    if (!cs[c].seen) { cs[c].seen = true; cs[c].lo = cs[c].hi = d.i; }
                    else { cs[c].lo = std::min(cs[c].lo, d.i); cs[c].hi = std::max(cs[c].hi, d.i); }
                }
            }
        }
        for (std::size_t c = 0; c < nc; ++c)
            cs[c].n_distinct = static_cast<std::int64_t>(distinct[c].size());
        t->col_stats = std::move(cs);
        t->row_count = rows.size();
        return ExecResult{};
    }

    // E5: SHOW TABLES — one row per table (in deterministic catalog order), column "table".
    ExecResult exec_show_tables() {
        ExecResult r;
        for (const auto& [name, t] : catalog_.all()) {
            (void)t;
            ResultRow row;
            row.cells.emplace_back("table", Datum::make_text(name));
            r.rows.push_back(std::move(row));
        }
        r.affected = r.rows.size();
        return r;
    }
    // E5: DESCRIBE t — one row per visible column: (column, type, nullable, key).
    ExecResult exec_describe(const std::string& table) {
        const Table* t = catalog_.find(table);
        if (t == nullptr) return ExecResult::failure("unknown table '" + table + "'");
        ExecResult r;
        for (std::size_t i = 0; i < t->columns.size(); ++i) {
            const Column& c = t->columns[i];
            if (c.dropped) continue;
            ResultRow row;
            row.cells.emplace_back("column", Datum::make_text(c.name));
            row.cells.emplace_back("type", Datum::make_text(describe_type(c)));
            row.cells.emplace_back("nullable", Datum::make_text(c.nullable ? "YES" : "NO"));
            row.cells.emplace_back("key", Datum::make_text(is_pk_col(*t, i) ? "PK" : (c.unique ? "UNIQUE" : "")));
            r.rows.push_back(std::move(row));
        }
        r.affected = r.rows.size();
        return r;
    }
    static std::string describe_type(const Column& c) {
        if (c.logical == 15)  // K1: VECTOR(n) — show the declared dimension
            return c.max_len != 0 ? "VECTOR(" + std::to_string(c.max_len) + ")" : "VECTOR";
        if (c.logical != 0) {
            std::string base = logical_name(c.logical);
            if ((c.logical == 1 || c.logical == 6) && c.scale) base += "(s=" + std::to_string(c.scale) + ")";
            return base;
        }
        return c.type == Type::Int ? "INT" : "TEXT";
    }

    // E2: TRUNCATE TABLE — delete every row (schema kept). Tombstones each row + its index entries.
    ExecResult exec_truncate(const TruncateStmt& tr) {
        Table* t = catalog_.find_mut(tr.table);
        if (t == nullptr) return ExecResult::failure("unknown table '" + tr.table + "'");
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(*t, rows)) return ExecResult::failure(*e);
        std::vector<std::pair<Key, Value>> writes;
        for (auto& [pk, r] : rows) {
            const Key rk = t->composite_pk() ? encode_key_row(*t, r) : encode_key(*t, pk);
            writes.emplace_back(rk, tombstone_marker());
            index_writes_for_row(*t, r, /*tombstone=*/true, writes);
        }
        commit_writes(writes);
        return ExecResult{};
    }

    // --- CREATE INDEX <name> ON <table> (<col>) -------------------------------
    // Registers a single-column secondary index in the catalog AND BACKFILLS it: an
    // index entry is written for EVERY existing row of the table, atomically per row
    // (the backfill rides the same verified write path the live maintenance uses). The
    // index then stays consistent with the table because every later INSERT/UPDATE/
    // DELETE maintains it in the SAME txn as the row write (commit_writes, below).
    ExecResult exec_create_index(const CreateIndexStmt& ci) {
        Table* t = catalog_.find_mut(ci.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + ci.table + "'");
        }
        if (t->index_by_name(ci.index) != nullptr) {
            return ExecResult::failure("index '" + ci.index + "' already exists on '" +
                                       ci.table + "'");
        }
        const bool is_expr = !ci.expr_src.empty();  // J2: CREATE INDEX ... ON t ((expr))
        Index ix;
        ix.name = ci.index;
        ix.id = t->next_index_id++;
        ix.unique = ci.unique;  // E5
        ix.hash = ci.hash;      // I7: equality-only index
        ix.partial_src = ci.partial_src;  // I5: partial index predicate
        if (!ix.partial_src.empty()) {    // validate it parses against this table
            const ParseResult pr = parse_sql("SELECT * FROM " + ci.table + " WHERE " + ix.partial_src);
            if (!pr.ok()) return ExecResult::failure("invalid partial index WHERE: " + pr.error().render());
        }
        if (ci.ivfflat && is_expr) {  // K1.3
            return ExecResult::failure("an IVFFLAT index cannot be an expression index");
        }
        if (is_expr) {
            // J2: EXPRESSION index. `columns` stays empty; the entry's leading token is the
            // evaluated expression value. Validate the expression parses + evaluates against the
            // table, then infer its physical type (authoritatively from a live row when one exists;
            // structurally for an empty table). The write path re-checks the type per row.
            ix.expr_src = ci.expr_src;
            ix.hash = false;  // an expression index is ordered (range-capable); HASH is moot here
            std::shared_ptr<Expr> ex;
            if (auto e = parse_index_expr(*t, ix.expr_src, ex)) return ExecResult::failure(*e);
            ix.expr_type = infer_index_expr_type(*ex, *t);
        } else {
            const auto col = t->column_index(ci.column);
            if (!col) {
                return ExecResult::failure("unknown column '" + ci.column +
                                           "' in table '" + ci.table + "'");
            }
            // Indexing the PK is pointless (the PK already has the table-row order index)
            // and would never be chosen over the PK fast path — reject it fail-closed so a
            // redundant index can never diverge.
            if (*col == t->pk_index) {
                return ExecResult::failure(
                    "cannot CREATE INDEX on the PRIMARY KEY column '" + ci.column +
                    "' (the PK is already an ordered access path)");
            }
            ix.column = *col;
            for (const std::string& cn : ci.columns) {  // E5: resolve the covered column list
                const auto c = t->column_index(cn);
                if (!c) return ExecResult::failure("unknown column '" + cn + "' in index");
                ix.columns.push_back(*c);
            }
            if (ci.gin) {  // J3: GIN — a single ARRAY column, indexed per element
                if (ix.columns.size() != 1) {
                    return ExecResult::failure("USING GIN indexes exactly one ARRAY column");
                }
                if (t->columns[ix.columns[0]].logical != 7) {
                    return ExecResult::failure("USING GIN requires an ARRAY column (got '" +
                                               ci.column + "')");
                }
                if (ci.unique) {
                    return ExecResult::failure("a GIN (array-element) index cannot be UNIQUE");
                }
                ix.gin = true;
                ix.hash = false;  // GIN entries are ordered per element (range is moot)
            }
            if (ci.ivfflat) {  // K1.3: IVFFLAT — one dimensioned VECTOR(n) column, non-unique
                if (ix.columns.size() != 1) {
                    return ExecResult::failure("USING IVFFLAT indexes exactly one VECTOR column");
                }
                const Column& vc = t->columns[ix.columns[0]];
                if (vc.logical != 15 || vc.max_len == 0) {
                    return ExecResult::failure(
                        "USING IVFFLAT requires a dimensioned VECTOR(n) column (got '" +
                        ci.column + "')");
                }
                if (ci.unique) return ExecResult::failure("an IVFFLAT index cannot be UNIQUE");
                if (!ci.partial_src.empty()) {
                    return ExecResult::failure("an IVFFLAT index cannot be partial");
                }
                if (t->composite_pk()) {
                    return ExecResult::failure(
                        "an IVFFLAT index is not supported on a composite-PK table");
                }
                ix.ivfflat = true;
                ix.hash = false;
                ix.lists = ci.lists != 0 ? ci.lists : 100;   // pgvector's default lists
                ix.probes = ci.probes != 0 ? ci.probes : 1;  // pgvector's default probes
                ix.vec_op = ci.vec_op;                       // K1.3c: operator class
            }
            if (!ci.ivfflat && !ci.hnsw && ci.vec_op != 0) {  // K1.3c/K1.4
                return ExecResult::failure(
                    "a vector operator class requires USING IVFFLAT or USING HNSW");
            }
            if (ci.bm25) {  // K2: BM25 — one TEXT column, non-unique, non-partial
                if (ix.columns.size() != 1) {
                    return ExecResult::failure("USING BM25 indexes exactly one TEXT column");
                }
                const Column& tc = t->columns[ix.columns[0]];
                if (tc.type != Type::Text || tc.logical != 0) {
                    return ExecResult::failure("USING BM25 requires a plain TEXT column (got '" +
                                               ci.column + "')");
                }
                if (ci.unique) return ExecResult::failure("a BM25 index cannot be UNIQUE");
                if (!ci.partial_src.empty()) {
                    return ExecResult::failure("a BM25 index cannot be partial");
                }
                if (t->composite_pk()) {
                    return ExecResult::failure(
                        "a BM25 index is not supported on a composite-PK table");
                }
                ix.bm25 = true;
                ix.hash = false;
            }
            if (ci.hnsw) {  // K1.4: HNSW — one dimensioned VECTOR(n) column, non-unique
                if (ix.columns.size() != 1) {
                    return ExecResult::failure("USING HNSW indexes exactly one VECTOR column");
                }
                const Column& vc = t->columns[ix.columns[0]];
                if (vc.logical != 15 || vc.max_len == 0) {
                    return ExecResult::failure(
                        "USING HNSW requires a dimensioned VECTOR(n) column (got '" +
                        ci.column + "')");
                }
                if (ci.unique) return ExecResult::failure("an HNSW index cannot be UNIQUE");
                if (!ci.partial_src.empty()) {
                    return ExecResult::failure("an HNSW index cannot be partial");
                }
                if (t->composite_pk()) {
                    return ExecResult::failure(
                        "an HNSW index is not supported on a composite-PK table");
                }
                ix.hnsw = true;
                ix.hash = false;
                ix.hnsw_m = ci.hnsw_m != 0 ? ci.hnsw_m : 16;      // pgvector's default m
                ix.hnsw_efc = ci.hnsw_efc != 0 ? ci.hnsw_efc : 64;  // default ef_construction
                ix.vec_op = ci.vec_op;
            }
        }
        t->indexes.push_back(ix);
        persist_schema(ci.table);  // C7: the new index is part of the durable schema
        const std::uint32_t table_id = t->id;

        // K2: BM25 backfill — chunked batches through the SAME maintenance path (postings
        // + running stats), so a backfilled index is byte-identical to one maintained live.
        if (ix.bm25) {
            std::vector<std::vector<Datum>> all_rows;
            if (t->columnar) {
                SelectStmt full;
                full.table = t->name;
                full.level = Level::StrictSerializable;
                std::vector<bool> all(t->columns.size(), true);
                if (auto e = columnar_build_rows(*t, full, all, all_rows)) {
                    t->indexes.pop_back();
                    persist_schema(ci.table);
                    return ExecResult::failure(*e);
                }
            } else {
                std::vector<storage::KeyValue> kvs;
                {
                    Query<Strict> q;
                    q.scan(table_prefix(table_id), table_prefix_end(table_id));
                    collect(db_.run(q), kvs);
                }
                for (const storage::KeyValue& kv : kvs) {
                    if (is_tombstone(kv.second)) continue;
                    all_rows.push_back(decode_row(*t, kv.first, kv.second));
                }
            }
            const Index& live = t->indexes.back();
            std::vector<std::pair<Key, Value>> writes;
            for (const auto& row : all_rows) {
                bm25_maintain(*t, live, row, /*tombstone=*/false, writes);
                if (writes.size() >= 4096) {
                    commit_writes(writes);
                    writes.clear();
                }
            }
            if (!writes.empty()) commit_writes(writes);
            return ExecResult{};
        }

        // K1.4: HNSW build — insert every live row's node in PK (scan) order, one atomic
        // commit per node (each insert's greedy search reads the graph the prior commits
        // built). Deterministic: same rows, same order => byte-identical graph on replicas.
        if (ix.hnsw) {
            std::vector<std::vector<Datum>> all_rows;
            if (t->columnar) {
                SelectStmt full;
                full.table = t->name;
                full.level = Level::StrictSerializable;
                std::vector<bool> all(t->columns.size(), true);
                if (auto e = columnar_build_rows(*t, full, all, all_rows)) {
                    t->indexes.pop_back();
                    persist_schema(ci.table);
                    return ExecResult::failure(*e);
                }
            } else {
                std::vector<storage::KeyValue> kvs;
                {
                    Query<Strict> q;
                    q.scan(table_prefix(table_id), table_prefix_end(table_id));
                    collect(db_.run(q), kvs);
                }
                for (const storage::KeyValue& kv : kvs) {
                    if (is_tombstone(kv.second)) continue;
                    all_rows.push_back(decode_row(*t, kv.first, kv.second));
                }
            }
            const std::size_t vcol = ix.columns[0];
            const Index& live = t->indexes.back();
            for (const auto& row : all_rows) {
                if (row[vcol].is_null) continue;
                std::vector<std::pair<Key, Value>> writes;
                hnsw_insert(*t, live, encode_pk(row[t->pk_index]), row[vcol].s, writes);
                commit_writes(writes);
            }
            return ExecResult{};
        }

        // K1.3: IVFFLAT build — enumerate every live row FIRST (the k-means needs the full
        // vector set), compute the FROZEN centroids (deterministic k-means over the PK-ordered
        // vectors), re-persist the schema (centroids are part of it), then bucket-backfill one
        // entry per row (value = the vector payload) through the verified write path.
        if (ix.ivfflat) {
            std::vector<std::vector<Datum>> all_rows;
            if (t->columnar) {
                SelectStmt full;
                full.table = t->name;
                full.level = Level::StrictSerializable;
                std::vector<bool> all(t->columns.size(), true);
                if (auto e = columnar_build_rows(*t, full, all, all_rows)) {
                    t->indexes.pop_back();
                    persist_schema(ci.table);
                    return ExecResult::failure(*e);
                }
            } else {
                std::vector<storage::KeyValue> kvs;
                {
                    Query<Strict> q;
                    q.scan(table_prefix(table_id), table_prefix_end(table_id));
                    collect(db_.run(q), kvs);
                }
                for (const storage::KeyValue& kv : kvs) {
                    if (is_tombstone(kv.second)) continue;
                    all_rows.push_back(decode_row(*t, kv.first, kv.second));
                }
            }
            const std::size_t vcol = ix.columns[0];
            const std::size_t dim = t->columns[vcol].max_len;
            std::vector<std::vector<double>> vecs;
            vecs.reserve(all_rows.size());
            for (const auto& row : all_rows)
                if (!row[vcol].is_null)
                    vecs.push_back(ivf_assign_vec(ix.vec_op, ivf_payload_doubles(row[vcol].s)));
            // Effective list count: never more lists than vectors (each seed needs a vector).
            const std::size_t k = std::max<std::size_t>(
                1, std::min<std::size_t>(ix.lists, vecs.empty() ? 1 : vecs.size()));
            // K1 perf: k-means over a DETERMINISTIC SAMPLE (every stride-th vector in PK
            // order, ~200 per list — 4x pgvector's rule: its 50/list bent centroids on overlapping clusters (v3 recall plateau 0.85)) instead of every vector:
            // the build was dominated by the full-set Lloyd iterations. stride == 1 (all
            // vectors) whenever the table is small, so existing behavior/centroids are
            // unchanged there. The ASSIGNMENT pass still covers every vector exactly.
            // Sampling is reserved for HUGE sets (> 1000 vectors/list): on overlapping
            // clusters a sampled k-means bends centroids and costs recall (v3 sweep:
            // 1.0 -> 0.85-0.90 plateau) — quality wins by default at normal sizes.
            const std::size_t stride =
                vecs.size() > 1000 * k ? std::max<std::size_t>(1, vecs.size() / (400 * k)) : 1;
            std::vector<std::vector<double>> cents;
            if (stride == 1) {
                cents = ivf_kmeans(vecs, k, dim);
            } else {
                std::vector<std::vector<double>> sample;
                sample.reserve(vecs.size() / stride + 1);
                for (std::size_t i = 0; i < vecs.size(); i += stride) sample.push_back(vecs[i]);
                cents = ivf_kmeans(sample, std::min<std::size_t>(k, sample.size()), dim);
                while (cents.size() < k) cents.push_back(std::vector<double>(dim, 0.0));
            }
            Index& live = t->indexes.back();
            live.lists = static_cast<std::uint32_t>(k);
            live.centroids = ivf_encode_centroids(dim, cents);
            persist_schema(ci.table);
            ivf_probe_cache_.erase(ivf_cache_key(table_id, live.id));  // K1 perf: fresh build
            // K1 perf: backfill in CHUNKED batches — one commit per row made the build
            // ~10x slower than pgvector's (each commit pays the full write pipeline).
            // Content and order are identical; the crash-mid-backfill window is the same
            // as per-row commits (the catalog entry precedes the backfill either way).
            std::vector<std::pair<Key, Value>> writes;
            constexpr std::size_t kBackfillChunk = 4096;
            writes.reserve(kBackfillChunk);
            for (const auto& row : all_rows) {
                if (row[vcol].is_null) continue;
                const std::size_t list =
                    ivf_nearest(cents, ivf_assign_vec(ix.vec_op, ivf_payload_doubles(row[vcol].s)));
                writes.emplace_back(encode_index_entry(table_id, live,
                                                       Datum::make_int(static_cast<std::int64_t>(list)),
                                                       row[t->pk_index]),
                                    row[vcol].s);
                if (writes.size() >= kBackfillChunk) {
                    commit_writes(writes);
                    writes.clear();
                }
            }
            if (!writes.empty()) commit_writes(writes);
            return ExecResult{};
        }

        // BACKFILL: scan every live row and write its index entry. Read through the
        // verified scan path (a full table scan), then commit one index-entry write per
        // row through the verified executor (one txn per row — atomic, ordered).
        if (t->columnar) {
            // Columnar: enumerate every live row (block+delta merge, all columns) and
            // write its index entry.
            SelectStmt full;  // a bare full-table SELECT for the merged enumeration
            full.table = t->name;
            full.level = Level::StrictSerializable;
            std::vector<bool> all(t->columns.size(), true);
            std::vector<std::vector<Datum>> rows;
            if (auto e = columnar_build_rows(*t, full, all, rows)) {
                return ExecResult::failure(*e);
            }
            std::set<std::string> uniq_seen;  // E5: UNIQUE backfill dedup (value prefix)
            for (const std::vector<Datum>& row : rows) {
                if (ix.gin) {  // J3: one entry per distinct array element
                    if (!row_matches_partial(*t, ix, row)) continue;
                    for (const Datum& el : gin_elements(ix, row))
                        commit_writes({{encode_index_entry(table_id, ix, el, row[t->pk_index]), std::string{}}});
                    continue;
                }
                bool skip = true;
                Key entry;
                std::string uk;
                if (auto e = index_backfill_entry(*t, t->indexes.back(), row, skip, entry, uk))
                    { t->indexes.pop_back(); persist_schema(ci.table); return ExecResult::failure(*e); }
                if (skip) continue;
                if (ix.unique && !uniq_seen.insert(uk).second)
                    { t->indexes.pop_back(); persist_schema(ci.table); return ExecResult::failure("CREATE UNIQUE INDEX failed: duplicate value"); }
                commit_writes({{entry, std::string{}}});
            }
            return ExecResult{};
        }
        std::vector<storage::KeyValue> kvs;
        {
            Query<Strict> q;
            q.scan(table_prefix(table_id), table_prefix_end(table_id));
            collect(db_.run(q), kvs);
        }
        std::set<std::string> uniq_seen;  // E5
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            const std::vector<Datum> row = decode_row(*t, kv.first, kv.second);
            if (ix.gin) {  // J3: one entry per distinct array element
                if (!row_matches_partial(*t, ix, row)) continue;
                for (const Datum& el : gin_elements(ix, row))
                    commit_writes({{encode_index_entry(table_id, ix, el, row[t->pk_index]), std::string{}}});
                continue;
            }
            bool skip = true;
            Key entry;
            std::string uk;
            if (auto e = index_backfill_entry(*t, t->indexes.back(), row, skip, entry, uk))
                { t->indexes.pop_back(); persist_schema(ci.table); return ExecResult::failure(*e); }
            if (skip) continue;
            if (ix.unique && !uniq_seen.insert(uk).second)
                { t->indexes.pop_back(); persist_schema(ci.table); return ExecResult::failure("CREATE UNIQUE INDEX failed: duplicate value"); }
            commit_writes({{entry, std::string{}}});
        }
        return ExecResult{};
    }
    // E5 helpers.
    static bool index_row_null(const Index& ix, const std::vector<Datum>& row) {
        for (const std::size_t c : ix.columns) if (row[c].is_null) return true;
        return false;
    }
    static std::string index_value_key(const Table& t, const Index& ix, const std::vector<Datum>& row) {
        return encode_index_value_prefix(t.id, ix, row);  // bytes that identify the indexed tuple
    }
    // J2: build ONE backfill entry for `row`. Sets `skip` when the row gets no entry (NULL value /
    // outside the partial set). Returns an error string only for an expression index whose row cannot
    // be soundly evaluated — the CREATE then aborts (fail-closed), so a stored row is never missing
    // from the index. `uniq_key` identifies the indexed value for the UNIQUE backfill dedup.
    // J2: the dedup key for a UNIQUE index, handling an expression index (its key is the evaluated
    // expression value; a NULL value is exempt from UNIQUE, like a NULL column). `skip` => no key.
    [[nodiscard]] std::optional<std::string> index_unique_key(const Table& t, const Index& ix,
                                                              const std::vector<Datum>& row,
                                                              bool& skip, std::string& key) {
        skip = true;
        if (!ix.expr_src.empty()) {
            Datum v;
            bool s = true;
            if (auto e = eval_index_expr(t, ix, row, s, v)) return e;
            if (s) return std::nullopt;
            key = encode_index_col(v);
            skip = false;
            return std::nullopt;
        }
        if (index_row_null(ix, row)) return std::nullopt;
        key = index_value_key(t, ix, row);
        skip = false;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> index_backfill_entry(
        const Table& t, const Index& ix, const std::vector<Datum>& row, bool& skip, Key& entry,
        std::string& uniq_key) {
        skip = true;
        if (!row_matches_partial(t, ix, row)) return std::nullopt;
        if (!ix.expr_src.empty()) {
            Datum v;
            if (auto e = eval_index_expr(t, ix, row, skip, v)) return e;
            if (skip) return std::nullopt;
            entry = encode_index_entry(t.id, ix, v, row[t.pk_index]);
            uniq_key = encode_index_col(v);
            return std::nullopt;
        }
        if (index_row_null(ix, row)) return std::nullopt;
        skip = false;
        entry = encode_index_entry_row(t.id, ix, row, row[t.pk_index]);
        uniq_key = index_value_key(t, ix, row);
        return std::nullopt;
    }

    // --- DROP INDEX <name> ON <table> -----------------------------------------
    // Remove the index from the catalog AND tombstone every one of its KV entries.
    ExecResult exec_drop_index(const DropIndexStmt& di) {
        Table* t = catalog_.find_mut(di.table);
        if (t == nullptr) {
            if (di.if_exists) return ExecResult{};  // E2: no-op
            return ExecResult::failure("unknown table '" + di.table + "'");
        }
        const Index* ixp = t->index_by_name(di.index);
        if (ixp == nullptr) {
            if (di.if_exists) return ExecResult{};  // E2: no-op
            return ExecResult::failure("unknown index '" + di.index + "' on table '" +
                                       di.table + "'");
        }
        const Index ix = *ixp;  // copy before mutating the vector
        const std::uint32_t table_id = t->id;
        ivf_probe_cache_.erase(ivf_cache_key(table_id, ix.id));  // K1 perf: derived cache
        // Tombstone every index entry (the scan path ignores tombstones, so this fully
        // retires the index's key range).
        std::vector<storage::KeyValue> kvs;
        {
            Query<Strict> q;
            const Key lo = index_prefix(table_id, ix.id);
            q.scan(lo, key_successor(lo));
            collect(db_.run(q), kvs);
        }
        for (const storage::KeyValue& kv : kvs) {
            if (!is_tombstone(kv.second)) {
                commit_writes({{kv.first, tombstone_marker()}});
            }
        }
        // Erase from the catalog (so the planner stops choosing it).
        for (std::size_t i = 0; i < t->indexes.size(); ++i) {
            if (t->indexes[i].name == di.index) {
                t->indexes.erase(t->indexes.begin() +
                                 static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        persist_schema(di.table);  // C7: drop reflected in the durable schema
        return ExecResult{};
    }

    // ALTER TABLE <t> ADD COLUMN (F7): append a column to the schema. Existing rows were written
    // with the OLD column count; the row decoder pads the new (suffix) columns with their DEFAULT or
    // NULL on read (see decode_row), so no data rewrite is needed. Row-mode only (columnar block
    // layout would need a per-column rewrite — OUT). A NOT NULL column without a DEFAULT is rejected
    // on a non-empty table (existing rows would have no value).
    ExecResult exec_alter(const AlterStmt& a) {
        Table* t = catalog_.find_mut(a.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + a.table + "'");
        }
        if (t->columnar) {
            return ExecResult::failure("ALTER TABLE on a columnar table is not supported");
        }
        // E1: ALTER TABLE RENAME TO — re-key the catalog (data/id unchanged).
        if (a.op == AlterOp::RenameTable) {
            if (catalog_.has(a.new_name)) return ExecResult::failure("table '" + a.new_name + "' already exists");
            if (!catalog_.rename(a.table, a.new_name)) return ExecResult::failure("rename failed");
            retire_schema(a.table);
            persist_schema(a.new_name);
            return ExecResult{};
        }
        // The remaining ops target a column (by name); resolve it (a dropped column is invisible).
        auto cidx = [&](const std::string& n) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < t->columns.size(); ++i)
                if (!t->columns[i].dropped && t->columns[i].name == n) return i;
            return std::nullopt;
        };
        if (a.op == AlterOp::AddColumn) {
            if (cidx(a.add_col.name)) return ExecResult::failure("column '" + a.add_col.name + "' already exists");
            if (!a.add_col.nullable && !a.add_col.has_default && t->row_count > 0)
                return ExecResult::failure("ADD a NOT NULL column requires a DEFAULT on a non-empty table");
            t->columns.push_back(a.add_col);
            t->col_stats.assign(t->columns.size(), Table::ColStat{});
            persist_schema(a.table);
            return ExecResult{};
        }
        if (a.op == AlterOp::AddCheck) {
            // Validate every existing row passes before adopting the constraint.
            t->checks.push_back(a.check_src);
            if (auto e = validate_all_rows_against_checks(*t)) {
                t->checks.pop_back();
                return ExecResult::failure(*e);
            }
            add_named_constraint(*t, 0, 0, a.check_src, a.constraint_name);  // droppable by name
            persist_schema(a.table);
            return ExecResult{};
        }
        if (a.op == AlterOp::AddUnique) {
            const auto ci = cidx(a.unique_col);
            if (!ci) return ExecResult::failure("unknown column '" + a.unique_col + "'");
            if (auto e = check_unique_existing(*t, *ci)) return ExecResult::failure(*e);
            t->columns[*ci].unique = true;
            add_named_constraint(*t, 1, *ci, "", a.constraint_name);  // droppable by name
            persist_schema(a.table);
            return ExecResult{};
        }
        if (a.op == AlterOp::DropConstraint) {
            std::size_t found = t->constraints.size();
            for (std::size_t i = 0; i < t->constraints.size(); ++i)
                if (t->constraints[i].name == a.constraint_name) { found = i; break; }
            if (found == t->constraints.size())
                return ExecResult::failure("constraint '" + a.constraint_name + "' does not exist on '" +
                                           a.table + "'");
            const Table::NamedConstraint nc = t->constraints[found];
            if (nc.kind == 0) {  // CHECK — remove the matching source from the enforced set
                for (std::size_t i = 0; i < t->checks.size(); ++i)
                    if (t->checks[i] == nc.check_src) { t->checks.erase(t->checks.begin() + i); break; }
            } else if (nc.kind == 1) {  // UNIQUE — clear the column flag
                if (nc.column < t->columns.size()) t->columns[nc.column].unique = false;
            } else if (nc.kind == 2) {  // FOREIGN KEY — clear the column's reference
                if (nc.column < t->columns.size()) {
                    t->columns[nc.column].fk_table.clear();
                    t->columns[nc.column].fk_column.clear();
                }
            }
            t->constraints.erase(t->constraints.begin() + static_cast<std::ptrdiff_t>(found));
            persist_schema(a.table);
            return ExecResult{};
        }
        const auto ci = cidx(a.col_name);
        if (!ci) return ExecResult::failure("unknown column '" + a.col_name + "' in table '" + a.table + "'");
        Column& col = t->columns[*ci];
        switch (a.op) {
            case AlterOp::RenameColumn:
                if (cidx(a.new_name)) return ExecResult::failure("column '" + a.new_name + "' already exists");
                col.name = a.new_name;
                break;
            case AlterOp::DropColumn:
                if (is_pk_col(*t, *ci)) return ExecResult::failure("cannot DROP a PRIMARY KEY column");
                col.dropped = true;
                break;
            case AlterOp::SetDefault: {
                Datum d;
                if (auto e = coerce(col, a.default_val, d)) return ExecResult::failure(*e);
                col.has_default = true;
                if (col.type == Type::Int) col.default_i = d.i;
                else col.default_s = d.s;
                break;
            }
            case AlterOp::DropDefault:
                col.has_default = false;
                break;
            case AlterOp::SetNotNull:
                if (auto e = check_no_nulls_existing(*t, *ci)) return ExecResult::failure(*e);
                col.nullable = false;
                break;
            case AlterOp::DropNotNull:
                if (is_pk_col(*t, *ci)) return ExecResult::failure("a PRIMARY KEY column is always NOT NULL");
                col.nullable = true;
                break;
            case AlterOp::DropUnique:
                col.unique = false;
                break;
            case AlterOp::AlterType:
                if (auto e = rewrite_column_type(*t, *ci, a.add_col)) return ExecResult::failure(*e);
                break;
            default:
                return ExecResult::failure("unsupported ALTER operation");
        }
        persist_schema(a.table);
        return ExecResult{};
    }

    // E1 helpers — all over the verified row scan/commit path.
    [[nodiscard]] std::optional<std::string> scan_all_rows(const Table& t,
                                                           std::vector<std::pair<Datum, std::vector<Datum>>>& out) {
        SelectStmt scan;
        scan.table = t.name;
        std::vector<std::vector<Datum>> rows;
        if (auto e = scan_table(t, scan, rows)) return e;
        for (auto& r : rows) { Datum pk = r[t.pk_index]; out.emplace_back(std::move(pk), std::move(r)); }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> check_no_nulls_existing(const Table& t, std::size_t ci) {
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(t, rows)) return e;
        for (const auto& [pk, r] : rows)
            if (r[ci].is_null) return "SET NOT NULL failed: column '" + t.columns[ci].name + "' has NULLs";
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> check_unique_existing(const Table& t, std::size_t ci) {
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(t, rows)) return e;
        std::set<std::string> seen;
        for (const auto& [pk, r] : rows)
            if (!r[ci].is_null && !seen.insert(group_key_field(r[ci])).second)
                return "ADD UNIQUE failed: column '" + t.columns[ci].name + "' has duplicates";
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> validate_all_rows_against_checks(const Table& t) {
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(t, rows)) return e;
        for (const auto& [pk, r] : rows)
            if (auto e = eval_checks(t, r)) return "existing row violates the new CHECK: " + *e;
        return std::nullopt;
    }
    // ALTER COLUMN TYPE — read every row, CAST the column's value to the new type, rewrite the row.
    [[nodiscard]] std::optional<std::string> rewrite_column_type(Table& t, std::size_t ci,
                                                                 const Column& newty) {
        if (is_pk_col(t, ci)) return "ALTER TYPE on a PRIMARY KEY column is not supported";
        std::vector<std::pair<Datum, std::vector<Datum>>> rows;
        if (auto e = scan_all_rows(t, rows)) return e;
        Column target = t.columns[ci];  // keep name/constraints, swap the type
        target.type = newty.type;
        target.logical = newty.logical;
        target.scale = newty.scale;
        target.is_unsigned = newty.is_unsigned;
        target.enum_labels = newty.enum_labels;
        std::vector<std::pair<Key, Value>> writes;
        for (auto& [pk, r] : rows) {
            Datum casted;
            if (r[ci].is_null) {
                casted = Datum::make_null(target.type);
            } else {
                Datum bridge = r[ci];
                // Bridge across a base-type change: INT -> TEXT renders; TEXT -> plain INT parses;
                // TEXT -> a logical-INT type (DATE/TIME/...) keeps the text so coerce can parse it.
                if (target.type == Type::Text && bridge.type == Type::Int) {
                    bridge = Datum::make_text(r[ci].render());
                } else if (target.type == Type::Int && target.logical == 0 && bridge.type == Type::Text) {
                    bridge = Datum::make_int(std::strtoll(bridge.s.c_str(), nullptr, 10));
                }
                if (auto e = coerce(target, bridge, casted))
                    return "ALTER TYPE cast failed for column '" + target.name + "': " + *e;
            }
            r[ci] = casted;
            emit_row_writes(t, r, writes);
        }
        t.columns[ci] = target;
        commit_writes(writes);
        return std::nullopt;
    }

    // DROP TABLE <t> (F8): forget the table (catalog) + durably TOMBSTONE its schema record (so a
    // restart does not resurrect it). The row/columnar/index/zone DATA under the table's monotonic
    // id is left orphaned-but-invisible (no query can name a dropped table; a re-CREATE gets a NEW
    // id) — the same no-space-reclaim model DROP INDEX uses. Unknown table => error.
    ExecResult exec_drop_table(const DropTableStmt& dt, bool keep_matview = false) {
        if (catalog_.find(dt.table) == nullptr) {
            if (dt.if_exists) return ExecResult{};  // E2: no-op
            return ExecResult::failure("unknown table '" + dt.table + "'");
        }
        // Durably retire the schema record in the SEPARATE catalog store (its own Seq line).
        (void)commit_batch(catalog_db_, {{catalog_key(dt.table), tombstone_marker()}},
                           /*nosync=*/false);
        catalog_.remove(dt.table);
        // If this was a materialized view, forget its source too (unless a REFRESH is
        // re-materializing it under the same name, which keeps the registration).
        if (!keep_matview) {
            const std::string qn = catalog_.qualify(dt.table);
            if (matviews_.erase(qn) != 0) {
                retire_matview(dt.table);
            }
        }
        // Invalidate any decoded-block / zone caches that keyed on this table's flush_gen.
        chunk_cache_.clear();
        concat_cache_.clear();
        text_dict_cache_.clear();
        zone_cache_.clear();
        return ExecResult{};
    }

    // H1: CREATE [OR REPLACE] VIEW — register/overwrite a named SELECT and durably persist it. The
    // body already parsed at parse time; re-validate here (CREATE OR REPLACE may carry a body that
    // parses but references a missing table — that surfaces at query time, matching PostgreSQL's
    // late binding, so we do NOT resolve the body's tables here).
    ExecResult exec_create_view(const CreateViewStmt& c) {
        if (catalog_.find(c.name) != nullptr) {
            return ExecResult::failure("'" + c.name + "' is a table, not a view");
        }
        const bool exists = catalog_.find_view(c.name) != nullptr;
        if (exists && !c.or_replace) {
            if (c.if_not_exists) return ExecResult{};  // no-op
            return ExecResult::failure("view '" + c.name + "' already exists");
        }
        View v;
        v.name = c.name;
        v.select_src = c.select_src;
        v.columns = c.columns;
        if (c.or_replace) {
            if (!catalog_.replace_view(std::move(v)))
                return ExecResult::failure("'" + c.name + "' is a table, not a view");
        } else {
            if (!catalog_.create_view(std::move(v)))
                return ExecResult::failure("view '" + c.name + "' already exists");
        }
        persist_view(c.name);  // durable (survives a restart)
        return ExecResult{};
    }

    // H1: DROP VIEW [IF EXISTS] — forget the view + durably tombstone it.
    ExecResult exec_drop_view(const DropViewStmt& d) {
        if (catalog_.find_view(d.name) == nullptr) {
            if (d.if_exists) return ExecResult{};  // no-op
            return ExecResult::failure("unknown view '" + d.name + "'");
        }
        retire_view(d.name);
        catalog_.remove_view(d.name);
        return ExecResult{};
    }

    // Coerce a parsed literal Datum to a column's declared type (type checking).
    // INT<->TEXT mismatch is an error (no implicit conversion in v1).
    // `for_write` true == an INSERT/UPDATE value (enforce the length cap); false == a comparison
    // literal (F10: a too-long literal must mean NO MATCH, not an error — so the length cap is
    // skipped; CHAR padding still applies so it compares equal to the padded stored value).
    [[nodiscard]] static std::optional<std::string> coerce(const Column& col,
                                                           const Datum& in,
                                                           Datum& out,
                                                           bool for_write = true) {
        // v4: a NULL literal carries a placeholder type — re-type it to the column's
        // declared type and accept it iff the column is NULLABLE (fail-closed otherwise).
        if (in.is_null) {
            if (!col.nullable) {
                return std::string("NULL not allowed for NOT NULL column '") + col.name +
                       "'";
            }
            out = Datum::make_null(col.type);
            out.logical = col.logical;
            out.scale = col.scale;
            return std::nullopt;
        }
        // F9b: a DECIMAL/DATE/TIMESTAMP column (physical INT). Accept a string literal
        // ('3.14', '2026-06-27', '2026-06-27 13:45:00') parsed to its INT representation, or
        // a bare INT (for DECIMAL a whole number scaled by 10^scale; for DATE/TIMESTAMP a raw
        // days/secs escape hatch). Storage/keys/comparison stay INT → byte-deterministic.
        if (col.type == Type::Int && col.logical != 0) {
            std::int64_t v = 0;
            if (in.type == Type::Text) {
                bool ok = false;
                if (col.logical == 1) ok = parse_decimal(in.s, col.scale, v);
                else if (col.logical == 2) ok = parse_date(in.s, v);
                else if (col.logical == 3) ok = parse_timestamp(in.s, v);
                else if (col.logical == 8) ok = parse_time(in.s, v);          // F13 TIME
                else if (col.logical == 10) ok = parse_interval(in.s, v);     // F13 INTERVAL
                else if (col.logical == 9) {                                  // F13 ENUM label -> ordinal
                    for (std::size_t k = 0; k < col.enum_labels.size(); ++k)
                        if (col.enum_labels[k] == in.s) { v = static_cast<std::int64_t>(k); ok = true; break; }
                }
                if (!ok) {
                    return std::string("invalid ") + logical_name(col.logical) +
                           " literal '" + in.s + "' for column '" + col.name + "'";
                }
            } else if (in.type == Type::Int) {
                if (col.logical == 1) {
                    std::int64_t pow = 1;
                    for (std::uint8_t k = 0; k < col.scale; ++k) pow *= 10;
                    if (mul_ovf(in.i, pow, v)) {  // F9d: whole-number literal scaled to fixed-point
                        return std::string("DECIMAL value out of range for column '") + col.name + "'";
                    }
                } else {
                    v = in.i;  // raw days/secs
                }
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected " +
                       logical_name(col.logical) + ", got " + type_name(in.type);
            }
            if (col.logical == 9) {  // F13 ENUM: range-check the ordinal + tag its label for render
                if (v < 0 || v >= static_cast<std::int64_t>(col.enum_labels.size()))
                    return std::string("ENUM ordinal out of range for column '") + col.name + "'";
            }
            out = Datum::make_int(v);
            out.logical = col.logical;
            out.scale = col.scale;
            if (col.logical == 9) out.s = col.enum_labels[static_cast<std::size_t>(v)];
            return check_domain(col, out, for_write);
        }
        // F14: a REAL column (logical 14, physical TEXT). Accept a bare INT (widened), a numeric
        // string literal ('3.14', '-2e3'), or another REAL — store the IEEE-754 double payload.
        if (col.type == Type::Text && col.logical == 14) {
            double d = 0.0;
            if (in.is_real()) {
                d = in.real_value();
            } else if (in.type == Type::Int) {
                d = static_cast<double>(in.i);
            } else if (in.type == Type::Text) {
                if (!parse_double_strict(in.s.data(), in.s.data() + in.s.size(), d)) {
                    return std::string("invalid REAL literal '") + in.s + "' for column '" + col.name + "'";
                }
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected REAL";
            }
            out = Datum::make_real(d);
            return check_domain(col, out, for_write);
        }
        // F13: a JSON column (logical 11, physical TEXT). Parse + store the CANONICAL form (sorted
        // keys, compact, normalized numbers) so equal documents are byte-identical.
        if (col.type == Type::Text && col.logical == 11) {
            if (in.type != Type::Text) {
                return std::string("type mismatch for column '") + col.name + "': expected JSON";
            }
            std::string canon;
            if (!json::canonical(in.s, canon)) {
                return std::string("invalid JSON for column '") + col.name + "'";
            }
            out = Datum::make_text(std::move(canon));
            out.logical = 11;
            return std::nullopt;
        }
        // F9c: a UUID column (physical TEXT). Validate + canonicalise (lowercase, dashed) the literal.
        if (col.type == Type::Text && col.logical == 4) {
            if (in.type != Type::Text) {
                return std::string("type mismatch for column '") + col.name +
                       "': expected UUID, got " + type_name(in.type);
            }
            std::string canon;
            if (!parse_uuid(in.s, canon)) {
                return std::string("invalid UUID literal '") + in.s + "' for column '" + col.name + "'";
            }
            out = Datum::make_text(canon);
            out.logical = 4;
            return std::nullopt;
        }
        // K1: a VECTOR(n) column (logical 15, physical TEXT; the payload is the ARRAY codec with
        // REAL elements). Accept an ARRAY[...] / VECTOR Datum, a '{...}' array text, or pgvector's
        // '[x,y,z]' text form. Every element must be numeric and non-NULL; a declared dimension
        // (col.max_len != 0) is enforced. The stored payload is canonical (every element a REAL).
        if (col.type == Type::Text && col.logical == 15) {
            std::vector<Datum> elems;
            if (in.type == Type::Text && (in.logical == 7 || in.logical == 15)) {
                elems = Datum::decode_array(in.s);
            } else if (in.type == Type::Text && in.logical == 0) {
                if (auto e = parse_vector_literal(in.s, elems)) return e;
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected VECTOR";
            }
            if (elems.empty()) {
                return std::string("VECTOR column '") + col.name + "' cannot hold an empty vector";
            }
            if (col.max_len != 0 && elems.size() != col.max_len) {
                return std::string("VECTOR dimension mismatch for column '") + col.name +
                       "': expected " + std::to_string(col.max_len) + ", got " +
                       std::to_string(elems.size());
            }
            std::vector<Datum> coerced;
            coerced.reserve(elems.size());
            for (const Datum& e : elems) {
                if (e.is_null) {
                    return std::string("VECTOR column '") + col.name + "' cannot hold a NULL element";
                }
                double d = 0.0;
                if (e.is_real()) {
                    d = e.real_value();
                } else if (e.type == Type::Int && e.logical == 0) {
                    d = static_cast<double>(e.i);
                } else if (e.type == Type::Text && e.logical == 0) {
                    if (!parse_double_strict(e.s.data(), e.s.data() + e.s.size(), d)) {
                        return std::string("invalid VECTOR element '") + e.s + "' for column '" +
                               col.name + "'";
                    }
                } else {
                    return std::string("VECTOR column '") + col.name +
                           "' requires numeric elements";
                }
                coerced.push_back(Datum::make_real(d));
            }
            out = Datum::make_text(Datum::encode_array(/*elem_logical=*/14, /*elem_scale=*/0, coerced));
            out.logical = 15;
            return std::nullopt;
        }
        // F12: an ARRAY column (logical 7, physical TEXT). Accept an already-built array Datum (from
        // an ARRAY[...] expression) or a '{...}' text literal; re-encode each element coerced to the
        // column's element type so the stored payload is canonical.
        if (col.type == Type::Text && col.logical == 7) {
            std::vector<Datum> elems;
            if (in.logical == 7 && in.type == Type::Text) {
                elems = Datum::decode_array(in.s);  // already an array value
            } else if (in.type == Type::Text) {
                if (auto e = parse_array_literal(in.s, elems)) return e;  // '{...}' text
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected ARRAY";
            }
            Column elem_col;  // a synthetic scalar column for per-element coercion
            elem_col.name = col.name;
            elem_col.type = col.elem_type;
            elem_col.logical = col.elem_logical;
            elem_col.scale = col.elem_scale;
            elem_col.nullable = true;
            std::vector<Datum> coerced;
            coerced.reserve(elems.size());
            for (const Datum& e : elems) {
                Datum ce;
                if (auto err = coerce(elem_col, e, ce, for_write)) return err;
                coerced.push_back(std::move(ce));
            }
            out = Datum::make_text(Datum::encode_array(col.elem_logical, col.elem_scale, coerced));
            out.logical = 7;
            return std::nullopt;
        }
        // F9e: an INT128 (logical 5) / DECIMAL128 (logical 6) column (physical TEXT, 16-byte payload).
        // Accept a decimal string ('123...' / '12.34'), or a bare INT widened to 128-bit.
        if (col.type == Type::Text && (col.logical == 5 || col.logical == 6)) {
            __int128 v = 0;
            if (in.type == Type::Text) {
                const bool ok = col.logical == 5 ? parse_i128(in.s, v)
                                                 : parse_decimal128(in.s, col.scale, v);
                if (!ok) {
                    return std::string("invalid ") + logical_name(col.logical) + " literal '" +
                           in.s + "' for column '" + col.name + "'";
                }
            } else if (in.type == Type::Int) {
                v = static_cast<__int128>(in.i);
                if (col.logical == 6) {  // whole-number literal scaled to fixed-point
                    for (std::uint8_t k = 0; k < col.scale; ++k) v *= 10;
                }
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected " +
                       logical_name(col.logical) + ", got " + type_name(in.type);
            }
            out = Datum::make_text(Datum::encode_i128(v));
            out.logical = col.logical;
            out.scale = col.scale;
            return check_domain(col, out, for_write);
        }
        // UINT256 (logical 13) — a 256-bit UNSIGNED column (physical TEXT, 32-byte payload). Accept an
        // unsigned decimal string or a NON-NEGATIVE INT widened to 256-bit.
        if (col.type == Type::Text && col.logical == 13) {
            u256 v{};
            if (in.type == Type::Text) {
                if (!u256_from_dec(in.s, v))
                    return std::string("invalid UINT256 literal '") + in.s + "' for column '" +
                           col.name + "' (must be an unsigned integer in [0, 2^256-1])";
            } else if (in.type == Type::Int) {
                if (in.i < 0)
                    return std::string("UINT256 column '") + col.name + "' cannot hold a negative value";
                v = u256_from_u64(static_cast<std::uint64_t>(in.i));
            } else {
                return std::string("type mismatch for column '") + col.name + "': expected UINT256, got " +
                       type_name(in.type);
            }
            out = Datum::make_text(u256_encode(v));
            out.logical = 13;
            return std::nullopt;
        }
        if (col.type != in.type) {
            return std::string("type mismatch for column '") + col.name +
                   "': expected " + type_name(col.type) + ", got " +
                   type_name(in.type);
        }
        out = in;
        return check_domain(col, out, for_write);
    }

    // F10: enforce a column's DOMAIN constraints on a coerced value (deterministic). Validates the
    // UNSIGNED sign, the integer-width range (TINYINT/SMALLINT/INT32), the DECIMAL precision, and the
    // VARCHAR/CHAR/BLOB length; CHAR right-pads to its length. `d` may be mutated (the CHAR pad).
    static std::optional<std::string> check_domain(const Column& col, Datum& d,
                                                    bool enforce_len = true) {
        if (d.is_null) return std::nullopt;
        // Numeric INT-backed (not DATE/TIMESTAMP — sign/width are meaningless there).
        if (d.type == Type::Int && col.logical != 2 && col.logical != 3) {
            if (col.is_unsigned && d.i < 0)
                return "value must be >= 0 (UNSIGNED column '" + col.name + "')";
            if (col.int_bits != 0 && col.logical == 0) {
                std::int64_t lo = 0, hi = 0;
                if (col.is_unsigned) { hi = (static_cast<std::int64_t>(1) << col.int_bits) - 1; }
                else { hi = (static_cast<std::int64_t>(1) << (col.int_bits - 1)) - 1; lo = -hi - 1; }
                if (d.i < lo || d.i > hi)
                    return "value " + std::to_string(d.i) + " out of range for column '" + col.name + "'";
            }
            if (col.logical == 1 && col.precision != 0) {  // DECIMAL64 precision
                std::int64_t pow = 1;
                bool ovf = false;
                for (std::uint8_t k = 0; k < col.precision; ++k)
                    if (mul_ovf(pow, 10, pow)) { ovf = true; break; }
                const std::uint64_t mag = d.i < 0 ? static_cast<std::uint64_t>(-(d.i + 1)) + 1
                                                  : static_cast<std::uint64_t>(d.i);
                if (!ovf && mag >= static_cast<std::uint64_t>(pow))
                    return "value exceeds DECIMAL(" + std::to_string(col.precision) + ") precision for column '" + col.name + "'";
            }
        }
        // 128-bit (INT128/DECIMAL128) — value is the 16-byte payload in s.
        if (d.type == Type::Text && (col.logical == 5 || col.logical == 6)) {
            const __int128 v = Datum::decode_i128(d.s);
            if (col.is_unsigned && v < 0)
                return "value must be >= 0 (UNSIGNED column '" + col.name + "')";
            if (col.logical == 6 && col.precision != 0) {
                __int128 pow = 1;
                for (std::uint8_t k = 0; k < col.precision; ++k) pow *= 10;  // precision<=38 fits int128
                const unsigned __int128 mag = Datum::abs_u128(v);
                if (mag >= static_cast<unsigned __int128>(pow))
                    return "value exceeds DECIMAL(" + std::to_string(col.precision) + ") precision for column '" + col.name + "'";
            }
        }
        // Plain TEXT / VARCHAR(n) / CHAR(n) / BLOB(n).
        if (d.type == Type::Text && col.logical < 5) {
            if (enforce_len && col.max_len != 0 && d.s.size() > col.max_len)
                return "value too long for column '" + col.name + "' (max " + std::to_string(col.max_len) + ")";
            if (col.fixed_char && col.max_len != 0 && d.s.size() < col.max_len)
                d.s.append(col.max_len - d.s.size(), ' ');  // CHAR right-pad
        }
        return std::nullopt;
    }

    // F12: parse a '{a,b,c}' array text literal into raw element Datums (numeric token -> INT, NULL ->
    // null, else TEXT with optional surrounding quotes stripped). Top-level only (no nested arrays).
    static std::optional<std::string> parse_array_literal(const std::string& in,
                                                          std::vector<Datum>& out) {
        std::size_t p = 0;
        while (p < in.size() && (in[p] == ' ' || in[p] == '\t')) ++p;
        if (p >= in.size() || in[p] != '{') return std::string("array literal must start with '{'");
        ++p;
        auto trim = [](std::string x) {
            std::size_t a = 0, b = x.size();
            while (a < b && (x[a] == ' ' || x[a] == '\t')) ++a;
            while (b > a && (x[b - 1] == ' ' || x[b - 1] == '\t')) --b;
            return x.substr(a, b - a);
        };
        std::string cur;
        bool closed = false;
        auto flush = [&]() {
            std::string tok = trim(cur);
            cur.clear();
            if (tok.empty()) return;  // tolerate trailing/empty (e.g. '{}')
            if (tok == "NULL" || tok == "null") { out.push_back(Datum::make_null(Type::Int)); return; }
            if (tok.size() >= 2 && (tok.front() == '\'' || tok.front() == '"') && tok.back() == tok.front()) {
                out.push_back(Datum::make_text(tok.substr(1, tok.size() - 2)));
                return;
            }
            bool numeric = !tok.empty();
            for (std::size_t k = (tok[0] == '-' || tok[0] == '+') ? 1 : 0; k < tok.size(); ++k)
                if (tok[k] < '0' || tok[k] > '9') { numeric = false; break; }
            if (numeric && tok != "-" && tok != "+")
                out.push_back(Datum::make_int(std::strtoll(tok.c_str(), nullptr, 10)));
            else
                out.push_back(Datum::make_text(tok));
        };
        for (; p < in.size(); ++p) {
            if (in[p] == '}') { closed = true; ++p; break; }
            if (in[p] == ',') { flush(); continue; }
            cur.push_back(in[p]);
        }
        if (!closed) return std::string("array literal missing closing '}'");
        flush();
        return std::nullopt;
    }

    // K1: parse a pgvector-style '[1,2,3]' (or '{1,2,3}') vector text literal into REAL Datums.
    // Every element must be a plain numeric token (locale-free from_chars); no NULLs, no nesting.
    static std::optional<std::string> parse_vector_literal(const std::string& in,
                                                           std::vector<Datum>& out) {
        std::size_t p = 0;
        while (p < in.size() && (in[p] == ' ' || in[p] == '\t')) ++p;
        if (p >= in.size() || (in[p] != '[' && in[p] != '{')) {
            return std::string("vector literal must start with '[' or '{'");
        }
        const char close = in[p] == '[' ? ']' : '}';
        ++p;
        std::string cur;
        bool closed = false;
        auto flush = [&]() -> bool {
            std::size_t a = 0, b = cur.size();
            while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
            while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
            const std::string tok = cur.substr(a, b - a);
            cur.clear();
            if (tok.empty()) return false;
            double d = 0.0;
            if (!parse_double_strict(tok.data(), tok.data() + tok.size(), d)) return false;
            out.push_back(Datum::make_real(d));
            return true;
        };
        for (; p < in.size(); ++p) {
            if (in[p] == close) { closed = true; ++p; break; }
            if (in[p] == ',') {
                if (!flush()) return std::string("invalid vector element in '") + in + "'";
                continue;
            }
            cur.push_back(in[p]);
        }
        if (!closed) return std::string("vector literal missing closing '") + close + "'";
        // The final element (empty only for the '[]' empty-vector form, rejected by the caller).
        std::size_t a = 0;
        while (a < cur.size() && (cur[a] == ' ' || cur[a] == '\t')) ++a;
        if (a < cur.size() && !flush()) return std::string("invalid vector element in '") + in + "'";
        return std::nullopt;
    }

    // --- K1.3: IVFFLAT helpers (all pure + deterministic — no rng, no wall-clock). -----------
    // K1 perf: walk an ARRAY-codec payload of REAL (and/or INT) elements STRAIGHT into
    // doubles — no per-element Datum/string materialisation (the generic decode_array
    // allocates a string per element; a probe scores thousands of candidates and the
    // brute path runs this per row). Bit-identical values. Returns false on any other
    // element shape (NULL / TEXT-ish) — the caller falls back to the generic decode.
    static bool ivf_doubles_fast(const std::string& s, std::vector<double>& out) {
        out.clear();
        if (s.size() < 6) return false;
        const std::uint8_t el = static_cast<unsigned char>(s[0]);
        const std::uint32_t n = Datum::get_be32_(s, 2);
        out.reserve(n);
        // All-REAL fixed-stride fast path (13 bytes/element; LE memcpy — same value).
        if (el == 14 && s.size() == 6 + static_cast<std::size_t>(n) * 13) {
            const char* p = s.data() + 6;
            bool ok = true;
            for (std::uint32_t k = 0; k < n; ++k, p += 13) {
                if (static_cast<unsigned char>(p[0]) != 2 || p[1] != 0 || p[2] != 0 ||
                    p[3] != 0 || static_cast<unsigned char>(p[4]) != 8) {
                    ok = false;
                    break;
                }
                double d = 0.0;
                if constexpr (std::endian::native == std::endian::little) {
                    std::memcpy(&d, p + 5, sizeof(d));
                } else {
                    std::uint64_t bits = 0;
                    for (int b = 0; b < 8; ++b)
                        bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[5 + b]))
                                << (8 * b);
                    std::memcpy(&d, &bits, sizeof(d));
                }
                out.push_back(d);
            }
            if (ok) return true;
            out.clear();
        }
        std::size_t off = 6;
        for (std::uint32_t k = 0; k < n; ++k) {
            if (off >= s.size()) return false;
            const std::uint8_t tag = static_cast<unsigned char>(s[off++]);
            if (tag == 1) {  // INT element: 8-byte big-endian int64
                if (off + 8 > s.size()) return false;
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b)
                    bits = (bits << 8) | static_cast<unsigned char>(s[off++]);
                out.push_back(static_cast<double>(static_cast<std::int64_t>(bits)));
            } else if (tag == 2 && el == 14) {  // REAL element: [len be32][8 LE payload bytes]
                if (off + 4 > s.size()) return false;
                const std::uint32_t len = Datum::get_be32_(s, off);
                off += 4;
                if (len != 8 || off + 8 > s.size()) return false;
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b)
                    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[off + static_cast<std::size_t>(b)]))
                            << (8 * b);
                off += 8;
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                out.push_back(d);
            } else {
                return false;
            }
        }
        return true;
    }
    // K1 perf: score ONE all-REAL ARRAY-codec payload against the query WITHOUT
    // materialising a doubles vector (the probe runs this per candidate). The element
    // order and the dot/sq/nu/nv accumulation are the kernel's — bit-identical distance.
    // Returns false (caller falls back to the generic path) on any other payload shape
    // or a dimension mismatch.
    // K1 perf: THE shared distance accumulator — FOUR independent partial sums per
    // quantity break the serial dot/sq/nu/nv FP dependency chain (the dominant probe
    // cost in the clean profile) and let the compiler vectorise the lanes. The lane
    // count and the final combine order are FIXED ((l0+l1)+(l2+l3), tail element k into
    // lane k&3) — no platform-width SIMD — so the value is bit-deterministic on every
    // host. EVERY distance consumer (the exact scalar kernel, the ivfflat probe, HNSW)
    // uses this one function, so index == scan equality holds by construction. NOTE:
    // the association differs from the old sequential kernel, so absolute values can
    // differ in low bits — cross-replica byte-identity is unaffected (one binary).
    template <class GetX>
    static void vec_accum4(std::size_t n, const std::vector<double>& q, GetX&& get_x,
                           double& dot, double& sq, double& nu, double& nv) {
        double d[4] = {0, 0, 0, 0}, s[4] = {0, 0, 0, 0}, a[4] = {0, 0, 0, 0},
               b[4] = {0, 0, 0, 0};
        std::size_t k = 0;
        for (; k + 4 <= n; k += 4) {
            for (std::size_t l = 0; l < 4; ++l) {
                const double x = get_x(k + l), y = q[k + l];
                d[l] += x * y;
                s[l] += (x - y) * (x - y);
                a[l] += x * x;
                b[l] += y * y;
            }
        }
        for (; k < n; ++k) {
            const std::size_t l = k & 3;
            const double x = get_x(k), y = q[k];
            d[l] += x * y;
            s[l] += (x - y) * (x - y);
            a[l] += x * x;
            b[l] += y * y;
        }
        dot = (d[0] + d[1]) + (d[2] + d[3]);
        sq = (s[0] + s[1]) + (s[2] + s[3]);
        nu = (a[0] + a[1]) + (a[2] + a[3]);
        nv = (b[0] + b[1]) + (b[2] + b[3]);
    }
    // K1 perf: op-specialised accumulators for the HOT probe path — the SAME lane
    // structure and combine order as vec_accum4, restricted to the quantities the op
    // needs (L2 uses only sq — 3 flops/element instead of 12; cosine needs dot+nu with
    // the query norm nv precomputed ONCE per query; IP needs dot). The lanes are
    // independent variables in vec_accum4, so each restricted sum is BIT-EQUAL to the
    // corresponding full-kernel sum — the exact path can keep the full kernel and
    // index == scan equality still holds value-for-value.
    template <class GetX>
    static double vec_sq4(std::size_t n, const std::vector<double>& q, GetX&& get_x) {
        double s[4] = {0, 0, 0, 0};
        std::size_t k = 0;
        for (; k + 4 <= n; k += 4)
            for (std::size_t l = 0; l < 4; ++l) {
                const double x = get_x(k + l), y = q[k + l];
                s[l] += (x - y) * (x - y);
            }
        for (; k < n; ++k) {
            const double x = get_x(k), y = q[k];
            s[k & 3] += (x - y) * (x - y);
        }
        return (s[0] + s[1]) + (s[2] + s[3]);
    }
    template <class GetX>
    static void vec_dot_nu4(std::size_t n, const std::vector<double>& q, GetX&& get_x,
                            double& dot, double& nu) {
        double d[4] = {0, 0, 0, 0}, a[4] = {0, 0, 0, 0};
        std::size_t k = 0;
        for (; k + 4 <= n; k += 4)
            for (std::size_t l = 0; l < 4; ++l) {
                const double x = get_x(k + l), y = q[k + l];
                d[l] += x * y;
                a[l] += x * x;
            }
        for (; k < n; ++k) {
            const double x = get_x(k), y = q[k];
            d[k & 3] += x * y;
            a[k & 3] += x * x;
        }
        dot = (d[0] + d[1]) + (d[2] + d[3]);
        nu = (a[0] + a[1]) + (a[2] + a[3]);
    }
    // The query's own norm accumulator (nv lanes of vec_accum4) — once per query.
    static double vec_nv4(const std::vector<double>& q) {
        double b[4] = {0, 0, 0, 0};
        std::size_t k = 0;
        for (; k + 4 <= q.size(); k += 4)
            for (std::size_t l = 0; l < 4; ++l) b[l] += q[k + l] * q[k + l];
        for (; k < q.size(); ++k) b[k & 3] += q[k] * q[k];
        return (b[0] + b[1]) + (b[2] + b[3]);
    }
    template <class GetX>
    static double vec_dot4(std::size_t n, const std::vector<double>& q, GetX&& get_x) {
        double d[4] = {0, 0, 0, 0};
        std::size_t k = 0;
        for (; k + 4 <= n; k += 4)
            for (std::size_t l = 0; l < 4; ++l) d[l] += get_x(k + l) * q[k + l];
        for (; k < n; ++k) d[k & 3] += get_x(k) * q[k];
        return (d[0] + d[1]) + (d[2] + d[3]);
    }

    // Finish a distance from the accumulated quantities (op: 0 L2, 1 cosine, 2 -dot).
    static double vec_finish(std::uint8_t op, double dot, double sq, double nu, double nv) {
        if (op == 0) return std::sqrt(sq);
        if (op == 1) {
            const double denom = std::sqrt(nu) * std::sqrt(nv);
            return denom == 0.0 ? 1.0 : 1.0 - dot / denom;
        }
        return -dot;
    }

    // An all-REAL payload has a FIXED 13-byte element stride ([tag 1][len be32 = 8][8 LE
    // payload bytes]) — one up-front size check replaces the per-element bounds checks,
    // and on a little-endian host the stored LE bit pattern memcpys straight into the
    // double (identical value; the portable byte-assembly stays for big-endian).
    static bool ivf_score_fast(const std::string& s, const std::vector<double>& q,
                               std::uint8_t op, double& dist) {
        if (s.size() < 6 || static_cast<unsigned char>(s[0]) != 14) return false;
        const std::uint32_t n = Datum::get_be32_(s, 2);
        if (n != q.size()) return false;
        if (s.size() != 6 + static_cast<std::size_t>(n) * 13) return false;  // not all-REAL
        // Validate the element headers first (cheap byte compares), then accumulate over
        // the fixed stride with no per-element branching.
        const char* base = s.data() + 6;
        for (std::uint32_t k = 0; k < n; ++k) {
            const char* p = base + static_cast<std::size_t>(k) * 13;
            if (static_cast<unsigned char>(p[0]) != 2 || p[1] != 0 || p[2] != 0 || p[3] != 0 ||
                static_cast<unsigned char>(p[4]) != 8) {
                return false;
            }
        }
        const auto get_x = [base](std::size_t k) {
            double x = 0.0;
            if constexpr (std::endian::native == std::endian::little) {
                std::memcpy(&x, base + k * 13 + 5, sizeof(x));
            } else {
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b)
                    bits |= static_cast<std::uint64_t>(
                                static_cast<unsigned char>(base[k * 13 + 5 + static_cast<std::size_t>(b)]))
                            << (8 * b);
                std::memcpy(&x, &bits, sizeof(x));
            }
            return x;
        };
        double dot = 0.0, sq = 0.0, nu = 0.0, nv = 0.0;
        vec_accum4(n, q, get_x, dot, sq, nu, nv);
        dist = vec_finish(op, dot, sq, nu, nv);
        return true;
    }
    // Decode a VECTOR payload (the ARRAY codec with REAL elements) into raw doubles.
    static std::vector<double> ivf_payload_doubles(const std::string& payload) {
        std::vector<double> v;
        if (ivf_doubles_fast(payload, v)) return v;
        v.clear();
        const std::vector<Datum> elems = Datum::decode_array(payload);
        v.reserve(elems.size());
        for (const Datum& d : elems)
            v.push_back(d.is_real() ? d.real_value() : static_cast<double>(d.i));
        return v;
    }
    // Centroid blob codec: [dim:be32][k:be32] then k*dim little-endian 8-byte doubles.
    static std::string ivf_encode_centroids(std::size_t dim,
                                            const std::vector<std::vector<double>>& cents) {
        std::string o;
        Datum::put_be32_(o, static_cast<std::uint32_t>(dim));
        Datum::put_be32_(o, static_cast<std::uint32_t>(cents.size()));
        for (const auto& c : cents)
            for (std::size_t d = 0; d < dim; ++d) o += Datum::encode_double(d < c.size() ? c[d] : 0.0);
        return o;
    }
    static void ivf_decode_centroids(const std::string& s, std::size_t& dim,
                                     std::vector<std::vector<double>>& cents) {
        cents.clear();
        dim = 0;
        if (s.size() < 8) return;
        dim = Datum::get_be32_(s, 0);
        const std::size_t k = Datum::get_be32_(s, 4);
        if (dim == 0 || s.size() < 8 + k * dim * 8) return;
        cents.resize(k, std::vector<double>(dim, 0.0));
        std::size_t off = 8;
        for (std::size_t c = 0; c < k; ++c)
            for (std::size_t d = 0; d < dim; ++d, off += 8)
                cents[c][d] = Datum::decode_double(s.substr(off, 8));
    }
    // K1.3c: normalize to a unit vector for the cosine/IP opclasses (direction space). A
    // zero-magnitude vector stays zero (it lands in whichever list is nearest — deterministic).
    static std::vector<double> ivf_normalize(std::vector<double> v) {
        double n2 = 0.0;
        for (const double x : v) n2 += x * x;
        const double n = std::sqrt(n2);
        if (n != 0.0)
            for (double& x : v) x /= n;
        return v;
    }
    // The vector used for CLUSTERING/ASSIGNMENT under an opclass (raw for L2, direction for
    // cosine/IP). Candidate RANKING always uses the opclass's exact distance over raw payloads.
    static std::vector<double> ivf_assign_vec(std::uint8_t vec_op, std::vector<double> v) {
        return vec_op == 0 ? v : ivf_normalize(std::move(v));
    }
    // Nearest centroid by squared L2; a tie breaks to the LOWEST index (deterministic).
    static std::size_t ivf_nearest(const std::vector<std::vector<double>>& cents,
                                   const std::vector<double>& v) {
        std::size_t best = 0;
        double bd = 0.0;
        bool first = true;
        for (std::size_t c = 0; c < cents.size(); ++c) {
            double d2 = 0.0;
            const std::size_t n = std::min(cents[c].size(), v.size());
            for (std::size_t d = 0; d < n; ++d) {
                const double x = cents[c][d] - v[d];
                d2 += x * x;
            }
            if (first || d2 < bd) { bd = d2; best = c; first = false; }
        }
        return best;
    }
    // Deterministic k-means: seed centroids evenly over the PK-ordered input, then a FIXED
    // number of Lloyd iterations (assignment ties -> lowest centroid; an empty cluster keeps
    // its previous centroid). Same input order on every replica => byte-identical centroids.
    static std::vector<std::vector<double>> ivf_kmeans(
        const std::vector<std::vector<double>>& vecs, std::size_t k, std::size_t dim) {
        std::vector<std::vector<double>> cents;
        if (vecs.empty() || k == 0) {
            cents.assign(std::max<std::size_t>(1, k), std::vector<double>(dim, 0.0));
            return cents;
        }
        cents.reserve(k);
        for (std::size_t i = 0; i < k; ++i) cents.push_back(vecs[(i * vecs.size()) / k]);
        constexpr int kIters = 5;
        for (int iter = 0; iter < kIters; ++iter) {
            std::vector<std::vector<double>> sum(k, std::vector<double>(dim, 0.0));
            std::vector<std::size_t> cnt(k, 0);
            for (const auto& v : vecs) {
                const std::size_t c = ivf_nearest(cents, v);
                for (std::size_t d = 0; d < dim && d < v.size(); ++d) sum[c][d] += v[d];
                ++cnt[c];
            }
            for (std::size_t c = 0; c < k; ++c)
                if (cnt[c] != 0)
                    for (std::size_t d = 0; d < dim; ++d)
                        cents[c][d] = sum[c][d] / static_cast<double>(cnt[c]);
        }
        return cents;
    }
    // Does this scalar expression reference any column? (A constant query vector must not.)
    // A CASE is conservatively treated as column-dependent (its WHEN predicates are not walked).
    static bool expr_has_col(const Expr& e) {
        if (e.kind == ExprKind::Col || e.kind == ExprKind::Case) return true;
        if (e.left && expr_has_col(*e.left)) return true;
        if (e.right && expr_has_col(*e.right)) return true;
        for (const auto& a : e.args) if (a && expr_has_col(*a)) return true;
        return false;
    }

    static const char* logical_name(std::uint8_t lg) {
        switch (lg) {
            case 1: return "DECIMAL";
            case 2: return "DATE";
            case 3: return "TIMESTAMP";
            case 4: return "UUID";
            case 5: return "INT128";
            case 6: return "DECIMAL128";
            case 8: return "TIME";
            case 9: return "ENUM";
            case 10: return "INTERVAL";
            case 14: return "REAL";    // F14
            case 15: return "VECTOR";  // K1
            default: return "INT";
        }
    }

    // Append the index-entry writes for `row` (one per secondary index) to `out`.
    // `make` decides the value: a new entry (empty value) on INSERT/UPDATE-new, or a
    // tombstone on DELETE/UPDATE-old. The PK is read from row[pk_index].
    // I5: a partial index only contains rows satisfying its predicate (re-parsed; like a CHECK). An
    // empty predicate = a full index (always true).
    [[nodiscard]] bool row_matches_partial(const Table& t, const Index& ix,
                                           const std::vector<Datum>& row) {
        if (ix.partial_src.empty()) return true;
        const ParseResult pr = parse_sql("SELECT * FROM " + t.name + " WHERE " + ix.partial_src);
        if (!pr.ok()) return true;  // defensive: a broken predicate indexes everything (validated at create)
        bool truth = false;
        if (eval_pred(pr.stmt().select.filter, pr.stmt().select.filter.root, t, row,
                      /*group=*/nullptr, truth))
            return true;
        return truth;
    }

    // J2: re-parse an expression index's stored source into an Expr tree (validated at CREATE; this
    // re-parse mirrors the partial-index / CHECK pattern). The source is the bare scalar expression,
    // so it is parsed as the single projection of a SELECT over the index's table.
    [[nodiscard]] std::optional<std::string> parse_index_expr(const Table& t, const std::string& src,
                                                              std::shared_ptr<Expr>& out) {
        const ParseResult pr = parse_sql("SELECT " + src + " FROM " + t.name);
        if (!pr.ok()) return "invalid expression index '" + src + "': " + pr.error().render();
        const SelectStmt& s = pr.stmt().select;
        if (s.items.size() != 1) {
            return "expression index must be a single scalar expression";
        }
        const SelectItem& it = s.items[0];
        if (it.kind == SelectItemKind::Expr && it.expr) {
            out = it.expr;
            return std::nullopt;
        }
        if (it.kind == SelectItemKind::Column) {  // a bare column wrapped in (( )) — degenerate but legal
            auto c = std::make_shared<Expr>();
            c->kind = ExprKind::Col;
            c->qualifier = it.qualifier;
            c->column = it.column;
            out = c;
            return std::nullopt;
        }
        return "expression index must be a scalar expression (no aggregate / window)";
    }

    // J2: the physical type (Int / Text) an index expression yields — needed by the entry codec to
    // know its leading token's width. Mirrors eval_func's result types structurally (the only two
    // physical types are Int and Text; logical tags like DECIMAL/DATE are Int-backed). Used only to
    // seed an EMPTY-table index; once rows exist every write re-validates against the live value.
    [[nodiscard]] Type infer_index_expr_type(const Expr& e, const Table& t) {
        switch (e.kind) {
            case ExprKind::Lit:
                return e.lit.type;
            case ExprKind::Col: {
                const auto idx = t.column_index(e.column);
                return idx ? t.columns[*idx].type : Type::Int;
            }
            case ExprKind::Neg:
            case ExprKind::Bin:
                return Type::Int;  // all arithmetic results are physically Int (DECIMAL/temporal too)
            case ExprKind::Array:
                return Type::Text;  // an array payload is Text-backed (logical 7)
            case ExprKind::Subscript:
                return e.left ? Type::Int : Type::Int;  // element type unknown statically; live value rules
            case ExprKind::Case: {
                if (!e.case_then.empty() && e.case_then[0]) return infer_index_expr_type(*e.case_then[0], t);
                if (e.case_else) return infer_index_expr_type(*e.case_else, t);
                return Type::Int;
            }
            case ExprKind::Func: {
                const std::string& f = e.func;
                if (f == "CAST") return e.cast_type;
                if (f == "COALESCE" && !e.args.empty() && e.args[0])
                    return infer_index_expr_type(*e.args[0], t);
                if (f == "UPPER" || f == "LOWER" || f == "SUBSTR" || f == "SUBSTRING" ||
                    f == "CONCAT" || f == "TRIM" || f == "REPLACE" || f == "->" || f == "->>")
                    return Type::Text;
                if (f == "ARRAY_APPEND" || f == "ARRAY_CAT") return Type::Text;  // array payload
                return Type::Int;  // LENGTH/ABS/*_LENGTH/CARDINALITY/ARRAY_CONTAINS/ARRAY_POSITION/...
            }
        }
        return Type::Int;
    }

    // J2: evaluate an expression index's value for `row`. `skip` is set when the row gets NO entry
    // (NULL result). Returns an error string only when the expression cannot be evaluated for the row
    // (e.g. division by zero) or its physical type drifts from the recorded expr_type — both are
    // soundness violations the WRITE path rejects, so the index never diverges from a full scan.
    [[nodiscard]] std::optional<std::string> eval_index_expr(const Table& t, const Index& ix,
                                                             const std::vector<Datum>& row,
                                                             bool& skip, Datum& out) {
        skip = true;
        std::shared_ptr<Expr> ex;
        if (auto e = parse_index_expr(t, ix.expr_src, ex)) return e;
        if (auto e = eval_expr(*ex, t, row, out)) return e;
        if (out.is_null) return std::nullopt;  // NULL is simply not indexed (like a NULL column)
        if (out.type != ix.expr_type) {
            return "expression index '" + ix.name + "' value changed physical type (" +
                   type_name(out.type) + " vs " + type_name(ix.expr_type) +
                   "); the indexed expression must yield a stable type";
        }
        skip = false;
        return std::nullopt;
    }

    // J2: reject a write whose row cannot be soundly placed in every expression index (the
    // expression errors, or its result type drifts). Called at INSERT / UPDATE alongside CHECK / FK
    // validation — guarantees every stored row is evaluable, so the index path == the full scan.
    [[nodiscard]] std::optional<std::string> validate_index_exprs(const Table& t,
                                                                  const std::vector<Datum>& row) {
        for (const Index& ix : t.indexes) {
            if (ix.expr_src.empty()) continue;
            if (!row_matches_partial(t, ix, row)) continue;
            bool skip = false;
            Datum v;
            if (auto e = eval_index_expr(t, ix, row, skip, v)) return e;
        }
        return std::nullopt;
    }

    // J3: the DISTINCT, non-NULL elements of a GIN index's array column for `row` (the per-element
    // index keys). Dedup is by the order-preserving element encoding so `['red','red']` yields one
    // entry. A NULL array / NULL element contributes nothing (a NULL is never `= ANY`-matched).
    static std::vector<Datum> gin_elements(const Index& ix, const std::vector<Datum>& row) {
        std::vector<Datum> out;
        if (ix.columns.empty()) return out;
        const Datum& arr = row[ix.columns[0]];
        if (arr.is_null || arr.logical != 7 || arr.type != Type::Text) return out;
        std::set<std::string> seen;
        for (const Datum& el : Datum::decode_array(arr.s)) {
            if (el.is_null) continue;
            if (seen.insert(encode_index_col(el)).second) out.push_back(el);
        }
        return out;
    }


    // ===================== K3: SQL queues (exactly-once messaging) =====================
    // A queue is SUGAR over two hidden row-mode tables sharing all verified machinery
    // (durability, replication, backup, conformance):
    //   __q_<name>      (mid AUTO_INCREMENT PK, payload, state 0=ready|1=in-flight,
    //                    deliveries, visible_seq)
    //   __q_<name>_dlq  (mid PK, payload, deliveries)
    // SEND lowers onto the ordinary INSERT path — so inside BEGIN..COMMIT it commits
    // ATOMICALLY with data writes (the transactional outbox in one statement). RECEIVE
    // atomically marks up to BATCH messages in-flight (state=1, deliveries+1,
    // visible_seq = live Seq + VISIBILITY) in ONE batch; a message whose deliveries
    // would exceed kQueueMaxDeliveries moves to the DLQ instead of being delivered.
    // VISIBILITY is in Seq UNITS (logical time): redelivery decisions are pure functions
    // of replicated state, so replicas replaying the same statement stream agree byte
    // for byte (a wall clock would diverge them). ACK deletes; it is idempotent.
    static constexpr std::int64_t kQueueMaxDeliveries = 5;
    [[nodiscard]] static std::string queue_table(const std::string& q) { return "__q_" + q; }

    ExecResult exec_create_queue(const QueueStmt& qs) {
        const std::string qt = queue_table(qs.queue);
        if (catalog_.find(qt) != nullptr) {
            return ExecResult::failure("queue '" + qs.queue + "' already exists");
        }
        // Queue tables are always ROW-mode: RECEIVE writes rows directly via the row codec.
        const bool saved = columnar_default_;
        columnar_default_ = false;
        ExecResult r = exec(
            "CREATE TABLE " + qt +
            " (mid INT AUTO_INCREMENT, payload TEXT NOT NULL, state INT DEFAULT 0, "
            "deliveries INT DEFAULT 0, visible_seq INT DEFAULT 0, PRIMARY KEY (mid))");
        if (r.ok) {
            r = exec("CREATE TABLE " + qt +
                     "_dlq (mid INT, payload TEXT NOT NULL, deliveries INT DEFAULT 0, "
                     "PRIMARY KEY (mid))");
        }
        columnar_default_ = saved;
        return r;
    }
    ExecResult exec_drop_queue(const QueueStmt& qs) {
        const std::string qt = queue_table(qs.queue);
        if (catalog_.find(qt) == nullptr) {
            return ExecResult::failure("unknown queue '" + qs.queue + "'");
        }
        ExecResult r = exec("DROP TABLE " + qt);
        if (r.ok) r = exec("DROP TABLE " + qt + "_dlq");
        return r;
    }
    ExecResult exec_send(const QueueStmt& qs) {
        const std::string qt = queue_table(qs.queue);
        const Table* t = catalog_.find(qt);
        if (t == nullptr) return ExecResult::failure("unknown queue '" + qs.queue + "'");
        if (!qs.payload) return ExecResult::failure("SEND requires a payload");
        Datum pd;
        const std::vector<Datum> dummy(t->columns.size());
        if (auto e = eval_expr(*qs.payload, *t, dummy, pd)) return ExecResult::failure(*e);
        if (pd.is_null || pd.type != Type::Text) {
            return ExecResult::failure("SEND payload must be a TEXT value");
        }
        // The ordinary INSERT path — built PROGRAMMATICALLY (no quote-and-reparse round
        // trip per message); exec_insert buffers under BEGIN..COMMIT, so the
        // transactional-outbox semantics are unchanged.
        InsertStmt ins;
        ins.table = qt;
        ins.columns = {"payload"};
        ins.values = {pd};
        return exec_insert(ins);
    }
    ExecResult exec_receive(const QueueStmt& qs) {
        if (in_txn_) {
            return ExecResult::failure("RECEIVE inside a transaction is not supported");
        }
        const std::string qt = queue_table(qs.queue) + (qs.dlq ? "_dlq" : "");
        const Table* t = catalog_.find(qt);
        if (t == nullptr) return ExecResult::failure("unknown queue '" + qs.queue + "'");
        if (qs.dlq) {  // read-only peek at the dead letters
            return exec("SELECT mid, payload, deliveries FROM " + qt + " ORDER BY mid LIMIT " +
                        std::to_string(qs.batch));
        }
        const Table* dt = catalog_.find(queue_table(qs.queue) + "_dlq");
        if (dt == nullptr) return ExecResult::failure("queue DLQ table missing");
        const std::int64_t now = static_cast<std::int64_t>(db_.live_snap_seq());
        std::vector<storage::KeyValue> kvs;
        {
            Query<Strict> q;
            q.scan(table_prefix(t->id), table_prefix_end(t->id));
            collect(db_.run(q), kvs);
        }
        std::vector<std::pair<Key, Value>> writes;
        ExecResult r;
        for (const storage::KeyValue& kv : kvs) {
            if (static_cast<std::int64_t>(r.rows.size()) >= qs.batch) break;
            if (is_tombstone(kv.second)) continue;
            std::vector<Datum> row = decode_row(*t, kv.first, kv.second);
            const std::int64_t state = row[2].i, vis = row[4].i;
            if (!(state == 0 || (state == 1 && vis <= now))) continue;  // in flight
            const std::int64_t nd = row[3].i + 1;
            if (nd > kQueueMaxDeliveries) {
                // Dead-letter: move (mid, payload, deliveries) and retire the message.
                std::vector<Datum> drow(dt->columns.size());
                drow[0] = row[0];
                drow[1] = row[1];
                drow[2] = Datum::make_int(nd);
                writes.emplace_back(encode_key(*dt, drow[0]), encode_value(*dt, drow));
                writes.emplace_back(kv.first, tombstone_marker());
                continue;  // not delivered — it is dead
            }
            row[2] = Datum::make_int(1);
            row[3] = Datum::make_int(nd);
            row[4] = Datum::make_int(now + qs.visibility);
            writes.emplace_back(kv.first, encode_value(*t, row));
            ResultRow out;
            out.cells.emplace_back("mid", row[0]);
            out.cells.emplace_back("payload", row[1]);
            out.cells.emplace_back("deliveries", row[3]);
            r.rows.push_back(std::move(out));
        }
        if (!writes.empty()) commit_writes(writes);  // marks + DLQ moves, ONE atomic batch
        r.affected = r.rows.size();
        return r;
    }
    ExecResult exec_ack(const QueueStmt& qs) {
        if (in_txn_) return ExecResult::failure("ACK inside a transaction is not supported");
        const std::string qt = queue_table(qs.queue);
        const Table* t = catalog_.find(qt);
        if (t == nullptr) return ExecResult::failure("unknown queue '" + qs.queue + "'");
        // Idempotent BATCHED ack: tombstone every listed live message in ONE commit
        // (pgmq parity with delete(ids[]); a per-message round trip dominated the
        // consumer loop). Absent ids are skipped — ACK stays idempotent.
        std::vector<std::pair<Key, Value>> writes;
        std::int64_t acked = 0;
        for (const std::int64_t m : qs.mids) {
            const Key rkey = encode_key(*t, Datum::make_int(m));
            bool live = false;
            (void)db_.engine().scan_visit(
                storage::Range{rkey, key_successor(rkey), /*hi_unbounded=*/false},
                db_.live_snap_seq(), [&](const Key&, const Value&) { live = true; });
            if (live) {
                writes.emplace_back(rkey, tombstone_marker());
                ++acked;
            }
        }
        if (!writes.empty()) commit_writes(writes);
        ExecResult r;
        r.affected = static_cast<std::size_t>(acked);
        return r;
    }
    // =================== end K3 queues ===================

    // ===================== K2: BM25 full-text search =====================
    // Deterministic tokenizer: ASCII lowercase, alphanumeric runs, length >= 2. No
    // locale, no ICU, no stemmer (v1) — the SAME bytes tokenize identically on every
    // replica and platform. Returns (term -> tf); `dl` = the doc length (token count).
    static std::map<std::string, std::uint32_t> bm25_tokens(const std::string& text,
                                                            std::uint32_t& dl) {
        std::map<std::string, std::uint32_t> tf;
        dl = 0;
        std::string cur;
        const auto flush = [&]() {
            if (cur.size() >= 2) {
                ++tf[cur];
                ++dl;
            }
            cur.clear();
        };
        for (const char ch : text) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
                cur.push_back(ch);
            } else if (u >= 'A' && u <= 'Z') {
                cur.push_back(static_cast<char>(u + 32));
            } else {
                flush();
            }
        }
        flush();
        return tf;
    }
    // Posting: 't' ++ put_index_col(term) ++ pk -> [tf be32][dl be32] (dl denormalised —
    // scoring needs no second lookup; an UPDATE rewrites the doc's postings anyway).
    // Stats: 'S' -> [nDocs be32][totalLen be64], read-modify-written in the row's batch.
    [[nodiscard]] static Key bm25_term_key(std::uint32_t tid, std::uint32_t iid,
                                           const std::string& term, const Key& pk) {
        Key k = index_prefix(tid, iid);
        k += "t";
        put_index_col(k, Datum::make_text(term));
        k += pk;
        return k;
    }
    [[nodiscard]] static Key bm25_term_prefix(std::uint32_t tid, std::uint32_t iid,
                                              const std::string& term) {
        Key k = index_prefix(tid, iid);
        k += "t";
        put_index_col(k, Datum::make_text(term));
        return k;
    }
    [[nodiscard]] static Key bm25_stats_key(std::uint32_t tid, std::uint32_t iid) {
        return index_prefix(tid, iid) + "S";
    }
    static std::string bm25_posting(std::uint32_t tf, std::uint32_t dl) {
        std::string v;
        Datum::put_be32_(v, tf);
        Datum::put_be32_(v, dl);
        return v;
    }
    static std::string bm25_stats_enc(std::uint32_t ndocs, std::uint64_t total_len) {
        std::string v;
        Datum::put_be32_(v, ndocs);
        Datum::put_be32_(v, static_cast<std::uint32_t>(total_len >> 32));
        Datum::put_be32_(v, static_cast<std::uint32_t>(total_len & 0xFFFFFFFFULL));
        return v;
    }
    static void bm25_stats_dec(const std::string& v, std::uint32_t& ndocs,
                               std::uint64_t& total_len) {
        ndocs = v.size() >= 12 ? Datum::get_be32_(v, 0) : 0;
        total_len = v.size() >= 12
                        ? ((static_cast<std::uint64_t>(Datum::get_be32_(v, 4)) << 32) |
                           Datum::get_be32_(v, 8))
                        : 0;
    }
    // Read one small KV through the in-flight batch overlay, then storage (Strict).
    [[nodiscard]] std::optional<Value> bm25_get(
        const Key& k, const std::vector<std::pair<Key, Value>>& out) {
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            if (it->first == k) {
                if (is_tombstone(it->second)) return std::nullopt;
                return it->second;
            }
        }
        std::vector<storage::KeyValue> kvs;
        Query<Strict> q;
        q.scan(k, key_successor(k));
        collect(db_.run(q), kvs);
        for (const storage::KeyValue& kv : kvs)
            if (kv.first == k && !is_tombstone(kv.second)) return kv.second;
        return std::nullopt;
    }
    // Maintenance: (un)index one row's TEXT value — postings + corpus stats in the SAME
    // atomic batch as the row write; the stats read goes through the batch overlay so a
    // multi-row INSERT accumulates correctly within one statement.
    void bm25_maintain(const Table& t, const Index& ix, const std::vector<Datum>& row,
                       bool tombstone, std::vector<std::pair<Key, Value>>& out) {
        const Datum& v = row[ix.columns[0]];
        if (v.is_null) return;  // a NULL indexes nothing (like every other index kind)
        std::uint32_t dl = 0;
        const std::map<std::string, std::uint32_t> tf = bm25_tokens(v.s, dl);
        const Key pk = encode_pk(row[t.pk_index]);
        for (const auto& [term, cnt] : tf) {
            out.emplace_back(bm25_term_key(t.id, ix.id, term, pk),
                             tombstone ? tombstone_marker() : bm25_posting(cnt, dl));
        }
        std::uint32_t nd = 0;
        std::uint64_t tl = 0;
        if (const auto sv = bm25_get(bm25_stats_key(t.id, ix.id), out)) {
            bm25_stats_dec(*sv, nd, tl);
        }
        if (tombstone) {
            if (nd > 0) --nd;
            tl = tl >= dl ? tl - dl : 0;
        } else {
            ++nd;
            tl += dl;
        }
        out.emplace_back(bm25_stats_key(t.id, ix.id), bm25_stats_enc(nd, tl));
    }
    // =================== end K2 BM25 core ===================

    // ===================== K1.4: HNSW (graph) approximate k-NN =====================
    // Deterministic HNSW over one VECTOR(n) column. KV records under the index prefix:
    //   'v' ++ pk            -> [flags u8 (1 = zombie)][top_level u8][vector payload]
    //   'n' ++ level u8 ++ pk -> repeated [be32 len][neighbor pk bytes]   (out-edges)
    //   'e'                  -> [top_level u8][entry-point pk bytes]
    // A node's LEVEL is a pure integer-geometric function of its PK bytes (repeated
    // splitmix64, success p = 1/m per level — no rng, and no libm ln() whose bits vary
    // across libc builds), and every heap/selection tie breaks on (real_cmp(dist), pk
    // bytes) — so replicas applying the same op sequence build a BYTE-IDENTICAL graph.
    // All maintenance writes ride the SAME atomic batch as the row write; reads during
    // maintenance overlay the in-flight batch so a multi-row INSERT links within itself.
    [[nodiscard]] static Key hnsw_vec_key(std::uint32_t tid, std::uint32_t iid,
                                          const std::string& pk) {
        return index_prefix(tid, iid) + "v" + pk;
    }
    [[nodiscard]] static Key hnsw_adj_key(std::uint32_t tid, std::uint32_t iid, std::uint8_t lvl,
                                          const std::string& pk) {
        Key k = index_prefix(tid, iid);
        k += "n";
        k.push_back(static_cast<char>(lvl));
        k += pk;
        return k;
    }
    [[nodiscard]] static Key hnsw_entry_key(std::uint32_t tid, std::uint32_t iid) {
        return index_prefix(tid, iid) + "e";
    }
    [[nodiscard]] static std::uint64_t hnsw_mix64(std::uint64_t x) {  // splitmix64 finalizer
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    // Geometric level with success probability 1/m per step (P(level >= l) = m^-l, the
    // HNSW distribution) — pure integer arithmetic over an FNV-1a hash of the PK bytes.
    [[nodiscard]] static std::uint32_t hnsw_level_for(const std::string& pk, std::uint32_t m) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const char c : pk) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ull;
        }
        std::uint32_t lvl = 0;
        while (lvl < 30) {
            h = hnsw_mix64(h);
            if (h % m != 0) break;
            ++lvl;
        }
        return lvl;
    }
    [[nodiscard]] static std::string hnsw_encode_list(const std::vector<std::string>& l) {
        std::string o;
        for (const std::string& s : l) {
            Datum::put_be32_(o, static_cast<std::uint32_t>(s.size()));
            o += s;
        }
        return o;
    }
    [[nodiscard]] static std::vector<std::string> hnsw_decode_list(const std::string& s) {
        std::vector<std::string> out;
        std::size_t p = 0;
        while (p + 4 <= s.size()) {
            const std::uint32_t len = Datum::get_be32_(s, p);
            p += 4;
            if (p + len > s.size()) break;
            out.push_back(s.substr(p, len));
            p += len;
        }
        return out;
    }
    // Opclass distance (same per-element accumulation order as the scalar kernel, u = the
    // STORED vector, v = the query — the candidate values are IEEE-identical to the exact
    // path's, so the large-ef == exact-scan gate holds).
    [[nodiscard]] static double hnsw_dist(std::uint8_t op, const std::vector<double>& u,
                                          const std::vector<double>& v) {
        double dot = 0.0, sq = 0.0, nu = 0.0, nv = 0.0;  // shared 4-lane kernel
        const std::size_t n = std::min(u.size(), v.size());
        vec_accum4(n, v, [&u](std::size_t k) { return u[k]; }, dot, sq, nu, nv);
        return vec_finish(op, dot, sq, nu, nv);
    }
    // Total deterministic order over (distance, pk bytes) — real_cmp keeps NaN total.
    struct HnswOrd {
        bool operator()(const std::pair<double, std::string>& a,
                        const std::pair<double, std::string>& b) const {
            const int c = Datum::real_cmp(a.first, b.first);
            if (c != 0) return c < 0;
            return a.second < b.second;
        }
    };
    // Read context: overlays the in-flight write batch (write path) and routes reads at
    // the statement's level (query path). Vectors are cached per operation (immutable
    // within one op — the only rewrite, the op's own node, happens before any search).
    struct HnswIO {
        const Table* t = nullptr;
        const Index* ix = nullptr;
        std::vector<std::pair<Key, Value>>* batch = nullptr;  // overlay + write sink
        const SelectStmt* sel = nullptr;                      // non-null => leveled reads
        std::string err;                                      // first read error (query path)
        std::unordered_map<std::string, std::vector<double>> vec_cache;
        std::size_t visited = 0;  // nodes examined (EXPLAIN ANALYZE "scanned")
    };
    [[nodiscard]] std::optional<Value> hnsw_get(HnswIO& io, const Key& k) {
        if (io.batch != nullptr) {
            for (auto it = io.batch->rbegin(); it != io.batch->rend(); ++it) {
                if (it->first == k) {
                    if (is_tombstone(it->second)) return std::nullopt;
                    return it->second;
                }
            }
        }
        std::vector<storage::KeyValue> kvs;
        if (io.sel != nullptr) {
            if (auto e = run_index_scan_at_level(*io.sel, k, key_successor(k), kvs)) {
                if (io.err.empty()) io.err = *e;
                return std::nullopt;
            }
        } else {
            Query<Strict> q;
            q.scan(k, key_successor(k));
            collect(db_.run(q), kvs);
        }
        for (const storage::KeyValue& kv : kvs)
            if (kv.first == k && !is_tombstone(kv.second)) return kv.second;
        return std::nullopt;
    }
    struct HnswNode {
        bool deleted = false;
        std::uint8_t top = 0;
    };
    [[nodiscard]] bool hnsw_node(HnswIO& io, const std::string& pk, HnswNode& n,
                                 const std::vector<double>** vec) {
        const auto v = hnsw_get(io, hnsw_vec_key(io.t->id, io.ix->id, pk));
        if (!v || v->size() < 2) return false;
        n.deleted = (*v)[0] != 0;
        n.top = static_cast<std::uint8_t>((*v)[1]);
        if (vec != nullptr) {
            auto it = io.vec_cache.find(pk);
            if (it == io.vec_cache.end())
                it = io.vec_cache.emplace(pk, ivf_payload_doubles(v->substr(2))).first;
            *vec = &it->second;
        }
        return true;
    }
    [[nodiscard]] std::vector<std::string> hnsw_neighbors(HnswIO& io, std::uint8_t lvl,
                                                          const std::string& pk) {
        const auto v = hnsw_get(io, hnsw_adj_key(io.t->id, io.ix->id, lvl, pk));
        return v ? hnsw_decode_list(*v) : std::vector<std::string>{};
    }
    // Best-first beam search within ONE layer (the classic HNSW SEARCH-LAYER), ef-wide.
    // Returns the ef best (distance, pk) pairs ascending. Deterministic: ordered sets with
    // the (real_cmp, pk-bytes) comparator; zombies participate (connectivity) — the CALLER
    // filters them from results.
    [[nodiscard]] std::vector<std::pair<double, std::string>> hnsw_search_layer(
        HnswIO& io, const std::vector<double>& q, const std::vector<std::string>& eps,
        std::size_t ef, std::uint8_t lvl) {
        std::set<std::pair<double, std::string>, HnswOrd> cand, best;
        std::set<std::string> visited;
        for (const std::string& ep : eps) {
            if (!visited.insert(ep).second) continue;
            HnswNode n;
            const std::vector<double>* v = nullptr;
            if (!hnsw_node(io, ep, n, &v)) continue;
            ++io.visited;
            const double d = hnsw_dist(io.ix->vec_op, *v, q);
            cand.emplace(d, ep);
            best.emplace(d, ep);
        }
        while (!cand.empty() && io.err.empty()) {
            const std::pair<double, std::string> c = *cand.begin();
            cand.erase(cand.begin());
            if (best.size() >= ef && HnswOrd{}(*std::prev(best.end()), c)) break;
            for (const std::string& nb : hnsw_neighbors(io, lvl, c.second)) {
                if (!visited.insert(nb).second) continue;
                HnswNode n;
                const std::vector<double>* v = nullptr;
                if (!hnsw_node(io, nb, n, &v)) continue;
                ++io.visited;
                const double d = hnsw_dist(io.ix->vec_op, *v, q);
                const std::pair<double, std::string> e{d, nb};
                if (best.size() < ef || HnswOrd{}(e, *std::prev(best.end()))) {
                    cand.insert(e);
                    best.insert(e);
                    if (best.size() > ef) best.erase(std::prev(best.end()));
                }
            }
        }
        return {best.begin(), best.end()};
    }
    // INSERT (or UPDATE — same PK => same level) one node: write the vector record first
    // (searches below then see the fresh value through the batch overlay), greedy-descend
    // from the entry, link the m nearest per level bidirectionally, trim an overflowing
    // neighbor to its own m-max (2m at level 0) nearest.
    void hnsw_insert(const Table& t, const Index& ix, const std::string& pk,
                     const std::string& payload, std::vector<std::pair<Key, Value>>& out) {
        HnswIO io;
        io.t = &t;
        io.ix = &ix;
        io.batch = &out;
        const std::uint32_t m = std::max<std::uint32_t>(2, ix.hnsw_m);
        const std::uint32_t lnew = hnsw_level_for(pk, m);
        std::string vrec;
        vrec.push_back(0);
        vrec.push_back(static_cast<char>(lnew));
        vrec += payload;
        out.emplace_back(hnsw_vec_key(t.id, ix.id, pk), vrec);
        const std::vector<double> q = ivf_payload_doubles(payload);
        io.vec_cache[pk] = q;
        const auto entry = hnsw_get(io, hnsw_entry_key(t.id, ix.id));
        std::vector<std::string> eps;
        std::uint8_t etop = 0;
        if (entry && !entry->empty()) {
            etop = static_cast<std::uint8_t>((*entry)[0]);
            const std::string epk = entry->substr(1);
            if (epk == pk) {
                // Re-inserting the ENTRY node itself (UPDATE): seed from its OLD out-edges
                // (still unwritten at this point — the overlay only holds the vector record).
                for (int lc = etop; lc >= 0 && eps.empty(); --lc)
                    eps = hnsw_neighbors(io, static_cast<std::uint8_t>(lc), pk);
            } else {
                eps = {epk};
            }
        }
        if (eps.empty()) {
            // First node ever (or the sole node re-inserted): it IS the graph.
            for (std::uint32_t l = 0; l <= lnew; ++l)
                out.emplace_back(hnsw_adj_key(t.id, ix.id, static_cast<std::uint8_t>(l), pk),
                                 std::string{});
            std::string erec;
            erec.push_back(static_cast<char>(lnew));
            erec += pk;
            out.emplace_back(hnsw_entry_key(t.id, ix.id), erec);
            return;
        }
        for (int lc = etop; lc > static_cast<int>(lnew); --lc) {
            const auto w = hnsw_search_layer(io, q, eps, 1, static_cast<std::uint8_t>(lc));
            if (!w.empty()) eps = {w.front().second};
        }
        for (int lc = std::min<int>(etop, static_cast<int>(lnew)); lc >= 0; --lc) {
            const std::uint8_t L = static_cast<std::uint8_t>(lc);
            const auto w =
                hnsw_search_layer(io, q, eps, std::max<std::uint32_t>(ix.hnsw_efc, m), L);
            std::vector<std::string> own;
            for (const auto& [d, nb] : w) {
                (void)d;
                if (nb == pk) continue;  // the UPDATE case can find itself
                own.push_back(nb);
                if (own.size() >= m) break;
            }
            out.emplace_back(hnsw_adj_key(t.id, ix.id, L, pk), hnsw_encode_list(own));
            const std::size_t mmax = L == 0 ? 2 * static_cast<std::size_t>(m) : m;
            for (const std::string& nb : own) {
                std::vector<std::string> lst = hnsw_neighbors(io, L, nb);
                bool dup = false;
                for (const std::string& x : lst)
                    if (x == pk) { dup = true; break; }
                if (!dup) lst.push_back(pk);
                if (lst.size() > mmax) {
                    HnswNode nn;
                    const std::vector<double>* nv = nullptr;
                    if (hnsw_node(io, nb, nn, &nv)) {
                        std::set<std::pair<double, std::string>, HnswOrd> scored;
                        for (const std::string& x : lst) {
                            HnswNode xn;
                            const std::vector<double>* xv = nullptr;
                            if (!hnsw_node(io, x, xn, &xv)) continue;
                            scored.emplace(hnsw_dist(ix.vec_op, *xv, *nv), x);
                        }
                        lst.clear();
                        for (const auto& [d2, x] : scored) {
                            (void)d2;
                            lst.push_back(x);
                            if (lst.size() >= mmax) break;
                        }
                    }
                }
                out.emplace_back(hnsw_adj_key(t.id, ix.id, L, nb), hnsw_encode_list(lst));
            }
            eps.clear();
            for (const auto& [d, nbp] : w) {
                (void)d;
                eps.push_back(nbp);
            }
            if (eps.empty()) eps = {entry->substr(1)};
        }
        if (lnew > etop) {
            for (std::uint32_t l = etop + 1; l <= lnew; ++l)
                out.emplace_back(hnsw_adj_key(t.id, ix.id, static_cast<std::uint8_t>(l), pk),
                                 std::string{});
            std::string erec;
            erec.push_back(static_cast<char>(lnew));
            erec += pk;
            out.emplace_back(hnsw_entry_key(t.id, ix.id), erec);
        }
    }
    // DELETE: zombie the node (flags = 1). Excluded from results, still traversable — the
    // standard HNSW deletion story (a rebuild/REINDEX compacts later).
    void hnsw_mark_deleted(const Table& t, const Index& ix, const std::string& pk,
                           std::vector<std::pair<Key, Value>>& out) {
        HnswIO io;
        io.t = &t;
        io.ix = &ix;
        io.batch = &out;
        const auto v = hnsw_get(io, hnsw_vec_key(t.id, ix.id, pk));
        if (!v || v->size() < 2) return;
        std::string nv = *v;
        nv[0] = 1;
        out.emplace_back(hnsw_vec_key(t.id, ix.id, pk), nv);
    }
    // =================== end K1.4 HNSW core ===================

    void index_writes_for_row(const Table& t, const std::vector<Datum>& row, bool tombstone,
                              std::vector<std::pair<Key, Value>>& out) {
        for (const Index& ix : t.indexes) {
            if (!row_matches_partial(t, ix, row)) continue;  // I5: outside the partial set
            if (ix.ivfflat) {  // K1 perf: any maintenance invalidates the probe cache
                ivf_probe_cache_.erase(ivf_cache_key(t.id, ix.id));
            }
            if (ix.bm25) {  // K2: postings + stats ride the SAME atomic batch as the row
                bm25_maintain(t, ix, row, tombstone, out);
                continue;
            }
            if (ix.hnsw) {  // K1.4: graph maintenance rides the SAME atomic batch as the row
                const Datum& v = row[ix.columns[0]];
                const std::string pk = encode_pk(row[t.pk_index]);
                if (tombstone) hnsw_mark_deleted(t, ix, pk, out);
                else if (!v.is_null) hnsw_insert(t, ix, pk, v.s, out);
                continue;
            }
            if (ix.ivfflat) {  // K1.3: one entry in the nearest-centroid list; value = the payload
                const Datum& v = row[ix.columns[0]];
                if (v.is_null) continue;  // a NULL vector gets no entry (like a NULL column)
                std::size_t dim = 0;
                std::vector<std::vector<double>> cents;
                ivf_decode_centroids(ix.centroids, dim, cents);
                if (cents.empty()) continue;  // defensive: unbuilt index
                const std::size_t list =
                    ivf_nearest(cents, ivf_assign_vec(ix.vec_op, ivf_payload_doubles(v.s)));
                out.emplace_back(
                    encode_index_entry(t.id, ix, Datum::make_int(static_cast<std::int64_t>(list)),
                                       row[t.pk_index]),
                    tombstone ? tombstone_marker() : v.s);
                continue;
            }
            if (ix.gin) {  // J3: one entry per DISTINCT array element
                for (const Datum& el : gin_elements(ix, row))
                    out.emplace_back(encode_index_entry(t.id, ix, el, row[t.pk_index]),
                                     tombstone ? tombstone_marker() : std::string{});
                continue;
            }
            // J2: an EXPRESSION index entry's leading token is the evaluated expression value. A NULL
            // (or, defensively, a type-drifted) result gets no entry — same as a NULL column below.
            // validate_index_exprs already rejected an un-evaluable row at the write entry, so the
            // eval here cannot error for a stored row.
            if (!ix.expr_src.empty()) {
                bool skip = true;
                Datum v;
                if (eval_index_expr(t, ix, row, skip, v) || skip) continue;
                out.emplace_back(encode_index_entry(t.id, ix, v, row[t.pk_index]),
                                 tombstone ? tombstone_marker() : std::string{});
                continue;
            }
            // v4: a NULL column value gets NO index entry (a NULL is never matched by an
            // `indexed_col = v` / BETWEEN lookup — comparison-with-NULL is UNKNOWN). So a
            // NULL row is simply absent from the index; the residual full-predicate (run
            // over point-got rows) still excludes it. This keeps the index == the table's
            // matchable rows. (UPDATE removes the OLD entry only if the old value was
            // non-NULL — symmetric, so a NULL<->value transition is maintained correctly.)
            bool any_null = false;  // E5: skip the entry if ANY covered column is NULL
            for (const std::size_t c : ix.columns) if (row[c].is_null) { any_null = true; break; }
            if (any_null) {
                continue;
            }
            const Key ikey = encode_index_entry_row(t.id, ix, row, row[t.pk_index]);
            out.emplace_back(ikey, tombstone ? tombstone_marker() : std::string{});
        }
    }

    // --- INSERT ---------------------------------------------------------------
    // ===== COLUMNAR layout helpers (write + read over the 'c' namespace) =======

    // The key whose presence means "a row with this PK exists" — the row key (row mode)
    // or the pk-column family entry (columnar). For dup-PK detect + existing-row probe.
    [[nodiscard]] Key existence_key(const Table& t, const Datum& pk) const {
        return t.columnar ? col_key(t.id, static_cast<std::uint32_t>(t.pk_index), pk)
                          : encode_key(t, pk);
    }

    // Emit the storage write that MATERIALISES a row. Row mode: {row_key, value} under
    // 't'. Columnar: {row_delta_key, value} under the 'd' delta overlay (same ROW codec);
    // FLUSH later compacts the delta into column blocks. One KV either way (atomic).
    void emit_row_writes(const Table& t, const std::vector<Datum>& row,
                         std::vector<std::pair<Key, Value>>& writes) const {
        const Datum& pk = row[t.pk_index];
        if (t.columnar) {
            writes.emplace_back(row_delta_key(t, pk), encode_value(t, row));
        } else {
            writes.emplace_back(encode_key_row(t, row), encode_value(t, row));  // F1: composite-aware
        }
    }

    // Emit the write that RETIRES a row. Row mode: a tombstone (scan-hidden). Columnar: a
    // LIVE del-marker in the delta so it SHADOWS any flushed block row (a plain tombstone
    // would be hidden by the scan and the block row would wrongly survive); FLUSH GCs it.
    void emit_row_tombstones(const Table& t, const Datum& pk,
                             std::vector<std::pair<Key, Value>>& writes) const {
        if (t.columnar) {
            writes.emplace_back(row_delta_key(t, pk), row_del_marker());
        } else {
            writes.emplace_back(encode_key(t, pk), tombstone_marker());
        }
    }

    // Read a columnar row by PK, MERGING the delta overlay over the flushed blocks. The
    // delta wins: a live delta value is the row; a del-marker means deleted (nullopt);
    // absent from the delta falls through to the blocks. nullopt if neither has a live
    // row. Used by UPDATE/DELETE (need the old row for index upkeep) + the index fetch +
    // the dup-PK probe.
    [[nodiscard]] std::optional<std::vector<Datum>> read_columnar_row(const Table& t,
                                                                      const Datum& pk) {
        const ReadResult dv = read_committed(row_delta_key(t, pk));
        if (dv.has_value() && !is_tombstone(*dv)) {
            if (is_row_del_marker(*dv)) {
                return std::nullopt;  // delta delete shadows any overlay/block row
            }
            return decode_row(t, encode_key(t, pk), *dv);  // live delta row (row codec)
        }
        // Not in the delta — check the overlay runs NEWEST->OLDEST (a run's tombstone or
        // live row shadows older runs + the base), then the base blocks.
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> manifest = load_manifest(t);
        const Key ek = encode_pk(pk);
        for (auto it = manifest.rbegin(); it != manifest.rend(); ++it) {
            const std::uint32_t run = it->first;
            bool tombstoned = false;
            for (const Key& tk : load_tombs(t, run)) {
                if (tk == ek) {
                    tombstoned = true;
                    break;
                }
            }
            if (tombstoned) {
                return std::nullopt;  // deleted in this run
            }
            if (auto row = read_overlay_row(t, run, pk)) {
                return row;
            }
        }
        return read_block_row(t, pk);  // base blocks (no overlay had it)
    }

    // Assemble a full row for `pk` from a specific overlay run (nullopt if not present).
    [[nodiscard]] std::optional<std::vector<Datum>> read_overlay_row(const Table& t,
                                                                     std::uint32_t run,
                                                                     const Datum& pk) {
        const std::uint32_t pkc_id = static_cast<std::uint32_t>(t.pk_index);
        const std::vector<ColumnChunk> pk_chunks = overlay_run_chunks(t, run, pkc_id);
        for (std::size_t j = 0; j < pk_chunks.size(); ++j) {
            const auto idx = chunk_find_pk(pk_chunks[j], pk);
            if (!idx) {
                continue;
            }
            std::vector<Datum> row(t.columns.size());
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                if (c == t.pk_index) {
                    row[c] = pk;
                    continue;
                }
                const std::vector<ColumnChunk> cch =
                    overlay_run_chunks(t, run, static_cast<std::uint32_t>(c));
                if (j < cch.size()) {
                    row[c] = cch[j].at(*idx);
                }
            }
            return row;
        }
        return std::nullopt;
    }

    // Range-scan ONE key range [lo,hi) at the statement's D5 level (the column-family
    // scan primitive; mirrors run_select_at_level's level dispatch for a raw range).
    [[nodiscard]] std::optional<std::string> scan_range_at_level(
        const SelectStmt& sel, const Key& lo, const Key& hi,
        std::vector<storage::KeyValue>& kvs) {
        switch (sel.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                q.scan(lo, hi);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::Snapshot: {
                Query<Snapshot> q = snapshot_query(sel.snapshot_version);
                q.scan(lo, hi);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q = bounded_query(sel.max_lag);
                q.scan(lo, hi);
                collect(db_.run(q, /*replica_lag=*/0), kvs);
                return std::nullopt;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q = ryw_query(sel.session);
                q.scan(lo, hi);
                collect(db_.run(q, /*replica_lag=*/0, /*session_last_write=*/tip_), kvs);
                return std::nullopt;
            }
        }
        return std::string("unsupported consistency level");
    }

    // ~rows-per-chunk for flushed column blocks: bounds block size + is the unit of
    // pk-range DATA SKIPPING. A column is stored as ceil(N/kChunkRows) chunks (block_no
    // 0..K-1), each a contiguous pk-ascending slice; the chunks are pk-disjoint + ascending.
    static constexpr std::uint32_t kChunkRows = 1024;

    // Does the table have flushed blocks? Probe the pk-column's first chunk (block_no 0).
    [[nodiscard]] bool has_blocks(const Table& t) {
        return read_committed(block_key(t.id, static_cast<std::uint32_t>(t.pk_index), 0))
            .has_value();
    }

    // Decode ALL flushed chunks of column c (block_no 0..K-1, pk-ascending), in order.
    [[nodiscard]] std::vector<ColumnChunk> read_col_chunks(const Table& t, std::uint32_t c) {
        std::vector<storage::KeyValue> kvs;
        Query<Strict> q;
        q.scan(block_col_prefix(t.id, c), block_col_prefix_end(t.id, c));
        collect(db_.run(q), kvs);
        std::vector<ColumnChunk> out;
        out.reserve(kvs.size());
        for (const storage::KeyValue& kv : kvs) {
            if (!is_tombstone(kv.second)) {
                out.push_back(decode_column_block(kv.second));
            }
        }
        return out;
    }

    // Decoded chunks of column c, CACHED by the table's flush generation: blocks change
    // only on flush, so between flushes the decode is done ONCE and every read reuses it
    // (kills the per-query decode that capped the columnar win). Perf-only — the cache is a
    // pure function of the committed blocks, so results + determinism are unchanged. The
    // cache is bumped on flush (flush_gen) and cleared on recover.
    [[nodiscard]] const std::vector<ColumnChunk>& col_chunks_cached(const Table& t,
                                                                    std::uint32_t c) {
        const std::uint64_t key = (static_cast<std::uint64_t>(t.id) << 20) | c;
        ChunkCacheEntry& slot = chunk_cache_[key];
        if (slot.gen != t.flush_gen + 1) {  // +1 so the default 0 never matches a real gen
            slot.gen = t.flush_gen + 1;
            slot.chunks = read_col_chunks(t, c);
        }
        return slot.chunks;
    }

    // Concatenate a column's (cached) chunks into one logical SoA chunk (all rows, pk-asc).
    // CACHED (flush_gen-tagged): the build runs once per flush, then every aggregate/GROUP BY
    // query over the column reuses it instead of re-concatenating ~8MB/Mrow each time.
    [[nodiscard]] const ColumnChunk& read_col_concat(const Table& t, std::uint32_t c) {
        const std::uint64_t key = (static_cast<std::uint64_t>(t.id) << 20) | c;
        ConcatCacheEntry& slot = concat_cache_[key];
        if (slot.gen == t.flush_gen + 1) {  // +1 so the default 0 never matches a real gen
            return slot.col;
        }
        slot.gen = t.flush_gen + 1;
        const std::vector<ColumnChunk>& chunks = col_chunks_cached(t, c);
        ColumnChunk out;
        if (!chunks.empty()) {
            out.type = chunks[0].type;
            for (const ColumnChunk& ch : chunks) {
                out.count += ch.count;
                out.nulls.insert(out.nulls.end(), ch.nulls.begin(), ch.nulls.end());
                if (out.type == Type::Int) {
                    out.ints.insert(out.ints.end(), ch.ints.begin(), ch.ints.end());
                } else {
                    out.texts.insert(out.texts.end(), ch.texts.begin(), ch.texts.end());
                }
            }
        }
        slot.col = std::move(out);
        return slot.col;
    }

    // A2 — dictionary-encoded TEXT column (codes aligned with the concat cache + a code->value
    // table), flush_gen-tagged. A low-cardinality TEXT GROUP BY groups by INT codes (a direct
    // array, ~9x faster than per-row string hashing). Pure function of the committed blocks.
    struct TextDictEntry {
        std::uint64_t gen = 0;
        std::vector<std::uint32_t> codes;  // per row -> code
        std::vector<std::string> values;   // code -> the distinct string
    };

    // Dictionary-encode a TEXT column: codes[row] = a dense int code for col.texts[row], and
    // values[code] = the distinct string. Built via open addressing (string -> code) over the
    // cached concat, cached flush_gen-tagged. A NOT NULL text column only (the caller gates).
    [[nodiscard]] const TextDictEntry& col_text_dict_cached(const Table& t, std::uint32_t c) {
        const std::uint64_t key = (static_cast<std::uint64_t>(t.id) << 20) | c;
        TextDictEntry& slot = text_dict_cache_[key];
        if (slot.gen == t.flush_gen + 1) {
            return slot;
        }
        slot.gen = t.flush_gen + 1;
        const ColumnChunk& col = read_col_concat(t, c);
        slot.codes.assign(col.count, 0);
        slot.values.clear();
        std::size_t cap = 256;
        std::vector<std::int32_t> dslot(cap, -1);
        const std::hash<std::string> hf;
        auto reinsert = [&](std::int32_t code) {
            std::size_t p = hf(slot.values[static_cast<std::size_t>(code)]) & (cap - 1);
            while (dslot[p] >= 0) p = (p + 1) & (cap - 1);
            dslot[p] = code;
        };
        for (std::uint32_t r = 0; r < col.count; ++r) {
            const std::string& s = col.texts[r];
            std::size_t p = hf(s) & (cap - 1);
            while (true) {
                if (dslot[p] < 0) {
                    if ((slot.values.size() + 1) * 10 >= cap * 7) {
                        cap *= 2;
                        dslot.assign(cap, -1);
                        for (std::int32_t code = 0;
                             code < static_cast<std::int32_t>(slot.values.size()); ++code) {
                            reinsert(code);
                        }
                        p = hf(s) & (cap - 1);
                        continue;
                    }
                    dslot[p] = static_cast<std::int32_t>(slot.values.size());
                    slot.values.push_back(s);
                    break;
                }
                if (slot.values[static_cast<std::size_t>(dslot[p])] == s) break;
                p = (p + 1) & (cap - 1);
            }
            slot.codes[r] = static_cast<std::uint32_t>(dslot[p]);
        }
        return slot;
    }

    // Concatenate ONLY the given chunk ids of column c (zone-map survivors) into one SoA
    // chunk. Skipped chunks are never read/decoded — the zone-map data-skipping win.
    [[nodiscard]] ColumnChunk read_col_concat_chunks(const Table& t, std::uint32_t c,
                                                     const std::vector<std::uint64_t>& chunk_ids) {
        const std::vector<ColumnChunk>& chunks = col_chunks_cached(t, c);
        ColumnChunk out;
        bool first = true;
        for (const std::uint64_t j : chunk_ids) {
            if (j >= chunks.size()) {
                continue;
            }
            const ColumnChunk& ch = chunks[j];
            if (first) {
                out.type = ch.type;
                first = false;
            }
            out.count += ch.count;
            out.nulls.insert(out.nulls.end(), ch.nulls.begin(), ch.nulls.end());
            if (out.type == Type::Int) {
                out.ints.insert(out.ints.end(), ch.ints.begin(), ch.ints.end());
            } else {
                out.texts.insert(out.texts.end(), ch.texts.begin(), ch.texts.end());
            }
        }
        return out;
    }

    // ===== MULTI-RUN LSM OVERLAYS (cheap non-append flush) ==========================
    // Base = run 0 = the 'B' blocks. An overlay run holds the rows changed by a non-append
    // flush; reads merge base + overlays (newest run wins per pk) + the live delta.

    // Manifest = the active overlay runs (run_id, chunk count), recency order (ascending id).
    [[nodiscard]] static std::string encode_manifest(
        const std::vector<std::pair<std::uint32_t, std::uint64_t>>& runs) {
        std::string out;
        put_be32(out, static_cast<std::uint32_t>(runs.size()));
        for (const auto& [r, n] : runs) {
            put_be32(out, r);
            put_be64(out, n);
        }
        return out;
    }
    [[nodiscard]] static std::vector<std::pair<std::uint32_t, std::uint64_t>> decode_manifest(
        const std::string& v) {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
        std::size_t off = 0;
        const std::uint32_t c = get_be32(v, off);
        off += 4;
        for (std::uint32_t i = 0; i < c; ++i) {
            const std::uint32_t r = get_be32(v, off);
            off += 4;
            const std::uint64_t n = get_be64(v, off);
            off += 8;
            out.emplace_back(r, n);
        }
        return out;
    }
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint64_t>> load_manifest(
        const Table& t) {
        const ReadResult mv = read_committed(overlay_manifest_key(t.id));
        if (!mv.has_value() || is_tombstone(*mv) || mv->empty()) {
            return {};
        }
        return decode_manifest(*mv);
    }
    [[nodiscard]] bool has_overlays(const Table& t) { return !load_manifest(t).empty(); }

    // Tombstone list = the encoded pks deleted in a run (sorted, for a binary-search set).
    [[nodiscard]] static std::string encode_tombs(const std::vector<Key>& pks) {
        std::string out;
        put_be32(out, static_cast<std::uint32_t>(pks.size()));
        for (const Key& p : pks) {
            put_be32(out, static_cast<std::uint32_t>(p.size()));
            out += p;
        }
        return out;
    }
    [[nodiscard]] std::vector<Key> load_tombs(const Table& t, std::uint32_t run) {
        const ReadResult tv = read_committed(overlay_tomb_key(t.id, run));
        std::vector<Key> out;
        if (!tv.has_value() || is_tombstone(*tv) || tv->empty()) {
            return out;
        }
        std::size_t off = 0;
        const std::uint32_t c = get_be32(*tv, off);
        off += 4;
        for (std::uint32_t i = 0; i < c; ++i) {
            const std::uint32_t l = get_be32(*tv, off);
            off += 4;
            out.push_back(tv->substr(off, l));
            off += l;
        }
        return out;
    }

    // Decode one overlay run's chunks for column c (all chunks of that run, pk-ascending).
    [[nodiscard]] std::vector<ColumnChunk> overlay_run_chunks(const Table& t, std::uint32_t run,
                                                             std::uint32_t c) {
        std::vector<storage::KeyValue> kvs;
        Query<Strict> q;
        q.scan(overlay_run_col_prefix(t.id, run, c), overlay_run_col_prefix_end(t.id, run, c));
        collect(db_.run(q), kvs);
        std::vector<ColumnChunk> out;
        for (const storage::KeyValue& kv : kvs) {
            if (!is_tombstone(kv.second)) {
                out.push_back(decode_column_block(kv.second));
            }
        }
        return out;
    }

    // Materialise one overlay run's live rows (NEEDED cols + the pk always), pk-ascending.
    [[nodiscard]] std::vector<std::vector<Datum>> overlay_run_rows(
        const Table& t, std::uint32_t run, const std::vector<bool>& need) {
        const std::uint32_t pkc_id = static_cast<std::uint32_t>(t.pk_index);
        const std::vector<ColumnChunk> pk_chunks = overlay_run_chunks(t, run, pkc_id);
        std::vector<std::vector<ColumnChunk>> colch(t.columns.size());
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            if (c != t.pk_index && need[c]) {
                colch[c] = overlay_run_chunks(t, run, static_cast<std::uint32_t>(c));
            }
        }
        std::vector<std::vector<Datum>> rows;
        for (std::size_t j = 0; j < pk_chunks.size(); ++j) {
            const ColumnChunk& pc = pk_chunks[j];
            for (std::uint32_t i = 0; i < pc.count; ++i) {
                std::vector<Datum> row(t.columns.size());
                row[t.pk_index] = pc.at(i);
                for (std::size_t c = 0; c < t.columns.size(); ++c) {
                    if (c != t.pk_index && need[c] && j < colch[c].size()) {
                        row[c] = colch[c][j].at(i);
                    }
                }
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }

    // A pk-ascending chunk's [first,last] pk range is DISJOINT from [lo,hi] (data skipping).
    [[nodiscard]] static bool chunk_pk_disjoint(const ColumnChunk& pc, const Datum& lo,
                                                const Datum& hi) {
        if (pc.count == 0) {
            return true;
        }
        const Datum first = pc.at(0);
        const Datum last = pc.at(pc.count - 1);
        return last.less_than(lo) || hi.less_than(first);  // last<lo || hi<first
    }

    // ===== ZONE MAPS (Phase 4 data skipping for NON-pk INT-column filters) ==========

    // VECTORIZED FILTER (Phase 3) — a single comparison term `col <op> literal`. (Defined
    // here so the zone-map skip predicate + the vectorized scan/agg paths can all see it.)
    struct VecTerm {
        std::size_t col = 0;
        CmpOp op = CmpOp::Eq;
        Datum lit;
    };

    // Per-chunk, per-column value range. `has` false => non-zoned, or the column is all-NULL
    // in this chunk (a comparison then matches nothing => the chunk is skippable). INT zones
    // use lo/hi; TEXT zones use tlo/thi (lexicographic min/max).
    struct ColZone {
        bool has = false;
        bool is_text = false;
        std::int64_t lo = 0;
        std::int64_t hi = 0;
        std::string tlo;
        std::string thi;
    };

    // Encode a zone map: [be32 nchunks][be32 ncols] then per (chunk,col) [u8 flags]; if has,
    // INT => [be64 lo][be64 hi], TEXT => [be32 lolen][lo][be32 hilen][hi]. flags bit0=has,
    // bit1=is_text. Variable length (TEXT min/max are strings).
    [[nodiscard]] static std::string encode_zone_map(
        const std::vector<std::vector<ColZone>>& z, std::size_t ncols) {
        std::string out;
        put_be32(out, static_cast<std::uint32_t>(z.size()));
        put_be32(out, static_cast<std::uint32_t>(ncols));
        for (const std::vector<ColZone>& chunk : z) {
            for (const ColZone& cz : chunk) {
                const unsigned flags = (cz.has ? 1u : 0u) | (cz.is_text ? 2u : 0u);
                out.push_back(static_cast<char>(flags));
                if (!cz.has) {
                    continue;
                }
                if (cz.is_text) {
                    put_be32(out, static_cast<std::uint32_t>(cz.tlo.size()));
                    out += cz.tlo;
                    put_be32(out, static_cast<std::uint32_t>(cz.thi.size()));
                    out += cz.thi;
                } else {
                    put_be64(out, static_cast<std::uint64_t>(cz.lo));
                    put_be64(out, static_cast<std::uint64_t>(cz.hi));
                }
            }
        }
        return out;
    }
    [[nodiscard]] static std::vector<std::vector<ColZone>> decode_zone_map(const std::string& v) {
        std::vector<std::vector<ColZone>> z;
        std::size_t off = 0;
        const std::uint32_t nch = get_be32(v, off);
        off += 4;
        const std::uint32_t nc = get_be32(v, off);
        off += 4;
        z.assign(nch, std::vector<ColZone>(nc));
        for (std::uint32_t j = 0; j < nch; ++j) {
            for (std::uint32_t c = 0; c < nc; ++c) {
                ColZone cz;
                const unsigned flags = static_cast<unsigned char>(v[off]);
                off += 1;
                cz.has = (flags & 1u) != 0;
                cz.is_text = (flags & 2u) != 0;
                if (cz.has) {
                    if (cz.is_text) {
                        const std::uint32_t ll = get_be32(v, off);
                        off += 4;
                        cz.tlo = v.substr(off, ll);
                        off += ll;
                        const std::uint32_t hl = get_be32(v, off);
                        off += 4;
                        cz.thi = v.substr(off, hl);
                        off += hl;
                    } else {
                        cz.lo = static_cast<std::int64_t>(get_be64(v, off));
                        off += 8;
                        cz.hi = static_cast<std::int64_t>(get_be64(v, off));
                        off += 8;
                    }
                }
                z[j][c] = cz;
            }
        }
        return z;
    }

    // Load the table's zone map (empty if none flushed), CACHED by flush generation.
    [[nodiscard]] const std::vector<std::vector<ColZone>>& load_zones(const Table& t) {
        ZoneCacheEntry& slot = zone_cache_[t.id];
        if (slot.gen != t.flush_gen + 1) {
            slot.gen = t.flush_gen + 1;
            const ReadResult zv = read_committed(zone_key(t.id));
            slot.zones = (zv.has_value() && !is_tombstone(*zv))
                             ? decode_zone_map(*zv)
                             : std::vector<std::vector<ColZone>>{};
        }
        return slot.zones;
    }

    // Fold one cell into a chunk's column zone (INT lo/hi or TEXT tlo/thi; NULL skipped).
    static void zone_add(ColZone& cz, const Datum& d) {
        if (d.is_null) {
            return;
        }
        if (d.type == Type::Int) {
            cz.is_text = false;
            if (!cz.has) {
                cz.has = true;
                cz.lo = cz.hi = d.i;
            } else {
                cz.lo = std::min(cz.lo, d.i);
                cz.hi = std::max(cz.hi, d.i);
            }
        } else {
            cz.is_text = true;
            if (!cz.has) {
                cz.has = true;
                cz.tlo = cz.thi = d.s;
            } else {
                if (d.s < cz.tlo) {
                    cz.tlo = d.s;
                }
                if (d.s > cz.thi) {
                    cz.thi = d.s;
                }
            }
        }
    }

    // Can chunk j be SKIPPED given the conjunctive filter? Skippable if ANY conjunct proves NO
    // row in the chunk can satisfy it (AND semantics: one unsatisfiable conjunct => the whole
    // AND fails for every row). INT + TEXT (lexicographic). Conservative: unknown => no skip.
    [[nodiscard]] static bool chunk_skippable(const std::vector<ColZone>& zrow,
                                              const std::vector<VecTerm>& vterms) {
        for (const VecTerm& vt : vterms) {
            if (vt.lit.is_null || vt.col >= zrow.size()) {
                continue;
            }
            const ColZone& cz = zrow[vt.col];
            if (vt.lit.type == Type::Int && !cz.is_text) {
                if (!cz.has) {
                    return true;  // INT col all-NULL in chunk => the conjunct matches nothing
                }
                const std::int64_t x = vt.lit.i;
                switch (vt.op) {
                    case CmpOp::Gt: if (cz.hi <= x) return true; break;
                    case CmpOp::Ge: if (cz.hi < x) return true; break;
                    case CmpOp::Lt: if (cz.lo >= x) return true; break;
                    case CmpOp::Le: if (cz.lo > x) return true; break;
                    case CmpOp::Eq: if (x < cz.lo || x > cz.hi) return true; break;
                    case CmpOp::Ne: break;
                    case CmpOp::Like:
                    case CmpOp::Contains: break;  // LIKE / @> — never an INT zone term
                }
            } else if (vt.lit.type == Type::Text && cz.is_text) {
                if (!cz.has) {
                    return true;  // TEXT col all-NULL in chunk => the conjunct matches nothing
                }
                const std::string& x = vt.lit.s;  // lexicographic min/max
                switch (vt.op) {
                    case CmpOp::Gt: if (cz.thi <= x) return true; break;
                    case CmpOp::Ge: if (cz.thi < x) return true; break;
                    case CmpOp::Lt: if (cz.tlo >= x) return true; break;
                    case CmpOp::Le: if (cz.tlo > x) return true; break;
                    case CmpOp::Eq: if (x < cz.tlo || x > cz.thi) return true; break;
                    case CmpOp::Ne: break;
                    case CmpOp::Like:
                    case CmpOp::Contains: break;  // LIKE / @> handled in the general path, not zone-skipped
                }
            }
        }
        return false;
    }

    // Locate a pk within a (pk-ascending) pk-column chunk by binary search.
    [[nodiscard]] static std::optional<std::uint32_t> chunk_find_pk(const ColumnChunk& pkc,
                                                                    const Datum& pk) {
        if (pkc.type == Type::Int) {
            auto it = std::lower_bound(pkc.ints.begin(), pkc.ints.end(), pk.i);
            if (it != pkc.ints.end() && *it == pk.i) {
                return static_cast<std::uint32_t>(it - pkc.ints.begin());
            }
        } else {
            auto it = std::lower_bound(pkc.texts.begin(), pkc.texts.end(), pk.s);
            if (it != pkc.texts.end() && *it == pk.s) {
                return static_cast<std::uint32_t>(it - pkc.texts.begin());
            }
        }
        return std::nullopt;
    }

    // Assemble a single row from the flushed column blocks (nullopt if no blocks, or the
    // pk is not present). Decodes the pk block to locate the row index, then reads every
    // column's block at that index (used by the single-pk merge: dup probe / UPDATE /
    // DELETE / index fetch, where the full row is needed).
    [[nodiscard]] std::optional<std::vector<Datum>> read_block_row(const Table& t,
                                                                   const Datum& pk) {
        const std::uint32_t pkc_id = static_cast<std::uint32_t>(t.pk_index);
        const std::vector<ColumnChunk>& pk_chunks = col_chunks_cached(t, pkc_id);
        for (std::size_t j = 0; j < pk_chunks.size(); ++j) {
            const auto idx = chunk_find_pk(pk_chunks[j], pk);
            if (!idx) {
                continue;  // pk not in this chunk (chunks are pk-disjoint) — try the next
            }
            std::vector<Datum> row(t.columns.size());
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                if (c == t.pk_index) {
                    row[c] = pk;
                    continue;
                }
                const std::vector<ColumnChunk>& cch =
                    col_chunks_cached(t, static_cast<std::uint32_t>(c));
                if (j >= cch.size()) {
                    return std::nullopt;  // missing column chunk (defensive)
                }
                row[c] = cch[j].at(*idx);
            }
            tag_logical_cols(t, row);  // at() is untagged; stamp logical/scale like decode_row (F14)
            return row;
        }
        return std::nullopt;  // no blocks, or pk absent
    }

    // Build the post-scan `rows` for a COLUMNAR table by MERGING the flushed column blocks
    // (base, A3.2) with the row 'd' delta overlay (the delta wins per pk; a del-marker
    // drops the pk). `pk_between` restricts the scan to the PK range [lo,hi] (the columnar
    // PK-fast BETWEEN path; encode_pk is order-preserving). Only NEEDED columns are
    // materialised. Output is pk-ascending (an ordered map keyed by the order-preserving
    // pk encoding) so it matches the row-mode full-scan order the conformance gate checks.
    [[nodiscard]] std::optional<std::string> columnar_build_rows(
        const Table& t, const SelectStmt& sel, const std::vector<bool>& need,
        std::vector<std::vector<Datum>>& rows_out, bool pk_between = false) {
        // Base: flushed blocks, pk-ascending, NEEDED columns only (SoA decode). Empty
        // pre-flush.
        std::vector<std::vector<Datum>> base;
        if (auto e = block_base_rows(t, sel, need, pk_between, base)) {
            return e;
        }
        // Active overlay runs (a non-append flush appends rows here instead of rewriting the
        // base). FAST PATH: a single run (no overlays) + clean delta — base IS the answer.
        const std::vector<std::pair<std::uint32_t, std::uint64_t>> manifest = load_manifest(t);
        if (!t.delta_dirty && manifest.empty()) {
            rows_out = std::move(base);
            return std::nullopt;
        }
        // MERGE: seed an ordered map from base, apply each overlay run (oldest->newest; a
        // live row overwrites, a tombstone drops the pk), then the live 'd' delta (newest).
        std::map<Key, std::vector<Datum>> merged;  // key = encode_pk(pk) (pk-ascending)
        for (std::vector<Datum>& row : base) {
            merged[encode_pk(row[t.pk_index])] = std::move(row);
        }
        for (const auto& [run, nch] : manifest) {
            (void)nch;
            std::vector<std::vector<Datum>> rrows = overlay_run_rows(t, run, need);
            for (std::vector<Datum>& row : rrows) {
                merged[encode_pk(row[t.pk_index])] = std::move(row);
            }
            for (const Key& tk : load_tombs(t, run)) {
                merged.erase(tk);  // a run's tombstone shadows older runs + base
            }
        }
        if (t.delta_dirty) {
            std::vector<storage::KeyValue> delta;
            if (auto e = scan_range_at_level(sel, row_delta_prefix(t.id),
                                             row_delta_prefix_end(t.id), delta)) {
                return e;
            }
            for (const storage::KeyValue& kv : delta) {
                if (is_tombstone(kv.second)) {
                    continue;
                }
                const Datum pk = decode_pk_from_delta_key(t, kv.first);
                const Key mk = encode_pk(pk);
                if (is_row_del_marker(kv.second)) {
                    merged.erase(mk);
                    continue;
                }
                std::vector<Datum> row =
                    decode_row_projected(t, encode_key(t, pk), kv.second, need);
                row[t.pk_index] = pk;  // ensure pk set (merge key + pk-between filter)
                merged[mk] = std::move(row);
            }
        }
        // Emit pk-ascending. Base was pre-filtered by pk_between; overlay/delta rows are not,
        // so re-apply the pk range at output.
        rows_out.reserve(merged.size());
        std::size_t polled = 0;
        for (auto& [k, row] : merged) {
            (void)k;
            if ((++polled & 0xFFFFu) == 0 && canceled()) {  // W3.2: cancel a long columnar build
                return std::string("query canceled");
            }
            if (pk_between) {
                const Datum& pk = row[t.pk_index];
                if (pk.less_than(sel.lo_value) || sel.hi_value.less_than(pk)) {
                    continue;
                }
            }
            rows_out.push_back(std::move(row));
        }
        return std::nullopt;
    }

    // Build `base` (pk-ascending) from the flushed column blocks for the NEEDED columns
    // over the pk range — the projection-pushdown read: decode the pk block + ONLY the
    // needed column blocks (SoA), zip by position. The pk slot is ALWAYS set (the caller
    // uses it as the merge key); downstream projection ignores columns not in `need`.
    // Empty if no blocks (pre-flush).
    [[nodiscard]] std::optional<std::string> block_base_rows(
        const Table& t, const SelectStmt& sel, const std::vector<bool>& need,
        bool pk_between, std::vector<std::vector<Datum>>& base) {
        const std::uint32_t pkc_id = static_cast<std::uint32_t>(t.pk_index);
        const std::vector<ColumnChunk>& pk_chunks = col_chunks_cached(t, pkc_id);
        const std::size_t nch = pk_chunks.size();
        // ZONE-MAP skipping (Phase 4): if the WHERE flattens to INT conjuncts, skip any chunk
        // whose per-column [min,max] proves no row matches — without decoding its blocks.
        std::vector<VecTerm> vterms;
        const bool zone_filter = sel.filter.present() && vectorize_ &&
                                 try_extract_conjuncts(sel.filter, sel.filter.root, t, vterms);
        const std::vector<std::vector<ColZone>> zones =
            zone_filter ? load_zones(t) : std::vector<std::vector<ColZone>>{};
        for (std::size_t j = 0; j < nch; ++j) {
            const ColumnChunk& pkc = pk_chunks[j];
            // DATA SKIPPING: a pk-bounded query skips any chunk whose pk range can't match;
            // a non-pk INT filter skips via the zone map. Skipped chunks aren't materialised.
            if (pk_between && chunk_pk_disjoint(pkc, sel.lo_value, sel.hi_value)) {
                continue;
            }
            if (zone_filter && j < zones.size() && chunk_skippable(zones[j], vterms)) {
                continue;
            }
            // Hoist this chunk's needed-column chunk pointers (cache lookups are stable).
            std::vector<const ColumnChunk*> cc(t.columns.size(), nullptr);
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                if (c != t.pk_index && need[c]) {
                    cc[c] = &col_chunks_cached(t, static_cast<std::uint32_t>(c))[j];
                }
            }
            for (std::uint32_t i = 0; i < pkc.count; ++i) {
                const Datum pk = pkc.type == Type::Int ? Datum::make_int(pkc.ints[i])
                                                       : Datum::make_text(pkc.texts[i]);
                if (pk_between &&
                    (pk.less_than(sel.lo_value) || sel.hi_value.less_than(pk))) {
                    continue;
                }
                std::vector<Datum> row(t.columns.size());
                row[t.pk_index] = pk;  // always set (merge key); projection drops if unneeded
                for (std::size_t c = 0; c < t.columns.size(); ++c) {
                    if (cc[c] != nullptr) {
                        row[c] = cc[c]->at(i);
                    }
                }
                // ColumnChunk::at() yields an UNTAGGED Datum (logical=0); stamp the logical/scale
                // from the schema so a materialised REAL/DECIMAL/DATE compares & renders like the
                // row-path decode (F14: REAL MIN/MAX via cmp_datum needs the logical=14 tag).
                tag_logical_cols(t, row);
                base.push_back(std::move(row));
            }
        }
        return std::nullopt;
    }

    // Fold ONE ungrouped aggregate PER CHUNK and merge the partials — each chunk's array is
    // cache-resident (~8KB), so this avoids the 8MB whole-column concat (read_col_concat) that
    // made a large scan cache-bound + super-linear. SIMD per chunk (NOT NULL => no null
    // branch). Byte-identical to compute_agg_full over the concatenation; same partial-merge
    // shape morsel parallelism will use.
    // One partition's running fold of ONE aggregate (no shared state — each parallel worker
    // owns its own). Merged across partitions in a FIXED order, so the final value is identical
    // regardless of how the chunks were split or scheduled.
    struct AggPartial {
        std::int64_t total = 0;  // Σ ch.count over this partition's chunks
        std::int64_t n = 0;      // non-null count (Count nullable / SUM / AVG)
        std::int64_t sum = 0;    // Σ values (SUM / AVG)
        bool any = false;        // saw a non-null value (MIN / MAX)
        std::int64_t ibest = 0;  // running MIN/MAX for the Int+NOT NULL fast path
        Datum dbest;             // running MIN/MAX for the generic (text / nullable) path
    };

    // Fold chunks [lo,hi) of one column into a partial. Loop bodies match the old single-pass
    // folds exactly (Int+NOT NULL stays a tight, branch-free, auto-vectorizable loop).
    [[nodiscard]] AggPartial fold_agg_range(const std::vector<ColumnChunk>& chunks,
                                            std::size_t lo, std::size_t hi, const AggExpr& a,
                                            Type ty, bool no_nulls) {
        AggPartial p;
        for (std::size_t k = lo; k < hi; ++k) p.total += chunks[k].count;
        if (a.kind == AggKind::CountStar) return p;
        if (a.kind == AggKind::Count) {
            if (no_nulls) { p.n = p.total; return p; }
            for (std::size_t k = lo; k < hi; ++k) {
                const ColumnChunk& ch = chunks[k];
                for (std::uint32_t r = 0; r < ch.count; ++r) if (!ch.nulls[r]) ++p.n;
            }
            return p;
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            if (ty == Type::Int && no_nulls) {
                // BRANCHLESS reduction (the `if (x < best)` branch defeats vectorization — MIN/MAX
                // measured ~3-4x SUM; a min/max reduction auto-vectorizes at -O2). Seed from the
                // first element (or the carried partial) so there is no per-element `any` test.
                for (std::size_t k = lo; k < hi; ++k) {
                    const ColumnChunk& ch = chunks[k];
                    if (ch.count == 0) continue;
                    const std::int64_t* x = ch.ints.data();
                    std::int64_t b = p.any ? p.ibest : x[0];
                    if (a.kind == AggKind::Min) {
                        for (std::uint32_t r = 0; r < ch.count; ++r) b = std::min(b, x[r]);
                    } else {
                        for (std::uint32_t r = 0; r < ch.count; ++r) b = std::max(b, x[r]);
                    }
                    p.ibest = b;
                    p.any = true;
                }
                return p;
            }
            for (std::size_t k = lo; k < hi; ++k) {
                const ColumnChunk& ch = chunks[k];
                for (std::uint32_t r = 0; r < ch.count; ++r) {
                    if (ch.nulls[r]) continue;
                    const Datum v = ch.at(r);
                    if (!p.any) { p.dbest = v; p.any = true; }
                    else {
                        const int c = cmp_datum(v, p.dbest);
                        if ((a.kind == AggKind::Min && c < 0) ||
                            (a.kind == AggKind::Max && c > 0)) p.dbest = v;
                    }
                }
            }
            return p;
        }
        for (std::size_t k = lo; k < hi; ++k) {  // SUM / AVG
            const ColumnChunk& ch = chunks[k];
            if (no_nulls) {
                for (std::uint32_t r = 0; r < ch.count; ++r) p.sum = wrap_add(p.sum, ch.ints[r]);  // F9d
                p.n += ch.count;
            } else {
                for (std::uint32_t r = 0; r < ch.count; ++r) {
                    if (ch.nulls[r]) continue;
                    p.sum = wrap_add(p.sum, ch.ints[r]);  // F9d
                    ++p.n;
                }
            }
        }
        return p;
    }

    // FUSED column stats (A3-scan): one pass over a column computes total/non-null/sum AND both min
    // and max, so a SELECT SUM(x),MIN(x),MAX(x) FROM t streams the column ONCE (3x less bandwidth
    // than three separate folds — the scan_agg DuckDB gap). The INT NOT NULL pass is branchless =>
    // SIMD. Maps to AggPartial per item for a byte-identical finalize_agg.
    struct ColStats {
        std::int64_t total = 0;  // rows
        std::int64_t n = 0;      // non-null
        std::int64_t sum = 0;    // sum of non-null (int)
        bool any = false;
        std::int64_t imin = 0, imax = 0;  // int min/max
        Datum dmin, dmax;                 // generic (text) min/max
    };
    [[nodiscard]] ColStats fold_col_stats(const std::vector<ColumnChunk>& chunks, Type ty,
                                          bool no_nulls) {
        ColStats s;
        for (const ColumnChunk& ch : chunks) s.total += ch.count;
        if (ty == Type::Int && no_nulls) {
            bool seeded = false;
            for (const ColumnChunk& ch : chunks) {
                if (ch.count == 0) continue;
                const std::int64_t* x = ch.ints.data();
                std::int64_t sm = 0;
                std::int64_t mn = seeded ? s.imin : x[0];
                std::int64_t mx = seeded ? s.imax : x[0];
                for (std::uint32_t r = 0; r < ch.count; ++r) {  // ONE fused branchless pass
                    sm = wrap_add(sm, x[r]);  // F9d (columnar: defined wrap, SIMD-safe)
                    mn = std::min(mn, x[r]);
                    mx = std::max(mx, x[r]);
                }
                s.sum = wrap_add(s.sum, sm);  // F9d
                s.imin = mn;
                s.imax = mx;
                seeded = true;
            }
            s.n = s.total;
            s.any = s.total > 0;
            return s;
        }
        // NULLABLE (or TEXT): MIN/MAX track DATUMS (dmin/dmax) — finalize_agg uses dbest on the
        // non-(int+NOT NULL) path, so this matches fold_agg_range's generic Datum fold byte-for-
        // byte. SUM (int) still accumulates the non-null values.
        for (const ColumnChunk& ch : chunks) {
            for (std::uint32_t r = 0; r < ch.count; ++r) {
                if (ch.nulls[r]) continue;
                ++s.n;
                const Datum v = ch.at(r);
                if (ty == Type::Int) s.sum = wrap_add(s.sum, v.i);  // F9d
                if (!s.any) { s.dmin = v; s.dmax = v; s.any = true; }
                else {
                    if (cmp_datum(v, s.dmin) < 0) s.dmin = v;
                    if (cmp_datum(v, s.dmax) > 0) s.dmax = v;
                }
            }
        }
        return s;
    }

    // Build an AggPartial for ONE aggregate from a column's fused ColStats (so the SAME finalize_agg
    // path produces a byte-identical result to the per-aggregate compute_agg_chunked).
    [[nodiscard]] AggPartial partial_from_stats(const ColStats& s, AggKind kind) {
        AggPartial p;
        p.total = s.total;
        p.n = s.n;
        p.sum = s.sum;
        p.any = s.any;
        if (kind == AggKind::Min) { p.ibest = s.imin; p.dbest = s.dmin; }
        else if (kind == AggKind::Max) { p.ibest = s.imax; p.dbest = s.dmax; }
        return p;
    }

    // Combine partition partials in a FIXED order (0..W-1). SUM/COUNT are integer-exact (order
    // independent); MIN/MAX are associative+commutative — so the merged value is byte-identical
    // to the serial single-pass fold.
    [[nodiscard]] AggPartial merge_partials(const std::vector<AggPartial>& ps, const AggExpr& a,
                                            Type ty, bool no_nulls) {
        AggPartial m;
        const bool int_minmax =
            (a.kind == AggKind::Min || a.kind == AggKind::Max) && ty == Type::Int && no_nulls;
        for (const AggPartial& p : ps) {
            m.total += p.total;
            m.n += p.n;
            m.sum = wrap_add(m.sum, p.sum);  // F9d (columnar)
            if (!p.any) continue;
            if (int_minmax) {
                if (!m.any || (a.kind == AggKind::Min ? p.ibest < m.ibest : p.ibest > m.ibest)) {
                    m.ibest = p.ibest; m.any = true;
                }
            } else if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
                if (!m.any) { m.dbest = p.dbest; m.any = true; }
                else {
                    const int c = cmp_datum(p.dbest, m.dbest);
                    if ((a.kind == AggKind::Min && c < 0) ||
                        (a.kind == AggKind::Max && c > 0)) m.dbest = p.dbest;
                }
            }
        }
        return m;
    }

    // Turn a merged partial into the result Datum — mirrors the serial fold's tail exactly
    // (including the count==0 short-circuit that returns int 0 even for AVG).
    [[nodiscard]] Datum finalize_agg(const AggPartial& m, const AggExpr& a, Type ty,
                                     bool no_nulls) {
        if (a.kind == AggKind::CountStar) return Datum::make_int(m.total);
        if (a.kind == AggKind::Count) return Datum::make_int(no_nulls ? m.total : m.n);
        if (m.total == 0) return Datum::make_int(0);
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            if (ty == Type::Int && no_nulls) return Datum::make_int(m.ibest);
            return m.any ? m.dbest : Datum::make_null(ty);
        }
        if (m.n == 0) return a.kind == AggKind::Sum ? Datum::make_int(0) : Datum::make_null(ty);
        if (a.kind == AggKind::Sum) return Datum::make_int(m.sum);
        return Datum::make_int(m.sum / m.n);
    }

    [[nodiscard]] Datum compute_agg_chunked(const AggExpr& a, const Table& t) {
        std::uint32_t col = static_cast<std::uint32_t>(t.pk_index);
        Type ty = Type::Int;
        bool no_nulls = true;
        if (a.kind != AggKind::CountStar) {
            const std::size_t ci = *t.column_index(a.column);
            col = static_cast<std::uint32_t>(ci);
            ty = t.columns[ci].type;
            no_nulls = !t.columns[ci].nullable;
        }
        // Decode + cache the chunks ONCE on this thread before any fan-out — the workers then
        // only READ the cached (const) chunk vector, never mutate the cache concurrently.
        const std::vector<ColumnChunk>& chunks = col_chunks_cached(t, col);
        std::int64_t total_rows = 0;
        for (const ColumnChunk& ch : chunks) total_rows += ch.count;

        const std::size_t W = parallel_executor_ ? parallel_executor_->workers() : 1;
        const std::size_t nparts = std::min<std::size_t>(W, chunks.size());
        if (nparts > 1 && total_rows >= kParallelMinRows) {
            std::vector<AggPartial> partials(nparts);
            const std::size_t per = (chunks.size() + nparts - 1) / nparts;
            parallel_executor_->parallel_for(nparts, [&](std::size_t w) {
                const std::size_t lo = w * per;
                const std::size_t hi = std::min(chunks.size(), lo + per);
                if (lo < hi) partials[w] = fold_agg_range(chunks, lo, hi, a, ty, no_nulls);
            });
            return finalize_agg(merge_partials(partials, a, ty, no_nulls), a, ty, no_nulls);
        }
        return finalize_agg(fold_agg_range(chunks, 0, chunks.size(), a, ty, no_nulls), a, ty,
                            no_nulls);
    }

    // Parallel GROUP BY grouping pass: split the row range [0,count) into W partitions, build a
    // partial group map per partition (each worker its own — no shared state), then merge into
    // `dst` in partition order 0..W-1. Because partitions are ascending row ranges and each
    // collects ascending row indices, the merged per-group index vector is byte-identical to the
    // serial single-pass order; the group keys land in `dst` (an ordered std::map) in the same
    // order too. So the emitted rows are identical to serial regardless of the split. Only the
    // CPU-heavy grouping (a map insert per row) is parallelized; emit_group stays serial.
    // Caller guarantees count >= kParallelMinRows and an injected multi-worker executor.
    template <class MapT, class Passes, class KeyOf, class KeyDatumOf>
    void build_groups_parallel(MapT& dst, std::uint32_t count, bool has_filter, Passes passes,
                               KeyOf key_of, KeyDatumOf key_datum_of) {
        std::size_t nparts = std::min<std::size_t>(parallel_executor_->workers(), count);
        if (nparts < 2) nparts = 2;
        std::vector<MapT> parts(nparts);
        const std::uint32_t per = static_cast<std::uint32_t>((count + nparts - 1) / nparts);
        parallel_executor_->parallel_for(nparts, [&](std::size_t w) {
            const std::uint32_t lo = static_cast<std::uint32_t>(w) * per;
            const std::uint32_t hi = std::min<std::uint32_t>(count, lo + per);
            MapT& m = parts[w];
            for (std::uint32_t rr = lo; rr < hi; ++rr) {
                if (has_filter && !passes(rr)) continue;
                auto& slot = m[key_of(rr)];
                if (slot.second.empty()) slot.second = key_datum_of(rr);
                slot.first.push_back(rr);
            }
        });
        for (std::size_t w = 0; w < nparts; ++w) {
            for (auto& [k, slot] : parts[w]) {
                auto& d = dst[k];
                if (d.second.empty()) d.second = std::move(slot.second);
                if (d.first.empty()) {
                    d.first = std::move(slot.first);
                } else {
                    d.first.insert(d.first.end(), slot.first.begin(), slot.first.end());
                }
            }
        }
    }

    // OPEN-ADDRESSING hash GROUP BY collect (A1): build the groups for a single NOT NULL key via a
    // flat linear-probe hash table (no std::map red-black tree — a tree's per-row key compares +
    // pointer chasing are the text-key GROUP BY cost; profiled ~2.8x slower than open addressing).
    // Collects each group's member row indices (row order) + the key datum, then returns the groups
    // SORTED by key — byte-identical to the ordered-map output (same group order, same per-group
    // index order). KeyT is std::int64_t or std::string (std::hash + operator< both defined).
    template <class KeyT, class Passes, class KeyOf, class KeyDatumOf>
    std::vector<std::pair<std::vector<Datum>, std::vector<std::uint32_t>>> hash_group_collect(
        std::uint32_t count, bool has_filter, Passes passes, KeyOf key_of,
        KeyDatumOf key_datum_of) {
        std::size_t cap = 256;  // power of 2 (mask probing)
        std::vector<std::int32_t> slots(cap, -1);
        std::vector<KeyT> keys;
        std::vector<std::vector<Datum>> kds;
        std::vector<std::vector<std::uint32_t>> idxs;
        const std::hash<KeyT> hashf;
        auto reinsert = [&](std::int32_t gi) {
            std::size_t p = hashf(keys[static_cast<std::size_t>(gi)]) & (cap - 1);
            while (slots[p] >= 0) p = (p + 1) & (cap - 1);
            slots[p] = gi;
        };
        for (std::uint32_t rr = 0; rr < count; ++rr) {
            if (has_filter && !passes(rr)) continue;
            KeyT k = key_of(rr);
            std::size_t p = hashf(k) & (cap - 1);
            while (true) {
                if (slots[p] < 0) {
                    if ((keys.size() + 1) * 10 >= cap * 7) {  // load factor 0.7 => grow + rehash
                        cap *= 2;
                        slots.assign(cap, -1);
                        for (std::int32_t gi = 0; gi < static_cast<std::int32_t>(keys.size()); ++gi) {
                            reinsert(gi);
                        }
                        p = hashf(k) & (cap - 1);
                        continue;
                    }
                    slots[p] = static_cast<std::int32_t>(keys.size());
                    keys.push_back(k);
                    kds.push_back(key_datum_of(rr));
                    idxs.emplace_back();
                    break;
                }
                if (keys[static_cast<std::size_t>(slots[p])] == k) break;
                p = (p + 1) & (cap - 1);
            }
            idxs[static_cast<std::size_t>(slots[p])].push_back(rr);
        }
        std::vector<std::size_t> order(keys.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
        std::vector<std::pair<std::vector<Datum>, std::vector<std::uint32_t>>> out;
        out.reserve(order.size());
        for (const std::size_t oi : order) {
            out.emplace_back(std::move(kds[oi]), std::move(idxs[oi]));
        }
        return out;
    }

    // Fold ONE aggregate over the WHOLE column [0,count) — CONTIGUOUS (no index gather), so
    // the SUM/MIN/MAX loops auto-vectorize (SIMD) at -O2. For a NOT NULL column there is no
    // per-element null branch (the branch that otherwise defeats vectorization). Used by the
    // ungrouped, unfiltered aggregate fast path; byte-identical to compute_agg_soa over
    // idxs=[0,count).
    [[nodiscard]] Datum compute_agg_full(const AggExpr& a, const Table& t,
                                         const std::vector<ColumnChunk>& cols,
                                         std::uint32_t count) {
        if (a.kind == AggKind::CountStar) {
            return Datum::make_int(static_cast<std::int64_t>(count));
        }
        const std::size_t ci = *t.column_index(a.column);
        const ColumnChunk& cc = cols[ci];
        const Type ty = t.columns[ci].type;
        const bool no_nulls = !t.columns[ci].nullable;  // NOT NULL => skip the null branch
        if (a.kind == AggKind::Count) {
            if (no_nulls) {
                return Datum::make_int(static_cast<std::int64_t>(count));
            }
            std::int64_t c = 0;
            for (std::uint32_t r = 0; r < count; ++r) {
                if (!cc.nulls[r]) {
                    ++c;
                }
            }
            return Datum::make_int(c);
        }
        if (count == 0) {
            return Datum::make_int(0);
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            if (ty == Type::Int && no_nulls) {  // tight SIMD min/max over the raw int64 array
                std::int64_t best = cc.ints[0];
                if (a.kind == AggKind::Min) {
                    for (std::uint32_t r = 1; r < count; ++r) {
                        if (cc.ints[r] < best) best = cc.ints[r];
                    }
                } else {
                    for (std::uint32_t r = 1; r < count; ++r) {
                        if (cc.ints[r] > best) best = cc.ints[r];
                    }
                }
                return Datum::make_int(best);
            }
            Datum best;  // nullable or TEXT — per-element with the null check
            bool any = false;
            for (std::uint32_t r = 0; r < count; ++r) {
                if (cc.nulls[r]) continue;
                const Datum v = cc.at(r);
                if (!any) { best = v; any = true; }
                else {
                    const int c = cmp_datum(v, best);
                    if ((a.kind == AggKind::Min && c < 0) || (a.kind == AggKind::Max && c > 0)) {
                        best = v;
                    }
                }
            }
            return any ? best : Datum::make_null(ty);
        }
        // SUM / AVG over INT
        std::int64_t sum = 0;
        std::int64_t n = 0;
        if (no_nulls) {
            for (std::uint32_t r = 0; r < count; ++r) {  // contiguous => auto-vectorized
                sum = wrap_add(sum, cc.ints[r]);  // F9d (columnar: defined wrap, no UB, SIMD-safe)
            }
            n = count;
        } else {
            for (std::uint32_t r = 0; r < count; ++r) {
                if (cc.nulls[r]) continue;
                sum = wrap_add(sum, cc.ints[r]);  // F9d
                ++n;
            }
        }
        if (n == 0) {
            return a.kind == AggKind::Sum ? Datum::make_int(0) : Datum::make_null(ty);
        }
        if (a.kind == AggKind::Sum) {
            return Datum::make_int(sum);
        }
        return Datum::make_int(sum / n);  // AVG: trunc toward zero
    }

    // AGGREGATE FUSION: SUM/MIN/MAX of an INT NOT NULL column folded in ONE branchless gather pass,
    // so a query selecting several aggregates of the same column (SUM(a),MIN(a),MAX(a)) reads the
    // column ONCE instead of once per aggregate, and MIN/MAX run on raw int64 (no per-element Datum
    // like the generic gather). Byte-identical to compute_agg_soa over the same idxs.
    struct IntFold {
        std::int64_t sum = 0;
        std::int64_t mn = INT64_MAX;
        std::int64_t mx = INT64_MIN;
    };
    static IntFold fold_int_col(const ColumnChunk& cc, const std::vector<std::uint32_t>& idxs) {
        const std::int64_t* v = cc.ints.data();
        std::int64_t s = 0, mn = INT64_MAX, mx = INT64_MIN;
        for (const std::uint32_t r : idxs) {
            const std::int64_t x = v[r];
            s = wrap_add(s, x);          // F9d: defined wrap, matches compute_agg_soa's SUM order
            mn = mn < x ? mn : x;        // branchless min (cmov)
            mx = mx > x ? mx : x;        // branchless max
        }
        return IntFold{s, mn, mx};
    }
    // Fused masked fold for the ungrouped filtered fast path: SUM/MIN/MAX over the rows where
    // mask[r]==1, branchless, in ONE pass. Byte-identical to compute_masked_agg per aggregate (the
    // SUM via v*mask, MIN/MAX via masked-out-keeps-best select). Caller guarantees nmatch > 0.
    static IntFold fold_masked_col(const ColumnChunk& cc, std::uint32_t count,
                                   const std::vector<std::uint8_t>& mask) {
        const std::int64_t* v = cc.ints.data();
        std::int64_t s = 0, mn = INT64_MAX, mx = INT64_MIN;
        for (std::uint32_t r = 0; r < count; ++r) {
            const std::int64_t x = v[r];
            const std::int64_t m = mask[r];
            s += x * m;
            const std::int64_t lo = m ? x : INT64_MAX;
            const std::int64_t hi = m ? x : INT64_MIN;
            mn = mn < lo ? mn : lo;
            mx = mx > hi ? mx : hi;
        }
        return IntFold{s, mn, mx};
    }
    // Row-RANGE variant of fold_masked_col (for morsel parallelism): fold ONLY rows [lo,hi).
    // Partial folds combine associatively (sum via 2's-complement add, min/max) so a fixed-order
    // merge is BYTE-IDENTICAL to the serial fold_masked_col over [0,count).
    static IntFold fold_masked_col_range(const ColumnChunk& cc, std::uint32_t lo, std::uint32_t hi,
                                         const std::vector<std::uint8_t>& mask) {
        const std::int64_t* v = cc.ints.data();
        std::int64_t s = 0, mn = INT64_MAX, mx = INT64_MIN;
        for (std::uint32_t r = lo; r < hi; ++r) {
            const std::int64_t x = v[r];
            const std::int64_t m = mask[r];
            s += x * m;
            const std::int64_t lov = m ? x : INT64_MAX;
            const std::int64_t hiv = m ? x : INT64_MIN;
            mn = mn < lov ? mn : lov;
            mx = mx > hiv ? mx : hiv;
        }
        return IntFold{s, mn, mx};
    }
    static IntFold combine_fold(const IntFold& a, const IntFold& b) {
        return IntFold{a.sum + b.sum, a.mn < b.mn ? a.mn : b.mn, a.mx > b.mx ? a.mx : b.mx};
    }
    // Satisfy one fusable aggregate (idxs NON-empty, INT NOT NULL column) from a precomputed fold.
    static Datum agg_from_fold(AggKind k, const IntFold& f, std::size_t cnt) {
        switch (k) {
            case AggKind::Sum: return Datum::make_int(f.sum);
            case AggKind::Min: return Datum::make_int(f.mn);
            case AggKind::Max: return Datum::make_int(f.mx);
            case AggKind::Avg: return Datum::make_int(f.sum / static_cast<std::int64_t>(cnt));
            case AggKind::Count: return Datum::make_int(static_cast<std::int64_t>(cnt));  // NOT NULL
            default: return Datum::make_int(0);
        }
    }
    // Is this aggregate fusable — a foldable kind over an INT NOT NULL column (so the fold == the
    // generic gather, but cheaper)?
    [[nodiscard]] bool agg_fusable(const AggExpr& a, const Table& t) const {
        if (a.kind != AggKind::Sum && a.kind != AggKind::Min && a.kind != AggKind::Max &&
            a.kind != AggKind::Avg && a.kind != AggKind::Count) {
            return false;
        }
        const auto ci = t.column_index(a.column);
        return ci && t.columns[*ci].type == Type::Int && t.columns[*ci].logical == 0 &&
               !t.columns[*ci].nullable;
    }

    // Fold ONE aggregate over a SoA index subset (a group's member rows, or the whole
    // selection for an ungrouped query). Replicates compute_agg's semantics EXACTLY: NULL
    // skipping, empty-group=0, all-NULL group => SUM 0 / MIN/MAX/AVG typed NULL, AVG int
    // truncation toward zero.
    [[nodiscard]] Datum compute_agg_soa(const AggExpr& a, const Table& t,
                                        const std::vector<const ColumnChunk*>& cols,
                                        const std::vector<std::uint32_t>& idxs) {
        if (a.kind == AggKind::CountStar) {
            return Datum::make_int(static_cast<std::int64_t>(idxs.size()));
        }
        const std::size_t ci = *t.column_index(a.column);
        const ColumnChunk& cc = *cols[ci];
        const Type ty = t.columns[ci].type;
        if (a.kind == AggKind::Count) {
            std::int64_t cnt = 0;
            for (const std::uint32_t r : idxs) {
                if (!cc.nulls[r]) {
                    ++cnt;
                }
            }
            return Datum::make_int(cnt);
        }
        if (idxs.empty()) {
            return Datum::make_int(0);  // synthetic empty group (compute_agg parity)
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            Datum best;
            bool any = false;
            for (const std::uint32_t r : idxs) {
                if (cc.nulls[r]) {
                    continue;
                }
                const Datum v = cc.at(r);
                if (!any) {
                    best = v;
                    any = true;
                } else {
                    const int c = cmp_datum(v, best);
                    if ((a.kind == AggKind::Min && c < 0) ||
                        (a.kind == AggKind::Max && c > 0)) {
                        best = v;
                    }
                }
            }
            if (!any) return Datum::make_null(ty);
            if (t.columns[ci].logical >= 5) {  // K1: re-tag a 128-bit MIN/MAX (at() yields an untagged payload)
                best.logical = t.columns[ci].logical;
                best.scale = t.columns[ci].scale;
            }
            return best;
        }
        // K1: SUM / AVG over a 128-bit column (INT128/DECIMAL128) — accumulate in __int128 (checked),
        // byte-identical to compute_agg's row path. An overflow signals a bail to the row path (which
        // reports "integer overflow in SUM"); AVG truncates toward zero; the result keeps the tag.
        if (t.columns[ci].logical >= 5) {
            __int128 acc = 0;
            std::int64_t n128 = 0;
            for (const std::uint32_t r : idxs) {
                if (cc.nulls[r]) continue;
                if (__builtin_add_overflow(acc, Datum::decode_i128(cc.texts[r]), &acc)) {
                    soa_overflow_ = true;
                    return Datum::make_int(0);  // dummy; columnar_vectorized_agg bails on the flag
                }
                ++n128;
            }
            if (n128 == 0) return a.kind == AggKind::Sum ? Datum::make_int(0) : Datum::make_null(ty);
            if (a.kind == AggKind::Avg) acc /= static_cast<__int128>(n128);
            Datum d = Datum::make_text(Datum::encode_i128(acc));
            d.logical = t.columns[ci].logical;
            d.scale = t.columns[ci].scale;
            return d;
        }
        std::int64_t sum = 0;  // SUM / AVG over INT (validated INT)
        std::int64_t n = 0;
        for (const std::uint32_t r : idxs) {
            if (cc.nulls[r]) {
                continue;
            }
            sum = wrap_add(sum, cc.ints[r]);  // F9d (columnar)
            ++n;
        }
        if (n == 0) {
            return a.kind == AggKind::Sum ? Datum::make_int(0) : Datum::make_null(ty);
        }
        if (a.kind == AggKind::Sum) {
            return Datum::make_int(sum);
        }
        return Datum::make_int(sum / n);  // AVG: trunc toward zero
    }

    // VECTORIZED FILTER: build a 0/1 row mask for an all-INT-NOT-NULL conjunctive predicate with
    // BRANCHLESS comparisons (each `a OP lit` yields 0/1; the per-term AND auto-vectorizes), then
    // count the matches. Replaces the per-row branchy `passes()` + index gather for the common
    // filtered scalar aggregate. mask[r]==1 iff row r satisfies EVERY term.
    // K2: decode a 128-bit (INT128/DECIMAL128) SoA column's 16-byte payloads into a CONTIGUOUS
    // __int128 array ONCE, so the masked filter + fold run branchlessly over it (no per-row Datum /
    // string construction — the vectorization the INT path already enjoys). NULL slots decode to 0
    // (the null mask stays authoritative).
    static std::vector<__int128> decode_i128_col(const ColumnChunk& cc) {
        std::vector<__int128> out(cc.count);
        for (std::uint32_t r = 0; r < cc.count; ++r) {
            out[r] = cc.nulls[r] ? 0 : Datum::decode_i128(cc.texts[r]);
        }
        return out;
    }

    // K2: branchless 0/1 mask for an all-NOT-NULL conjunctive filter where each term is INT (cols->
    // ints) or 128-bit (the pre-decoded `i128[col]`). Mirrors build_filter_mask, extended to __int128.
    [[nodiscard]] static std::int64_t build_filter_mask_i128(
        const std::vector<const ColumnChunk*>& cols, std::uint32_t count,
        const std::vector<VecTerm>& vterms, const std::vector<std::vector<__int128>>& i128,
        const std::vector<bool>& is128, std::vector<std::uint8_t>& mask) {
        mask.assign(count, 1);
        for (const VecTerm& vt : vterms) {
            if (is128[vt.col]) {
                const __int128* a = i128[vt.col].data();
                const __int128 b = Datum::decode_i128(vt.lit.s);
                switch (vt.op) {
                    case CmpOp::Eq: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] == b); break;
                    case CmpOp::Ne: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] != b); break;
                    case CmpOp::Lt: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] < b); break;
                    case CmpOp::Le: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] <= b); break;
                    case CmpOp::Gt: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] > b); break;
                    case CmpOp::Ge: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] >= b); break;
                    case CmpOp::Like:
                    case CmpOp::Contains: break;
                }
            } else {
                const std::int64_t* a = cols[vt.col]->ints.data();
                const std::int64_t b = vt.lit.i;
                switch (vt.op) {
                    case CmpOp::Eq: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] == b); break;
                    case CmpOp::Ne: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] != b); break;
                    case CmpOp::Lt: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] < b); break;
                    case CmpOp::Le: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] <= b); break;
                    case CmpOp::Gt: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] > b); break;
                    case CmpOp::Ge: for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] >= b); break;
                    case CmpOp::Like:
                    case CmpOp::Contains: break;
                }
            }
        }
        std::int64_t n = 0;
        for (std::uint32_t r = 0; r < count; ++r) n += mask[r];
        return n;
    }

    [[nodiscard]] std::int64_t build_filter_mask(const std::vector<const ColumnChunk*>& cols,
                                                 std::uint32_t count,
                                                 const std::vector<VecTerm>& vterms,
                                                 std::vector<std::uint8_t>& mask) {
        mask.assign(count, 1);
        for (const VecTerm& vt : vterms) {
            const std::int64_t* a = cols[vt.col]->ints.data();
            const std::int64_t b = vt.lit.i;
            switch (vt.op) {
                case CmpOp::Eq:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] == b);
                    break;
                case CmpOp::Ne:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] != b);
                    break;
                case CmpOp::Lt:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] < b);
                    break;
                case CmpOp::Le:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] <= b);
                    break;
                case CmpOp::Gt:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] > b);
                    break;
                case CmpOp::Ge:
                    for (std::uint32_t r = 0; r < count; ++r) mask[r] &= (a[r] >= b);
                    break;
                case CmpOp::Like:
                case CmpOp::Contains:
                    break;  // LIKE / @> are never INT-vectorizable terms (the extractor rejects them)
            }
        }
        std::int64_t n = 0;
        for (std::uint32_t r = 0; r < count; ++r) n += mask[r];
        return n;
    }
    // Row-RANGE variant of build_filter_mask (morsel parallelism): write mask[lo,hi) + return the
    // slice's match count. Workers own DISJOINT ranges of the shared mask (no contention); the
    // slice counts sum to the same total, so the result is byte-identical to the serial build.
    static std::int64_t build_filter_mask_range(const std::vector<const ColumnChunk*>& cols,
                                                std::uint32_t lo, std::uint32_t hi,
                                                const std::vector<VecTerm>& vterms,
                                                std::vector<std::uint8_t>& mask) {
        for (std::uint32_t r = lo; r < hi; ++r) mask[r] = 1;
        for (const VecTerm& vt : vterms) {
            const std::int64_t* a = cols[vt.col]->ints.data();
            const std::int64_t b = vt.lit.i;
            switch (vt.op) {
                case CmpOp::Eq: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] == b); break;
                case CmpOp::Ne: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] != b); break;
                case CmpOp::Lt: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] < b); break;
                case CmpOp::Le: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] <= b); break;
                case CmpOp::Gt: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] > b); break;
                case CmpOp::Ge: for (std::uint32_t r = lo; r < hi; ++r) mask[r] &= (a[r] >= b); break;
                case CmpOp::Like:
                case CmpOp::Contains: break;
            }
        }
        std::int64_t n = 0;
        for (std::uint32_t r = lo; r < hi; ++r) n += mask[r];
        return n;
    }

    // Fold ONE aggregate over the masked rows (mask[r]==1 => row counts). Byte-identical to
    // compute_agg_soa over the set of matching indices, INCLUDING compute_agg_soa's empty-set rule
    // (zero matches => int 0 for every kind). Caller guarantees the agg column is INT NOT NULL.
    [[nodiscard]] Datum compute_masked_agg(const AggExpr& a, const Table& t,
                                           const std::vector<const ColumnChunk*>& cols,
                                           std::uint32_t count,
                                           const std::vector<std::uint8_t>& mask,
                                           std::int64_t nmatch) {
        if (a.kind == AggKind::CountStar || a.kind == AggKind::Count) {
            return Datum::make_int(nmatch);  // INT NOT NULL => Count == match count
        }
        if (nmatch == 0) {
            return Datum::make_int(0);  // compute_agg_soa empty-index parity (int 0 for all kinds)
        }
        const ColumnChunk& cc = *cols[*t.column_index(a.column)];
        const std::int64_t* v = cc.ints.data();
        if (a.kind == AggKind::Sum || a.kind == AggKind::Avg) {
            std::int64_t s = 0;
            for (std::uint32_t r = 0; r < count; ++r) s += v[r] * mask[r];  // branchless masked sum
            return a.kind == AggKind::Sum ? Datum::make_int(s) : Datum::make_int(s / nmatch);
        }
        std::int64_t best = (a.kind == AggKind::Min) ? INT64_MAX : INT64_MIN;
        for (std::uint32_t r = 0; r < count; ++r) {
            const std::int64_t cand = mask[r] ? v[r] : best;  // masked-out rows keep `best`
            best = (a.kind == AggKind::Min) ? std::min(best, cand) : std::max(best, cand);
        }
        return Datum::make_int(best);
    }

    // K2: masked fold of ONE aggregate over a 128-bit (INT128/DECIMAL128) NOT-NULL column, branchless
    // over the pre-decoded __int128 array. Byte-identical to compute_agg_soa over the matching rows
    // (same int-0 empty rule, same tag). A SUM overflow sets soa_overflow_ => the caller bails to the
    // row path (which raises the error).
    [[nodiscard]] Datum compute_masked_agg_i128(const AggExpr& a, const Table& t,
                                                const std::vector<__int128>& xs, std::size_t ci,
                                                std::uint32_t count,
                                                const std::vector<std::uint8_t>& mask,
                                                std::int64_t nmatch) {
        if (a.kind == AggKind::CountStar || a.kind == AggKind::Count) {
            return Datum::make_int(nmatch);  // NOT NULL => Count == match count
        }
        if (nmatch == 0) {
            return Datum::make_int(0);  // compute_agg_soa empty-index parity (int 0 for all kinds)
        }
        const __int128* x = xs.data();
        Datum d;
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            __int128 best = 0;
            bool any = false;
            for (std::uint32_t r = 0; r < count; ++r) {
                if (!mask[r]) continue;
                if (!any) { best = x[r]; any = true; }
                else if (a.kind == AggKind::Min ? x[r] < best : x[r] > best) { best = x[r]; }
            }
            d = Datum::make_text(Datum::encode_i128(best));
        } else {  // SUM / AVG — checked __int128 accumulation
            __int128 acc = 0;
            for (std::uint32_t r = 0; r < count; ++r) {
                if (!mask[r]) continue;
                if (__builtin_add_overflow(acc, x[r], &acc)) {
                    soa_overflow_ = true;
                    return Datum::make_int(0);  // dummy; caller bails on the flag
                }
            }
            if (a.kind == AggKind::Avg) acc /= static_cast<__int128>(nmatch);
            d = Datum::make_text(Datum::encode_i128(acc));
        }
        d.logical = t.columns[ci].logical;
        d.scale = t.columns[ci].scale;
        return d;
    }

    // Is the HAVING predicate SoA-evaluable (only And/Or/Not + Cmp with an Agg or grouped-
    // column LHS and a literal RHS)? If so, collect its referenced columns into `need` so
    // their blocks are decoded. Returns false => the caller bails to the generic AoS path
    // (subquery RHS / IS NULL / IN / EXISTS in HAVING are rare — not vectorized here).
    [[nodiscard]] bool having_soa_ok(const Predicate& p, std::int32_t node, const Table& t,
                                     std::vector<bool>& need) {
        if (node < 0) {
            return true;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        switch (n.kind) {
            case PredNodeKind::And:
            case PredNodeKind::Or:
                return having_soa_ok(p, n.left, t, need) &&
                       having_soa_ok(p, n.right, t, need);
            case PredNodeKind::Not:
                return having_soa_ok(p, n.left, t, need);
            case PredNodeKind::Cmp:
                break;
            default:
                return false;  // IS NULL / IN / EXISTS in HAVING — not vectorized
        }
        if (n.rhs_is_subquery) {
            return false;  // scalar-subquery RHS — not vectorized
        }
        if (n.operand == OperandKind::Agg) {
            if (n.agg.kind != AggKind::CountStar) {
                if (const auto idx = t.column_index(n.agg.column)) {
                    need[*idx] = true;
                }
            }
        } else if (const auto idx = t.column_index(n.column)) {
            need[*idx] = true;
        }
        return true;
    }

    // Evaluate the (SoA-evaluable) HAVING predicate for one group: Agg operands fold via
    // compute_agg_soa over the group's member indices; a column operand reads the group key.
    [[nodiscard]] std::optional<std::string> having_eval_soa(
        const Predicate& p, std::int32_t node, const Table& t,
        const std::vector<const ColumnChunk*>& cols, const std::vector<std::uint32_t>& idxs,
        const std::vector<std::size_t>& gcols, const std::vector<Datum>& keyd, bool& truth) {
        if (node < 0) {
            truth = true;
            return std::nullopt;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        if (n.kind == PredNodeKind::And) {
            bool l = false;
            if (auto e = having_eval_soa(p, n.left, t, cols, idxs, gcols, keyd, l)) return e;
            if (!l) { truth = false; return std::nullopt; }
            return having_eval_soa(p, n.right, t, cols, idxs, gcols, keyd, truth);
        }
        if (n.kind == PredNodeKind::Or) {
            bool l = false;
            if (auto e = having_eval_soa(p, n.left, t, cols, idxs, gcols, keyd, l)) return e;
            if (l) { truth = true; return std::nullopt; }
            return having_eval_soa(p, n.right, t, cols, idxs, gcols, keyd, truth);
        }
        if (n.kind == PredNodeKind::Not) {
            bool c = false;
            if (auto e = having_eval_soa(p, n.left, t, cols, idxs, gcols, keyd, c)) return e;
            truth = !c;
            return std::nullopt;
        }
        // Cmp leaf.
        Datum lhs;
        if (n.operand == OperandKind::Agg) {
            lhs = compute_agg_soa(n.agg, t, cols, idxs);
        } else {
            const auto idx = t.column_index(n.column);
            if (!idx) {
                return std::string("unknown column '" + n.column + "' in HAVING");
            }
            bool found = false;
            for (std::size_t j = 0; j < gcols.size(); ++j) {
                if (gcols[j] == *idx) {
                    lhs = keyd[j];
                    found = true;
                    break;
                }
            }
            if (!found) {
                return std::string("non-grouped column '" + n.column + "' in HAVING");
            }
        }
        const Datum& rhs = n.literal;
        if (lhs.is_null || rhs.is_null) {
            truth = false;
            return std::nullopt;
        }
        if (lhs.type != rhs.type) {
            return std::string("type mismatch in HAVING");
        }
        truth = leaf_truth(n.op, lhs, rhs);
        return std::nullopt;
    }

    // A4: compute an aggregate query (scalar OR GROUP BY, optional SoA-evaluable HAVING)
    // DIRECTLY over the SoA
    // column blocks — decode the needed columns, build groups from the group columns' SoA
    // (an ordered map keyed exactly as exec_aggregate, so group order is byte-identical),
    // fold each aggregate over each group's member indices. NO AoS row materialisation.
    // nullopt => not applicable (caller falls back to the generic AoS path, which the
    // conformance gate cross-checks this against).
    [[nodiscard]] std::optional<ExecResult> columnar_vectorized_agg(const Table& t,
                                                                    const SelectStmt& sel) {
        if (t.delta_dirty || !has_blocks(t) || has_overlays(t)) {
            // delta non-empty, pre-flush, or overlay runs present => the generic merge path
            // (columnar_build_rows merges base+overlays+delta). The fast SoA path is the
            // single-run clean-delta case (restored by compaction).
            return std::nullopt;
        }
        // A per-aggregate FILTER (WHERE ...) needs the row-AoS fold (compute_agg applies it); the
        // SoA fast path has no per-aggregate predicate, so bail to the generic path.
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && item.agg.filter && item.agg.filter->present())
                return std::nullopt;
        }
        // F14: a REAL column (logical=14, TEXT-physical) is NOT one of the SoA folders' shapes (its
        // 8-byte payload would be misread by the logical>=5 128-bit paths); any REAL column takes the
        // row-AoS path, which handles it via cmp_datum + a double-fold in compute_agg.
        for (const Column& c : t.columns) {
            if (c.logical == 14) return std::nullopt;
        }
        soa_overflow_ = false;  // K1: set if a 128-bit SoA SUM overflows => bail (the row path errors)
        std::vector<VecTerm> vterms;
        const bool has_filter = sel.filter.present();
        if (has_filter &&
            (!vectorize_ || !try_extract_conjuncts(sel.filter, sel.filter.root, t, vterms))) {
            return std::nullopt;  // non-vectorizable predicate — generic path
        }
        // Resolve + validate GROUP BY columns and plain SELECT columns (identical errors to
        // exec_aggregate, so the gate's error cases match too).
        std::vector<std::size_t> gcols;
        for (const std::string& gc : sel.group_by) {
            const auto idx = t.column_index(gc);
            if (!idx) {
                return ExecResult::failure("unknown GROUP BY column '" + gc +
                                           "' in table '" + t.name + "'");
            }
            gcols.push_back(*idx);
        }
        for (const SelectItem& item : sel.items) {
            if (item.kind != SelectItemKind::Column) {
                continue;
            }
            const auto idx = t.column_index(item.column);
            if (!idx) {
                return ExecResult::failure("unknown column '" + item.column +
                                           "' in table '" + t.name + "'");
            }
            bool grouped = false;
            for (const std::size_t g : gcols) {
                if (g == *idx) {
                    grouped = true;
                    break;
                }
            }
            if (!grouped) {
                return ExecResult::failure(
                    "column '" + item.column +
                    "' must appear in GROUP BY or be used in an aggregate "
                    "(non-grouped non-aggregate column)");
            }
        }
        if (auto e = validate_aggs(sel, t)) {
            return ExecResult::failure(*e);
        }
        // K1: a 128-bit (INT128/DECIMAL128) aggregate column IS handled by the SoA gather path
        // (compute_agg_soa folds the 16-byte payloads in __int128). It must NOT take the unfiltered
        // AggPartial / fused-stats fast path (which accumulates in int64), so flag it to route through
        // the gather path. ARRAY_AGG stays row-AoS only.
        // UINT256 (logical 13): its 32-byte payload is not the i128 SoA folders' shape, so any UINT256
        // column anywhere (filter / aggregate / group) takes the row-AoS path (correct via cmp_datum's
        // order-preserving TEXT compare + the u256 SUM in compute_agg).
        for (const VecTerm& vt : vterms)
            if (t.columns[vt.col].logical == 13) return std::nullopt;
        for (const std::size_t g : gcols)
            if (t.columns[g].logical == 13) return std::nullopt;
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && item.agg.kind != AggKind::CountStar) {
                if (const auto idx = t.column_index(item.agg.column))
                    if (t.columns[*idx].logical == 13) return std::nullopt;
            }
        }
        bool has_i128_agg = false;
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && item.agg.kind != AggKind::CountStar) {
                if (item.agg.kind >= AggKind::ArrayAgg)
                    return std::nullopt;  // ARRAY/JSON/STRING_AGG + BOOL_*/BIT_*: row-AoS only
                if (const auto idx = t.column_index(item.agg.column))
                    if (t.columns[*idx].logical >= 5) has_i128_agg = true;  // 5/6 only reach here (13 bailed)
            }
        }
        // Decode the needed column blocks: filter + group + aggregate-target columns.
        std::vector<bool> need(t.columns.size(), false);
        for (const VecTerm& vt : vterms) {
            need[vt.col] = true;
        }
        for (const std::size_t g : gcols) {
            need[g] = true;
        }
        for (const SelectItem& item : sel.items) {
            if (item.kind != SelectItemKind::Column && item.agg.kind != AggKind::CountStar) {
                if (const auto idx = t.column_index(item.agg.column)) {
                    need[*idx] = true;
                }
            }
        }
        // HAVING: bail to the generic AoS path if not SoA-evaluable; else add its columns
        // to `need` (so their blocks are decoded) and evaluate it per group below.
        const bool has_having = sel.having.present();
        if (has_having && !having_soa_ok(sel.having, sel.having.root, t, need)) {
            return std::nullopt;
        }
        // SIMD FAST PATH: an UNGROUPED, UNFILTERED scalar aggregate folds each column PER CHUNK
        // (cache-resident, SIMD) + merges partials — no group map, no index gather, and NO 8MB
        // whole-column concat (decoded only via the cached chunks). The common analytical
        // SELECT SUM/COUNT/MIN/MAX FROM t. Placed BEFORE the column decode so the concat is
        // never built for this path.
        if (gcols.empty() && !has_filter && !has_having && !has_i128_agg) {
            // FUSED per-column fold: stream each distinct aggregate column ONCE computing
            // total/sum/min/max together (3x less bandwidth than a fold per aggregate — the
            // scan_agg bandwidth gap). Now a WIN because MIN/MAX are branchless (the prior fusion
            // was a NEGATIVE only because the branchy min/max defeated SIMD). Maps each item to an
            // AggPartial so finalize_agg keeps results byte-identical to compute_agg_chunked.
            std::map<std::size_t, ColStats> col_stats;
            std::int64_t total_rows = 0;
            {  // table row count from the pk column (free; also covers COUNT(*))
                const auto& pkc = col_chunks_cached(t, static_cast<std::uint32_t>(t.pk_index));
                for (const ColumnChunk& ch : pkc) total_rows += ch.count;
            }
            for (const SelectItem& item : sel.items) {
                if (item.kind != SelectItemKind::Aggregate ||
                    item.agg.kind == AggKind::CountStar) {
                    continue;
                }
                const std::size_t ci = *t.column_index(item.agg.column);
                if (col_stats.find(ci) == col_stats.end()) {
                    col_stats.emplace(ci, fold_col_stats(
                                              col_chunks_cached(t, static_cast<std::uint32_t>(ci)),
                                              t.columns[ci].type, !t.columns[ci].nullable));
                }
            }
            ResultRow out;
            for (const SelectItem& item : sel.items) {
                if (item.kind != SelectItemKind::Aggregate) {
                    out.cells.emplace_back(item.label, compute_agg_chunked(item.agg, t));
                    continue;
                }
                if (item.agg.kind == AggKind::CountStar) {
                    out.cells.emplace_back(item.label, Datum::make_int(total_rows));
                    continue;
                }
                const std::size_t ci = *t.column_index(item.agg.column);
                const AggPartial p = partial_from_stats(col_stats.at(ci), item.agg.kind);
                out.cells.emplace_back(
                    item.label,
                    finalize_agg(p, item.agg, t.columns[ci].type, !t.columns[ci].nullable));
            }
            ExecResult rr;
            rr.rows.push_back(std::move(out));
            apply_distinct(sel, rr.rows);
            if (auto e = apply_order_by_labels(sel, rr.rows)) {
                return ExecResult::failure(*e);
            }
            apply_limit(sel, rr.rows);
            rr.affected = rr.rows.size();
            return rr;
        }
        // ZONE-MAP data skipping: with an INT filter, fold the aggregate over ONLY the
        // chunks whose [min,max] can match — skipped chunks are never read/decoded. (This
        // is where a FILTERED aggregate wins: row-mode has no fast path for a non-pk filter,
        // so it full-scans.) Without a filter / zone map, concat all chunks.
        std::vector<std::vector<ColZone>> zones;
        std::vector<std::uint64_t> survivors;
        bool use_survivors = false;
        if (has_filter && !vterms.empty()) {
            zones = load_zones(t);
            if (!zones.empty()) {
                for (std::uint64_t j = 0; j < zones.size(); ++j) {
                    if (!chunk_skippable(zones[j], vterms)) {
                        survivors.push_back(j);
                    }
                }
                use_survivors = true;
            }
        }
        // Bind each needed column. The non-survivor (full-column) case points DIRECTLY at the
        // cached flat concat — zero copy. The survivor (zone-skipped) case is query-specific, so
        // it is built into `owned` and pointed at. `cols[c]` is a pointer either way.
        std::vector<ColumnChunk> owned(t.columns.size());  // backs survivor / non-cached columns
        std::vector<const ColumnChunk*> cols(t.columns.size(), nullptr);
        std::uint32_t count = 0;
        bool count_set = false;
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            if (!need[c]) {
                continue;
            }
            if (use_survivors) {
                owned[c] = read_col_concat_chunks(t, static_cast<std::uint32_t>(c), survivors);
                cols[c] = &owned[c];
            } else {
                cols[c] = &read_col_concat(t, static_cast<std::uint32_t>(c));
            }
            count = cols[c]->count;
            count_set = true;
        }
        if (!count_set) {  // COUNT(*) only, no filter/group — the pk column gives the count
            const std::uint32_t pkc = static_cast<std::uint32_t>(t.pk_index);
            count = use_survivors ? read_col_concat_chunks(t, pkc, survivors).count
                                  : read_col_concat(t, pkc).count;
        }
        // A row filter shared by both group paths.
        auto passes = [&](std::uint32_t rr) {
            for (const VecTerm& vt : vterms) {
                if (!apply_cmp(vt.op, cmp_cell(*cols[vt.col], rr, vt.lit))) {
                    return false;
                }
            }
            return true;
        };
        // VECTORIZED FILTERED SCALAR AGGREGATE fast path: ungrouped + a fully INT-NOT-NULL
        // conjunctive filter + no HAVING + every aggregate foldable on an INT NOT NULL column.
        // Build a branchless 0/1 row mask (SIMD compares) and fold each aggregate over it — no
        // per-row branch, no index gather (the generic path collects matching indices then folds
        // by gather). Byte-identical to the generic path; the columnar conformance gate proves it.
        if (gcols.empty() && has_filter && !has_having && !vterms.empty()) {
            // A column is mask-vectorizable when NOT NULL and either plain INT or 128-bit
            // (INT128/DECIMAL128). K2 extends the INT-only branchless path to 128-bit columns.
            bool vec_ok = true;
            bool any_i128 = false;
            std::vector<bool> is128(t.columns.size(), false);
            auto classify = [&](std::size_t c, Type lit_ty) {
                const Column& col = t.columns[c];
                if (col.nullable) return false;
                if (col.logical >= 5 && col.type == Type::Text) {  // 128-bit
                    is128[c] = true;
                    any_i128 = true;
                    return lit_ty == Type::Text;  // literal coerced to the 16-byte payload
                }
                return col.type == Type::Int && col.logical == 0 && lit_ty == Type::Int;
            };
            for (const VecTerm& vt : vterms) {
                if (vt.lit.is_null || !classify(vt.col, vt.lit.type)) { vec_ok = false; break; }
            }
            if (vec_ok) {
                for (const SelectItem& item : sel.items) {
                    if (item.kind != SelectItemKind::Aggregate) { vec_ok = false; break; }
                    if (item.agg.kind == AggKind::CountStar) continue;
                    const auto ci = t.column_index(item.agg.column);
                    if (!ci || t.columns[*ci].nullable) { vec_ok = false; break; }
                    const Column& col = t.columns[*ci];
                    if (col.logical >= 5 && col.type == Type::Text) { is128[*ci] = true; any_i128 = true; }
                    else if (col.type != Type::Int || col.logical != 0) { vec_ok = false; break; }
                }
            }
            if (vec_ok && !any_i128) {
                // build_filter_mask + per-column fold are tight SIMD passes; a fused one-pass
                // filter+fold was MEASURED SLOWER (nested per-row loops + cmov selects defeat
                // vectorization — same lesson as the reverted running-accumulator). AGGREGATE FUSION:
                // fold each DISTINCT aggregated INT column ONCE over the mask (so SUM(a),MIN(a),MAX(a)
                // read column a a single time). The empty-match case keeps the int-0 rule.
                // Collect the DISTINCT aggregated INT columns (fused: each folded ONCE).
                std::vector<int> col_to_fuse(t.columns.size(), -1);
                std::vector<std::size_t> agg_cols;
                for (const SelectItem& item : sel.items) {
                    if (item.kind != SelectItemKind::Aggregate ||
                        item.agg.kind == AggKind::CountStar)
                        continue;
                    const std::size_t ci = *t.column_index(item.agg.column);
                    if (col_to_fuse[ci] < 0) {
                        col_to_fuse[ci] = static_cast<int>(agg_cols.size());
                        agg_cols.push_back(ci);
                    }
                }
                std::vector<std::uint8_t> mask(count);
                std::vector<IntFold> folds(agg_cols.size());
                std::int64_t nmatch = 0;
                const bool par = parallel_executor_ != nullptr &&
                                 parallel_executor_->workers() > 1 &&
                                 static_cast<std::int64_t>(count) >= kParallelMinRows;
                if (!par) {
                    nmatch = build_filter_mask(cols, count, vterms, mask);
                    if (nmatch > 0) {
                        for (std::size_t k = 0; k < agg_cols.size(); ++k) {
                            folds[k] = fold_masked_col(*cols[agg_cols[k]], count, mask);
                        }
                    }
                } else {
                    // MORSEL PARALLELISM for the scalar FILTERED aggregate (the profile showed this
                    // shape did NOT parallelize). Each worker builds its DISJOINT slice of the shared
                    // mask AND folds that slice; slice counts + partial folds merge in a FIXED order,
                    // so the result is byte-identical to the serial build+fold.
                    const std::size_t nparts =
                        std::min<std::size_t>(parallel_executor_->workers(), count);
                    const std::uint32_t per =
                        static_cast<std::uint32_t>((count + nparts - 1) / nparts);
                    std::vector<std::int64_t> pn(nparts, 0);
                    std::vector<std::vector<IntFold>> pf(nparts,
                                                         std::vector<IntFold>(agg_cols.size()));
                    parallel_executor_->parallel_for(nparts, [&](std::size_t w) {
                        const std::uint32_t lo = static_cast<std::uint32_t>(w) * per;
                        if (lo >= count) return;
                        const std::uint32_t hi = std::min(count, lo + per);
                        pn[w] = build_filter_mask_range(cols, lo, hi, vterms, mask);
                        for (std::size_t k = 0; k < agg_cols.size(); ++k) {
                            pf[w][k] = fold_masked_col_range(*cols[agg_cols[k]], lo, hi, mask);
                        }
                    });
                    for (std::size_t w = 0; w < nparts; ++w) nmatch += pn[w];
                    if (nmatch > 0) {
                        for (std::size_t k = 0; k < agg_cols.size(); ++k) {
                            IntFold m{0, INT64_MAX, INT64_MIN};
                            for (std::size_t w = 0; w < nparts; ++w) m = combine_fold(m, pf[w][k]);
                            folds[k] = m;
                        }
                    }
                }
                ResultRow out;
                for (const SelectItem& item : sel.items) {
                    if (nmatch > 0 && item.agg.kind != AggKind::CountStar) {
                        const std::size_t ci = *t.column_index(item.agg.column);
                        out.cells.emplace_back(
                            item.label, agg_from_fold(item.agg.kind,
                                                      folds[static_cast<std::size_t>(col_to_fuse[ci])],
                                                      static_cast<std::size_t>(nmatch)));
                    } else {
                        out.cells.emplace_back(
                            item.label, compute_masked_agg(item.agg, t, cols, count, mask, nmatch));
                    }
                }
                ExecResult rr;
                rr.rows.push_back(std::move(out));
                apply_distinct(sel, rr.rows);
                if (auto e = apply_order_by_labels(sel, rr.rows)) return ExecResult::failure(*e);
                apply_limit(sel, rr.rows);
                rr.affected = rr.rows.size();
                return rr;
            }
            if (vec_ok && any_i128) {  // K2: branchless 128-bit (mixed INT/128-bit) masked aggregate
                std::vector<std::vector<__int128>> dec(t.columns.size());
                for (std::size_t c = 0; c < t.columns.size(); ++c)
                    if (is128[c] && cols[c] != nullptr) dec[c] = decode_i128_col(*cols[c]);
                std::vector<std::uint8_t> mask;
                const std::int64_t nmatch =
                    build_filter_mask_i128(cols, count, vterms, dec, is128, mask);
                ResultRow out;
                for (const SelectItem& item : sel.items) {
                    if (item.agg.kind != AggKind::CountStar) {
                        const std::size_t ci = *t.column_index(item.agg.column);
                        if (is128[ci]) {
                            out.cells.emplace_back(item.label, compute_masked_agg_i128(
                                                                   item.agg, t, dec[ci], ci, count,
                                                                   mask, nmatch));
                            continue;
                        }
                    }
                    out.cells.emplace_back(
                        item.label, compute_masked_agg(item.agg, t, cols, count, mask, nmatch));
                }
                if (soa_overflow_) return std::nullopt;  // SUM overflow => row path raises the error
                ExecResult rr;
                rr.rows.push_back(std::move(out));
                apply_distinct(sel, rr.rows);
                if (auto e = apply_order_by_labels(sel, rr.rows)) return ExecResult::failure(*e);
                apply_limit(sel, rr.rows);
                rr.affected = rr.rows.size();
                return rr;
            }
        }
        ExecResult r;
        // Emit ONE group's result row (HAVING-filtered): aggregates fold via compute_agg_soa
        // (TIGHT per-column second pass; a 1-pass branchy running-accumulator measured SLOWER).
        // Appends to r.rows; returns an error string, or nullopt (emitted OR skipped by HAVING).
        // Compute ONE group's result row into `out` (HAVING-filtered via `keep`). Reads only
        // shared CONST state (cols / sel / catalog) + writes its own `out`, so groups can be
        // folded in PARALLEL across workers (compute_agg_soa / having_eval_soa mutate no engine
        // state). Returns an error string, or nullopt (keep=false => the group is dropped).
        // AGGREGATE FUSION: collect the DISTINCT INT-NOT-NULL columns aggregated by SUM/MIN/MAX/AVG/
        // COUNT, so each is folded ONCE per group (one gather pass shared across its aggregates).
        std::vector<std::size_t> fuse_cols;
        std::vector<int> col_to_fuse(t.columns.size(), -1);
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && agg_fusable(item.agg, t)) {
                const std::size_t ci = *t.column_index(item.agg.column);
                if (col_to_fuse[ci] < 0) {
                    col_to_fuse[ci] = static_cast<int>(fuse_cols.size());
                    fuse_cols.push_back(ci);
                }
            }
        }
        // W4: whether the general (composite-key) path can accumulate ONE-PASS: no HAVING and
        // every aggregate is COUNT(*) or an INT-NOT-NULL fusable fold (so no per-group gather /
        // compute_agg_soa is needed and {cnt, folds} fully determine the output).
        bool gen_all_fusable = !has_having;
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && item.agg.kind != AggKind::CountStar &&
                !agg_fusable(item.agg, t)) {
                gen_all_fusable = false;
                break;
            }
        }
        auto compute_group_row = [&](const std::vector<Datum>& keyd,
                                     const std::vector<std::uint32_t>& idxs, ResultRow& out,
                                     bool& keep) -> std::optional<std::string> {
            keep = true;
            if (has_having) {
                if (auto e = having_eval_soa(sel.having, sel.having.root, t, cols, idxs, gcols,
                                             keyd, keep)) {
                    return e;
                }
                if (!keep) {
                    return std::nullopt;
                }
            }
            // Fold each fusable column once for this group (idxs non-empty groups only; the synthetic
            // empty group falls through to compute_agg_soa, which encodes the empty-group result).
            std::vector<IntFold> folds;
            const bool fused = !idxs.empty() && !fuse_cols.empty();
            if (fused) {
                folds.resize(fuse_cols.size());
                for (std::size_t k = 0; k < fuse_cols.size(); ++k)
                    folds[k] = fold_int_col(*cols[fuse_cols[k]], idxs);
            }
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Column) {
                    const std::size_t ci = *t.column_index(item.column);
                    Datum d;
                    for (std::size_t j = 0; j < gcols.size(); ++j) {
                        if (gcols[j] == ci) {
                            d = keyd[j];
                            break;
                        }
                    }
                    out.cells.emplace_back(item.label, d);
                } else if (fused && agg_fusable(item.agg, t)) {
                    const std::size_t ci = *t.column_index(item.agg.column);
                    out.cells.emplace_back(
                        item.label, agg_from_fold(item.agg.kind, folds[static_cast<std::size_t>(col_to_fuse[ci])],
                                                  idxs.size()));
                } else {
                    out.cells.emplace_back(item.label, compute_agg_soa(item.agg, t, cols, idxs));
                }
            }
            return std::nullopt;
        };
        // Emit all groups (in the supplied order — the ordered-map order, so output is sorted by
        // key). The per-group FOLD is the dominant GROUP BY cost (a gather over each group's row
        // indices), so parallelize it ACROSS groups when an executor is set and there is real
        // work: each worker folds a block of groups into its own slot; kept rows are appended in
        // the original order and the lowest-index error wins — byte-identical to serial.
        using GroupRef =
            std::pair<const std::vector<Datum>*, const std::vector<std::uint32_t>*>;
        auto emit_all = [&](const std::vector<GroupRef>& refs) -> std::optional<std::string> {
            const bool par_fold = parallel_executor_ != nullptr &&
                                  parallel_executor_->workers() > 1 &&
                                  refs.size() >= kParallelMinGroups &&
                                  static_cast<std::int64_t>(count) >= kParallelMinRows;
            if (!par_fold) {
                for (const GroupRef& ref : refs) {
                    ResultRow out;
                    bool keep = true;
                    if (auto e = compute_group_row(*ref.first, *ref.second, out, keep)) return e;
                    if (keep) r.rows.push_back(std::move(out));
                }
                return std::nullopt;
            }
            std::vector<ResultRow> outs(refs.size());
            std::vector<unsigned char> keep(refs.size(), 0);
            std::vector<std::optional<std::string>> errs(refs.size());
            const std::size_t nparts =
                std::min<std::size_t>(parallel_executor_->workers(), refs.size());
            const std::size_t per = (refs.size() + nparts - 1) / nparts;
            parallel_executor_->parallel_for(nparts, [&](std::size_t w) {
                const std::size_t lo = w * per;
                const std::size_t hi = std::min(refs.size(), lo + per);
                for (std::size_t i = lo; i < hi; ++i) {
                    bool k = true;
                    if (auto e = compute_group_row(*refs[i].first, *refs[i].second, outs[i], k)) {
                        errs[i] = std::move(e);
                    } else if (k) {
                        keep[i] = 1;
                    }
                }
            });
            for (const auto& e : errs) {
                if (e) return e;  // lowest-index error (deterministic)
            }
            for (std::size_t i = 0; i < refs.size(); ++i) {
                if (keep[i]) r.rows.push_back(std::move(outs[i]));
            }
            return std::nullopt;
        };
        // ONE-PASS INT-KEY HASH-AGGREGATE: a single PLAIN-INT NOT NULL group key + every aggregate
        // fusable (SUM/MIN/MAX/AVG/COUNT over INT NOT NULL) or COUNT(*), no HAVING. The WHERE (if any)
        // is evaluated INLINE per row (no materialized mask) — for a GROUPED aggregate the win is
        // avoiding the per-group idx-vector appends + the scattered fold gather, which dominates here
        // (unlike the ungrouped scalar case, where the two tight SIMD passes win). One sequential pass
        // accumulates each group's {count, per-column IntFold} DIRECTLY. Output sorted by key.
        const bool would_parallelize =
            parallel_executor_ != nullptr && parallel_executor_->workers() > 1 &&
            static_cast<std::int64_t>(count) >= kParallelMinRows;
        if (!has_having && !gs_mode_ && gcols.size() == 1 &&
            t.columns[gcols[0]].type == Type::Int && t.columns[gcols[0]].logical == 0 &&
            !t.columns[gcols[0]].nullable) {
            bool all_ok = true;
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Column) {
                    if (*t.column_index(item.column) != gcols[0]) { all_ok = false; break; }
                } else if (item.agg.kind != AggKind::CountStar && !agg_fusable(item.agg, t)) {
                    all_ok = false;
                    break;
                }
            }
            if (all_ok) {
                const std::int64_t* gk = cols[gcols[0]]->ints.data();
                const std::size_t nf = fuse_cols.size();
                std::vector<const std::int64_t*> ap(nf);
                for (std::size_t k = 0; k < nf; ++k) ap[k] = cols[fuse_cols[k]]->ints.data();
                std::vector<std::int64_t> keys;
                std::vector<std::int64_t> cnt;
                std::vector<IntFold> folds;  // G*nf flat (group g, col k => folds[g*nf+k])
                // Accumulate rows [lo,hi) into a LOCAL open-addressing hash accumulator
                // (keys/cnt/folds by reference). Shared by the serial and per-worker paths.
                auto accumulate = [&](std::uint32_t lo, std::uint32_t hi,
                                      std::vector<std::int64_t>& lkeys,
                                      std::vector<std::int64_t>& lcnt,
                                      std::vector<IntFold>& lfolds) {
                    std::size_t cap = 256;  // power of 2
                    std::vector<std::int32_t> slots(cap, -1);
                    const std::hash<std::int64_t> H;
                    auto reinsert = [&](std::int32_t gi) {
                        std::size_t p = H(lkeys[static_cast<std::size_t>(gi)]) & (cap - 1);
                        while (slots[p] >= 0) p = (p + 1) & (cap - 1);
                        slots[p] = gi;
                    };
                    for (std::uint32_t r = lo; r < hi; ++r) {
                        if (has_filter && !passes(r)) continue;  // inline WHERE — no gather
                        const std::int64_t k = gk[r];
                        std::size_t p = H(k) & (cap - 1);
                        std::int32_t gi = -1;
                        while (true) {
                            if (slots[p] < 0) {
                                if ((lkeys.size() + 1) * 10 >= cap * 7) {  // grow + rehash at 0.7
                                    cap *= 2;
                                    slots.assign(cap, -1);
                                    for (std::int32_t g = 0; g < static_cast<std::int32_t>(lkeys.size()); ++g)
                                        reinsert(g);
                                    p = H(k) & (cap - 1);
                                    continue;
                                }
                                gi = static_cast<std::int32_t>(lkeys.size());
                                slots[p] = gi;
                                lkeys.push_back(k);
                                lcnt.push_back(0);
                                lfolds.insert(lfolds.end(), nf, IntFold{0, INT64_MAX, INT64_MIN});
                                break;
                            }
                            if (lkeys[static_cast<std::size_t>(slots[p])] == k) { gi = slots[p]; break; }
                            p = (p + 1) & (cap - 1);
                        }
                        ++lcnt[static_cast<std::size_t>(gi)];
                        if (nf > 0) {
                            IntFold* f = &lfolds[static_cast<std::size_t>(gi) * nf];
                            for (std::size_t kk = 0; kk < nf; ++kk) {
                                const std::int64_t x = ap[kk][r];
                                f[kk].sum = wrap_add(f[kk].sum, x);
                                f[kk].mn = f[kk].mn < x ? f[kk].mn : x;
                                f[kk].mx = f[kk].mx > x ? f[kk].mx : x;
                            }
                        }
                    }
                };
                // W4: PARALLEL ONE-PASS INT-key hash aggregate. Each worker accumulates its row
                // range into a LOCAL hash accumulator; the locals then merge BY KEY into the final
                // one. Byte-identical to serial (wrap_add assoc+commut; MIN/MAX/COUNT order-indep),
                // and safe for any INT key distribution (no array-sizing assumption). Replaces the
                // generic two-pass parallel gather for this shape.
                if (would_parallelize) {
                    const std::size_t W =
                        std::min<std::size_t>(parallel_executor_->workers(), count);
                    std::vector<std::vector<std::int64_t>> lkeys(W), lcnt(W);
                    std::vector<std::vector<IntFold>> lfolds(W);
                    const std::uint32_t per = static_cast<std::uint32_t>((count + W - 1) / W);
                    parallel_executor_->parallel_for(W, [&](std::size_t w) {
                        const std::uint32_t lo = static_cast<std::uint32_t>(w) * per;
                        const std::uint32_t hi = std::min<std::uint32_t>(count, lo + per);
                        accumulate(lo, hi, lkeys[w], lcnt[w], lfolds[w]);
                    });
                    // Merge by key into the final keys/cnt/folds (deterministic worker order 0..W-1).
                    std::size_t mcap = 256;
                    std::vector<std::int32_t> mslots(mcap, -1);
                    const std::hash<std::int64_t> H;
                    auto mreinsert = [&](std::int32_t gi) {
                        std::size_t p = H(keys[static_cast<std::size_t>(gi)]) & (mcap - 1);
                        while (mslots[p] >= 0) p = (p + 1) & (mcap - 1);
                        mslots[p] = gi;
                    };
                    for (std::size_t w = 0; w < W; ++w) {
                        for (std::size_t g = 0; g < lkeys[w].size(); ++g) {
                            const std::int64_t k = lkeys[w][g];
                            std::size_t p = H(k) & (mcap - 1);
                            std::int32_t gi = -1;
                            while (true) {
                                if (mslots[p] < 0) {
                                    if ((keys.size() + 1) * 10 >= mcap * 7) {
                                        mcap *= 2;
                                        mslots.assign(mcap, -1);
                                        for (std::int32_t gg = 0; gg < static_cast<std::int32_t>(keys.size()); ++gg)
                                            mreinsert(gg);
                                        p = H(k) & (mcap - 1);
                                        continue;
                                    }
                                    gi = static_cast<std::int32_t>(keys.size());
                                    mslots[p] = gi;
                                    keys.push_back(k);
                                    cnt.push_back(0);
                                    folds.insert(folds.end(), nf, IntFold{0, INT64_MAX, INT64_MIN});
                                    break;
                                }
                                if (keys[static_cast<std::size_t>(mslots[p])] == k) { gi = mslots[p]; break; }
                                p = (p + 1) & (mcap - 1);
                            }
                            cnt[static_cast<std::size_t>(gi)] += lcnt[w][g];
                            for (std::size_t kk = 0; kk < nf; ++kk) {
                                IntFold& d = folds[static_cast<std::size_t>(gi) * nf + kk];
                                const IntFold& s = lfolds[w][g * nf + kk];
                                d.sum = wrap_add(d.sum, s.sum);
                                if (s.mn < d.mn) d.mn = s.mn;
                                if (s.mx > d.mx) d.mx = s.mx;
                            }
                        }
                    }
                } else {
                    accumulate(0, count, keys, cnt, folds);
                }
                std::vector<std::size_t> order(keys.size());
                for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(),
                          [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
                for (const std::size_t oi : order) {
                    ResultRow out;
                    for (const SelectItem& item : sel.items) {
                        if (item.kind == SelectItemKind::Column) {
                            out.cells.emplace_back(item.label, Datum::make_int(keys[oi]));
                        } else if (item.agg.kind == AggKind::CountStar) {
                            out.cells.emplace_back(item.label,
                                                   Datum::make_int(cnt[oi]));
                        } else {
                            const std::size_t ci = *t.column_index(item.agg.column);
                            out.cells.emplace_back(
                                item.label,
                                agg_from_fold(item.agg.kind,
                                              folds[oi * nf + static_cast<std::size_t>(col_to_fuse[ci])],
                                              static_cast<std::size_t>(cnt[oi])));
                        }
                    }
                    r.rows.push_back(std::move(out));
                }
                apply_distinct(sel, r.rows);
                if (auto e = apply_order_by_labels(sel, r.rows)) return ExecResult::failure(*e);
                apply_limit(sel, r.rows);
                r.affected = r.rows.size();
                return r;
            }
        }
        // ONE-PASS TEXT-KEY HASH-AGGREGATE via the cached dictionary: a single NOT NULL TEXT group
        // key + all-fusable aggregates, unfiltered (so codes align with the full concat), no HAVING.
        // The dictionary's codes are DENSE (0..G-1, cached flush-gen-tagged), so each group's
        // {count, per-column IntFold} accumulates by DIRECT code index — no hash, no idx vectors, no
        // gather. Emit sorted by the string value (== the byte-identical group order for NOT NULL TEXT).
        if (!has_filter && !has_having && !gs_mode_ && gcols.size() == 1 &&
            t.columns[gcols[0]].type == Type::Text && t.columns[gcols[0]].logical == 0 &&
            !t.columns[gcols[0]].nullable) {
            bool all_ok = true;
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Column) {
                    if (*t.column_index(item.column) != gcols[0]) { all_ok = false; break; }
                } else if (item.agg.kind != AggKind::CountStar && !agg_fusable(item.agg, t)) {
                    all_ok = false;
                    break;
                }
            }
            if (all_ok) {
                const TextDictEntry& dict =
                    col_text_dict_cached(t, static_cast<std::uint32_t>(gcols[0]));
                const std::size_t G = dict.values.size();
                const std::size_t nf = fuse_cols.size();
                std::vector<const std::int64_t*> ap(nf);
                for (std::size_t k = 0; k < nf; ++k) ap[k] = cols[fuse_cols[k]]->ints.data();
                std::vector<std::int64_t> cnt(G, 0);
                std::vector<IntFold> folds(G * nf, IntFold{0, INT64_MAX, INT64_MIN});
                // W4: PARALLEL ONE-PASS dict-code accumulation. Each worker folds a contiguous
                // row range into a LOCAL {cnt[G], folds[G*nf]}, then the locals merge element-wise
                // in worker order. Byte-identical to the serial accumulation: wrap_add is
                // associative+commutative (two's-complement), MIN/MAX/COUNT are order-independent,
                // so the merged per-group aggregate does not depend on the partition. This replaces
                // the generic two-pass parallel gather (build idx vectors -> scattered fold) which
                // was ~3x SLOWER than serial on a low-cardinality TEXT GROUP BY (profiled).
                if (would_parallelize) {
                    const std::size_t W =
                        std::min<std::size_t>(parallel_executor_->workers(), count);
                    std::vector<std::vector<std::int64_t>> lc(W, std::vector<std::int64_t>(G, 0));
                    std::vector<std::vector<IntFold>> lf(
                        W, std::vector<IntFold>(G * nf, IntFold{0, INT64_MAX, INT64_MIN}));
                    const std::uint32_t per = static_cast<std::uint32_t>((count + W - 1) / W);
                    parallel_executor_->parallel_for(W, [&](std::size_t w) {
                        const std::uint32_t lo = static_cast<std::uint32_t>(w) * per;
                        const std::uint32_t hi = std::min<std::uint32_t>(count, lo + per);
                        std::int64_t* c = lc[w].data();
                        IntFold* fbase = lf[w].data();
                        for (std::uint32_t rw = lo; rw < hi; ++rw) {
                            const std::size_t g = dict.codes[rw];
                            ++c[g];
                            if (nf > 0) {
                                IntFold* f = &fbase[g * nf];
                                for (std::size_t kk = 0; kk < nf; ++kk) {
                                    const std::int64_t x = ap[kk][rw];
                                    f[kk].sum = wrap_add(f[kk].sum, x);
                                    f[kk].mn = f[kk].mn < x ? f[kk].mn : x;
                                    f[kk].mx = f[kk].mx > x ? f[kk].mx : x;
                                }
                            }
                        }
                    });
                    for (std::size_t w = 0; w < W; ++w) {
                        for (std::size_t g = 0; g < G; ++g) cnt[g] += lc[w][g];
                        for (std::size_t j = 0; j < G * nf; ++j) {
                            IntFold& d = folds[j];
                            const IntFold& s = lf[w][j];
                            d.sum = wrap_add(d.sum, s.sum);
                            if (s.mn < d.mn) d.mn = s.mn;
                            if (s.mx > d.mx) d.mx = s.mx;
                        }
                    }
                } else {
                    for (std::uint32_t rw = 0; rw < count; ++rw) {
                        const std::size_t g = dict.codes[rw];  // dense code == group index (no hash)
                        ++cnt[g];
                        if (nf > 0) {
                            IntFold* f = &folds[g * nf];
                            for (std::size_t kk = 0; kk < nf; ++kk) {
                                const std::int64_t x = ap[kk][rw];
                                f[kk].sum = wrap_add(f[kk].sum, x);
                                f[kk].mn = f[kk].mn < x ? f[kk].mn : x;
                                f[kk].mx = f[kk].mx > x ? f[kk].mx : x;
                            }
                        }
                    }
                }
                std::vector<std::size_t> order(G);
                for (std::size_t i = 0; i < G; ++i) order[i] = i;
                std::sort(order.begin(), order.end(),
                          [&](std::size_t a, std::size_t b) { return dict.values[a] < dict.values[b]; });
                for (const std::size_t oi : order) {
                    if (cnt[oi] == 0) continue;  // a dict code with no live row (defensive)
                    ResultRow out;
                    for (const SelectItem& item : sel.items) {
                        if (item.kind == SelectItemKind::Column) {
                            out.cells.emplace_back(item.label, Datum::make_text(dict.values[oi]));
                        } else if (item.agg.kind == AggKind::CountStar) {
                            out.cells.emplace_back(item.label, Datum::make_int(cnt[oi]));
                        } else {
                            const std::size_t ci = *t.column_index(item.agg.column);
                            out.cells.emplace_back(
                                item.label, agg_from_fold(
                                                item.agg.kind,
                                                folds[oi * nf + static_cast<std::size_t>(col_to_fuse[ci])],
                                                static_cast<std::size_t>(cnt[oi])));
                        }
                    }
                    r.rows.push_back(std::move(out));
                }
                apply_distinct(sel, r.rows);
                if (auto e = apply_order_by_labels(sel, r.rows)) return ExecResult::failure(*e);
                apply_limit(sel, r.rows);
                r.affected = r.rows.size();
                return r;
            }
        }
        // RAW-INT-KEY fast path: a single NOT NULL INT group column keys the map by the raw
        // int64 — no per-row group_key_field STRING + no vector<string> key. The map is ordered
        // by int, which equals the order-preserving group_key_field order, so output order is
        // byte-identical. (The common GROUP BY <int col> analytical shape.)
        // Parallelize the grouping pass when an executor is injected and there are enough rows
        // to amortize dispatch — partial group maps merged in a fixed order (byte-identical).
        const bool par_group =
            parallel_executor_ != nullptr && parallel_executor_->workers() > 1 &&
            static_cast<std::int64_t>(count) >= kParallelMinRows;
        if (gcols.size() == 1 && t.columns[gcols[0]].type == Type::Int &&
            !t.columns[gcols[0]].nullable) {
            const std::size_t gc = gcols[0];
            std::map<std::int64_t, std::pair<std::vector<std::uint32_t>, std::vector<Datum>>> ig;
            std::vector<std::pair<std::vector<Datum>, std::vector<std::uint32_t>>> hg;  // open-addr
            if (par_group) {
                build_groups_parallel(
                    ig, count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->ints[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_int(cols[gc]->ints[rr])};
                    });
            } else {
                hg = hash_group_collect<std::int64_t>(  // open-addressing (A1) — sorted by key
                    count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->ints[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_int(cols[gc]->ints[rr])};
                    });
            }
            std::vector<GroupRef> refs;
            if (par_group) {
                refs.reserve(ig.size());
                for (const auto& [k, slot] : ig) {
                    (void)k;
                    refs.emplace_back(&slot.second, &slot.first);
                }
            } else {
                refs.reserve(hg.size());
                for (const auto& g : hg) refs.emplace_back(&g.first, &g.second);
            }
            if (auto e = emit_all(refs)) {
                return ExecResult::failure(*e);
            }
        } else if (gcols.size() == 1 && t.columns[gcols[0]].type == Type::Text &&
                   !t.columns[gcols[0]].nullable) {
            // RAW-STRING-KEY fast path: a single NOT NULL TEXT group column keys the map by the
            // raw text value — no group_key_field re-encode + no vector<string> wrapper. Ordered
            // by the raw bytes == group_key_field order for non-null TEXT (the type/null prefix
            // is constant), so output order is byte-identical.
            const std::size_t gc = gcols[0];
            std::map<std::string, std::pair<std::vector<std::uint32_t>, std::vector<Datum>>> tg;
            std::vector<std::pair<std::vector<Datum>, std::vector<std::uint32_t>>> hg;  // open-addr
            // The DICTIONARY path (group by INT dict codes) is the fast serial grouping for TEXT
            // keys and BEATS parallel per-row STRING hashing — forcing par_group here caused a
            // measured 3x REGRESSION (groupby_region 0.34ms serial dict -> 1.02ms parallel
            // string-hash at 4 workers), because string hashing + per-thread string maps + a
            // string-map merge cost far more than the dict path saves. So prefer the dict path
            // whenever it applies (the full-concat / non-survivor case); only fall back to the
            // parallel string grouping when the dict codes don't align (a zone-skipped subset).
            const bool use_dict = !use_survivors;
            if (use_dict) {
                // A2 — DICTIONARY path: codes align with the full concat (non-survivor only), so
                // group by INT codes (direct/fast) instead of per-row string hashing, then re-sort
                // the (few) groups by the STRING value to match the string-ordered output.
                const TextDictEntry& dict = col_text_dict_cached(t, static_cast<std::uint32_t>(gc));
                hg = hash_group_collect<std::int64_t>(
                    count, has_filter, passes,
                    [&](std::uint32_t rr) { return static_cast<std::int64_t>(dict.codes[rr]); },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_text(dict.values[dict.codes[rr]])};
                    });
                std::sort(hg.begin(), hg.end(), [](const auto& a, const auto& b) {
                    return a.first[0].s < b.first[0].s;  // string order (codes are insertion order)
                });
            } else if (par_group) {
                build_groups_parallel(
                    tg, count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->texts[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_text(cols[gc]->texts[rr])};
                    });
            } else {
                hg = hash_group_collect<std::string>(  // open-addressing (A1) — sorted by key
                    count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->texts[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_text(cols[gc]->texts[rr])};
                    });
            }
            std::vector<GroupRef> refs;
            if (!use_dict && par_group) {  // only the parallel string path fills `tg`
                refs.reserve(tg.size());
                for (const auto& [k, slot] : tg) {
                    (void)k;
                    refs.emplace_back(&slot.second, &slot.first);
                }
            } else {
                refs.reserve(hg.size());
                for (const auto& g : hg) refs.emplace_back(&g.first, &g.second);
            }
            if (auto e = emit_all(refs)) {
                return ExecResult::failure(*e);
            }
        } else if (par_group && gen_all_fusable && !gcols.empty()) {
            // W4: PARALLEL ONE-PASS general (composite-key) GROUP BY. Each worker accumulates
            // {cnt, folds} per composite key into a LOCAL ordered map; the locals merge BY KEY.
            // Byte-identical to the two-pass gather: the per-group folds are the same aggregate,
            // and wrap_add/MIN/MAX/COUNT are reassociation-safe, so the partition does not matter.
            // Handles multi-column and NULLABLE keys (group_key_field encodes the key + NULLs).
            auto key_of = [&](std::uint32_t rr) {
                std::vector<std::string> key;
                key.reserve(gcols.size());
                for (const std::size_t g : gcols) key.push_back(group_key_field(cols[g]->at(rr)));
                return key;
            };
            auto key_datum_of = [&](std::uint32_t rr) {
                std::vector<Datum> kd;
                kd.reserve(gcols.size());
                for (const std::size_t g : gcols) kd.push_back(cols[g]->at(rr));
                return kd;
            };
            const std::size_t nf = fuse_cols.size();
            std::vector<const std::int64_t*> ap(nf);
            for (std::size_t k = 0; k < nf; ++k) ap[k] = cols[fuse_cols[k]]->ints.data();
            struct GAcc {
                std::vector<Datum> keyd;
                std::int64_t cnt = 0;
                std::vector<IntFold> folds;
            };
            const std::size_t W = std::min<std::size_t>(parallel_executor_->workers(), count);
            std::vector<std::map<std::vector<std::string>, GAcc>> parts(W);
            const std::uint32_t per = static_cast<std::uint32_t>((count + W - 1) / W);
            parallel_executor_->parallel_for(W, [&](std::size_t w) {
                const std::uint32_t lo = static_cast<std::uint32_t>(w) * per;
                const std::uint32_t hi = std::min<std::uint32_t>(count, lo + per);
                auto& m = parts[w];
                for (std::uint32_t rr = lo; rr < hi; ++rr) {
                    if (has_filter && !passes(rr)) continue;
                    GAcc& a = m[key_of(rr)];
                    if (a.keyd.empty()) {
                        a.keyd = key_datum_of(rr);
                        a.folds.assign(nf, IntFold{0, INT64_MAX, INT64_MIN});
                    }
                    ++a.cnt;
                    for (std::size_t kk = 0; kk < nf; ++kk) {
                        const std::int64_t x = ap[kk][rr];
                        IntFold& f = a.folds[kk];
                        f.sum = wrap_add(f.sum, x);
                        f.mn = f.mn < x ? f.mn : x;
                        f.mx = f.mx > x ? f.mx : x;
                    }
                }
            });
            std::map<std::vector<std::string>, GAcc> merged;
            for (std::size_t w = 0; w < W; ++w) {
                for (auto& [k, a] : parts[w]) {
                    GAcc& d = merged[k];
                    if (d.keyd.empty()) {
                        d.keyd = std::move(a.keyd);
                        d.folds = std::move(a.folds);
                        d.cnt = a.cnt;
                    } else {
                        d.cnt += a.cnt;
                        for (std::size_t kk = 0; kk < nf; ++kk) {
                            IntFold& x = d.folds[kk];
                            const IntFold& s = a.folds[kk];
                            x.sum = wrap_add(x.sum, s.sum);
                            if (s.mn < x.mn) x.mn = s.mn;
                            if (s.mx > x.mx) x.mx = s.mx;
                        }
                    }
                }
            }
            for (auto& [k, a] : merged) {  // std::map => sorted by composite key == output order
                (void)k;
                ResultRow out;
                for (const SelectItem& item : sel.items) {
                    if (item.kind == SelectItemKind::Column) {
                        const std::size_t ci = *t.column_index(item.column);
                        Datum d;
                        for (std::size_t j = 0; j < gcols.size(); ++j) {
                            if (gcols[j] == ci) { d = a.keyd[j]; break; }
                        }
                        out.cells.emplace_back(item.label, d);
                    } else if (item.agg.kind == AggKind::CountStar) {
                        out.cells.emplace_back(item.label, Datum::make_int(a.cnt));
                    } else {
                        const std::size_t ci = *t.column_index(item.agg.column);
                        out.cells.emplace_back(
                            item.label,
                            agg_from_fold(item.agg.kind,
                                          a.folds[static_cast<std::size_t>(col_to_fuse[ci])], a.cnt));
                    }
                }
                r.rows.push_back(std::move(out));
            }
        } else {
            // GENERAL path: group-key tuple (group_key_field). Key buffer reused across rows.
            std::map<std::vector<std::string>,
                     std::pair<std::vector<std::uint32_t>, std::vector<Datum>>>
                groups;
            auto key_of = [&](std::uint32_t rr) {
                std::vector<std::string> key;
                key.reserve(gcols.size());
                for (const std::size_t g : gcols) key.push_back(group_key_field(cols[g]->at(rr)));
                return key;
            };
            auto key_datum_of = [&](std::uint32_t rr) {
                std::vector<Datum> kd;
                if (!gcols.empty()) {
                    kd.reserve(gcols.size());
                    for (const std::size_t g : gcols) kd.push_back(cols[g]->at(rr));
                }
                return kd;
            };
            // gcols.empty() (ungrouped + filter/having here) => one '{}' group; build_groups_
            // parallel handles it (all rows share the empty key), but a parallel split of zero
            // surviving rows leaves `groups` empty, the same as serial — the guard below re-adds
            // the single empty group either way.
            if (par_group) {
                build_groups_parallel(groups, count, has_filter, passes, key_of, key_datum_of);
            } else {
                for (std::uint32_t rr = 0; rr < count; ++rr) {
                    if (has_filter && !passes(rr)) {
                        continue;
                    }
                    auto& slot = groups[key_of(rr)];
                    if (slot.second.empty() && !gcols.empty()) {
                        slot.second = key_datum_of(rr);
                    }
                    slot.first.push_back(rr);
                }
            }
            if (gcols.empty() && groups.empty()) {  // ungrouped over zero rows => ONE row
                groups[std::vector<std::string>{}];
            }
            std::vector<GroupRef> refs;
            refs.reserve(groups.size());
            for (const auto& [k, slot] : groups) {
                (void)k;
                refs.emplace_back(&slot.second, &slot.first);
            }
            if (auto e = emit_all(refs)) {
                return ExecResult::failure(*e);
            }
        }
        if (soa_overflow_) {  // K1: a 128-bit SUM overflowed mid-fold => let the row path raise the error
            return std::nullopt;
        }
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by_labels(sel, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    ExecResult exec_insert(const InsertStmt& ins) {
        const Table* t = catalog_.find(ins.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + ins.table + "'");
        }
        // F6: a working copy of the AUTO_INCREMENT counter, advanced as rows are built and written
        // back to the catalog (+ persisted) after a successful commit.
        std::int64_t auto_next = t->next_auto_id;
        std::uint64_t uuid_next = t->next_uuid;  // F9c: working copy of the gen_uuid() counter

        // F2: UNIQUE constraint enforcement. Pre-scan the table once to collect the existing non-NULL
        // values of every UNIQUE column; staging then rejects a row that repeats one (existing OR
        // earlier in this batch). NULLs are allowed to repeat (SQL UNIQUE permits multiple NULLs).
        std::vector<std::size_t> unique_cols;
        for (std::size_t c = 0; c < t->columns.size(); ++c) {
            if (t->columns[c].unique && c != t->pk_index) unique_cols.push_back(c);
        }
        std::vector<std::set<std::string>> seen_unique(unique_cols.size());
        // E5: UNIQUE INDEXES — enforce like a UNIQUE column, but over the index's (composite) tuple.
        std::vector<const Index*> uniq_indexes;
        for (const Index& ix : t->indexes) if (ix.unique) uniq_indexes.push_back(&ix);
        std::vector<std::set<std::string>> seen_uidx(uniq_indexes.size());
        if (!unique_cols.empty() || !uniq_indexes.empty()) {
            SelectStmt scan;
            scan.table = t->name;
            std::vector<std::vector<Datum>> existing;
            if (auto e = scan_table(*t, scan, existing)) {
                return ExecResult::failure(*e);
            }
            for (const std::vector<Datum>& er : existing) {
                for (std::size_t u = 0; u < unique_cols.size(); ++u) {
                    const Datum& d = er[unique_cols[u]];
                    if (!d.is_null) seen_unique[u].insert(group_key_field(d));
                }
                for (std::size_t u = 0; u < uniq_indexes.size(); ++u) {
                    bool uskip = true;
                    std::string ukey;
                    if (index_unique_key(*t, *uniq_indexes[u], er, uskip, ukey)) continue;  // J2 (existing row)
                    if (!uskip) seen_uidx[u].insert(ukey);
                }
            }
        }
        // Build ONE row from a value tuple: a named column is set (type-checked + NULL re-typed);
        // an omitted column defaults to NULL iff NULLABLE, else the INSERT is rejected (NOT NULL
        // REQUIRES a value; the PK is NOT NULL so omitting it always errors). Shared by every row
        // of a multi-row INSERT (D6).
        auto build_row = [&](const std::vector<Datum>& vals,
                             std::vector<Datum>& row) -> std::optional<std::string> {
            row.assign(t->columns.size(), Datum{});
            std::vector<bool> set(t->columns.size(), false);
            for (std::size_t k = 0; k < ins.columns.size(); ++k) {
                const auto idx = t->column_index(ins.columns[k]);
                if (!idx) {
                    return "unknown column '" + ins.columns[k] + "' in table '" + ins.table + "'";
                }
                if (set[*idx]) {
                    return "column '" + ins.columns[k] + "' specified more than once";
                }
                Datum d;
                if (auto e = coerce(t->columns[*idx], vals[k], d)) {
                    return e;
                }
                row[*idx] = d;
                set[*idx] = true;
                // F6: an explicit value for an AUTO_INCREMENT column advances the counter past it.
                if (t->columns[*idx].auto_increment && !d.is_null && d.type == Type::Int &&
                    d.i >= auto_next) {
                    auto_next = d.i + 1;
                }
            }
            for (std::size_t c = 0; c < t->columns.size(); ++c) {
                if (set[c]) {
                    continue;
                }
                if (t->columns[c].dropped) {  // E1: a dropped slot is written as NULL (keeps alignment)
                    row[c] = Datum::make_null(t->columns[c].type);
                    continue;
                }
                // F6: an omitted AUTO_INCREMENT column gets the next monotonic id.
                if (t->columns[c].auto_increment) {
                    row[c] = Datum::make_int(auto_next++);
                    continue;
                }
                // F9c: an omitted DEFAULT gen_uuid() column gets a deterministic v4-shaped id.
                if (t->columns[c].uuid_default) {
                    Datum d = Datum::make_text(format_uuid(t->id, uuid_next++));
                    d.logical = 4;
                    row[c] = d;
                    continue;
                }
                // F4: an omitted column with a DEFAULT takes the default value.
                if (t->columns[c].has_default) {
                    row[c] = t->columns[c].type == Type::Int
                                 ? Datum::make_int(t->columns[c].default_i)
                                 : Datum::make_text(t->columns[c].default_s);
                    continue;
                }
                if (!t->columns[c].nullable) {
                    return "INSERT omits NOT NULL column '" + t->columns[c].name +
                           "' (provide a value, or declare a DEFAULT)";
                }
                row[c] = Datum::make_null(t->columns[c].type);  // omitted nullable => NULL
            }
            return std::nullopt;
        };

        // D6: a multi-row INSERT is ATOMIC — build EVERY row, detect dup PKs (against the live
        // store AND against PKs earlier in THIS batch), accumulate all writes, then ONE commit.
        // A single-row INSERT is the 1-iteration case → byte-identical to the pre-D6 path.
        std::vector<std::vector<Datum>> rows;
        rows.reserve(1 + ins.more_rows.size());
        std::vector<std::pair<Key, Value>> writes;
        std::set<std::string> batch_pks;  // encode_key of PKs seen in this batch (within-batch dup)
        std::size_t conflict_updated = 0;  // G2: rows updated/skipped by ON CONFLICT (not new inserts)
        auto stage = [&](const std::vector<Datum>& vals) -> std::optional<std::string> {
            std::vector<Datum> row;
            if (auto e = build_row(vals, row)) {
                return e;
            }
            const Datum& pk = row[t->pk_index];
            const Key pk_key = t->composite_pk() ? encode_key_row(*t, row) : encode_key(*t, pk);  // F1
            if (!batch_pks.insert(pk_key).second) {
                return "duplicate primary key within the INSERT batch (table '" + ins.table + "')";
            }
            bool exists;
            std::vector<Datum> old_row;
            if (t->columnar) {
                auto rr = read_columnar_row(*t, pk);
                exists = rr.has_value();
                if (exists) old_row = std::move(*rr);
            } else {
                const ReadResult existing = read_committed(pk_key);
                exists = existing.has_value() && !is_tombstone(*existing);
                if (exists) old_row = decode_row(*t, pk_key, *existing);
            }
            if (exists) {
                // G2 UPSERT: a dup PK is an error (default), a skip (DO NOTHING), or an update.
                if (ins.on_conflict == InsertStmt::OnConflict::Error) {
                    return "duplicate primary key in table '" + ins.table + "' (row already exists)";
                }
                if (ins.on_conflict == InsertStmt::OnConflict::Nothing) {
                    return std::nullopt;  // leave the existing row untouched
                }
                // DO UPDATE SET: apply each col=literal to the existing row, re-emit (retire the old
                // index entries, write the new row + new index entries) in this same atomic batch.
                std::vector<Datum> new_row = old_row;
                for (const auto& [c, v] : ins.conflict_updates) {
                    const auto ci = t->column_index(c);
                    if (!ci) return "unknown column '" + c + "' in ON CONFLICT DO UPDATE";
                    if (*ci == t->pk_index) return "cannot UPDATE the primary key in ON CONFLICT";
                    Datum d;
                    if (auto e = coerce(t->columns[*ci], v, d)) return e;
                    new_row[*ci] = d;
                }
                if (auto e = validate_index_exprs(*t, new_row)) return e;  // J2
                index_writes_for_row(*t, old_row, /*tombstone=*/true, writes);
                emit_row_writes(*t, new_row, writes);
                index_writes_for_row(*t, new_row, /*tombstone=*/false, writes);
                ++conflict_updated;
                return std::nullopt;
            }
            // F5: a NEW row must satisfy every CHECK constraint.
            if (auto e = eval_checks(*t, row)) {
                return e;
            }
            // J2: a NEW row must be soundly placeable in every EXPRESSION index (its expression must
            // evaluate and yield the recorded physical type) — else the index path could diverge.
            if (auto e = validate_index_exprs(*t, row)) {
                return e;
            }
            // F3: a NEW row must satisfy every FOREIGN KEY (referenced parent PK exists).
            if (auto e = eval_foreign_keys(*t, row)) {
                return e;
            }
            // F2: a NEW row must not repeat a UNIQUE column value (existing or earlier in the batch).
            for (std::size_t u = 0; u < unique_cols.size(); ++u) {
                const Datum& d = row[unique_cols[u]];
                if (d.is_null) continue;  // NULLs may repeat
                if (!seen_unique[u].insert(group_key_field(d)).second) {
                    return "UNIQUE constraint violated on column '" +
                           t->columns[unique_cols[u]].name + "'";
                }
            }
            // E5: a NEW row must not repeat a UNIQUE INDEX tuple (a NULL in any covered column exempts).
            for (std::size_t u = 0; u < uniq_indexes.size(); ++u) {
                bool uskip = true;
                std::string ukey;
                if (auto e = index_unique_key(*t, *uniq_indexes[u], row, uskip, ukey)) return e;  // J2
                if (uskip) continue;
                if (!seen_uidx[u].insert(ukey).second)
                    return "UNIQUE index '" + uniq_indexes[u]->name + "' violated";
            }
            emit_row_writes(*t, row, writes);
            index_writes_for_row(*t, row, /*tombstone=*/false, writes);
            rows.push_back(std::move(row));
            return std::nullopt;
        };
        // D5: INSERT ... SELECT — materialise the source query's rows as value tuples (the SELECT
        // output arity must match the named columns), then stage them through the SAME atomic path
        // as VALUES. The SELECT reads a CONSISTENT snapshot before any write (no self-feedback).
        if (ins.select_source) {
            const ExecResult src = exec_select(*ins.select_source);
            if (!src.ok) {
                return src;  // surface the SELECT's parse/exec error verbatim
            }
            for (const ResultRow& srow : src.rows) {
                if (srow.cells.size() != ins.columns.size()) {
                    return ExecResult::failure(
                        "INSERT ... SELECT: the query produces " +
                        std::to_string(srow.cells.size()) + " columns but " +
                        std::to_string(ins.columns.size()) + " were named");
                }
                std::vector<Datum> vals;
                vals.reserve(srow.cells.size());
                for (const auto& [label, d] : srow.cells) {
                    (void)label;
                    vals.push_back(d);
                }
                if (auto e = stage(vals)) {
                    return ExecResult::failure(*e);
                }
            }
        } else {
            if (auto e = stage(ins.values)) {
                return ExecResult::failure(*e);
            }
            for (const std::vector<Datum>& extra : ins.more_rows) {
                if (auto e = stage(extra)) {
                    return ExecResult::failure(*e);
                }
            }
        }
        // ATOMIC: every row's materialisation + secondary-index entries in ONE txn (index/columns
        // can never lag the rows after a committed INSERT; a multi-row INSERT is all-or-nothing).
        commit_writes(writes);
        if (Table* mt = catalog_.find_mut(ins.table)) {
            // F6: advance + durably persist the AUTO_INCREMENT counter so a restart never re-issues
            // an id (persist only when it actually moved — keeps the catalog write off the no-auto path).
            if (auto_next != mt->next_auto_id || uuid_next != mt->next_uuid) {
                mt->next_auto_id = auto_next;
                mt->next_uuid = uuid_next;  // F9c
                persist_schema(ins.table);
            }
            // G2: a columnar ON CONFLICT DO UPDATE wrote new delta rows — mark the delta dirty.
            if (mt->columnar && conflict_updated > 0) {
                mt->delta_dirty = true;
                mt->delta_count += conflict_updated;
            }
            for (const std::vector<Datum>& row : rows) {
                if (mt->columnar) {
                    mt->delta_dirty = true;
                    ++mt->delta_count;
                }
                ++mt->row_count;
                if (mt->col_stats.size() == row.size()) {
                    for (std::size_t c = 0; c < row.size(); ++c) {
                        if (row[c].type != Type::Int || row[c].is_null) {
                            continue;
                        }
                        Table::ColStat& cs = mt->col_stats[c];
                        if (!cs.seen) {
                            cs.seen = true;
                            cs.lo = row[c].i;
                            cs.hi = row[c].i;
                        } else {
                            cs.lo = row[c].i < cs.lo ? row[c].i : cs.lo;
                            cs.hi = row[c].i > cs.hi ? row[c].i : cs.hi;
                        }
                    }
                }
            }
        }
        maybe_auto_flush(ins.table);
        ExecResult r;
        r.affected = static_cast<std::int64_t>(rows.size() + conflict_updated);  // inserts + upserts
        return r;
    }

    // --- UPDATE ---------------------------------------------------------------
    ExecResult exec_update(const UpdateStmt& up) {
        const Table* t = catalog_.find(up.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + up.table + "'");
        }
        if (t->composite_pk()) {  // F1: composite-PK UPDATE addressing is a follow-on
            return ExecResult::failure("UPDATE on a composite-PRIMARY-KEY table is not supported");
        }
        if (up.where_column != t->pk().name) {
            return ExecResult::failure(
                "UPDATE WHERE must filter on the primary key '" + t->pk().name +
                "' in v1 (got '" + up.where_column + "')");
        }
        const auto set_idx = t->column_index(up.set_column);
        if (!set_idx) {
            return ExecResult::failure("unknown column '" + up.set_column +
                                       "' in table '" + up.table + "'");
        }
        if (*set_idx == t->pk_index) {
            return ExecResult::failure("cannot UPDATE the primary key '" +
                                       t->pk().name + "' in v1");
        }
        // Type-check the WHERE pk value + the SET value.
        Datum pk;
        if (auto e = coerce(t->pk(), up.where_value, pk)) {
            return ExecResult::failure(*e);
        }
        Datum sv;
        if (auto e = coerce(t->columns[*set_idx], up.set_value, sv)) {
            return ExecResult::failure(*e);
        }
        const std::size_t col = *set_idx;
        // INCREMENTAL: read the prior committed row over the verified read path (row KV
        // in row mode, or assembled from the column families in columnar), set the
        // column in the Engine, then commit the new row.
        std::vector<Datum> old_row;
        if (t->columnar) {
            auto r = read_columnar_row(*t, pk);
            if (!r) {
                ExecResult er;
                er.affected = 0;  // no row to update
                return er;
            }
            old_row = std::move(*r);
        } else {
            const Key key = encode_key(*t, pk);
            const ReadResult existing = read_committed(key);
            if (!existing.has_value() || is_tombstone(*existing)) {
                ExecResult r;
                r.affected = 0;  // no row to update
                return r;
            }
            old_row = decode_row(*t, key, *existing);
        }
        std::vector<Datum> row = old_row;
        row[col] = sv;
        // ATOMIC: row write + index maintenance in ONE txn. For each secondary index,
        // remove the OLD entry (old col value) and write the NEW entry (new col value);
        // an index whose column is unchanged just rewrites the SAME entry key (idempotent
        // — empty re-write). The row + all index deltas land in one committed write-set.
        // (Columnar rewrites all column families for the row — correct + atomic; a
        // single-column write optimisation can come later.)
        if (auto e = validate_index_exprs(*t, row)) return ExecResult::failure(*e);  // J2
        std::vector<std::pair<Key, Value>> writes;
        emit_row_writes(*t, row, writes);
        index_writes_for_row(*t, old_row, /*tombstone=*/true, writes);
        index_writes_for_row(*t, row, /*tombstone=*/false, writes);
        commit_writes(writes);
        if (t->columnar) {
            if (Table* mt = catalog_.find_mut(up.table)) {
                mt->delta_dirty = true;
                ++mt->delta_count;
            }
            maybe_auto_flush(up.table);
        }
        ExecResult r;
        r.affected = 1;
        return r;
    }

    // --- DELETE ---------------------------------------------------------------
    ExecResult exec_delete(const DeleteStmt& del) {
        const Table* t = catalog_.find(del.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + del.table + "'");
        }
        if (t->composite_pk()) {  // F1: composite-PK DELETE addressing is a follow-on
            return ExecResult::failure("DELETE on a composite-PRIMARY-KEY table is not supported");
        }
        if (del.where_column != t->pk().name) {
            return ExecResult::failure(
                "DELETE WHERE must filter on the primary key '" + t->pk().name +
                "' in v1 (got '" + del.where_column + "')");
        }
        Datum pk;
        if (auto e = coerce(t->pk(), del.where_value, pk)) {
            return ExecResult::failure(*e);
        }
        // F3 DELETE RESTRICT: refuse to delete a row still referenced by a child FOREIGN KEY.
        if (fk_referenced(t->name, pk)) {
            return ExecResult::failure(
                "FOREIGN KEY violation: row in '" + del.table +
                "' is still referenced by a child table (DELETE restricted)");
        }
        // INCREMENTAL: read the prior committed row; if a live row exists, commit a
        // tombstone (the verified executor + incremental apply path do the write).
        std::vector<Datum> old_row;
        if (t->columnar) {
            auto r = read_columnar_row(*t, pk);
            if (!r) {
                ExecResult er;
                er.affected = 0;  // nothing to delete
                return er;
            }
            old_row = std::move(*r);
        } else {
            const Key key = encode_key(*t, pk);
            const ReadResult existing = read_committed(key);
            if (!existing.has_value() || is_tombstone(*existing)) {
                ExecResult r;
                r.affected = 0;  // nothing to delete
                return r;
            }
            old_row = decode_row(*t, key, *existing);
        }
        // ATOMIC: tombstone the row (every column family in columnar) + every secondary-
        // index entry in ONE txn.
        std::vector<std::pair<Key, Value>> writes;
        emit_row_tombstones(*t, pk, writes);
        index_writes_for_row(*t, old_row, /*tombstone=*/true, writes);
        commit_writes(writes);
        if (Table* mt = catalog_.find_mut(del.table)) {
            if (mt->columnar) {
                mt->delta_dirty = true;  // a del-marker now lives in the delta
                ++mt->delta_count;
            }
            if (mt->row_count > 0) {
                --mt->row_count;  // a committed DELETE removes exactly one present row
            }
        }
        maybe_auto_flush(del.table);
        ExecResult r;
        r.affected = 1;
        return r;
    }

    // --- SELECT (the v2 pipeline) --------------------------------------------
    // DETERMINISTIC plan counters (EXPLAIN ANALYZE). Pure functions of the seed — rows /
    // groups at each pipeline boundary, never wall-clock. A null plan_stats_ means a normal
    // run (zero overhead beyond the null check).
    struct PlanStats {
        std::size_t scanned = 0;       // rows read from storage (post-access-path)
        std::size_t after_filter = 0;  // rows surviving the WHERE residual
        std::size_t groups = 0;        // GROUP BY group count (0 if no grouping)
        std::size_t output = 0;        // final rows returned
    };

    // EXPLICIT PHYSICAL PLAN TREE (PERF_PLAN Phase 1). A SELECT is a tree of operators; the
    // planner BUILDS it (build_plan), EXPLAIN walks it, and later phases rewrite it (cost
    // model) + re-implement the operators (vectorization). Today the executor (exec_select)
    // still runs the equivalent linear pipeline; the tree is the planner-visible structure +
    // the EXPLAIN source. `actual` (from a PlanStats run) annotates the node under ANALYZE.
    struct PlanNode {
        enum class Kind {
            SeqScan, PkPointGet, PkRangeScan, IndexScan, IvfflatScan, HnswScan,  // access (leaves)
            Filter, Project, HashAggregate, Having, Distinct, Sort, Limit,  // unary
            HashJoin, NestedLoopJoin  // binary
        };
        Kind kind = Kind::SeqScan;
        std::string detail;                    // table / index / key description
        std::vector<PlanNode> children;        // inputs (0 for a scan, 1 unary, 2 join)
        std::int64_t est = -1;                 // cost-model ESTIMATED rows out (-1 = n/a)
        std::int64_t actual = -1;              // ANALYZE: actual rows out (-1 = not measured)
    };

    static const char* op_name(PlanNode::Kind k) {
        switch (k) {
            case PlanNode::Kind::SeqScan: return "Seq Scan";
            case PlanNode::Kind::PkPointGet: return "PK Point Get";
            case PlanNode::Kind::PkRangeScan: return "PK Range Scan";
            case PlanNode::Kind::IndexScan: return "Index Scan";
            case PlanNode::Kind::IvfflatScan: return "Ivfflat Scan";  // K1.3d: approximate k-NN
            case PlanNode::Kind::HnswScan: return "Hnsw Scan";        // K1.4: graph k-NN
            case PlanNode::Kind::Filter: return "Filter";
            case PlanNode::Kind::Project: return "Project";
            case PlanNode::Kind::HashAggregate: return "HashAggregate";
            case PlanNode::Kind::Having: return "Having";
            case PlanNode::Kind::Distinct: return "Distinct";
            case PlanNode::Kind::Sort: return "Sort";
            case PlanNode::Kind::Limit: return "Limit";
            case PlanNode::Kind::HashJoin: return "Hash Join";
            case PlanNode::Kind::NestedLoopJoin: return "Nested Loop Join";
        }
        return "?";
    }

    // Wrap a child under a parent operator (parent takes the existing tree as its input).
    static PlanNode wrap(PlanNode::Kind k, std::string detail, PlanNode child) {
        PlanNode p;
        p.kind = k;
        p.detail = std::move(detail);
        p.children.push_back(std::move(child));
        return p;
    }

    // Build the physical plan tree for a single-table SELECT (uses the shared access-path
    // chooser, so EXPLAIN's plan == the executed plan).
    PlanNode build_plan_single(const SelectStmt& sel, const Table& t) {
        // K1.3d: the IVFFLAT ANN path — SAME matcher the executor uses, so the plan is honest.
        // The matcher guarantees no WHERE/GROUP BY/DISTINCT and exactly ORDER BY <dist> LIMIT k;
        // the index provides the order, so the tree is just Limit over the probe scan.
        if (const IvfMatch m = ivfflat_match(t, sel); m.ix != nullptr) {
            static const char* kOpNames[] = {"vector_l2_ops", "vector_cosine_ops", "vector_ip_ops"};
            PlanNode scan;
            if (m.ix->hnsw) {  // K1.4
                scan.kind = PlanNode::Kind::HnswScan;
                scan.detail = "using " + m.ix->name + " on " + t.name + " (" +
                              kOpNames[m.want_op <= 2 ? m.want_op : 0] + ", m=" +
                              std::to_string(m.ix->hnsw_m) + ", ef_search=" +
                              std::to_string(hnsw_ef_search_) + ")";
            } else {
            const std::uint32_t probes = ivfflat_probes_ != 0 ? ivfflat_probes_ : m.ix->probes;
            scan.kind = PlanNode::Kind::IvfflatScan;
            scan.detail = "using " + m.ix->name + " on " + t.name + " (" +
                          kOpNames[m.want_op <= 2 ? m.want_op : 0] + ", lists=" +
                          std::to_string(m.ix->lists) + ", probes=" + std::to_string(probes) + ")";
            }
            const std::int64_t want = std::max<std::int64_t>(0, sel.limit) +
                                      std::max<std::int64_t>(0, sel.offset);
            const std::int64_t scan_est =
                std::min<std::int64_t>(want, static_cast<std::int64_t>(t.row_count));
            scan.est = scan_est;
            PlanNode lim = wrap(PlanNode::Kind::Limit,
                                std::to_string(sel.limit) +
                                    (sel.offset > 0 ? " offset " + std::to_string(sel.offset) : ""),
                                std::move(scan));
            lim.est = std::min<std::int64_t>(std::max<std::int64_t>(0, sel.limit), scan_est);
            return lim;
        }
        const AccessPath ap = choose_access_path(sel, t);
        PlanNode node;
        switch (ap.kind) {
            case AccessPath::Kind::PkPoint:
                node.kind = PlanNode::Kind::PkPointGet;
                node.detail = t.name + " (" + t.pk().name + " = const)";
                break;
            case AccessPath::Kind::PkRange:
                node.kind = PlanNode::Kind::PkRangeScan;
                node.detail = t.name + " (" + t.pk().name + " BETWEEN)";
                break;
            case AccessPath::Kind::Index: {
                node.kind = PlanNode::Kind::IndexScan;
                const bool cov = !t.columnar &&
                                 index_covers_need(*ap.index.index, needed_columns(t, sel), t.pk_index);
                node.detail = std::string(cov ? "Index Only Scan: using " : "using ") +
                              ap.index.index->name +
                              (ap.index.is_eq ? " (= const)" : " (range)") + " on " + t.name;
                break;
            }
            case AccessPath::Kind::IndexMerge:
                node.kind = PlanNode::Kind::IndexScan;
                node.detail = "Index Merge: " + ap.index.index->name + " ∩ " +
                              ap.index2.index->name + " on " + t.name;
                break;
            case AccessPath::Kind::Seq:
                node.kind = PlanNode::Kind::SeqScan;
                node.detail = t.name;
                break;
        }
        node.est = static_cast<std::int64_t>(est_path_rows(ap, t));
        // Residual WHERE filter (unless the whole WHERE was a pure-PK fast path).
        if (sel.filter.present() && !ap.pk_fast()) {
            const std::int64_t child_est = node.est;
            node = wrap(PlanNode::Kind::Filter, "WHERE residual predicate", std::move(node));
            node.est = child_est >= 0 ? child_est / 2 : -1;  // ~50% residual selectivity
        }
        if (sel.has_aggregates || !sel.group_by.empty()) {
            node = wrap(PlanNode::Kind::HashAggregate,
                        sel.group_by.empty() ? "scalar aggregate"
                                             : "GROUP BY " +
                                                   std::to_string(sel.group_by.size()) + " col(s)",
                        std::move(node));
            if (sel.having.present()) {
                node = wrap(PlanNode::Kind::Having, "filter groups", std::move(node));
            }
        }
        if (sel.distinct) {
            node = wrap(PlanNode::Kind::Distinct, "", std::move(node));
        }
        // I2/I7: omit the Sort node when the index scan already yields the ORDER BY order.
        const bool order_via_index = ap.kind == AccessPath::Kind::Index &&
                                     order_by_matches_index(sel, ap.index, t) != 0;
        if (!sel.order_by.empty() && !order_via_index) {
            node = wrap(PlanNode::Kind::Sort,
                        "ORDER BY " + std::to_string(sel.order_by.size()) +
                            " key(s) (stable, PK tie-break)",
                        std::move(node));
        }
        if (sel.has_limit) {
            node = wrap(PlanNode::Kind::Limit,
                        std::to_string(sel.limit) +
                            (sel.offset > 0 ? " offset " + std::to_string(sel.offset) : ""),
                        std::move(node));
        }
        return node;
    }

    // Render the plan tree as EXPLAIN text rows (Postgres-style indented, root first).
    void render_plan(const PlanNode& n, int depth, bool analyze,
                     const std::function<void(const std::string&)>& emit) const {
        std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        std::string s = indent + op_name(n.kind);
        if (!n.detail.empty()) {
            s += ": " + n.detail;
        }
        if (n.est >= 0) {
            s += "  (est rows=" + std::to_string(n.est) + ")";
        }
        if (analyze && n.actual >= 0) {
            s += "  (actual rows=" + std::to_string(n.actual) + ")";
        }
        emit(s);
        for (const PlanNode& c : n.children) {
            render_plan(c, depth + 1, analyze, emit);
        }
    }

    // Attach ANALYZE actuals from a PlanStats run onto the matching nodes (scan/filter/group/
    // output). Walks the unary chain bottom-up: the scan leaf gets `scanned`, the Filter gets
    // `after_filter`, the HashAggregate gets `groups`, the topmost node gets `output`.
    void annotate_actuals(PlanNode& n, const PlanStats& st) const {
        // Recurse to the leaf first.
        for (PlanNode& c : n.children) {
            annotate_actuals(c, st);
        }
        switch (n.kind) {
            case PlanNode::Kind::SeqScan:
            case PlanNode::Kind::PkPointGet:
            case PlanNode::Kind::PkRangeScan:
            case PlanNode::Kind::IndexScan:
            case PlanNode::Kind::IvfflatScan:  // K1.3d: index entries examined across the probes
            case PlanNode::Kind::HnswScan:     // K1.4: graph nodes examined during the search
                n.actual = static_cast<std::int64_t>(st.scanned);
                break;
            case PlanNode::Kind::Filter:
                n.actual = static_cast<std::int64_t>(st.after_filter);
                break;
            case PlanNode::Kind::HashAggregate:
                n.actual = static_cast<std::int64_t>(st.groups);
                break;
            default:
                break;
        }
    }

    // EXPLAIN [ANALYZE] <select>: return the chosen PLAN (access path + pipeline stages) as
    // text rows. ANALYZE additionally RUNS the query and appends DETERMINISTIC actual counts
    // (rows scanned/filtered, groups, output) — the transparency surface for finding
    // bottlenecks (a "Seq Scan: scanned 1,000,000 -> 12" line screams "add an index").
    ExecResult explain_select(const SelectStmt& sel) {
        ExecResult out;
        auto line = [&](const std::string& s) {
            ResultRow r;
            r.cells.emplace_back("QUERY PLAN", Datum::make_text(s));
            out.rows.push_back(std::move(r));
        };
        const Table* t = catalog_.find(sel.table);
        if (t == nullptr && !sel.is_join()) {
            return ExecResult::failure("unknown table '" + sel.table + "'");
        }

        // Run first (ANALYZE) so actual counts are available to annotate each node.
        PlanStats st;
        ExecResult run_res;
        if (sel.explain_analyze) {
            SelectStmt run = sel;
            run.explain = false;
            run.explain_analyze = false;
            plan_stats_ = &st;
            run_res = exec_select(run);
            plan_stats_ = nullptr;
            if (!run_res.ok) {
                return run_res;  // surface the execution error
            }
            st.output = run_res.rows.size();
        }
        // Build the explicit plan TREE + render it (root first, indented). Single-table goes
        // through build_plan_single; a join is described at a coarse grain for now (Phase 1
        // tree-ifies joins next).
        if (sel.is_join()) {
            PlanNode jn;
            jn.kind = PlanNode::Kind::HashJoin;
            jn.detail = std::to_string(sel.from.size()) + " tables (left-deep)";
            jn.actual = sel.explain_analyze ? static_cast<std::int64_t>(st.output) : -1;
            for (const JoinEntry& je : sel.from) {
                PlanNode leaf;
                leaf.kind = PlanNode::Kind::SeqScan;
                leaf.detail = je.table + (je.alias != je.table ? " " + je.alias : "");
                jn.children.push_back(std::move(leaf));
            }
            render_plan(jn, 0, sel.explain_analyze,
                        [&](const std::string& s) { line(s); });
        } else {
            PlanNode root = build_plan_single(sel, *t);
            if (sel.explain_analyze) {
                annotate_actuals(root, st);
                root.actual = static_cast<std::int64_t>(st.output);  // top node = final output
            }
            render_plan(root, 0, sel.explain_analyze,
                        [&](const std::string& s) { line(s); });
        }
        line("Level: " + level_name(sel.level) +
             (sel.explain_analyze
                  ? std::string("  | EXPLAIN ANALYZE (deterministic counters, not wall-clock)")
                  : std::string{}));
        out.affected = out.rows.size();
        return out;
    }

    static std::string level_name(Level l) {
        switch (l) {
            case Level::StrictSerializable:
                return "StrictSerializable";
            case Level::Snapshot:
                return "Snapshot";
            case Level::BoundedStaleness:
                return "BoundedStaleness";
            case Level::ReadYourWrites:
                return "ReadYourWrites";
        }
        return "?";
    }

    // D1/D2: UNION / INTERSECT / EXCEPT [ALL]. Flatten the right-linked chain into arms, run each
    // arm's CORE (no per-arm ORDER BY / LIMIT / set_op), combine left-to-right by the op between
    // consecutive arms, then apply the LAST arm's ORDER BY + LIMIT to the whole combined result
    // (SQL set-op semantics). All arms must have the same output arity. UNION/INTERSECT/EXCEPT
    // without ALL deduplicate whole rows (deterministic, by the canonical row rendering).
    ExecResult exec_set_operation(const SelectStmt& sel) {
        // Collect the arms + the op that joins each to the next.
        std::vector<const SelectStmt*> arms;
        std::vector<SetOp> ops;            // ops[i] joins arms[i] with arms[i+1]
        std::vector<bool> alls;
        for (const SelectStmt* s = &sel;;) {
            arms.push_back(s);
            if (s->set_op == SetOp::None || !s->set_op_rhs) break;
            ops.push_back(s->set_op);
            alls.push_back(s->set_op_all);
            s = s->set_op_rhs.get();
        }
        // Run one arm's core (strip ORDER BY / LIMIT / set_op so they don't apply per-arm).
        auto run_core = [&](const SelectStmt* a) -> ExecResult {
            SelectStmt core = *a;
            core.set_op = SetOp::None;
            core.set_op_rhs.reset();
            core.order_by.clear();
            core.has_limit = false;
            core.offset = 0;
            return exec_select(core);
        };
        ExecResult acc = run_core(arms[0]);
        if (!acc.ok) return acc;
        for (std::size_t i = 1; i < arms.size(); ++i) {
            const ExecResult rhs = run_core(arms[i]);
            if (!rhs.ok) return rhs;
            if (!acc.rows.empty() && !rhs.rows.empty() &&
                acc.rows[0].cells.size() != rhs.rows[0].cells.size()) {
                return ExecResult::failure(
                    "set operation: each SELECT must have the same number of columns");
            }
            acc.rows = combine_rows(ops[i - 1], alls[i - 1], acc.rows, rhs.rows);
        }
        // The labels come from the FIRST arm (already in acc). Apply the LAST arm's ORDER BY/LIMIT.
        const SelectStmt* last = arms.back();
        if (auto e = apply_order_by_labels(*last, acc.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(*last, acc.rows);
        acc.affected = acc.rows.size();
        return acc;
    }

    // Combine two row sets per a set operator. `all` keeps multiplicity; otherwise the result is
    // whole-row deduplicated (stable, first occurrence) by the canonical render_row key.
    static std::vector<ResultRow> combine_rows(SetOp op, bool all,
                                               const std::vector<ResultRow>& l,
                                               const std::vector<ResultRow>& r) {
        std::vector<ResultRow> out;
        auto key = [](const ResultRow& row) { return render_row(row); };
        if (op == SetOp::Union) {
            out = l;
            out.insert(out.end(), r.begin(), r.end());
        } else if (op == SetOp::Intersect) {
            std::multiset<std::string> rk;
            for (const ResultRow& row : r) rk.insert(key(row));
            for (const ResultRow& row : l) {
                auto it = rk.find(key(row));
                if (it != rk.end()) {
                    out.push_back(row);
                    if (all) rk.erase(it);  // ALL: pair up multiplicities
                }
            }
        } else {  // Except: rows of l not matched in r
            std::multiset<std::string> rk;
            for (const ResultRow& row : r) rk.insert(key(row));
            for (const ResultRow& row : l) {
                auto it = rk.find(key(row));
                if (it != rk.end()) {
                    if (all) rk.erase(it);  // ALL: each right row cancels one left row
                    continue;
                }
                out.push_back(row);
            }
        }
        if (!all) {
            std::vector<ResultRow> dedup;
            std::set<std::string> seen;
            for (ResultRow& row : out) {
                if (seen.insert(key(row)).second) dedup.push_back(std::move(row));
            }
            out = std::move(dedup);
        }
        return out;
    }

    // C2: GROUP BY GROUPING SETS — run the aggregate ONCE per grouping set (each a normal GROUP BY)
    // and UNION the rows; ORDER BY / LIMIT apply once over the union. A SELECT column not in a given
    // set renders NULL (gs_mode_ tells exec_aggregate to NULL it instead of requiring it grouped).
    ExecResult exec_grouping_sets(const SelectStmt& sel) {
        ExecResult out;
        for (const std::vector<std::string>& set : sel.grouping_sets) {
            SelectStmt sub = sel;
            sub.grouping_sets.clear();
            sub.group_by = set;
            sub.order_by.clear();
            sub.has_limit = false;
            sub.offset = 0;
            gs_mode_ = true;
            ExecResult r = exec_select(sub);
            gs_mode_ = false;
            if (!r.ok) return r;
            out.rows.insert(out.rows.end(), r.rows.begin(), r.rows.end());
        }
        if (auto e = apply_order_by_labels(sel, out.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, out.rows);
        out.affected = out.rows.size();
        return out;
    }

    // D4: drop the ephemeral tables a WITH clause materialized (best-effort, if_exists).
    void drop_materialized(const std::vector<std::string>& names) {
        for (const std::string& n : names) {
            DropTableStmt dt;
            dt.table = n;
            dt.if_exists = true;
            (void)exec_drop_table(dt);
        }
    }

    ExecResult exec_select(const SelectStmt& sel) {
        // D4 WITH common table expressions + D3 FROM-subqueries (derived tables). Materialize each
        // CTE (in order — a later CTE may read an earlier one) and each derived-table subquery into
        // an ephemeral table named by the CTE / the derived alias, run the main query (a copy with
        // the ctes + subqueries stripped so this does not recurse), then drop them. A name that
        // clashes with an existing table is rejected (no shadowing here); an empty subquery result
        // is an error (schema is inferred from the rows, like CREATE TABLE AS SELECT).
        bool has_derived = false;
        for (const JoinEntry& je : sel.from) if (je.subquery) { has_derived = true; break; }
        // H1: a FROM entry naming a VIEW (not a real table, not a subquery) expands to the view's
        // stored SELECT — re-parsed here and materialized like a derived table (recursively: a view
        // body may reference other views). A real table of the same name always wins (shadows the
        // view). Collect them first so the trigger below fires even for a plain `SELECT * FROM v`.
        std::vector<std::size_t> view_idx;                     // from-entry indices that are views
        std::vector<std::shared_ptr<SelectStmt>> view_body;    // parsed view bodies (parallel to view_idx)
        for (std::size_t i = 0; i < sel.from.size(); ++i) {
            const JoinEntry& je = sel.from[i];
            if (je.subquery || catalog_.find(je.table) != nullptr) continue;
            const View* v = catalog_.find_view(je.table);
            if (v == nullptr) continue;
            ParseResult pr = parse_sql(v->select_src);
            if (!pr.ok())
                return ExecResult::failure("view '" + je.table + "': " + pr.error().render());
            if (pr.stmt().kind != StmtKind::Select)
                return ExecResult::failure("view '" + je.table + "' body is not a SELECT");
            view_idx.push_back(i);
            view_body.push_back(std::make_shared<SelectStmt>(pr.stmt().select));
        }
        // W9: FROM entries naming a system relation (information_schema.tables / .columns) are
        // synthesised from the live catalog into an ephemeral table, then read via the normal
        // path (so WHERE / projection / joins all work). A real table of the same name wins.
        std::vector<std::size_t> sys_idx;
        std::vector<std::string> sys_name;  // lower-cased relation name, parallel to sys_idx
        for (std::size_t i = 0; i < sel.from.size(); ++i) {
            const JoinEntry& je = sel.from[i];
            if (je.subquery || catalog_.find(je.table) != nullptr) continue;
            std::string low;
            low.reserve(je.table.size());
            for (char c : je.table) low.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
            if (!is_system_relation(low)) continue;
            sys_idx.push_back(i);
            sys_name.push_back(low);
        }
        if (!sel.ctes.empty() || has_derived || !view_idx.empty() || !sys_idx.empty()) {
            std::vector<std::string> created;
            auto materialize = [&](const std::string& nm, const SelectStmt& sub) -> ExecResult {
                if (catalog_.find(nm) != nullptr) {
                    drop_materialized(created);
                    return ExecResult::failure("subquery/CTE name '" + nm + "' clashes with a table");
                }
                if (auto e = materialize_select(nm, sub)) {
                    drop_materialized(created);
                    return ExecResult::failure(*e);
                }
                created.push_back(nm);
                return ExecResult{};
            };
            // Views first (a CTE/derived table may read a view). Each is materialized under its
            // binding alias; a cycle (a view whose body reaches itself) is refused, not looped.
            for (std::size_t k = 0; k < view_idx.size(); ++k) {
                const JoinEntry& je = sel.from[view_idx[k]];
                const std::string vname = je.table;  // the VIEW name (before body rewrites it to alias)
                if (expanding_views_.count(vname) != 0) {
                    drop_materialized(created);
                    return ExecResult::failure("recursive view '" + vname + "' is not supported");
                }
                expanding_views_.insert(vname);
                const ExecResult m = materialize(je.alias, *view_body[k]);
                expanding_views_.erase(vname);
                if (!m.ok) return m;
                if (const View* v = catalog_.find_view(vname); v != nullptr && !v->columns.empty()) {
                    if (auto e = rename_materialized_columns(je.alias, v->columns)) {
                        drop_materialized(created);
                        return ExecResult::failure(*e);
                    }
                }
            }
            // W9: system relations (synthesised from the catalog) — materialise each under its
            // binding alias, exactly like a view, then the rewrite below points the ref at it.
            for (std::size_t k = 0; k < sys_idx.size(); ++k) {
                const JoinEntry& je = sel.from[sys_idx[k]];
                if (catalog_.find(je.alias) != nullptr) {
                    drop_materialized(created);
                    return ExecResult::failure("alias '" + je.alias + "' clashes with a table");
                }
                auto [scols, srows] = build_system_relation(sys_name[k]);
                if (auto e = materialize_typed(je.alias, scols, srows)) {
                    drop_materialized(created);
                    return ExecResult::failure(*e);
                }
                created.push_back(je.alias);
            }
            for (const auto& [name, sub] : sel.ctes) {       // CTEs next (derived may read them)
                if (sub->recursive) {  // WITH RECURSIVE — fixpoint materialization
                    if (catalog_.find(name) != nullptr) {
                        drop_materialized(created);
                        return ExecResult::failure("CTE name '" + name + "' clashes with a table");
                    }
                    if (auto e = materialize_recursive_cte(name, *sub)) {
                        drop_materialized(created);
                        return ExecResult::failure(*e);
                    }
                    created.push_back(name);
                    continue;
                }
                if (const ExecResult m = materialize(name, *sub); !m.ok) return m;
                if (!sub->cte_columns.empty()) {  // WITH cte(col, ...) explicit output names
                    if (auto e = rename_materialized_columns(name, sub->cte_columns)) {
                        drop_materialized(created);
                        return ExecResult::failure(*e);
                    }
                }
            }
            for (const JoinEntry& je : sel.from) {           // then the derived tables
                if (je.subquery) {
                    if (const ExecResult m = materialize(je.alias, *je.subquery); !m.ok) return m;
                }
            }
            SelectStmt body = sel;
            body.ctes.clear();
            for (JoinEntry& je : body.from) je.subquery.reset();  // now plain table refs (== alias)
            for (std::size_t idx : view_idx) {                // a view ref now resolves to its ephemeral
                body.from[idx].table = body.from[idx].alias;  // table (named by the alias)
                if (body.table == sel.from[idx].table) body.table = body.from[idx].alias;
            }
            for (std::size_t idx : sys_idx) {                 // W9: system-relation ref -> ephemeral table
                body.from[idx].table = body.from[idx].alias;
                if (body.table == sel.from[idx].table) body.table = body.from[idx].alias;
            }
            const ExecResult r = exec_select(body);
            drop_materialized(created);
            return r;
        }
        if (sel.set_op != SetOp::None) {
            return exec_set_operation(sel);
        }
        if (!sel.grouping_sets.empty()) {
            return exec_grouping_sets(sel);
        }
        if (sel.explain) {
            return explain_select(sel);
        }
        // v3: a genuine JOIN (>1 FROM entry) OR an aliased single table takes the
        // joined pipeline (qualified-column resolution over a multi-table schema).
        const bool joined =
            sel.from.size() > 1 ||
            (sel.from.size() == 1 && sel.from[0].alias != sel.from[0].table);
        if (joined) {
            return exec_select_joined(sel);
        }

        const Table* t = catalog_.find(sel.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + sel.table + "'");
        }

        // A4 VECTORIZED SCALAR AGGREGATE: a columnar+flushed table, an ungrouped aggregate
        // (no GROUP BY / HAVING / DISTINCT), and a vectorizable (or absent) WHERE — fold
        // SUM/COUNT/MIN/MAX/AVG DIRECTLY over the SoA column blocks (no AoS row
        // materialisation, the measured win). nullopt => not applicable, fall through to
        // the generic AoS path (which the conformance gate cross-checks this against).
        if (t->columnar && sel.has_aggregates && !agg_has_distinct(sel) && !gs_mode_) {
            if (auto fast = columnar_vectorized_agg(*t, sel)) {
                return std::move(*fast);
            }
        }

        // K1.3: IVFFLAT approximate k-NN — probe centroid lists instead of scanning when the
        // query is the pgvector `ORDER BY vec <-> const LIMIT k` idiom (nullopt = exact path).
        if (auto ann = ivfflat_try_knn(*t, sel)) {
            return std::move(*ann);
        }
        // K2: BM25 top-k — `ORDER BY bm25_score(col, 'query') DESC LIMIT k` over a
        // BM25-indexed TEXT column (nullopt = generic path / clean BM25_SCORE error).
        if (auto fts = bm25_try_topk(*t, sel)) {
            return std::move(*fts);
        }
        // K2.4: hybrid RRF — vectors + BM25 fused in one query.
        if (auto hyb = rrf_try_topk(*t, sel)) {
            return std::move(*hyb);
        }

        // (1) READ — pick the storage read (PK fast path vs full scan), run at the
        // call-site-visible D5 level. The fast path is taken only when the WHERE is
        // EXACTLY a PK eq / PK BETWEEN over the REAL PK column; the rest of the
        // general predicate (if any) is applied as a row filter in (2).
        // THE EXECUTOR FOLLOWS THE PLAN. The access path comes from the SHARED chooser —
        // the same call build_plan_single (EXPLAIN) uses — so what EXPLAIN shows is exactly
        // what runs, and Phase 2's cost-based chooser will steer execution automatically.
        const AccessPath ap = choose_access_path(sel, *t);
        const bool pk_fast = ap.pk_fast();
        std::vector<bool> need = needed_columns(*t, sel);
        std::vector<std::vector<Datum>> rows;
        const bool used_index = ap.kind == AccessPath::Kind::Index;
        const bool used_merge = ap.kind == AccessPath::Kind::IndexMerge;  // I7
        // Phase 3: pre-extract a vectorizable conjunctive filter ONCE. On a seq scan it FUSES
        // into the decode (decode-into-scratch + push only survivors → no per-row alloc for a
        // filtered-out row, the measured bottleneck). On the index path it applies post-fetch.
        std::vector<VecTerm> vterms;
        const bool vectorizable = sel.filter.present() && !pk_fast && vectorize_ &&
                                  try_extract_conjuncts(sel.filter, sel.filter.root, *t, vterms);
        bool filter_applied = false;
        // I2/I7: 0 = sort needed; 1 = index order satisfies ORDER BY (forward); 2 = reverse (DESC).
        const int omode = used_index ? order_by_matches_index(sel, ap.index, *t) : 0;
        const bool order_via_index = omode != 0;
        // I3: index-only (covering) scan — serve from the index when it covers every needed column.
        const bool covering = used_index && !t->columnar &&
                              index_covers_need(*ap.index.index, need, t->pk_index);
        if (used_merge) {  // I7: intersect two single-column eq indexes
            if (auto err = read_via_index_merge(*t, sel, ap.index, ap.index2, need, rows)) {
                return ExecResult::failure(*err);
            }
        } else if (used_index) {
            // Index scan: range-scan the index for the PKs, point-get each row; the full
            // predicate still runs as the residual filter in (2). (read_via_index assembles
            // columnar rows from their column families when t is columnar.)
            if (auto err = read_via_index(*t, sel, ap.index, need, rows,
                                          /*preserve_index_order=*/omode == 1, covering,
                                          /*reverse_index_order=*/omode == 2)) {
                return ExecResult::failure(*err);
            }
        } else if (t->columnar) {
            // COLUMNAR seq/PK-fast read: scan ONLY the needed column families (projection
            // pushdown), zip by pk. PK-fast Eq is a single row assembly; PK-fast BETWEEN
            // bounds the family scans; otherwise a full scan + the residual filter in (2).
            if (pk_fast && sel.where == SelectWhereKind::Eq) {
                Datum pk;
                if (auto e = coerce(t->pk(), sel.eq_value, pk)) {
                    return ExecResult::failure(*e);
                }
                if (auto row = read_columnar_row(*t, pk)) {
                    rows.push_back(std::move(*row));
                }
            } else if (auto err = columnar_build_rows(
                           *t, sel, need, rows,
                           /*pk_between=*/pk_fast && sel.where == SelectWhereKind::Between)) {
                return ExecResult::failure(*err);
            }
            if (plan_stats_ != nullptr) {
                plan_stats_->scanned = rows.size();
            }
        }

        if (!used_index && !used_merge && !t->columnar) {
            std::vector<storage::KeyValue> kvs;
            if (auto err = run_select_at_level(*t, sel, pk_fast, kvs)) {
                return ExecResult::failure(*err);
            }
            // LAZY/PROJECTED DECODE (the scan-decode optimization). Compute the set of
            // columns the query ACTUALLY references (projection + WHERE + GROUP BY +
            // aggregates) and decode ONLY those per row, SKIPPING the rest (a wide TEXT
            // column the query never touches is not copied/parsed). For a few-column
            // projection or a filtered scan this cuts the dominant per-row decode cost.
            // Byte-identical to a full decode on the needed columns => result unchanged.
            rows.reserve(kvs.size());
            if (vectorizable) {
                // FUSED: decode into a REUSED scratch buffer, apply the conjuncts, push only
                // survivors. A filtered-out row never allocates a persistent vector<Datum>.
                std::vector<Datum> scratch;
                std::size_t scanned_n = 0;
                for (const storage::KeyValue& kv : kvs) {
                    if (is_tombstone(kv.second)) {
                        continue;
                    }
                    ++scanned_n;
                    decode_row_projected_into(*t, kv.first, kv.second, need, scratch);
                    bool ok = true;
                    for (const VecTerm& vt : vterms) {
                        if (!apply_cmp(vt.op, cmp_datum(scratch[vt.col], vt.lit))) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        rows.push_back(scratch);  // survivor — the only persistent alloc
                    }
                }
                filter_applied = true;
                if (plan_stats_ != nullptr) {
                    plan_stats_->scanned = scanned_n;
                    plan_stats_->after_filter = rows.size();
                }
            } else {
                for (const storage::KeyValue& kv : kvs) {
                    if (is_tombstone(kv.second)) {
                        continue;
                    }
                    rows.push_back(decode_row_projected(*t, kv.first, kv.second, need));
                }
            }
        }

        if (!filter_applied && plan_stats_ != nullptr) {
            plan_stats_->scanned = rows.size();
        }

        // (2) WHERE residual filter — UNLESS the seq scan already FUSED it (filter_applied),
        // or the PK fast path enforced the whole WHERE. Runs for the index path + any
        // non-vectorizable predicate. Vectorizable conjuncts reuse the pre-extracted vterms
        // (byte-identical to the interpreter, which handles everything else).
        if (sel.filter.present() && !pk_fast && !filter_applied) {
            std::vector<std::vector<Datum>> kept;
            if (vectorizable) {
                for (auto& row : rows) {
                    bool ok = true;
                    for (const VecTerm& vt : vterms) {
                        if (!apply_cmp(vt.op, cmp_datum(row[vt.col], vt.lit))) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        kept.push_back(std::move(row));
                    }
                }
            } else {
                for (auto& row : rows) {
                    bool truth = false;
                    if (auto e = eval_pred(sel.filter, sel.filter.root, *t, row,
                                           /*group=*/nullptr, truth)) {
                        return ExecResult::failure(*e);
                    }
                    if (truth) {
                        kept.push_back(std::move(row));
                    }
                }
            }
            rows = std::move(kept);
        }
        if (plan_stats_ != nullptr) {
            plan_stats_->after_filter = rows.size();
        }

        // (3)/(4) GROUP + AGGREGATE + HAVING when the query has aggregates / GROUP BY.
        if (sel.has_aggregates || !sel.group_by.empty()) {
            ExecResult ar = exec_aggregate(sel, *t, rows);
            if (plan_stats_ != nullptr) {
                plan_stats_->groups = ar.rows.size();
                plan_stats_->output = ar.rows.size();
            }
            return ar;
        }

        // No aggregates: project each item — a plain column (validated), or an A1-A4 scalar
        // EXPRESSION evaluated per row (computed column). VALIDATE columns/expr-columns UP FRONT so
        // an unknown reference errors even on an empty table (the row loop would otherwise skip it).
        if (!sel.star) {
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Expr) {
                    if (auto e = validate_expr_columns(*item.expr, *t)) {
                        return ExecResult::failure(*e);
                    }
                } else if (item.kind == SelectItemKind::Column && !t->column_index(item.column)) {
                    return ExecResult::failure("unknown column '" + item.column + "' in table '" +
                                               sel.table + "'");
                }
            }
        }
        // C3: precompute each WINDOW function's value per row (over the post-WHERE row set, in scan
        // order) — window functions see the whole partition, so they cannot be evaluated row-locally.
        std::vector<std::vector<Datum>> win_vals(sel.items.size());
        for (std::size_t a = 0; a < sel.items.size(); ++a) {
            if (sel.items[a].kind == SelectItemKind::Window) {
                if (auto e = compute_window(*sel.items[a].win, *t, rows, win_vals[a])) {
                    return ExecResult::failure(*e);
                }
            }
        }
        // F12: UNNEST(arr) in the SELECT list expands each input row into ONE row per array element.
        // Support a single UNNEST (not combined with window functions); other items repeat per element.
        int unnest_idx = -1;
        for (std::size_t a = 0; a < sel.items.size(); ++a) {
            const SelectItem& item = sel.items[a];
            if (item.kind == SelectItemKind::Expr && item.expr->kind == ExprKind::Func &&
                item.expr->func == "UNNEST") {
                if (unnest_idx >= 0) return ExecResult::failure("only one UNNEST per SELECT is supported");
                if (item.expr->args.size() != 1) return ExecResult::failure("UNNEST takes one ARRAY argument");
                unnest_idx = static_cast<int>(a);
            }
        }
        if (unnest_idx >= 0)
            for (const auto& it : sel.items)
                if (it.kind == SelectItemKind::Window)
                    return ExecResult::failure("UNNEST cannot be combined with window functions");

        // K1.2: ORDER BY <scalar-expr> (`ORDER BY emb <-> '[1,0,0]' LIMIT k`). Each expression
        // key is evaluated per SOURCE row into a HIDDEN output cell (label "\x01ob<i>" — the
        // control byte cannot collide with a user alias); the sort reads the hidden label, and
        // the cells are stripped after LIMIT. DISTINCT/UNNEST would change the row set between
        // projection and sort, so the combination is rejected (PostgreSQL's DISTINCT rule).
        bool has_expr_ob = false;
        for (const OrderKey& k : sel.order_by) has_expr_ob = has_expr_ob || k.expr != nullptr;
        if (has_expr_ob && (sel.distinct || unnest_idx >= 0)) {
            return ExecResult::failure(
                "ORDER BY expression cannot be combined with DISTINCT or UNNEST");
        }

        ExecResult r;
        for (std::size_t ri = 0; ri < rows.size(); ++ri) {
            const auto& row = rows[ri];
            // Evaluate one projected item to a Datum (column / expression / window).
            auto eval_item = [&](std::size_t a, Datum& d) -> std::optional<std::string> {
                const SelectItem& item = sel.items[a];
                if (item.kind == SelectItemKind::Window) { d = win_vals[a][ri]; return std::nullopt; }
                if (item.kind == SelectItemKind::Expr) return eval_expr(*item.expr, *t, row, d);
                const auto idx = t->column_index(item.column);
                if (!idx) return std::string("unknown column '" + item.column + "'");
                d = row[*idx];
                return std::nullopt;
            };
            // K1.2: append the hidden ORDER BY-expression cells to one built output row.
            auto append_ob_cells = [&](ResultRow& out) -> std::optional<std::string> {
                if (!has_expr_ob) return std::nullopt;
                for (std::size_t ki = 0; ki < sel.order_by.size(); ++ki) {
                    if (!sel.order_by[ki].expr) continue;
                    Datum d;
                    if (auto e = eval_expr(*sel.order_by[ki].expr, *t, row, d)) return e;
                    out.cells.emplace_back(ob_label(ki), d);
                }
                return std::nullopt;
            };
            if (sel.star) {
                ResultRow out;
                for (std::size_t i = 0; i < t->columns.size(); ++i) {
                    if (t->columns[i].dropped) continue;  // E1: hidden
                    out.cells.emplace_back(t->columns[i].name, row[i]);
                }
                if (auto e = append_ob_cells(out)) return ExecResult::failure(*e);
                r.rows.push_back(std::move(out));
            } else if (unnest_idx < 0) {
                ResultRow out;
                for (std::size_t a = 0; a < sel.items.size(); ++a) {
                    Datum d;
                    if (auto e = eval_item(a, d)) return ExecResult::failure(*e);
                    out.cells.emplace_back(sel.items[a].label, d);
                }
                if (auto e = append_ob_cells(out)) return ExecResult::failure(*e);
                r.rows.push_back(std::move(out));
            } else {
                // Decode the UNNEST array for this row, then emit one output row per element.
                Datum arr;
                if (auto e = eval_expr(*sel.items[unnest_idx].expr->args[0], *t, row, arr))
                    return ExecResult::failure(*e);
                if (arr.is_null || arr.logical != 7) continue;  // NULL / non-array -> no rows
                const std::vector<Datum> elems = Datum::decode_array(arr.s);
                for (const Datum& el : elems) {
                    ResultRow out;
                    for (std::size_t a = 0; a < sel.items.size(); ++a) {
                        if (static_cast<int>(a) == unnest_idx) {
                            out.cells.emplace_back(sel.items[a].label, el);
                        } else {
                            Datum d;
                            if (auto e = eval_item(a, d)) return ExecResult::failure(*e);
                            out.cells.emplace_back(sel.items[a].label, d);
                        }
                    }
                    r.rows.push_back(std::move(out));
                }
            }
        }

        // (5) DISTINCT, (6) ORDER BY, (7) LIMIT/OFFSET.
        apply_distinct(sel, r.rows);
        if (!order_via_index) {  // I2: skip the sort when the index already produced ORDER BY order
            if (auto e = apply_order_by(sel, *t, r.rows)) {
                return ExecResult::failure(*e);
            }
        }
        apply_limit(sel, r.rows);
        strip_ob_cells(r.rows);  // K1.2: drop the hidden ORDER BY-expression cells
        r.affected = r.rows.size();
        return r;
    }

    // K1.2: the hidden output-cell label for ORDER BY-expression key <i>. The leading control
    // byte cannot appear in a user identifier/alias, so no collision is possible.
    [[nodiscard]] static std::string ob_label(std::size_t ki) {
        return std::string("\x01ob") + std::to_string(ki);
    }
    static void strip_ob_cells(std::vector<ResultRow>& rows) {
        for (ResultRow& rr : rows)
            while (!rr.cells.empty() && rr.cells.back().first.rfind("\x01ob", 0) == 0)
                rr.cells.pop_back();
    }

    // The grouped/aggregated execution path. `rows` is the post-WHERE row set (full
    // table rows in schema order). Produces one output ROW per group, DETERMINISTIC
    // (groups sorted by their group-key tuple; ungrouped == one synthetic group).
    // C1: true iff ANY aggregate in the SELECT is DISTINCT. The vectorized/columnar/fused-join
    // aggregate fast paths fold EVERY value (no per-group dedup), so they must defer to the general
    // AoS exec_aggregate (which dedups in compute_agg) whenever this holds.
    [[nodiscard]] static bool agg_has_distinct(const SelectStmt& sel) {
        for (const SelectItem& it : sel.items) {
            if (it.kind != SelectItemKind::Column && it.agg.distinct) {
                return true;
            }
        }
        return false;
    }

    // F9d: CHECKED int64 arithmetic. INT is 64-bit (BIGINT is the same width); an operation whose
    // true result exceeds int64 is signed overflow == UB (an UBSan trap; a silent 2's-complement wrap
    // in a release build). These return true ON OVERFLOW so the caller can raise a clean
    // "integer overflow" error instead. `wrap_add` is a DEFINED 2's-complement add via unsigned (no
    // UB, no branch — stays SIMD-vectorizable); used on the opt-in columnar accumulator paths (tight
    // per-element scans), where threading an error through the parallel partial-merge machinery would
    // both serialize the SIMD folds and race. The wrap is deterministic and UB-free; row/joined/
    // window/expr paths raise the hard error, and conformance never overflows so row==columnar stays
    // byte-identical.
    static bool add_ovf(std::int64_t a, std::int64_t b, std::int64_t& r) {
        return __builtin_add_overflow(a, b, &r);
    }
    static bool sub_ovf(std::int64_t a, std::int64_t b, std::int64_t& r) {
        return __builtin_sub_overflow(a, b, &r);
    }
    static bool mul_ovf(std::int64_t a, std::int64_t b, std::int64_t& r) {
        return __builtin_mul_overflow(a, b, &r);
    }
    // Checked 128-bit signed multiply WITHOUT __builtin_mul_overflow. On __int128 that builtin
    // lowers to __muloti4, which lives in compiler-rt but NOT libgcc — so it is an undefined symbol
    // under the CI clang's default rtlib (Apple clang links compiler-rt and hides this). Plain
    // 128-bit * and / use __multi3 / __divti3 (in libgcc, always linked). Returns true on overflow
    // (out then holds the wrapped product). Pure; no UB (wrapping product via unsigned; the only
    // trapping division — INT128_MIN / -1 — is special-cased before the verify divide).
    static bool mul_ovf_i128(__int128 a, __int128 b, __int128& out) {
        out = static_cast<__int128>(static_cast<unsigned __int128>(a) *
                                    static_cast<unsigned __int128>(b));  // wrapping, UB-free
        if (a == 0) return false;
        const __int128 kMin = static_cast<__int128>(static_cast<unsigned __int128>(1) << 127);
        if (a == -1 && b == kMin) return true;  // verify divide would trap; this IS an overflow
        return out / a != b;                    // exact: out == a*b iff no overflow
    }
    static std::int64_t wrap_add(std::int64_t a, std::int64_t b) {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) +
                                         static_cast<std::uint64_t>(b));
    }

    // F9e: 128-bit checked arithmetic over INT128/DECIMAL128 (operands carry their payload in `s`; a
    // narrow INT/DECIMAL64 operand widens in). Mirrors the int64 DECIMAL rules (Add/Sub align scales,
    // Mul adds, Div keeps the left scale) but on __int128, with __builtin overflow checks. The result
    // is INT128 (logical 5) when both operands are scale-0, else DECIMAL128 (logical 6).
    // UINT256 binary arithmetic. Each operand is a UINT256 (32-byte payload) or a non-negative INT
    // (widened). +/-/*/ / % with overflow / negative-result / division-by-zero reported as errors.
    [[nodiscard]] std::optional<std::string> eval_bin_u256(BinOp op, const Datum& l, const Datum& r,
                                                           Datum& out) {
        auto decode = [](const Datum& d, u256& v) -> std::optional<std::string> {
            if (d.logical == 13) {
                if (d.type != Type::Text || d.s.size() != 32) return std::string("malformed UINT256");
                v = u256_decode(d.s);
            } else if (d.type == Type::Int) {
                if (d.i < 0) return std::string("a UINT256 operand cannot be negative");
                v = u256_from_u64(static_cast<std::uint64_t>(d.i));
            } else {
                return std::string("UINT256 arithmetic requires integer operands");
            }
            return std::nullopt;
        };
        u256 a, b, res;
        if (auto e = decode(l, a)) return e;
        if (auto e = decode(r, b)) return e;
        switch (op) {
            case BinOp::Add:
                if (u256_add(a, b, res)) return std::string("UINT256 overflow in addition");
                break;
            case BinOp::Sub:
                if (u256_sub(a, b, res)) return std::string("UINT256 underflow (result is negative)");
                break;
            case BinOp::Mul:
                if (u256_mul(a, b, res)) return std::string("UINT256 overflow in multiplication");
                break;
            case BinOp::Div: {
                u256 rem;
                if (!u256_divmod(a, b, res, rem)) return std::string("division by zero");
                break;
            }
            case BinOp::Mod: {
                u256 q;
                if (!u256_divmod(a, b, q, res)) return std::string("modulo by zero");
                break;
            }
        }
        out = Datum::make_text(u256_encode(res));
        out.logical = 13;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> eval_bin_i128(BinOp op, const Datum& l, const Datum& r,
                                                           Datum& out) {
        auto decode = [](const Datum& d, __int128& val, std::uint8_t& sc) -> bool {
            if (d.logical >= 5) {
                if (d.type != Type::Text || d.s.size() != 16) return false;
                val = Datum::decode_i128(d.s);
                sc = d.logical == 6 ? d.scale : 0;
            } else if (d.type == Type::Int) {
                val = static_cast<__int128>(d.i);
                sc = d.logical == 1 ? d.scale : 0;  // DECIMAL64 keeps its scale
            } else {  // F9e: a bare numeric string literal — infer its scale from the fraction
                return parse_decimal128_infer(d.s, val, sc);
            }
            return true;
        };
        __int128 lv = 0, rv = 0;
        std::uint8_t ls = 0, rs = 0;
        if (!decode(l, lv, ls) || !decode(r, rv, rs)) return "arithmetic requires numeric operands";
        auto p10 = [](std::uint8_t k) { __int128 p = 1; while (k--) p *= 10; return p; };
        __int128 v = 0;
        std::uint8_t out_sc = 0;
        switch (op) {
            case BinOp::Add:
            case BinOp::Sub: {
                const std::uint8_t cs = ls > rs ? ls : rs;
                __int128 li = 0, ri = 0;
                if (mul_ovf_i128(lv, p10(cs - ls), li) ||
                    mul_ovf_i128(rv, p10(cs - rs), ri))
                    return "integer overflow";
                if (op == BinOp::Add ? __builtin_add_overflow(li, ri, &v)
                                     : __builtin_sub_overflow(li, ri, &v))
                    return "integer overflow";
                out_sc = cs;
                break;
            }
            case BinOp::Mul:
                if (mul_ovf_i128(lv, rv, v)) return "integer overflow";
                out_sc = static_cast<std::uint8_t>(ls + rs);
                break;
            case BinOp::Div: {
                if (rv == 0) return "division by zero";
                __int128 num = 0;
                if (mul_ovf_i128(lv, p10(rs), num)) return "integer overflow";
                v = num / rv;  // result scale = ls
                out_sc = ls;
                break;
            }
            case BinOp::Mod:
                if (rv == 0) return "modulo by zero";
                v = lv % rv;
                out_sc = ls;
                break;
        }
        out = Datum::make_text(Datum::encode_i128(v));
        out.logical = out_sc > 0 ? 6 : 5;
        out.scale = out_sc;
        return std::nullopt;
    }

    // A1-A4: evaluate a scalar expression over a single-table `row` to a Datum. Arithmetic is INT
    // (NULL-propagating; division/mod by zero is an error); string/scalar functions + CAST + CASE.
    [[nodiscard]] std::optional<std::string> eval_expr(const Expr& e, const Table& t,
                                                       const std::vector<Datum>& row, Datum& out) {
        switch (e.kind) {
            case ExprKind::Lit:
                out = e.lit;
                return std::nullopt;
            case ExprKind::Col: {
                const auto idx = t.column_index(e.column);
                if (!idx) return "unknown column '" + e.column + "' in expression";
                out = row[*idx];
                return std::nullopt;
            }
            case ExprKind::Neg: {
                Datum c;
                if (auto er = eval_expr(*e.left, t, row, c)) return er;
                if (c.is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
                if (c.logical == 5 || c.logical == 6) {  // F9e: negate a 128-bit operand (0 - c)
                    Datum zero = Datum::make_int(0);
                    return eval_bin_i128(BinOp::Sub, zero, c, out);
                }
                if (c.logical == 13) return "UINT256 is unsigned; it cannot be negated";
                if (c.type != Type::Int) return "unary '-' requires an INT operand";
                std::int64_t neg = 0;
                if (sub_ovf(0, c.i, neg)) return "integer overflow";  // -INT64_MIN overflows
                out = Datum::make_int(neg);
                out.logical = c.logical;
                out.scale = c.scale;
                return std::nullopt;
            }
            case ExprKind::Bin: {
                Datum l, r;
                if (auto er = eval_expr(*e.left, t, row, l)) return er;
                if (auto er = eval_expr(*e.right, t, row, r)) return er;
                if (l.is_null || r.is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
                // UINT256 (logical 13): if either operand is UINT256, compute in u256 (a narrow
                // non-negative INT widens in). Checked BEFORE the 128-bit path so a UINT256 operand is
                // never mis-decoded as __int128 (mixing 128-bit with 256-bit is a clean error).
                if (l.logical == 13 || r.logical == 13) {
                    return eval_bin_u256(e.op, l, r, out);
                }
                // F9e: if either operand is 128-bit (INT128/DECIMAL128), compute in __int128. A plain
                // INT / DECIMAL64 operand widens in (so 128-bit mixes with the narrow types).
                if (l.logical == 5 || l.logical == 6 || r.logical == 5 || r.logical == 6) {
                    return eval_bin_i128(e.op, l, r, out);
                }
                // F14: REAL arithmetic — if either operand is a REAL, compute in IEEE-754 double. A
                // plain INT operand widens in; any other logical type (DECIMAL/temporal) mixed with a
                // REAL is a clean error (no silent use of a scaled/encoded int as a double).
                if (l.is_real() || r.is_real()) {
                    if ((!l.is_real() && (l.type != Type::Int || l.logical != 0)) ||
                        (!r.is_real() && (r.type != Type::Int || r.logical != 0)))
                        return "REAL arithmetic requires a plain INT or REAL operand";
                    const double a = l.is_real() ? l.real_value() : static_cast<double>(l.i);
                    const double b = r.is_real() ? r.real_value() : static_cast<double>(r.i);
                    double v = 0.0;
                    switch (e.op) {
                        case BinOp::Add: v = a + b; break;
                        case BinOp::Sub: v = a - b; break;
                        case BinOp::Mul: v = a * b; break;
                        case BinOp::Div:
                            if (b == 0.0) return "division by zero";
                            v = a / b;
                            break;
                        default: return "unsupported operation on a REAL operand";
                    }
                    out = Datum::make_real(v);
                    return std::nullopt;
                }
                if (l.type != Type::Int || r.type != Type::Int)
                    return "arithmetic requires INT operands";
                // F13: temporal arithmetic (DATE/TIMESTAMP/TIME/INTERVAL — all INT-backed seconds,
                // except DATE = days). DATE is normalised to seconds. Supported: temporal ± INTERVAL
                // -> same temporal (TIME wraps into a day; DATE+interval promotes to TIMESTAMP);
                // INTERVAL ± INTERVAL; TIMESTAMP/TIME/DATE difference -> INTERVAL; INTERVAL */ INT.
                // F13b: MONTH-INTERVAL (logical 12) arithmetic — calendar (variable-length) months,
                // kept SEPARATE from the seconds-based temporal math below (we don't store a composite
                // months+seconds interval, so a month interval mixed with a seconds value is rejected).
                if (l.logical == 12 || r.logical == 12) {
                    const bool lm = l.logical == 12, rm = r.logical == 12;
                    if (e.op == BinOp::Mul || e.op == BinOp::Div) {  // month-interval * / int
                        const bool other_int = (lm ? r.logical : l.logical) == 0;
                        if (!(lm ^ rm) || !other_int) return "unsupported MONTH interval operation";
                        const std::int64_t mv = lm ? l.i : r.i, k = lm ? r.i : l.i;
                        if (e.op == BinOp::Div && k == 0) return "division by zero";
                        std::int64_t v = 0;
                        bool ovf = false;
                        if (e.op == BinOp::Mul) {
                            ovf = mul_ovf(mv, k, v);
                        } else {
                            v = mv / k;
                        }
                        if (ovf) return "integer overflow";
                        out = Datum::make_int(v); out.logical = 12; return std::nullopt;
                    }
                    if (lm && rm) {  // month-interval ± month-interval
                        std::int64_t v = 0;
                        if (e.op == BinOp::Add ? add_ovf(l.i, r.i, v) : sub_ovf(l.i, r.i, v))
                            return "integer overflow";
                        out = Datum::make_int(v); out.logical = 12; return std::nullopt;
                    }
                    // DATE / TIMESTAMP ± month-interval (calendar add, day clamped to month length).
                    if (lm && e.op == BinOp::Sub)
                        return "cannot subtract a date/timestamp from a MONTH interval";
                    const std::uint8_t base_lg = lm ? r.logical : l.logical;
                    const std::int64_t base = lm ? r.i : l.i;
                    std::int64_t mon = lm ? l.i : r.i;
                    if (e.op == BinOp::Sub) mon = -mon;  // date - interval
                    if (base_lg == 2) {
                        out = Datum::make_int(add_months_days(base, mon));
                        out.logical = 2; return std::nullopt;
                    }
                    if (base_lg == 3) {
                        out = Datum::make_int(add_months_secs(base, mon));
                        out.logical = 3; return std::nullopt;
                    }
                    return "a MONTH interval may only be combined with a DATE / TIMESTAMP / MONTH interval";
                }
                auto temporal = [](std::uint8_t lg) { return lg == 2 || lg == 3 || lg == 8 || lg == 10; };
                if (temporal(l.logical) || temporal(r.logical)) {
                    if (e.op == BinOp::Mul || e.op == BinOp::Div) {  // INTERVAL scaled by a number
                        const bool li = l.logical == 10, ri = r.logical == 10;
                        if (li ^ ri) {
                            const std::int64_t iv = li ? l.i : r.i, k = li ? r.i : l.i;
                            if (e.op == BinOp::Div && k == 0) return "division by zero";
                            std::int64_t v = 0;
                            bool ovf = false;
                            if (e.op == BinOp::Mul) {
                                ovf = mul_ovf(iv, k, v);
                            } else {
                                v = iv / k;
                            }
                            if (ovf) return "integer overflow";
                            out = Datum::make_int(v); out.logical = 10; return std::nullopt;
                        }
                        return "unsupported temporal operation";
                    }
                    std::uint8_t llg = l.logical, rlg = r.logical;
                    std::int64_t lv = l.i, rv = r.i;
                    if (llg == 2) { lv = l.i * 86400; llg = 3; }  // DATE days -> seconds (TIMESTAMP)
                    if (rlg == 2) { rv = r.i * 86400; rlg = 3; }
                    std::int64_t v = 0;
                    if (e.op == BinOp::Add ? add_ovf(lv, rv, v) : sub_ovf(lv, rv, v))
                        return "integer overflow";
                    std::uint8_t out_lg = 0;
                    if ((llg == 3 && rlg == 10) || (llg == 10 && rlg == 3 && e.op == BinOp::Add)) out_lg = 3;
                    else if (llg == 8 && rlg == 10) { out_lg = 8; v = ((v % 86400) + 86400) % 86400; }
                    else if (llg == 10 && rlg == 10) out_lg = 10;
                    else if (llg == 3 && rlg == 3 && e.op == BinOp::Sub) out_lg = 10;
                    else if (llg == 8 && rlg == 8 && e.op == BinOp::Sub) out_lg = 10;
                    else if (llg == 10 && rlg == 0) out_lg = 10;
                    else if (llg == 0 && rlg == 10) out_lg = 10;
                    else return "unsupported temporal operand combination";
                    out = Datum::make_int(v); out.logical = out_lg; return std::nullopt;
                }
                // F9b: DECIMAL fixed-point arithmetic — all on int64 (deterministic). The
                // effective scale of a non-DECIMAL operand is 0. Add/Sub align to the wider
                // scale; Mul adds scales; Div keeps the left scale. The result is tagged
                // DECIMAL when either operand is. (DATE/TIMESTAMP arithmetic stays raw INT —
                // adding days/secs is a sensible, deterministic plain-int operation.)
                const bool ldec = l.logical == 1, rdec = r.logical == 1;
                const std::uint8_t ls = ldec ? l.scale : 0, rs = rdec ? r.scale : 0;
                auto p10 = [](std::uint8_t k) { std::int64_t p = 1; while (k--) p *= 10; return p; };
                std::int64_t v = 0;
                std::uint8_t out_sc = 0;
                switch (e.op) {
                    case BinOp::Add:
                    case BinOp::Sub: {
                        const std::uint8_t cs = ls > rs ? ls : rs;
                        std::int64_t li = 0, ri = 0;
                        if (mul_ovf(l.i, p10(cs - ls), li) || mul_ovf(r.i, p10(cs - rs), ri))
                            return "integer overflow";
                        if (e.op == BinOp::Add ? add_ovf(li, ri, v) : sub_ovf(li, ri, v))
                            return "integer overflow";
                        out_sc = cs;
                        break;
                    }
                    case BinOp::Mul:
                        if (mul_ovf(l.i, r.i, v)) return "integer overflow";
                        out_sc = static_cast<std::uint8_t>(ls + rs);
                        break;
                    case BinOp::Div: {
                        if (r.i == 0) return "division by zero";
                        std::int64_t num = 0;
                        if (mul_ovf(l.i, p10(rs), num)) return "integer overflow";  // pre-scale by rs
                        if (l.i == INT64_MIN && r.i == -1) return "integer overflow";  // INT_MIN/-1 UB
                        v = num / r.i;  // result scale = ls
                        out_sc = ls;
                        break;
                    }
                    case BinOp::Mod:
                        if (r.i == 0) return "modulo by zero";
                        v = (l.i == INT64_MIN && r.i == -1) ? 0 : l.i % r.i;  // INT_MIN%-1 UB -> 0
                        out_sc = ls;
                        break;
                }
                out = Datum::make_int(v);
                if (ldec || rdec) { out.logical = 1; out.scale = out_sc; }
                return std::nullopt;
            }
            case ExprKind::Func:
                return eval_func(e, t, row, out);
            case ExprKind::Case: {
                for (std::size_t i = 0; i < e.case_when.size(); ++i) {
                    bool truth = false;
                    if (auto er = eval_pred(e.case_when[i], e.case_when[i].root, t, row,
                                            /*group=*/nullptr, truth))
                        return er;
                    if (truth) return eval_expr(*e.case_then[i], t, row, out);
                }
                if (e.case_else) return eval_expr(*e.case_else, t, row, out);
                out = Datum::make_null(Type::Int);
                return std::nullopt;
            }
            case ExprKind::Array: {  // F12: build an array value from the element expressions
                std::vector<Datum> elems;
                elems.reserve(e.args.size());
                for (const auto& a : e.args) {
                    Datum d;
                    if (auto er = eval_expr(*a, t, row, d)) return er;
                    elems.push_back(std::move(d));
                }
                const std::uint8_t el = elems.empty() ? 0 : elems.front().logical;
                const std::uint8_t es = elems.empty() ? 0 : elems.front().scale;
                out = Datum::make_text(Datum::encode_array(el, es, elems));
                out.logical = 7;
                return std::nullopt;
            }
            case ExprKind::Subscript: {  // F12: arr[idx] — 1-based; out-of-range / NULL -> NULL
                Datum arr, idx;
                if (auto er = eval_expr(*e.left, t, row, arr)) return er;
                if (auto er = eval_expr(*e.right, t, row, idx)) return er;
                if (arr.is_null || idx.is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
                if (arr.logical != 7) return "subscript ([]) requires an ARRAY operand";
                if (idx.type != Type::Int) return "array subscript must be an INT";
                const std::vector<Datum> elems = Datum::decode_array(arr.s);
                if (idx.i < 1 || idx.i > static_cast<std::int64_t>(elems.size()))
                    out = Datum::make_null(Type::Int);  // out of range -> NULL (SQL)
                else
                    out = elems[static_cast<std::size_t>(idx.i - 1)];
                return std::nullopt;
            }
        }
        return "unhandled expression";
    }

    // F3: does the parent table `tname` have a row whose PK == `v`? (A FOREIGN KEY references the
    // parent's PK.) Reads the parent over the verified read path (columnar or row).
    [[nodiscard]] bool fk_parent_has(const std::string& tname, const Datum& v) {
        const Table* p = catalog_.find(tname);
        if (p == nullptr) return false;
        if (p->columnar) return read_columnar_row(*p, v).has_value();
        const ReadResult r = read_committed(encode_key(*p, v));
        return r.has_value() && !is_tombstone(*r);
    }

    // F3: enforce every FOREIGN KEY of a row about to be inserted — a non-NULL FK value must exist as
    // the referenced parent's PK. A NULL FK is allowed (unenforced, SQL semantics).
    [[nodiscard]] std::optional<std::string> eval_foreign_keys(const Table& t,
                                                              const std::vector<Datum>& row) {
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            const Column& col = t.columns[c];
            if (col.fk_table.empty()) continue;
            const Datum& v = row[c];
            if (v.is_null) continue;
            if (!fk_parent_has(col.fk_table, v)) {
                return "FOREIGN KEY violation: column '" + col.name +
                       "' has no matching row in '" + col.fk_table + "'";
            }
        }
        return std::nullopt;
    }

    // F3 DELETE RESTRICT: true iff some CHILD table has a row whose FK references `parent`'s PK `pk`
    // (so the parent row may not be deleted). Scans every table that declares such an FK.
    [[nodiscard]] bool fk_referenced(const std::string& parent, const Datum& pk) {
        for (const auto& [name, child] : catalog_.all()) {
            for (std::size_t c = 0; c < child.columns.size(); ++c) {
                if (child.columns[c].fk_table != parent) continue;
                SelectStmt scan;
                scan.table = child.name;
                std::vector<std::vector<Datum>> rows;
                if (scan_table(child, scan, rows)) continue;  // unreadable child — skip (best effort)
                for (const std::vector<Datum>& r : rows) {
                    if (!r[c].is_null && cmp_datum(r[c], pk) == 0) return true;
                }
            }
        }
        return false;
    }

    // F5: evaluate every CHECK constraint of `t` against a row about to be written. Each check is
    // re-parsed (its stored source text) as `SELECT * FROM <t> WHERE <check>` and its predicate is
    // evaluated; a check that is not TRUE rejects the write. NULL/UNKNOWN counts as NOT satisfied
    // here is too strict — SQL CHECK passes on UNKNOWN; so a NULL/UNKNOWN result is treated as OK.
    [[nodiscard]] std::optional<std::string> eval_checks(const Table& t,
                                                         const std::vector<Datum>& row) {
        for (const std::string& chk : t.checks) {
            ParseResult pr = parse_sql("SELECT * FROM " + t.name + " WHERE " + chk);
            if (!pr.ok()) {
                return "invalid CHECK constraint '" + chk + "': " + pr.error().render();
            }
            const SelectStmt& s = pr.stmt().select;
            // eval_pred returns truth=false for both FALSE and UNKNOWN; to let CHECK pass on UNKNOWN
            // (SQL semantics) we re-check: a check fails only when the predicate is definitively false
            // for a fully-present row. For simplicity (and matching the common `col >= 0` shape) we
            // reject when the predicate is not true.
            bool truth = false;
            if (auto e = eval_pred(s.filter, s.filter.root, t, row, /*group=*/nullptr, truth)) {
                return e;
            }
            if (!truth) {
                return "CHECK constraint violated: " + chk;
            }
        }
        return std::nullopt;
    }

    // C3: compute a window function's value for EVERY row (returned in `rows` order). Rows are
    // partitioned by PARTITION BY; each partition is ordered by the window's ORDER BY; then
    // ROW_NUMBER/RANK number the ordered rows and SUM/COUNT/MIN/MAX aggregate over the whole
    // partition (same value for every partition row). Deterministic (ordered map + stable sort).
    [[nodiscard]] std::optional<std::string> compute_window(const WindowFunc& w, const Table& t,
                                                            const std::vector<std::vector<Datum>>& rows,
                                                            std::vector<Datum>& out) {
        out.assign(rows.size(), Datum::make_null(Type::Int));
        std::vector<std::size_t> pcols;
        for (const std::string& p : w.partition_by) {
            const auto i = t.column_index(p);
            if (!i) return "unknown PARTITION BY column '" + p + "'";
            pcols.push_back(*i);
        }
        std::vector<std::size_t> ocols;
        for (const OrderKey& k : w.order_by) {
            const auto i = t.column_index(k.column);
            if (!i) return "unknown window ORDER BY column '" + k.column + "'";
            ocols.push_back(*i);
        }
        std::size_t argc = 0;
        if (w.kind == WinKind::Sum || w.kind == WinKind::Min || w.kind == WinKind::Max ||
            w.kind == WinKind::Count || w.kind == WinKind::Avg || w.kind == WinKind::Lag ||
            w.kind == WinKind::Lead || w.kind == WinKind::FirstValue ||
            w.kind == WinKind::LastValue) {
            const auto i = t.column_index(w.arg_column);
            if (!i) return "unknown column '" + w.arg_column + "' in window function";
            argc = *i;
        }
        // Partition the row indices (ordered map => deterministic partition order).
        std::map<std::vector<std::string>, std::vector<std::size_t>> parts;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            std::vector<std::string> key;
            key.reserve(pcols.size());
            for (const std::size_t pc : pcols) key.push_back(group_key_field(rows[r][pc]));
            parts[key].push_back(r);
        }
        for (auto& [k, idxs] : parts) {
            (void)k;
            if (!ocols.empty()) {
                std::stable_sort(idxs.begin(), idxs.end(), [&](std::size_t a, std::size_t b) {
                    for (std::size_t j = 0; j < ocols.size(); ++j) {
                        const int c = cmp_datum(rows[a][ocols[j]], rows[b][ocols[j]]);
                        if (c != 0) return w.order_by[j].descending ? (c > 0) : (c < 0);
                    }
                    return false;
                });
            }
            if (w.kind == WinKind::RowNumber) {
                for (std::size_t k2 = 0; k2 < idxs.size(); ++k2)
                    out[idxs[k2]] = Datum::make_int(static_cast<std::int64_t>(k2 + 1));
            } else if (w.kind == WinKind::Rank) {
                std::int64_t rank = 0;
                for (std::size_t k2 = 0; k2 < idxs.size(); ++k2) {
                    bool tie = false;
                    if (k2 > 0 && !ocols.empty()) {
                        tie = true;
                        for (const std::size_t oc : ocols)
                            if (cmp_datum(rows[idxs[k2]][oc], rows[idxs[k2 - 1]][oc]) != 0) { tie = false; break; }
                    }
                    if (!tie) rank = static_cast<std::int64_t>(k2 + 1);  // gaps on ties
                    out[idxs[k2]] = Datum::make_int(rank);
                }
            } else if (w.kind == WinKind::DenseRank) {
                std::int64_t rank = 0;
                for (std::size_t k2 = 0; k2 < idxs.size(); ++k2) {
                    bool tie = false;
                    if (k2 > 0 && !ocols.empty()) {
                        tie = true;
                        for (const std::size_t oc : ocols)
                            if (cmp_datum(rows[idxs[k2]][oc], rows[idxs[k2 - 1]][oc]) != 0) { tie = false; break; }
                    }
                    if (!tie) ++rank;  // NO gaps on ties (dense)
                    out[idxs[k2]] = Datum::make_int(rank);
                }
            } else if (w.kind == WinKind::Lag || w.kind == WinKind::Lead) {
                for (std::size_t k2 = 0; k2 < idxs.size(); ++k2) {
                    const std::size_t n = idxs.size();
                    const bool have = (w.kind == WinKind::Lag) ? (k2 > 0) : (k2 + 1 < n);
                    const std::size_t src = (w.kind == WinKind::Lag) ? k2 - 1 : k2 + 1;
                    out[idxs[k2]] = have ? rows[idxs[src]][argc]
                                         : Datum::make_null(t.columns[argc].type);
                }
            } else if (w.kind == WinKind::FirstValue || w.kind == WinKind::LastValue) {
                if (!idxs.empty()) {
                    const Datum& v = (w.kind == WinKind::FirstValue) ? rows[idxs.front()][argc]
                                                                     : rows[idxs.back()][argc];
                    for (const std::size_t r : idxs) out[r] = v;
                }
            } else if (w.kind == WinKind::Ntile) {
                // Bucket 1..n: the first (size mod n) buckets get one extra row (PostgreSQL).
                const std::int64_t sz = static_cast<std::int64_t>(idxs.size());
                const std::int64_t n = w.ntile_n < 1 ? 1 : w.ntile_n;
                const std::int64_t base = sz / n, rem = sz % n;
                std::int64_t pos = 0, bucket = 1;
                for (std::size_t k2 = 0; k2 < idxs.size(); ++k2) {
                    const std::int64_t cap = base + (bucket <= rem ? 1 : 0);
                    if (pos >= cap && bucket < n) { ++bucket; pos = 0; }
                    out[idxs[k2]] = Datum::make_int(bucket);
                    ++pos;
                }
            } else if (w.has_frame) {
                // ROWS-frame aggregate: for each row, aggregate over its frame slice of the
                // ordered partition (running total / moving average). Physical row offsets.
                const std::int64_t sz = static_cast<std::int64_t>(idxs.size());
                auto bound_pos = [&](const FrameBound& b, std::int64_t k2) -> std::int64_t {
                    switch (b.kind) {
                        case FrameBoundKind::UnboundedPreceding: return 0;
                        case FrameBoundKind::Preceding: return k2 - b.offset;
                        case FrameBoundKind::CurrentRow: return k2;
                        case FrameBoundKind::Following: return k2 + b.offset;
                        case FrameBoundKind::UnboundedFollowing: return sz - 1;
                    }
                    return k2;
                };
                for (std::int64_t k2 = 0; k2 < sz; ++k2) {
                    std::int64_t lo = bound_pos(w.frame_start, k2);
                    std::int64_t hi = bound_pos(w.frame_end, k2);
                    if (lo < 0) lo = 0;
                    if (hi > sz - 1) hi = sz - 1;
                    std::int64_t npres = 0, sum = 0, total = 0;
                    Datum best;
                    bool any = false;
                    for (std::int64_t p = lo; p <= hi; ++p) {  // empty frame if lo > hi
                        ++total;
                        const Datum& d = rows[idxs[static_cast<std::size_t>(p)]][argc];
                        if (w.kind == WinKind::CountStar) continue;
                        if (d.is_null) continue;
                        ++npres;
                        if (d.type == Type::Int && add_ovf(sum, d.i, sum))
                            return "integer overflow in window SUM";
                        if (!any) { best = d; any = true; }
                        else {
                            const int c = cmp_datum(d, best);
                            if ((w.kind == WinKind::Min && c < 0) || (w.kind == WinKind::Max && c > 0)) best = d;
                        }
                    }
                    Datum v;
                    if (w.kind == WinKind::CountStar) v = Datum::make_int(total);
                    else if (w.kind == WinKind::Count) v = Datum::make_int(npres);
                    else if (w.kind == WinKind::Sum) v = Datum::make_int(sum);
                    else if (w.kind == WinKind::Avg)
                        v = npres > 0 ? Datum::make_int(sum / npres) : Datum::make_null(Type::Int);
                    else v = any ? best : Datum::make_null(t.columns[argc].type);
                    out[idxs[static_cast<std::size_t>(k2)]] = v;
                }
            } else {
                // Whole-partition aggregate (same value for every row in the partition).
                std::int64_t total = static_cast<std::int64_t>(idxs.size());
                std::int64_t npres = 0, sum = 0;
                Datum best;
                bool any = false;
                for (const std::size_t r : idxs) {
                    const Datum& d = rows[r][argc];
                    if (w.kind == WinKind::CountStar) continue;
                    if (d.is_null) continue;
                    ++npres;
                    if (d.type == Type::Int && add_ovf(sum, d.i, sum))  // F9d
                        return "integer overflow in window SUM";
                    if (!any) { best = d; any = true; }
                    else {
                        const int c = cmp_datum(d, best);
                        if ((w.kind == WinKind::Min && c < 0) || (w.kind == WinKind::Max && c > 0)) best = d;
                    }
                }
                Datum v;
                if (w.kind == WinKind::CountStar) v = Datum::make_int(total);
                else if (w.kind == WinKind::Count) v = Datum::make_int(npres);
                else if (w.kind == WinKind::Sum) v = Datum::make_int(sum);
                else if (w.kind == WinKind::Avg)  // INT truncation toward zero; NULL over an empty partition
                    v = npres > 0 ? Datum::make_int(sum / npres) : Datum::make_null(Type::Int);
                else v = any ? best : Datum::make_null(t.columns[argc].type);  // Min/Max
                for (const std::size_t r : idxs) out[r] = v;
            }
        }
        return std::nullopt;
    }

    // A1: walk an expression and verify every column reference exists (so an unknown column errors
    // up front — even on an empty table where the per-row eval would never run).
    [[nodiscard]] std::optional<std::string> validate_expr_columns(const Expr& e, const Table& t) {
        switch (e.kind) {
            case ExprKind::Col:
                if (!t.column_index(e.column))
                    return "unknown column '" + e.column + "' in expression";
                return std::nullopt;
            case ExprKind::Lit:
                return std::nullopt;
            case ExprKind::Neg:
                return validate_expr_columns(*e.left, t);
            case ExprKind::Bin:
                if (auto er = validate_expr_columns(*e.left, t)) return er;
                return validate_expr_columns(*e.right, t);
            case ExprKind::Func:
                for (const auto& a : e.args)
                    if (auto er = validate_expr_columns(*a, t)) return er;
                return std::nullopt;
            case ExprKind::Case:
                for (const auto& th : e.case_then)
                    if (auto er = validate_expr_columns(*th, t)) return er;
                if (e.case_else)
                    if (auto er = validate_expr_columns(*e.case_else, t)) return er;
                return std::nullopt;
            case ExprKind::Array:  // F12
                for (const auto& a : e.args)
                    if (auto er = validate_expr_columns(*a, t)) return er;
                return std::nullopt;
            case ExprKind::Subscript:
                if (auto er = validate_expr_columns(*e.left, t)) return er;
                return validate_expr_columns(*e.right, t);
        }
        return std::nullopt;
    }

    // A2/A4: scalar function evaluation. NULL args generally propagate (COALESCE is the exception).
    [[nodiscard]] std::optional<std::string> eval_func(const Expr& e, const Table& t,
                                                       const std::vector<Datum>& row, Datum& out) {
        std::vector<Datum> a(e.args.size());
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            if (auto er = eval_expr(*e.args[i], t, row, a[i])) return er;
        }
        const std::string& f = e.func;
        auto need = [&](std::size_t n) { return a.size() == n; };
        // W9 CONNECTION / SESSION functions — drivers and ORMs call these on connect. Zero-arg,
        // pure, constant per session (args, if any, ignored). version() reports a PostgreSQL-
        // compatible string so a driver's `PostgreSQL <major>` parse succeeds.
        if (f == "VERSION") {
            out = Datum::make_text("PostgreSQL 16.0 (Lockstep 0.1.0)");
            return std::nullopt;
        }
        if (f == "CURRENT_DATABASE" || f == "CURRENT_CATALOG") {
            out = Datum::make_text("lockstep");
            return std::nullopt;
        }
        if (f == "CURRENT_SCHEMA") {
            out = Datum::make_text("public");
            return std::nullopt;
        }
        if (f == "CURRENT_USER" || f == "SESSION_USER" || f == "USER") {
            out = Datum::make_text("lockstep");
            return std::nullopt;
        }
        if (f == "CAST") {
            if (!need(1)) return "CAST takes one argument";
            if (a[0].is_null) { out = Datum::make_null(e.cast_type); return std::nullopt; }
            if (e.cast_type == Type::Int) {
                out = a[0].type == Type::Int ? a[0]
                                             : Datum::make_int(std::strtoll(a[0].s.c_str(), nullptr, 10));
            } else {
                out = a[0].type == Type::Text ? a[0] : Datum::make_text(std::to_string(a[0].i));
            }
            return std::nullopt;
        }
        if (f == "COALESCE") {
            for (const Datum& d : a) if (!d.is_null) { out = d; return std::nullopt; }
            out = a.empty() ? Datum::make_null(Type::Int) : Datum::make_null(a.back().type);
            return std::nullopt;
        }
        // W9 extra scalar functions (pure, deterministic). NULLIF / GREATEST / LEAST follow
        // PostgreSQL NULL semantics; the numeric ones require INT args, the string ones TEXT.
        if (f == "NULLIF") {
            if (!need(2)) return "NULLIF takes two arguments";
            if (!a[0].is_null && !a[1].is_null && a[0] == a[1]) {
                out = Datum::make_null(a[0].type);
            } else {
                out = a[0];
            }
            return std::nullopt;
        }
        if (f == "GREATEST" || f == "LEAST") {
            const bool want_max = (f == "GREATEST");
            bool have = false;
            Datum best;
            for (const Datum& d : a) {
                if (d.is_null) continue;  // PG: NULLs are ignored
                if (!have) { best = d; have = true; continue; }
                const bool d_wins = want_max ? best.less_than(d) : d.less_than(best);
                if (d_wins) best = d;
            }
            out = have ? best : Datum::make_null(a.empty() ? Type::Int : a.back().type);
            return std::nullopt;
        }
        if (f == "MOD") {
            if (!need(2)) return "MOD takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Int || a[1].type != Type::Int) return "MOD requires INT arguments";
            if (a[1].i == 0) return "MOD by zero";
            out = Datum::make_int(a[0].i % a[1].i);
            return std::nullopt;
        }
        if (f == "SIGN") {
            if (!need(1)) return "SIGN takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Int) return "SIGN requires an INT argument";
            out = Datum::make_int(a[0].i > 0 ? 1 : (a[0].i < 0 ? -1 : 0));
            return std::nullopt;
        }
        if (f == "REVERSE") {
            if (!need(1)) return "REVERSE takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text) return "REVERSE requires a TEXT argument";
            std::string s = a[0].s;
            std::reverse(s.begin(), s.end());
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        if (f == "REPEAT") {
            if (!need(2)) return "REPEAT takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Int) return "REPEAT requires (TEXT, INT)";
            std::string s;
            for (std::int64_t k = 0; k < a[1].i && k < 1'000'000; ++k) s += a[0].s;
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        if (f == "LEFT" || f == "RIGHT") {
            if (!need(2)) return f + " takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Int) return f + " requires (TEXT, INT)";
            const std::string& s = a[0].s;
            const std::int64_t n = a[1].i;
            std::string res;
            if (n > 0) {
                const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(n), s.size());
                res = (f == "LEFT") ? s.substr(0, take) : s.substr(s.size() - take);
            }  // n <= 0 -> empty (PG semantics for the simple case)
            out = Datum::make_text(std::move(res));
            return std::nullopt;
        }
        if (f == "LTRIM" || f == "RTRIM") {
            if (!need(1)) return f + " takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text) return f + " requires a TEXT argument";
            std::string s = a[0].s;
            if (f == "LTRIM") {
                std::size_t i = 0;
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                s.erase(0, i);
            } else {
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            }
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        if (f == "STRPOS" || f == "POSITION") {
            if (!need(2)) return f + " takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Text) return f + " requires TEXT arguments";
            const auto pos = a[0].s.find(a[1].s);
            out = Datum::make_int(pos == std::string::npos ? 0 : static_cast<std::int64_t>(pos + 1));
            return std::nullopt;
        }
        if (f == "SPLIT_PART") {  // SPLIT_PART(text, delim, n) — 1-based field, '' if out of range
            if (!need(3)) return "SPLIT_PART takes three arguments";
            if (a[0].is_null || a[1].is_null || a[2].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Text || a[2].type != Type::Int)
                return "SPLIT_PART requires (TEXT, TEXT, INT)";
            const std::string& s = a[0].s;
            const std::string& d = a[1].s;
            const std::int64_t n = a[2].i;
            std::string res;
            if (n >= 1) {
                std::int64_t idx = 1;
                std::size_t start = 0;
                if (d.empty()) {
                    if (n == 1) res = s;
                } else {
                    while (true) {
                        const std::size_t hit = s.find(d, start);
                        if (idx == n) {
                            res = s.substr(start, hit == std::string::npos ? std::string::npos : hit - start);
                            break;
                        }
                        if (hit == std::string::npos) break;  // n beyond the last field -> ''
                        start = hit + d.size();
                        ++idx;
                    }
                }
            }
            out = Datum::make_text(std::move(res));
            return std::nullopt;
        }
        if (f == "INITCAP") {  // capitalise the first letter of each word, lowercase the rest
            if (!need(1)) return "INITCAP takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text) return "INITCAP requires a TEXT argument";
            std::string s = a[0].s;
            bool start_word = true;
            for (char& c : s) {
                const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (!alnum) { start_word = true; continue; }
                if (start_word) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
                else c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
                start_word = false;
            }
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        if (f == "LPAD" || f == "RPAD") {  // pad/truncate to length with a fill string (default space)
            if (a.size() != 2 && a.size() != 3) return f + " takes two or three arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Int) return f + " requires (TEXT, INT[, TEXT])";
            const std::string fill = (a.size() == 3 && !a[2].is_null) ? a[2].s : std::string(" ");
            const std::int64_t len = a[1].i;
            const std::string& s = a[0].s;
            std::string res;
            if (len <= 0) {
                res.clear();
            } else if (static_cast<std::size_t>(len) <= s.size()) {
                res = s.substr(0, static_cast<std::size_t>(len));
            } else if (fill.empty()) {
                res = s;
            } else {
                std::string pad;
                const std::size_t need_pad = static_cast<std::size_t>(len) - s.size();
                while (pad.size() < need_pad) pad += fill;
                pad.resize(need_pad);
                res = (f == "LPAD") ? pad + s : s + pad;
            }
            out = Datum::make_text(std::move(res));
            return std::nullopt;
        }
        if (f == "ASCII") {  // codepoint of the first byte (0 for empty)
            if (!need(1)) return "ASCII takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Text) return "ASCII requires a TEXT argument";
            out = Datum::make_int(a[0].s.empty() ? 0 : static_cast<std::int64_t>(static_cast<unsigned char>(a[0].s[0])));
            return std::nullopt;
        }
        if (f == "CHR") {  // single-byte char for a codepoint in 1..255
            if (!need(1)) return "CHR takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (a[0].type != Type::Int) return "CHR requires an INT argument";
            if (a[0].i < 1 || a[0].i > 255) return "CHR argument out of range (1..255)";
            out = Datum::make_text(std::string(1, static_cast<char>(a[0].i)));
            return std::nullopt;
        }
        if (f == "DATE_PART") {  // DATE_PART('field', date|timestamp) -> INT component
            if (!need(2)) return "DATE_PART takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Text) return "DATE_PART field must be TEXT";
            // The value must be a DATE (logical 2, days) or TIMESTAMP (logical 3, seconds).
            const std::uint8_t lg = a[1].logical;
            if (a[1].type != Type::Int || (lg != 2 && lg != 3))
                return "DATE_PART requires a DATE or TIMESTAMP argument";
            std::string field;
            for (char c : a[0].s) field.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
            const std::int64_t raw = a[1].i;                     // days (DATE) or seconds (TIMESTAMP)
            const std::int64_t secs = (lg == 3) ? raw : raw * 86400;
            // Floor-divide to days + non-negative time-of-day remainder.
            std::int64_t days = secs / 86400;
            std::int64_t tod = secs - days * 86400;
            if (tod < 0) { tod += 86400; --days; }
            std::int64_t y = 0;
            unsigned mo = 0, d = 0;
            days_to_civil(days, y, mo, d);
            std::int64_t res = 0;
            if (field == "year") res = y;
            else if (field == "month") res = mo;
            else if (field == "day") res = d;
            else if (field == "hour") res = tod / 3600;
            else if (field == "minute") res = (tod % 3600) / 60;
            else if (field == "second") res = tod % 60;
            else if (field == "dow") res = ((days % 7) + 4 + 7) % 7;   // 0=Sunday (1970-01-01 = Thu)
            else if (field == "doy") res = days - civil_to_days(y, 1, 1) + 1;
            else if (field == "epoch") res = secs;
            else return "DATE_PART: unknown field '" + a[0].s + "'";
            out = Datum::make_int(res);
            return std::nullopt;
        }
        if (f == "DATE_TRUNC") {  // DATE_TRUNC('unit', date|timestamp) -> same type, truncated
            if (!need(2)) return "DATE_TRUNC takes two arguments";
            if (a[0].is_null || a[1].is_null) { out = a[1]; out.is_null = true; return std::nullopt; }
            if (a[0].type != Type::Text) return "DATE_TRUNC unit must be TEXT";
            const std::uint8_t lg = a[1].logical;
            if (a[1].type != Type::Int || (lg != 2 && lg != 3))
                return "DATE_TRUNC requires a DATE or TIMESTAMP argument";
            std::string unit;
            for (char c : a[0].s) unit.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
            const std::int64_t raw = a[1].i;
            const std::int64_t secs = (lg == 3) ? raw : raw * 86400;
            std::int64_t days = secs / 86400;
            std::int64_t tod = secs - days * 86400;
            if (tod < 0) { tod += 86400; --days; }
            std::int64_t y = 0;
            unsigned mo = 0, d = 0;
            days_to_civil(days, y, mo, d);
            std::int64_t out_days = days;
            std::int64_t out_tod = 0;  // truncated units below drop the time-of-day
            if (unit == "year") out_days = civil_to_days(y, 1, 1);
            else if (unit == "month") out_days = civil_to_days(y, mo, 1);
            else if (unit == "day") { /* out_days = days, tod=0 */ }
            else if (unit == "hour") out_tod = (tod / 3600) * 3600;
            else if (unit == "minute") out_tod = (tod / 60) * 60;
            else if (unit == "second") out_tod = tod;
            else return "DATE_TRUNC: unknown unit '" + a[0].s + "'";
            const std::int64_t out_secs = out_days * 86400 + out_tod;
            out = Datum::make_int(lg == 3 ? out_secs : out_days);
            out.logical = lg;  // preserve DATE (2) / TIMESTAMP (3)
            return std::nullopt;
        }
        if (f == "ABS") {
            if (!need(1)) return "ABS takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Int) return "ABS requires an INT argument";
            out = Datum::make_int(a[0].i < 0 ? -a[0].i : a[0].i);
            return std::nullopt;
        }
        // String functions below need their (single, where applicable) arg present.
        if (f == "UPPER" || f == "LOWER" || f == "LENGTH") {
            if (!need(1)) return f + " takes one argument";
            if (a[0].is_null) { out = Datum::make_null(f == "LENGTH" ? Type::Int : Type::Text); return std::nullopt; }
            if (a[0].type != Type::Text) return f + " requires a TEXT argument";
            if (f == "LENGTH") { out = Datum::make_int(static_cast<std::int64_t>(a[0].s.size())); return std::nullopt; }
            std::string s = a[0].s;
            for (char& c : s) {
                if (f == "UPPER") c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
                else c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            }
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        if (f == "SUBSTR") {
            if (a.size() != 2 && a.size() != 3) return "SUBSTR takes (text, start[, len])";
            if (a[0].is_null || a[1].is_null || (a.size() == 3 && a[2].is_null)) {
                out = Datum::make_null(Type::Text);
                return std::nullopt;
            }
            if (a[0].type != Type::Text || a[1].type != Type::Int ||
                (a.size() == 3 && a[2].type != Type::Int))
                return "SUBSTR(text, INT[, INT])";
            const std::string& s = a[0].s;
            // 1-based start (SQL); clamp into range.
            std::int64_t start = a[1].i < 1 ? 1 : a[1].i;
            std::size_t b = static_cast<std::size_t>(start - 1);
            if (b > s.size()) b = s.size();
            std::size_t len = a.size() == 3 ? static_cast<std::size_t>(a[2].i < 0 ? 0 : a[2].i)
                                            : s.size() - b;
            out = Datum::make_text(s.substr(b, len));
            return std::nullopt;
        }
        if (f == "CONCAT") {
            std::string s;
            for (const Datum& d : a) {
                if (d.is_null) continue;  // SQL CONCAT skips NULLs
                s += d.type == Type::Text ? d.s : std::to_string(d.i);
            }
            out = Datum::make_text(std::move(s));
            return std::nullopt;
        }
        // F13: JSON access `json -> key` / `json ->> key` (key = TEXT object member, or INT 0-based
        // array index). `->` yields JSON (NULL if absent); `->>` yields the value as TEXT.
        if (f == "->" || f == "->>") {
            if (a[0].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            json::JVal root;
            std::size_t jp = 0;
            if (!json::parse_value(a[0].s, jp, root)) return "left operand of -> is not JSON";
            const json::JVal* found = nullptr;
            if (a[1].type == Type::Text) {
                if (root.kind == json::JVal::Obj)
                    for (const auto& en : root.obj)
                        if (en.key == a[1].s) { found = &en.val; break; }
            } else if (root.kind == json::JVal::Arr && a[1].i >= 0 &&
                       a[1].i < static_cast<std::int64_t>(root.arr.size())) {
                found = &root.arr[static_cast<std::size_t>(a[1].i)];
            }
            if (!found) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (f == "->>") {
                if (found->kind == json::JVal::Str) { out = Datum::make_text(found->text); }
                else if (found->kind == json::JVal::Null) { out = Datum::make_null(Type::Text); }
                else { std::string s; json::serialize(*found, s); out = Datum::make_text(std::move(s)); }
            } else {
                std::string s;
                json::serialize(*found, s);
                out = Datum::make_text(std::move(s));
                out.logical = 11;
            }
            return std::nullopt;
        }
        // JSON path access `json #> path` / `json #>> path`: the path is a TEXT/INT array of
        // object keys / array indices walked left-to-right. `#>` yields JSON, `#>>` yields TEXT.
        if (f == "#>" || f == "#>>") {
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Text); return std::nullopt; }
            json::JVal root;
            std::size_t jp = 0;
            if (!json::parse_value(a[0].s, jp, root)) return "left operand of #> is not JSON";
            std::vector<Datum> path;
            if (a[1].logical == 7 && a[1].type == Type::Text) path = Datum::decode_array(a[1].s);
            else path.push_back(a[1]);  // a bare key/index is a one-element path
            const json::JVal* cur = &root;
            for (const Datum& step : path) {
                if (cur == nullptr || step.is_null) { cur = nullptr; break; }
                if (cur->kind == json::JVal::Obj) {
                    const std::string key = step.type == Type::Text ? step.s : std::to_string(step.i);
                    const json::JVal* nx = nullptr;
                    for (const auto& en : cur->obj)
                        if (en.key == key) { nx = &en.val; break; }
                    cur = nx;
                } else if (cur->kind == json::JVal::Arr) {
                    const std::int64_t idx =
                        step.type == Type::Int ? step.i : std::strtoll(step.s.c_str(), nullptr, 10);
                    cur = (idx >= 0 && idx < static_cast<std::int64_t>(cur->arr.size()))
                              ? &cur->arr[static_cast<std::size_t>(idx)] : nullptr;
                } else {
                    cur = nullptr;
                }
            }
            if (cur == nullptr) { out = Datum::make_null(Type::Text); return std::nullopt; }
            if (f == "#>>") {
                if (cur->kind == json::JVal::Str) out = Datum::make_text(cur->text);
                else if (cur->kind == json::JVal::Null) out = Datum::make_null(Type::Text);
                else { std::string s; json::serialize(*cur, s); out = Datum::make_text(std::move(s)); }
            } else {
                std::string s;
                json::serialize(*cur, s);
                out = Datum::make_text(std::move(s));
                out.logical = 11;  // JSON
            }
            return std::nullopt;
        }
        // JSON_CONTAINS(doc, sub) -> 1/0 — the function form of the `@>` containment operator.
        if (f == "JSON_CONTAINS") {
            if (!need(2)) return "JSON_CONTAINS takes (json, json)";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            out = Datum::make_int(json_contains(a[0].s, a[1].s) ? 1 : 0);
            return std::nullopt;
        }
        if (f == "JSON_ARRAY_LENGTH") {
            if (!need(1)) return "JSON_ARRAY_LENGTH takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            json::JVal root;
            std::size_t jp = 0;
            if (!json::parse_value(a[0].s, jp, root) || root.kind != json::JVal::Arr)
                return "JSON_ARRAY_LENGTH requires a JSON array";
            out = Datum::make_int(static_cast<std::int64_t>(root.arr.size()));
            return std::nullopt;
        }
        // F12: array functions. An array argument is logical==7 (16-byte-ish self-describing payload).
        auto is_array = [](const Datum& d) { return d.logical == 7 && d.type == Type::Text; };
        if (f == "ARRAY_LENGTH" || f == "CARDINALITY") {
            if (!need(1)) return f + " takes one argument";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (!is_array(a[0])) return f + " requires an ARRAY argument";
            out = Datum::make_int(static_cast<std::int64_t>(Datum::decode_array(a[0].s).size()));
            return std::nullopt;
        }
        if (f == "ARRAY_CONTAINS") {  // array_contains(arr, x) -> 1/0 (== the `x = ANY(arr)` test)
            if (!need(2)) return "ARRAY_CONTAINS takes (array, element)";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (!is_array(a[0])) return "ARRAY_CONTAINS requires an ARRAY first argument";
            std::int64_t found = 0;
            for (const Datum& el : Datum::decode_array(a[0].s))
                if (!el.is_null && !a[1].is_null && cmp_datum(el, a[1]) == 0) { found = 1; break; }
            out = Datum::make_int(found);
            return std::nullopt;
        }
        if (f == "ARRAY_POSITION") {  // 1-based index of the first match, else NULL
            if (!need(2)) return "ARRAY_POSITION takes (array, element)";
            if (a[0].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (!is_array(a[0])) return "ARRAY_POSITION requires an ARRAY first argument";
            const std::vector<Datum> elems = Datum::decode_array(a[0].s);
            out = Datum::make_null(Type::Int);
            for (std::size_t k = 0; k < elems.size(); ++k)
                if (!elems[k].is_null && !a[1].is_null && cmp_datum(elems[k], a[1]) == 0) {
                    out = Datum::make_int(static_cast<std::int64_t>(k + 1));
                    break;
                }
            return std::nullopt;
        }
        // K2: MATCHES(text_col, 'query') — 1 iff EVERY query term occurs in the text
        // (deterministic tokenizer; needs no index, evaluable per row anywhere in SQL).
        if (f == "MATCHES") {
            if (!need(2)) return "MATCHES takes (text, 'query')";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
            if (a[0].type != Type::Text || a[1].type != Type::Text)
                return "MATCHES requires TEXT arguments";
            std::uint32_t dl = 0;
            const auto doc = bm25_tokens(a[0].s, dl);
            const auto q2 = bm25_tokens(a[1].s, dl);
            std::int64_t all = 1;
            for (const auto& [term, cnt] : q2) {
                (void)cnt;
                if (doc.find(term) == doc.end()) { all = 0; break; }
            }
            out = Datum::make_int(all);
            return std::nullopt;
        }
        // K2.4: RRF_SCORE is served by the fused hybrid top-k path (ORDER BY
        // rrf_score(vec_col, '[q]', text_col, 'q') DESC LIMIT k) — see rrf_try_topk.
        if (f == "RRF_SCORE") {
            return "RRF_SCORE is supported in ORDER BY rrf_score(vec_col, '[qvec]', text_col, "
                   "'qtext') DESC LIMIT k over IVFFLAT + BM25 indexed columns";
        }
        // K2: BM25_SCORE is served by the index-backed top-k path (ORDER BY
        // bm25_score(col,'q') DESC LIMIT k). Projecting the score per row would need
        // per-row corpus scans — a clean error until the projected form ships.
        if (f == "BM25_SCORE") {
            return "BM25_SCORE is supported in ORDER BY bm25_score(col, 'query') DESC LIMIT k "
                   "over a BM25-indexed column (projecting the score is not supported yet)";
        }
        // K1 (vector search): distance / similarity over two equal-length numeric vectors (VECTOR,
        // REAL[] or INT[]; a plain '[x,y,z]' text literal is parsed as a vector). Returns a REAL.
        // `ORDER BY l2_distance(embedding, ARRAY[...]) LIMIT k` is brute-force k-NN. L2_DISTANCE =
        // Euclidean; INNER_PRODUCT / DOT_PRODUCT = dot; COSINE_DISTANCE = 1 - cosine similarity (a
        // zero-magnitude vector yields distance 1, i.e. no similarity).
        if (f == "L2_DISTANCE" || f == "EUCLIDEAN_DISTANCE" || f == "COSINE_DISTANCE" ||
            f == "INNER_PRODUCT" || f == "DOT_PRODUCT" ||
            f == "<->" || f == "<#>" || f == "<=>") {  // K1.2: pgvector operator spellings
            if (!need(2)) return f + " takes (vector, vector)";
            if (a[0].is_null || a[1].is_null) { out = Datum::make_null(Type::Text); out.logical = 14; return std::nullopt; }
            // K1 perf: extract each operand STRAIGHT into doubles. An array/vector payload
            // walks bytes (ivf_doubles_fast — no per-element Datum/string); a '[x,y,z]'
            // TEXT literal (the constant query vector, otherwise re-parsed through strtod
            // on EVERY row of a brute k-NN) is parsed once and memoised by content in
            // vec_lit_cache_. Values and error surface are byte-identical to the Datum
            // path: a NULL element is flagged (0.0 placeholder keeps the length exact)
            // and reported AFTER the length check, exactly like the old per-element loop.
            const auto load_vec = [this](const Datum& d, std::vector<double>& o, bool& null_el) -> bool {
                null_el = false;
                if (d.type != Type::Text) return false;
                if (d.logical == 7 || d.logical == 15) {
                    if (ivf_doubles_fast(d.s, o)) return true;
                    const std::vector<Datum> elems = Datum::decode_array(d.s);
                    o.clear();
                    o.reserve(elems.size());
                    for (const Datum& e2 : elems) {
                        if (e2.is_null) { null_el = true; o.push_back(0.0); continue; }
                        o.push_back(e2.is_real() ? e2.real_value() : static_cast<double>(e2.i));
                    }
                    return true;
                }
                if (d.logical == 0) {
                    const auto it = vec_lit_cache_.find(d.s);
                    if (it != vec_lit_cache_.end()) { o = it->second; return true; }
                    std::vector<Datum> elems;
                    if (parse_vector_literal(d.s, elems)) return false;
                    o.clear();
                    o.reserve(elems.size());
                    for (const Datum& e2 : elems) o.push_back(e2.real_value());
                    if (vec_lit_cache_.size() >= 64) vec_lit_cache_.clear();  // bound the memo
                    vec_lit_cache_.emplace(d.s, o);
                    return true;
                }
                return false;
            };
            std::vector<double> u, v;
            bool u_null = false, v_null = false;
            if (!load_vec(a[0], u, u_null) || !load_vec(a[1], v, v_null))
                return f + " requires two vector arguments (VECTOR, ARRAY, or a '[x,y,z]' literal)";
            if (u.size() != v.size()) return f + " requires vectors of equal length";
            if (u_null || v_null) return f + " does not accept NULL vector elements";
            double dot = 0.0, sq = 0.0, nu = 0.0, nv = 0.0;  // shared 4-lane kernel
            vec_accum4(u.size(), v, [&u](std::size_t k) { return u[k]; }, dot, sq, nu, nv);
            double r = 0.0;
            if (f == "L2_DISTANCE" || f == "EUCLIDEAN_DISTANCE" || f == "<->") r = std::sqrt(sq);
            else if (f == "COSINE_DISTANCE" || f == "<=>") {
                const double denom = std::sqrt(nu) * std::sqrt(nv);
                r = (denom == 0.0) ? 1.0 : 1.0 - dot / denom;
            } else if (f == "<#>") r = -dot;  // K1.2: pgvector's NEGATIVE inner product
            else r = dot;  // INNER_PRODUCT / DOT_PRODUCT
            out = Datum::make_real(r);
            return std::nullopt;
        }
        if (f == "ARRAY_APPEND") {  // array_append(arr, x) -> arr with x added
            if (!need(2)) return "ARRAY_APPEND takes (array, element)";
            if (!is_array(a[0])) return "ARRAY_APPEND requires an ARRAY first argument";
            std::vector<Datum> elems = Datum::decode_array(a[0].s);
            elems.push_back(a[1]);
            out = Datum::make_text(Datum::encode_array(static_cast<std::uint8_t>(a[0].s[0]),
                                                       static_cast<std::uint8_t>(a[0].s[1]), elems));
            out.logical = 7;
            return std::nullopt;
        }
        if (f == "ARRAY_CAT") {  // array_cat(a, b) -> concatenation
            if (!need(2)) return "ARRAY_CAT takes (array, array)";
            if (!is_array(a[0]) || !is_array(a[1])) return "ARRAY_CAT requires two ARRAY arguments";
            std::vector<Datum> elems = Datum::decode_array(a[0].s);
            for (Datum& el : Datum::decode_array(a[1].s)) elems.push_back(std::move(el));
            out = Datum::make_text(Datum::encode_array(static_cast<std::uint8_t>(a[0].s[0]),
                                                       static_cast<std::uint8_t>(a[0].s[1]), elems));
            out.logical = 7;
            return std::nullopt;
        }
        return "unknown function '" + f + "'";
    }

    ExecResult exec_aggregate(const SelectStmt& sel, const Table& t,
                              std::vector<std::vector<Datum>>& rows) {
        // Resolve GROUP BY column indices.
        std::vector<std::size_t> gcols;
        for (const std::string& gc : sel.group_by) {
            const auto idx = t.column_index(gc);
            if (!idx) {
                return ExecResult::failure("unknown GROUP BY column '" + gc +
                                           "' in table '" + t.name + "'");
            }
            gcols.push_back(*idx);
        }

        // Validate the SELECT list: each plain column must be a GROUP BY column (real
        // SQL rejects a non-grouped, non-aggregate column). With NO group_by, ANY
        // plain column in the list is illegal alongside aggregates.
        for (const SelectItem& item : sel.items) {
            if (item.kind != SelectItemKind::Column) {
                continue;
            }
            const auto idx = t.column_index(item.column);
            if (!idx) {
                return ExecResult::failure("unknown column '" + item.column +
                                           "' in table '" + t.name + "'");
            }
            bool grouped = false;
            for (const std::size_t g : gcols) {
                if (g == *idx) {
                    grouped = true;
                    break;
                }
            }
            if (!grouped && !gs_mode_) {  // C2: in a GROUPING SETS run a non-set column is NULL, not an error
                return ExecResult::failure(
                    "column '" + item.column +
                    "' must appear in GROUP BY or be used in an aggregate "
                    "(non-grouped non-aggregate column)");
            }
        }
        // Validate aggregate target columns + their types up front.
        if (auto e = validate_aggs(sel, t)) {
            return ExecResult::failure(*e);
        }

        // Fold rows into groups keyed by the group-column tuple. Ordered map =>
        // deterministic group order (sorted by the encoded key tuple).
        std::map<std::vector<std::string>, Group> groups;
        const bool ungrouped = gcols.empty();
        for (const auto& row : rows) {
            std::vector<std::string> key;
            key.reserve(gcols.size());
            for (const std::size_t g : gcols) {
                key.push_back(group_key_field(row[g]));
            }
            Group& grp = groups[key];
            if (grp.rows.empty()) {
                grp.key_datums.reserve(gcols.size());
                for (const std::size_t g : gcols) {
                    grp.key_datums.push_back(row[g]);
                }
            }
            grp.rows.push_back(&row);
        }
        // An ungrouped aggregate over ZERO rows still yields ONE row (e.g.
        // SELECT COUNT(*) FROM empty => 0). Synthesize the empty group.
        if (ungrouped && groups.empty()) {
            groups[std::vector<std::string>{}] = Group{};
        }

        ExecResult r;
        for (const auto& [key, grp] : groups) {
            // Compute each aggregate / grouped-column cell for this group.
            ResultRow out;
            bool having_ok = true;
            if (sel.having.present()) {
                if (auto e = eval_pred(sel.having, sel.having.root, t,
                                       /*row=*/dummy_row(), &grp, having_ok)) {
                    return ExecResult::failure(*e);
                }
            }
            if (!having_ok) {
                continue;
            }
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Column) {
                    const std::size_t idx = *t.column_index(item.column);
                    // C2: a column not in THIS grouping set renders NULL.
                    bool grouped = false;
                    for (const std::size_t g : gcols) if (g == idx) { grouped = true; break; }
                    Datum d = grouped ? grouped_column_value(gcols, grp, idx)
                                      : Datum::make_null(t.columns[idx].type);
                    out.cells.emplace_back(item.label, d);
                } else {
                    Datum d;
                    if (auto e = compute_agg(item.agg, t, grp, d)) {
                        return ExecResult::failure(*e);
                    }
                    out.cells.emplace_back(item.label, d);
                }
            }
            r.rows.push_back(std::move(out));
        }

        // DISTINCT / ORDER BY / LIMIT over the grouped output.
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by_labels(sel, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    // ========================================================================
    // v3 JOIN PIPELINE. FROM produces a per-table row stream (a scan); JOIN combines
    // streams (hash/nested-loop, LEFT NULL-fills) into a JOINED row; then WHERE / GROUP
    // / HAVING / ORDER / LIMIT apply over the joined row. Qualified columns resolve
    // against the joined schema. All sugar over the SAME Database scan reads.
    // ========================================================================

    // The joined-schema column layout: a flat list of (alias, column-name, type), in
    // FROM order. A joined ROW is a parallel std::vector<Datum> of the same length.
    struct JoinColumn {
        std::string alias;
        std::string column;
        Type type = Type::Int;
        std::uint8_t logical = 0;  // F9b+: carries DECIMAL/DATE/TS/UUID/INT128 tag through a join
        std::uint8_t scale = 0;
    };
    struct JoinSchema {
        std::vector<JoinColumn> cols;  // flat, in FROM order (table0 cols, table1 ...)
        // alias -> [lo, hi) slice of `cols` for that table (for NULL-filling a side).
        std::map<std::string, std::pair<std::size_t, std::size_t>> alias_span;

        // Resolve a (qualifier, column) reference to a flat column index. Returns an
        // error string on unknown alias/column or an AMBIGUOUS bare column.
        [[nodiscard]] std::optional<std::string> resolve(
            const std::string& qualifier, const std::string& column,
            std::size_t& out) const {
            if (!qualifier.empty()) {
                const auto it = alias_span.find(qualifier);
                if (it == alias_span.end()) {
                    return std::string("unknown table/alias '" + qualifier +
                                       "' in column reference");
                }
                for (std::size_t i = it->second.first; i < it->second.second; ++i) {
                    if (cols[i].column == column) {
                        out = i;
                        return std::nullopt;
                    }
                }
                return std::string("unknown column '" + column + "' in table/alias '" +
                                   qualifier + "'");
            }
            // Bare column: must be unique across the joined schema.
            std::optional<std::size_t> found;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (cols[i].column == column) {
                    if (found) {
                        return std::string("ambiguous column '" + column +
                                           "' (present in multiple joined tables; "
                                           "qualify it as <table>." + column + ")");
                    }
                    found = i;
                }
            }
            if (!found) {
                return std::string("unknown column '" + column +
                                   "' in the joined schema");
            }
            out = *found;
            return std::nullopt;
        }
    };

    // A scanned base table: its schema + its decoded rows (PK-ascending scan order).
    struct ScannedTable {
        const Table* table = nullptr;
        std::string alias;
        std::vector<std::vector<Datum>> rows;  // each row in the table's schema order
    };

    // A3 — fused streaming JOIN + aggregate for the star-schema shape. Returns nullopt (fall back
    // to the AoS join) unless: exactly 2 tables, inner equi-join (fact.key = dim.key), an aggregate
    // / GROUP BY query, NO WHERE / HAVING, and the FACT (from[0]) is a clean flushed COLUMNAR table
    // (its columns readable as SoA). Folds EXACTLY as exec_aggregate_joined (same group_key_field
    // ordered map, same compute_agg_joined per-aggregate semantics) — proven byte-identical by the
    // columnar==row-mode JOIN conformance gate (row-mode falls to the AoS path).
    [[nodiscard]] std::optional<ExecResult> try_fused_join_aggregate(const SelectStmt& sel) {
        if (sel.from.size() != 2 || sel.from[1].kind != JoinKind::Inner) return std::nullopt;
        if (sel.filter.present() || sel.having.present()) return std::nullopt;
        if (agg_has_distinct(sel)) return std::nullopt;  // C1: fold can't dedup → general path
        if (!sel.has_aggregates && sel.group_by.empty()) return std::nullopt;
        const Table* fact = catalog_.find(sel.from[0].table);
        const Table* dim = catalog_.find(sel.from[1].table);
        if (fact == nullptr || dim == nullptr) return std::nullopt;
        if (!fact->columnar || fact->delta_dirty || !has_blocks(*fact) || has_overlays(*fact)) {
            return std::nullopt;  // need fact columns as a complete SoA (clean flushed single run)
        }
        // Build the joined schema (fact cols then dim cols) — same layout as exec_select_joined.
        JoinSchema schema;
        const std::size_t fnc = fact->columns.size();
        for (const Column& c : fact->columns)
            schema.cols.push_back(JoinColumn{sel.from[0].alias, c.name, c.type, c.logical, c.scale});
        schema.alias_span.emplace(sel.from[0].alias, std::make_pair(std::size_t{0}, fnc));
        for (const Column& c : dim->columns)
            schema.cols.push_back(JoinColumn{sel.from[1].alias, c.name, c.type, c.logical, c.scale});
        schema.alias_span.emplace(sel.from[1].alias,
                                  std::make_pair(fnc, fnc + dim->columns.size()));
        if (sel.from[0].alias == sel.from[1].alias) return std::nullopt;  // self-join: AoS path

        std::optional<std::pair<std::size_t, std::size_t>> equi;
        if (!sel.from[1].on.present() || detect_equi_key(sel.from[1].on, schema, sel.from[1].alias,
                                                         equi) ||
            !equi) {
            return std::nullopt;  // not a clean equi-join
        }
        // equi = (left_idx, right_idx) flat. fact key = left_idx (< fnc); dim key = right_idx - fnc.
        if (equi->first >= fnc || equi->second < fnc) return std::nullopt;
        const std::size_t fact_key_col = equi->first;
        const std::size_t dim_key_col = equi->second - fnc;

        // Resolve every GROUP BY + SELECT/agg column to a flat index, then to (is_fact, local col).
        struct Src {
            bool is_fact = true;
            std::size_t col = 0;
            Type type = Type::Int;
            const ColumnChunk* fp = nullptr;  // hoisted fact-col SoA pointer (is_fact only)
        };
        auto resolve_src = [&](const std::string& qual, const std::string& col,
                               Src& out) -> std::optional<std::string> {
            std::size_t idx = 0;
            if (auto e = schema.resolve(qual, col, idx)) return e;
            out.is_fact = idx < fnc;
            out.col = out.is_fact ? idx : (idx - fnc);
            out.type = schema.cols[idx].type;
            return std::nullopt;
        };
        std::vector<Src> gsrc;
        for (const std::string& gc : sel.group_by) {
            std::string q;
            std::string c;
            split_qualified(gc, q, c);
            Src s;
            if (resolve_src(q, c, s)) return std::nullopt;
            gsrc.push_back(s);
        }
        // Per SELECT item: a group-key index (>=0) OR an aggregate (kind + optional value Src).
        struct ItemPlan {
            bool is_col = false;
            std::size_t gkey = 0;        // for a column: which group key
            AggKind kind = AggKind::CountStar;
            bool has_val = false;
            Src val;
        };
        std::vector<ItemPlan> plans;
        for (const SelectItem& item : sel.items) {
            ItemPlan p;
            if (item.kind == SelectItemKind::Column) {
                Src s;
                if (resolve_src(item.qualifier, item.column, s)) return std::nullopt;
                std::size_t gk = sel.group_by.size();
                for (std::size_t k = 0; k < gsrc.size(); ++k) {
                    if (gsrc[k].is_fact == s.is_fact && gsrc[k].col == s.col) { gk = k; break; }
                }
                if (gk == sel.group_by.size()) return std::nullopt;  // ungrouped column
                p.is_col = true;
                p.gkey = gk;
            } else {
                p.kind = item.agg.kind;
                if (item.agg.kind != AggKind::CountStar) {
                    Src s;
                    if (resolve_src(item.agg.qualifier, item.agg.column, s)) return std::nullopt;
                    if ((item.agg.kind == AggKind::Sum || item.agg.kind == AggKind::Avg) &&
                        s.type != Type::Int) {
                        return std::nullopt;  // SUM/AVG needs INT (the AoS path errors; let it)
                    }
                    p.has_val = true;
                    p.val = s;
                }
            }
            plans.push_back(p);
        }

        // SCAN dim -> AoS rows (small) + a hash group_key_field(dim.key) -> matching dim rows.
        std::vector<std::vector<Datum>> dim_rows;
        if (auto e = scan_table(*dim, sel, dim_rows)) return std::nullopt;

        // HOIST: resolve every fact-column SoA pointer ONCE (cached concat; stable std::map ref) so
        // the hot loop never re-does a per-row cache lookup.
        const ColumnChunk* fkeyp = &read_col_concat(*fact, static_cast<std::uint32_t>(fact_key_col));
        for (Src& s : gsrc)
            if (s.is_fact) s.fp = &read_col_concat(*fact, static_cast<std::uint32_t>(s.col));
        for (ItemPlan& p : plans)
            if (!p.is_col && p.has_val && p.val.is_fact)
                p.val.fp = &read_col_concat(*fact, static_cast<std::uint32_t>(p.val.col));
        const ColumnChunk& fkey = *fkeyp;
        const std::uint32_t fcount = fkey.count;
        const bool key_int = fkey.type == Type::Int;

        struct FAcc {
            std::int64_t total = 0;  // CountStar
            std::int64_t npres = 0;  // Count / SUM / AVG divisor
            std::int64_t sum = 0;    // SUM / AVG
            bool any = false;
            Datum best;  // MIN / MAX
        };
        struct FGroup {
            std::vector<Datum> keyd;
            std::vector<FAcc> accs;
        };
        std::map<std::vector<std::string>, FGroup> groups;
        const std::size_t nagg = plans.size();

        // DIM hash by the RAW key (int -> int map, no per-row string encode for the probe).
        std::map<std::int64_t, std::vector<std::uint32_t>> dim_hash_i;
        std::map<std::string, std::vector<std::uint32_t>> dim_hash_s;
        for (std::uint32_t di = 0; di < dim_rows.size(); ++di) {
            const Datum& dk = dim_rows[di][dim_key_col];
            if (key_int) dim_hash_i[dk.is_null ? 0 : dk.i].push_back(di);
            else dim_hash_s[group_key_field(dk)].push_back(di);
        }

        // Fold ONE joined pair (fact row rr x dim row di) into a group's accumulators.
        auto fold_pair = [&](FGroup& g, std::uint32_t rr, std::uint32_t di) {
            if (g.accs.empty()) {
                g.accs.resize(nagg);
                g.keyd.reserve(gsrc.size());
                for (const Src& s : gsrc)
                    g.keyd.push_back(s.is_fact ? s.fp->at(rr) : dim_rows[di][s.col]);
            }
            for (std::size_t a = 0; a < nagg; ++a) {
                const ItemPlan& p = plans[a];
                if (p.is_col) continue;
                FAcc& acc = g.accs[a];
                ++acc.total;
                if (p.kind == AggKind::CountStar) continue;
                const Datum v = p.val.is_fact ? p.val.fp->at(rr) : dim_rows[di][p.val.col];
                if (v.is_null) continue;
                ++acc.npres;
                if (p.kind == AggKind::Sum || p.kind == AggKind::Avg) acc.sum = wrap_add(acc.sum, v.i);  // F9d (columnar)
                else if (p.kind == AggKind::Min || p.kind == AggKind::Max) {
                    if (!acc.any) { acc.best = v; acc.any = true; }
                    else {
                        const int c = cmp_datum(v, acc.best);
                        if ((p.kind == AggKind::Min && c < 0) ||
                            (p.kind == AggKind::Max && c > 0)) acc.best = v;
                    }
                }
            }
        };
        auto probe = [&](std::uint32_t rr) -> const std::vector<std::uint32_t>* {
            if (key_int) {
                const auto it = dim_hash_i.find(fkey.ints[rr]);
                return it == dim_hash_i.end() ? nullptr : &it->second;
            }
            const auto it = dim_hash_s.find(group_key_field(fkey.at(rr)));
            return it == dim_hash_s.end() ? nullptr : &it->second;
        };

        // FAST: when every GROUP BY column is a DIM column, the group is constant per dim row —
        // precompute each dim row's group slot ONCE, so the per-fact-row work is just probe + fold
        // (no per-row group_key_field, no per-row map insert). A FACT group column falls to the
        // general per-pair path below.
        const bool all_dim_groups = [&] {
            for (const Src& s : gsrc) if (s.is_fact) return false;
            return true;
        }();
        if (all_dim_groups) {
            std::vector<FGroup*> dgrp(dim_rows.size(), nullptr);
            std::vector<std::string> kb;
            for (std::uint32_t di = 0; di < dim_rows.size(); ++di) {
                kb.clear();
                for (const Src& s : gsrc) kb.push_back(group_key_field(dim_rows[di][s.col]));
                FGroup& g = groups[kb];
                if (g.keyd.empty() && !gsrc.empty()) {
                    g.keyd.reserve(gsrc.size());
                    for (const Src& s : gsrc) g.keyd.push_back(dim_rows[di][s.col]);
                }
                if (g.accs.empty()) g.accs.resize(nagg);
                dgrp[di] = &g;
            }
            for (std::uint32_t rr = 0; rr < fcount; ++rr) {
                const std::vector<std::uint32_t>* m = probe(rr);
                if (m == nullptr) continue;
                for (const std::uint32_t di : *m) fold_pair(*dgrp[di], rr, di);
            }
        } else {
            for (std::uint32_t rr = 0; rr < fcount; ++rr) {
                const std::vector<std::uint32_t>* m = probe(rr);
                if (m == nullptr) continue;  // inner join: no match => row drops
                std::vector<std::string> keybuf;
                for (const std::uint32_t di : *m) {
                    keybuf.clear();
                    keybuf.reserve(gsrc.size());
                    for (const Src& s : gsrc) {
                        keybuf.push_back(
                            group_key_field(s.is_fact ? s.fp->at(rr) : dim_rows[di][s.col]));
                    }
                    fold_pair(groups[keybuf], rr, di);
                }
            }
        }
        // Empty grouped result is valid (zero rows). An ungrouped query over zero matches still
        // emits ONE row — but the AoS path handles that; bail so semantics never diverge.
        if (groups.empty() && sel.group_by.empty()) return std::nullopt;

        ExecResult r;
        for (auto& [k, g] : groups) {
            (void)k;
            ResultRow out;
            for (std::size_t a = 0; a < nagg; ++a) {
                const ItemPlan& p = plans[a];
                if (p.is_col) {
                    out.cells.emplace_back(sel.items[a].label, g.keyd[p.gkey]);
                    continue;
                }
                const FAcc& acc = g.accs[a];
                Datum d;
                if (p.kind == AggKind::CountStar) {
                    d = Datum::make_int(acc.total);
                } else if (p.kind == AggKind::Count) {
                    d = Datum::make_int(acc.npres);
                } else if (acc.npres == 0) {
                    d = (p.kind == AggKind::Sum) ? Datum::make_int(0)
                                                 : Datum::make_null(p.val.type);
                } else if (p.kind == AggKind::Min || p.kind == AggKind::Max) {
                    d = acc.best;
                } else if (p.kind == AggKind::Sum) {
                    d = Datum::make_int(acc.sum);
                } else {  // AVG
                    d = Datum::make_int(acc.sum / acc.npres);
                }
                out.cells.emplace_back(sel.items[a].label, d);
            }
            r.rows.push_back(std::move(out));
        }
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by_labels_joined(sel, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    ExecResult exec_select_joined(const SelectStmt& sel) {
        // A3 — FUSED vectorized JOIN+GROUP BY fast path (star schema): stream the columnar fact
        // table's SoA columns, probe the small dim hash, and fold aggregates DIRECTLY — never
        // materializing the joined AoS row set (the ~100ms cost for a 500k-row join). Returns
        // nullopt to fall back to the general AoS join below; the columnar==row-mode JOIN
        // conformance gate cross-checks it byte-identical (row-mode takes the AoS path).
        last_join_index_nl_ = false;  // I4 test signal (reset per join)
        if (auto fused = try_fused_join_aggregate(sel)) {
            return std::move(*fused);
        }
        // I4: index nested-loop — a 2-table INNER equi-join whose INNER table is indexed on the join
        // key. Scan the outer; probe the inner's index per outer row (no inner full scan). Produces
        // the SAME joined rows as the hash path (left-major, inner matches PK-ascending), so the
        // result is byte-identical (the join conformance gate proves it).
        if (auto nl = try_index_nl_join(sel)) {
            return std::move(*nl);
        }
        // (0) Validate aliases unique + tables exist; build the joined schema.
        JoinSchema schema;
        std::vector<ScannedTable> scans;
        for (const JoinEntry& je : sel.from) {
            const Table* t = catalog_.find(je.table);
            if (t == nullptr) {
                return ExecResult::failure("unknown table '" + je.table + "'");
            }
            if (schema.alias_span.count(je.alias) != 0) {
                return ExecResult::failure(
                    "duplicate table alias '" + je.alias +
                    "' (each FROM/JOIN binding must be unique; use AS for a self-join)");
            }
            const std::size_t lo = schema.cols.size();
            for (const Column& c : t->columns) {
                schema.cols.push_back(JoinColumn{je.alias, c.name, c.type, c.logical, c.scale});
            }
            schema.alias_span.emplace(je.alias,
                                      std::make_pair(lo, schema.cols.size()));
            ScannedTable st;
            st.table = t;
            st.alias = je.alias;
            if (auto err = scan_table(*t, sel, st.rows)) {
                return ExecResult::failure(*err);
            }
            scans.push_back(std::move(st));
        }

        // (1) COMBINE the streams left-deep into the joined row set. Start with the
        // base table's rows widened to the full joined width (right side absent yet).
        std::vector<std::vector<Datum>> acc;  // joined rows (schema.cols.size() wide)
        acc.reserve(scans[0].rows.size());
        for (const auto& row : scans[0].rows) {
            std::vector<Datum> jr(schema.cols.size(), Datum{});
            place(schema, scans[0].alias, row, jr);
            acc.push_back(std::move(jr));
        }
        for (std::size_t k = 1; k < scans.size(); ++k) {
            std::vector<std::vector<Datum>> next;
            if (auto err = join_one(sel.from[k], schema, acc, scans[k], next)) {
                return ExecResult::failure(*err);
            }
            acc = std::move(next);
        }

        return finish_joined(sel, schema, acc);
    }

    // I4: index nested-loop join fast path. 2-table INNER equi-join where the INNER (from[1]) is a
    // row-mode table with a single/leading-column index on the join key. Scans the OUTER once and
    // index-probes the inner per outer row (no inner full scan); returns nullopt to fall back to the
    // general AoS join. The joined-row order (outer-major, inner matches PK-ascending) is identical to
    // hash_join's, so finish_joined yields a byte-identical result.
    [[nodiscard]] std::optional<ExecResult> try_index_nl_join(const SelectStmt& sel) {
        if (sel.from.size() != 2 || sel.from[1].kind != JoinKind::Inner) return std::nullopt;
        if (sel.from[0].alias == sel.from[1].alias) return std::nullopt;  // self-join: general path
        const Table* outer = catalog_.find(sel.from[0].table);
        const Table* inner = catalog_.find(sel.from[1].table);
        if (outer == nullptr || inner == nullptr || inner->columnar) return std::nullopt;
        // Build the joined schema (outer cols then inner cols) — same layout as exec_select_joined.
        JoinSchema schema;
        for (const Column& c : outer->columns)
            schema.cols.push_back(JoinColumn{sel.from[0].alias, c.name, c.type, c.logical, c.scale});
        schema.alias_span.emplace(sel.from[0].alias, std::make_pair(std::size_t{0}, outer->columns.size()));
        const std::size_t in_lo = schema.cols.size();
        for (const Column& c : inner->columns)
            schema.cols.push_back(JoinColumn{sel.from[1].alias, c.name, c.type, c.logical, c.scale});
        schema.alias_span.emplace(sel.from[1].alias, std::make_pair(in_lo, schema.cols.size()));
        // The ON must be a single equi key resolvable to (outer col, inner col).
        std::optional<std::pair<std::size_t, std::size_t>> equi;
        if (!sel.from[1].on.present() || detect_equi_key(sel.from[1].on, schema, sel.from[1].alias, equi))
            return std::nullopt;
        if (!equi) return std::nullopt;
        const std::size_t outer_col = equi->first;            // flat schema idx (in the outer span)
        const std::size_t inner_local = equi->second - in_lo;  // inner table local column
        if (equi->first >= in_lo) return std::nullopt;         // expect (outer, inner) orientation
        // The inner must have a single/leading-column index on the join column (eq lookup).
        const Index* ix = inner->index_for_column(inner_local);
        if (ix == nullptr || ix->columns.empty() || ix->columns[0] != inner_local ||
            !ix->partial_src.empty() || ix->gin)  // I5: partial / J3: GIN can't serve a whole-value join key
            return std::nullopt;

        last_join_index_nl_ = true;  // TEST signal: the index-NL path was taken
        // Scan the outer once; probe the inner index per outer row.
        std::vector<std::vector<Datum>> orows;
        if (auto e = scan_table(*outer, sel, orows)) return ExecResult::failure(*e);
        const std::vector<bool> in_need(inner->columns.size(), true);
        std::vector<std::vector<Datum>> acc;
        for (const auto& orow : orows) {
            const Datum& key = orow[outer_col];
            if (key.is_null) continue;  // a NULL join key matches nothing (SQL)
            IndexPlan plan;
            plan.index = ix;
            plan.is_eq = true;
            plan.eq = key;
            plan.eq_prefix.push_back(key);
            std::vector<std::vector<Datum>> matches;
            if (auto e = read_via_index(*inner, sel, plan, in_need, matches)) return ExecResult::failure(*e);
            for (auto& m : matches) {
                // The index entry's leading column equals the encoded key; a TEXT/logical key could
                // collide on encoding only if equal, but guard with an exact compare for safety.
                if (cmp_datum(m[inner_local], key) != 0) continue;
                std::vector<Datum> jr(schema.cols.size(), Datum{});
                place(schema, sel.from[0].alias, orow, jr);
                place(schema, sel.from[1].alias, m, jr);
                acc.push_back(std::move(jr));
            }
        }
        return finish_joined(sel, schema, acc);
    }

    // The post-COMBINE pipeline shared by the general join and the I4 index-NL fast path: (2) WHERE
    // over the joined rows, then (3) GROUP/AGGREGATE/HAVING or a plain projection (+ DISTINCT/ORDER/
    // LIMIT inside those). `acc` is the joined AoS row set in the deterministic left-major order.
    [[nodiscard]] ExecResult finish_joined(const SelectStmt& sel, const JoinSchema& schema,
                                           std::vector<std::vector<Datum>>& acc) {
        // (2) WHERE — keep a row iff the predicate is TRUE (NULL/UNKNOWN drops it).
        if (sel.filter.present()) {
            std::vector<std::vector<Datum>> kept;
            for (auto& jr : acc) {
                bool truth = false;
                if (auto e = eval_pred_joined(sel.filter, sel.filter.root, schema, jr, truth)) {
                    return ExecResult::failure(*e);
                }
                if (truth) kept.push_back(std::move(jr));
            }
            acc = std::move(kept);
        }
        // (3) GROUP/AGGREGATE/HAVING, else plain projection; then DISTINCT/ORDER/LIMIT.
        if (sel.has_aggregates || !sel.group_by.empty()) {
            return exec_aggregate_joined(sel, schema, acc);
        }
        return project_joined(sel, schema, acc);
    }

    // Scan ONE base table at the SELECT's D5 level into decoded rows (schema order).
    // A JOIN never takes the PK fast path (the parser suppresses it), so this is a
    // full scan + decode (tombstones dropped) — the SAME read the v2 path uses.
    [[nodiscard]] std::optional<std::string> scan_table(
        const Table& t, const SelectStmt& sel,
        std::vector<std::vector<Datum>>& rows_out) {
        // COLUMNAR tables store rows in blocks + the row 'd' delta, NOT the row-mode 't' prefix —
        // a 't'-prefix scan returns EMPTY (the columnar-JOIN bug). Read via the columnar path with
        // a clean FULL scan (all columns; the JOIN applies ON/WHERE later over the joined rows).
        if (t.columnar) {
            SelectStmt full;
            full.table = t.name;
            full.star = true;
            const std::vector<bool> need(t.columns.size(), true);
            return columnar_build_rows(t, full, need, rows_out);
        }
        std::vector<storage::KeyValue> kvs;
        if (auto err = run_select_at_level(t, sel, /*pk_fast=*/false, kvs)) {
            return err;
        }
        rows_out.reserve(kvs.size());
        std::size_t polled = 0;
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            // W3.2: poll the cancel flag every 64K decoded rows (cheap; a long scan aborts).
            if ((++polled & 0xFFFFu) == 0 && canceled()) {
                return std::string("query canceled");
            }
            rows_out.push_back(decode_row(t, kv.first, kv.second));
        }
        return std::nullopt;
    }

    // Copy a base-table row's cells into the joined row at the alias's flat span.
    static void place(const JoinSchema& schema, const std::string& alias,
                      const std::vector<Datum>& src, std::vector<Datum>& dst) {
        const auto it = schema.alias_span.find(alias);
        const std::size_t lo = it->second.first;
        for (std::size_t i = 0; i < src.size(); ++i) {
            dst[lo + i] = src[i];
        }
    }

    // NULL-fill an alias's flat span (a LEFT JOIN's unmatched right side).
    static void place_null(const JoinSchema& schema, const std::string& alias,
                           std::vector<Datum>& dst) {
        const auto it = schema.alias_span.find(alias);
        for (std::size_t i = it->second.first; i < it->second.second; ++i) {
            dst[i] = Datum::make_null(schema.cols[i].type);
        }
    }

    // Combine the accumulated left joined-rows `left` with one right table `rt` per the
    // JoinEntry. INNER/LEFT use the ON predicate; CROSS is the cartesian. When the ON
    // top-level is an equi-join key resolvable to (left-col, right-col) we HASH JOIN
    // (build a map on the right key, probe with left); else NESTED LOOP the full ON.
    // Output order is left-major, right-minor (the default deterministic join order).
    [[nodiscard]] std::optional<std::string> join_one(
        const JoinEntry& je, const JoinSchema& schema,
        const std::vector<std::vector<Datum>>& left, const ScannedTable& rt,
        std::vector<std::vector<Datum>>& out) {
        // Detect a hash-join equi-key: ON is exactly `lcol = rcol` (a single Cmp Eq,
        // col-vs-col) where one side resolves into the right table's span and the other
        // into the already-joined left columns. (Otherwise nested-loop the full ON.)
        std::optional<std::pair<std::size_t, std::size_t>> equi;  // (left_idx,right_idx)
        if (je.kind != JoinKind::Cross && je.on.present()) {
            if (auto e = detect_equi_key(je.on, schema, rt.alias, equi)) {
                return e;
            }
        }

        if (equi) {
            return hash_join(je, schema, left, rt, *equi, out);
        }
        return nested_loop_join(je, schema, left, rt, out);
    }

    // Build (right-key -> [right rows]) then probe with each left row's key. Preserves
    // left-major order; within a left row, right matches are in right scan order.
    [[nodiscard]] std::optional<std::string> hash_join(
        const JoinEntry& je, const JoinSchema& schema,
        const std::vector<std::vector<Datum>>& left, const ScannedTable& rt,
        std::pair<std::size_t, std::size_t> equi,
        std::vector<std::vector<Datum>>& out) {
        const std::size_t left_idx = equi.first;
        const auto rspan = schema.alias_span.find(rt.alias)->second;
        const std::size_t right_local =
            equi.second - rspan.first;  // column index within the right table

        // Ordered map keyed by the type-tagged key field => deterministic, NULL keys
        // never match (a NULL join key matches nothing, SQL semantics).
        std::map<std::string, std::vector<std::size_t>> index;
        for (std::size_t i = 0; i < rt.rows.size(); ++i) {
            const Datum& kd = rt.rows[i][right_local];
            if (kd.is_null) {
                continue;
            }
            index[group_key_field(kd)].push_back(i);
        }
        // E1: a RIGHT/FULL join must also emit each RIGHT row that no LEFT row matched, with the
        // LEFT side NULL-filled. Track which right rows got matched.
        const bool left_fill = je.kind == JoinKind::Left || je.kind == JoinKind::Full;
        const bool right_fill = je.kind == JoinKind::Right || je.kind == JoinKind::Full;
        std::vector<bool> rmatched(rt.rows.size(), false);
        std::size_t jpoll = 0;
        std::size_t mcharged = 0;  // W3.1: joined rows already charged to the memory budget
        const std::size_t row_bytes_est = schema.cols.size() * 24 + 8;
        for (const auto& ljr : left) {
            // W3.2: cancel poll per 1024 left rows (fires even when the join emits nothing).
            if ((++jpoll & 0x3FFu) == 0 && canceled()) return std::string("query canceled");
            // W3.1: charge the joined row set as it grows (per 4096 output rows) so an
            // O(n*m) join is bounded by the memory cap instead of growing unbounded (OOM).
            if (out.size() >= mcharged + 4096) {
                if (auto e = charge_query_mem((out.size() - mcharged) * row_bytes_est)) return e;
                mcharged = out.size();
            }
            const Datum& lk = ljr[left_idx];
            bool matched = false;
            if (!lk.is_null) {
                const auto it = index.find(group_key_field(lk));
                if (it != index.end()) {
                    for (const std::size_t ri : it->second) {
                        std::vector<Datum> jr = ljr;
                        place(schema, rt.alias, rt.rows[ri], jr);
                        out.push_back(std::move(jr));
                        matched = true;
                        rmatched[ri] = true;
                    }
                }
            }
            if (!matched && left_fill) {
                std::vector<Datum> jr = ljr;
                place_null(schema, rt.alias, jr);
                out.push_back(std::move(jr));
            }
        }
        if (right_fill) {
            for (std::size_t i = 0; i < rt.rows.size(); ++i) {
                if (!rmatched[i]) {
                    out.push_back(right_unmatched_row(schema, rt, rt.rows[i]));
                }
            }
        }
        return std::nullopt;
    }

    // E1: build a joined row for a RIGHT/FULL-join's unmatched right row — every column NULL except
    // the right table's span, which holds `rrow`. (The left side has no matching row.)
    [[nodiscard]] static std::vector<Datum> right_unmatched_row(
        const JoinSchema& schema, const ScannedTable& rt, const std::vector<Datum>& rrow) {
        std::vector<Datum> jr(schema.cols.size());
        for (std::size_t i = 0; i < jr.size(); ++i) {
            jr[i] = Datum::make_null(schema.cols[i].type);
        }
        place(schema, rt.alias, rrow, jr);
        return jr;
    }

    // Nested-loop: for each left row, scan every right row, evaluate the full ON
    // predicate (CROSS == always true). LEFT NULL-fills a left row with no match.
    [[nodiscard]] std::optional<std::string> nested_loop_join(
        const JoinEntry& je, const JoinSchema& schema,
        const std::vector<std::vector<Datum>>& left, const ScannedTable& rt,
        std::vector<std::vector<Datum>>& out) {
        const bool left_fill = je.kind == JoinKind::Left || je.kind == JoinKind::Full;
        const bool right_fill = je.kind == JoinKind::Right || je.kind == JoinKind::Full;
        std::vector<bool> rmatched(rt.rows.size(), false);
        std::size_t jpoll = 0;
        std::size_t mcharged = 0;  // W3.1: joined rows already charged to the memory budget
        const std::size_t row_bytes_est = schema.cols.size() * 24 + 8;
        for (const auto& ljr : left) {
            // W3.2: cancel poll per 1024 left rows (fires even when the join emits nothing).
            if ((++jpoll & 0x3FFu) == 0 && canceled()) return std::string("query canceled");
            // W3.1: charge the joined row set as it grows (per 4096 output rows) so an
            // O(n*m) join is bounded by the memory cap instead of growing unbounded (OOM).
            if (out.size() >= mcharged + 4096) {
                if (auto e = charge_query_mem((out.size() - mcharged) * row_bytes_est)) return e;
                mcharged = out.size();
            }
            bool matched = false;
            for (std::size_t rj = 0; rj < rt.rows.size(); ++rj) {
                const auto& rrow = rt.rows[rj];
                std::vector<Datum> jr = ljr;
                place(schema, rt.alias, rrow, jr);
                bool truth = true;  // CROSS: no ON => always true
                if (je.kind != JoinKind::Cross && je.on.present()) {
                    if (auto e = eval_pred_joined(je.on, je.on.root, schema, jr,
                                                  truth)) {
                        return e;
                    }
                }
                if (truth) {
                    out.push_back(std::move(jr));
                    matched = true;
                    rmatched[rj] = true;
                }
            }
            if (!matched && left_fill) {
                std::vector<Datum> jr = ljr;
                place_null(schema, rt.alias, jr);
                out.push_back(std::move(jr));
            }
        }
        if (right_fill) {
            for (std::size_t i = 0; i < rt.rows.size(); ++i) {
                if (!rmatched[i]) {
                    out.push_back(right_unmatched_row(schema, rt, rt.rows[i]));
                }
            }
        }
        return std::nullopt;
    }

    // Detect ON == a single top-level `a.x = b.y` equi-key. Fills `equi` with
    // (left-flat-idx, right-flat-idx) when one operand resolves into the right table's
    // span and the other into the LEFT (already-joined, NOT the right alias). Leaves
    // `equi` empty (=> nested loop) for anything else. Errors on an unresolved column.
    [[nodiscard]] std::optional<std::string> detect_equi_key(
        const Predicate& on, const JoinSchema& schema, const std::string& right_alias,
        std::optional<std::pair<std::size_t, std::size_t>>& equi) {
        if (on.root < 0) {
            return std::nullopt;
        }
        const PredNode& n = on.nodes[static_cast<std::size_t>(on.root)];
        if (n.kind != PredNodeKind::Cmp || n.op != CmpOp::Eq || !n.rhs_is_column ||
            n.operand != OperandKind::Column) {
            return std::nullopt;  // not a single col=col equality => nested loop
        }
        std::size_t a = 0;
        std::size_t b = 0;
        if (auto e = schema.resolve(n.qualifier, n.column, a)) {
            return e;
        }
        if (auto e = schema.resolve(n.rhs_qualifier, n.rhs_column, b)) {
            return e;
        }
        const auto rspan = schema.alias_span.find(right_alias)->second;
        const bool a_right = a >= rspan.first && a < rspan.second;
        const bool b_right = b >= rspan.first && b < rspan.second;
        // Exactly one operand must be the right table; the other the left side.
        if (a_right && !b_right) {
            // a is right, b is left. Types must match for the key to ever equal.
            if (schema.cols[a].type != schema.cols[b].type) {
                return std::nullopt;  // type-mismatched key never matches; nested loop
            }
            equi = std::make_pair(b, a);
        } else if (b_right && !a_right) {
            if (schema.cols[a].type != schema.cols[b].type) {
                return std::nullopt;
            }
            equi = std::make_pair(a, b);
        }
        return std::nullopt;
    }

    // Evaluate a predicate over a JOINED row (qualified-column resolution + NULL/three-
    // valued logic). `truth` is the TWO-valued collapse (UNKNOWN -> false). Used for
    // WHERE, ON, and HAVING (HAVING aggregates are handled by the grouped evaluator).
    [[nodiscard]] std::optional<std::string> eval_pred_joined(
        const Predicate& p, std::int32_t node, const JoinSchema& schema,
        const std::vector<Datum>& jr, bool& truth) {
        if (node < 0) {
            truth = true;
            return std::nullopt;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        switch (n.kind) {
            case PredNodeKind::And: {
                bool l = false;
                if (auto e = eval_pred_joined(p, n.left, schema, jr, l)) return e;
                if (!l) { truth = false; return std::nullopt; }
                bool r = false;
                if (auto e = eval_pred_joined(p, n.right, schema, jr, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Or: {
                bool l = false;
                if (auto e = eval_pred_joined(p, n.left, schema, jr, l)) return e;
                if (l) { truth = true; return std::nullopt; }
                bool r = false;
                if (auto e = eval_pred_joined(p, n.right, schema, jr, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Not: {
                bool c = false;
                if (auto e = eval_pred_joined(p, n.left, schema, jr, c)) return e;
                // DOCUMENTED, MODEL-MIRRORED two-valued collapse: a NULL comparison is
                // FALSE, and NOT is boolean negation of that collapsed child. (This is
                // simpler than full SQL three-valued NOT; the reference model uses the
                // identical rule so the conformance check is exact.)
                truth = !c;
                return std::nullopt;
            }
            case PredNodeKind::IsNull: {
                // v4: <col> IS [NOT] NULL over the joined row.
                std::size_t i = 0;
                if (auto e = schema.resolve(n.qualifier, n.column, i)) return e;
                const bool null = jr[i].is_null;
                truth = n.is_not ? !null : null;
                return std::nullopt;
            }
            case PredNodeKind::InList: {
                // v4: <col> [NOT] IN (SELECT ...) over the joined row.
                std::size_t i = 0;
                if (auto e = schema.resolve(n.qualifier, n.column, i)) return e;
                SubColumn sub;
                if (auto e = run_sub_column(*n.subquery, sub)) return e;
                return apply_in(jr[i], n.is_not, sub, truth);
            }
            case PredNodeKind::Exists: {
                // v4: [NOT] EXISTS (SELECT ...).
                bool ex = false;
                if (auto e = run_exists_sub(*n.subquery, ex)) return e;
                truth = n.is_not ? !ex : ex;
                return std::nullopt;
            }
            case PredNodeKind::Cmp:
                break;
        }
        // A comparison leaf over the joined row. Aggregates are not valid here.
        if (n.operand == OperandKind::Agg) {
            return std::string("an aggregate may not appear in WHERE / ON");
        }
        if (n.operand == OperandKind::Expr) {
            // J1: scalar-expression predicates are supported on single-table queries; over a
            // join the per-table eval_expr does not see the joined schema. Fail-closed.
            return std::string("expression predicates are not supported in a JOIN's WHERE/ON");
        }
        std::size_t li = 0;
        if (auto e = schema.resolve(n.qualifier, n.column, li)) {
            return e;
        }
        const Datum& lhs = jr[li];
        Datum rhs;
        if (n.rhs_is_subquery) {
            // v4: scalar subquery RHS over the joined row.
            bool snull = false;
            if (auto e = run_scalar_sub(*n.subquery, snull, rhs)) return e;
            if (snull) {
                truth = false;
                return std::nullopt;
            }
        } else if (n.rhs_is_column) {
            std::size_t ri = 0;
            if (auto e = schema.resolve(n.rhs_qualifier, n.rhs_column, ri)) {
                return e;
            }
            rhs = jr[ri];
        } else {
            rhs = n.literal;
        }
        // NULL operand => UNKNOWN => false (three-valued logic collapsed at filter).
        if (lhs.is_null || rhs.is_null) {
            truth = false;
            return std::nullopt;
        }
        if (lhs.type != rhs.type) {
            return std::string("type mismatch in predicate: comparing ") +
                   type_name(lhs.type) + " to " + type_name(rhs.type);
        }
        truth = leaf_truth(n.op, lhs, rhs);
        return std::nullopt;
    }

    // Resolve a SELECT-list / ORDER-BY / GROUP-BY column reference (qualifier+column)
    // to a flat joined index. A GROUP BY entry is a SPELLING ("a.x" or "x"): split it.
    static void split_qualified(const std::string& spelling, std::string& qual,
                                std::string& col) {
        const std::size_t dot = spelling.find('.');
        if (dot == std::string::npos) {
            qual.clear();
            col = spelling;
        } else {
            qual = spelling.substr(0, dot);
            col = spelling.substr(dot + 1);
        }
    }

    // Plain (non-aggregate) projection over the joined rows. Output labels are the
    // QUALIFIED spelling for a qualified ref (so a self-join's a.x vs b.x differ),
    // else the bare column name. SELECT * expands to ALL joined columns, labelled
    // alias.col (so duplicate column names across tables stay distinct).
    ExecResult project_joined(const SelectStmt& sel, const JoinSchema& schema,
                              const std::vector<std::vector<Datum>>& acc) {
        std::vector<std::size_t> proj;
        std::vector<std::string> labels;
        if (sel.star) {
            for (std::size_t i = 0; i < schema.cols.size(); ++i) {
                proj.push_back(i);
                labels.push_back(schema.cols[i].alias + "." + schema.cols[i].column);
            }
        } else {
            for (const SelectItem& item : sel.items) {
                std::size_t idx = 0;
                if (auto e = schema.resolve(item.qualifier, item.column, idx)) {
                    return ExecResult::failure(*e);
                }
                proj.push_back(idx);
                labels.push_back(item.label);
            }
        }
        ExecResult r;
        for (const auto& jr : acc) {
            ResultRow out;
            for (std::size_t k = 0; k < proj.size(); ++k) {
                out.cells.emplace_back(labels[k], jr[proj[k]]);
            }
            r.rows.push_back(std::move(out));
        }
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by_joined(sel, schema, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    // ORDER BY over a joined non-aggregate result. A key resolves to an output label
    // first (the projected/qualified label), else to a joined-schema column (resolved
    // + appended onto the row's comparison only if present in the output). For a JOIN
    // we require the ORDER BY key to be a PROJECTED output cell (like the v2 path: we
    // order over the SELECT output). The tie-break is the full rendered row (the
    // default join order is already the stable input order to stable_sort).
    [[nodiscard]] std::optional<std::string> apply_order_by_joined(
        const SelectStmt& sel, const JoinSchema& schema,
        std::vector<ResultRow>& rows) {
        if (sel.order_by.empty()) {
            return std::nullopt;
        }
        std::vector<std::string> keys;
        keys.reserve(sel.order_by.size());
        for (const OrderKey& k : sel.order_by) {
            if (k.expr) {  // K1.2: only the single-table scalar SELECT path evaluates these
                return std::string("ORDER BY expression is not supported in a JOIN SELECT");
            }
            const std::string spelling =
                k.qualifier.empty() ? k.column : k.qualifier + "." + k.column;
            // Accept either the bare column label or the qualified spelling, matching
            // however the SELECT list labelled it.
            if (has_label(rows, spelling)) {
                keys.push_back(spelling);
            } else if (has_label(rows, k.column)) {
                keys.push_back(k.column);
            } else {
                // Validate it at least resolves to a real joined column (clean error).
                std::size_t idx = 0;
                if (auto e = schema.resolve(k.qualifier, k.column, idx)) {
                    return e;
                }
                return std::string("ORDER BY '" + spelling +
                                   "' must reference a projected output column in a "
                                   "JOIN SELECT");
            }
        }
        const std::vector<OrderKey>& ob = sel.order_by;
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ResultRow& x, const ResultRow& y) {
                             for (std::size_t i = 0; i < ob.size(); ++i) {
                                 int dir = 0;
                                 if (order_key_less_label(x, y, keys[i], ob[i], dir)) return true;
                                 if (dir != 0) return false;  // G3: honor NULLS FIRST/LAST
                             }
                             return render_row(x) < render_row(y);  // total tie-break
                         });
        return std::nullopt;
    }

    // The grouped/aggregated path over JOINED rows. Mirrors exec_aggregate but resolves
    // qualified columns against the joined schema + honors NULL aggregate semantics
    // (COUNT(col)/SUM/MIN/MAX/AVG skip NULLs; COUNT(*) counts every row).
    ExecResult exec_aggregate_joined(const SelectStmt& sel, const JoinSchema& schema,
                                     std::vector<std::vector<Datum>>& acc) {
        // Resolve GROUP BY columns to flat indices.
        std::vector<std::size_t> gcols;
        for (const std::string& gc : sel.group_by) {
            std::string q;
            std::string c;
            split_qualified(gc, q, c);
            std::size_t idx = 0;
            if (auto e = schema.resolve(q, c, idx)) {
                return ExecResult::failure(*e);
            }
            gcols.push_back(idx);
        }
        // Validate SELECT-list plain columns are grouped + aggregate targets resolve.
        for (const SelectItem& item : sel.items) {
            if (item.kind != SelectItemKind::Column) {
                continue;
            }
            std::size_t idx = 0;
            if (auto e = schema.resolve(item.qualifier, item.column, idx)) {
                return ExecResult::failure(*e);
            }
            bool grouped = false;
            for (const std::size_t g : gcols) {
                if (g == idx) { grouped = true; break; }
            }
            if (!grouped) {
                return ExecResult::failure(
                    "column '" + item.label +
                    "' must appear in GROUP BY or be used in an aggregate");
            }
        }
        if (auto e = validate_aggs_joined(sel, schema)) {
            return ExecResult::failure(*e);
        }

        // Fold into groups keyed by the group tuple (ordered map => deterministic).
        struct JGroup {
            std::vector<const std::vector<Datum>*> rows;
            std::vector<Datum> key_datums;
        };
        std::map<std::vector<std::string>, JGroup> groups;
        const bool ungrouped = gcols.empty();
        for (const auto& jr : acc) {
            std::vector<std::string> key;
            key.reserve(gcols.size());
            for (const std::size_t g : gcols) {
                key.push_back(group_key_field(jr[g]));
            }
            JGroup& grp = groups[key];
            if (grp.rows.empty()) {
                for (const std::size_t g : gcols) {
                    grp.key_datums.push_back(jr[g]);
                }
            }
            grp.rows.push_back(&jr);
        }
        if (ungrouped && groups.empty()) {
            groups[std::vector<std::string>{}] = JGroup{};
        }

        ExecResult r;
        for (const auto& [key, grp] : groups) {
            bool having_ok = true;
            if (sel.having.present()) {
                if (auto e = eval_having_joined(sel.having, sel.having.root, schema,
                                                grp.rows, having_ok)) {
                    return ExecResult::failure(*e);
                }
            }
            if (!having_ok) {
                continue;
            }
            ResultRow out;
            for (const SelectItem& item : sel.items) {
                if (item.kind == SelectItemKind::Column) {
                    std::size_t idx = 0;
                    (void)schema.resolve(item.qualifier, item.column, idx);
                    Datum d;
                    for (std::size_t k = 0; k < gcols.size(); ++k) {
                        if (gcols[k] == idx) { d = grp.key_datums[k]; break; }
                    }
                    out.cells.emplace_back(item.label, d);
                } else {
                    Datum d;
                    if (auto e = compute_agg_joined(item.agg, schema, grp.rows, d)) {
                        return ExecResult::failure(*e);
                    }
                    out.cells.emplace_back(item.label, d);
                }
            }
            r.rows.push_back(std::move(out));
        }
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by_labels_joined(sel, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    [[nodiscard]] std::optional<std::string> validate_aggs_joined(
        const SelectStmt& sel, const JoinSchema& schema) {
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate) {
                if (auto e = validate_one_agg_joined(item.agg, schema)) return e;
            }
        }
        for (const PredNode& n : sel.having.nodes) {
            if (n.kind == PredNodeKind::Cmp && n.operand == OperandKind::Agg) {
                if (auto e = validate_one_agg_joined(n.agg, schema)) return e;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> validate_one_agg_joined(
        const AggExpr& a, const JoinSchema& schema) {
        if (a.kind == AggKind::CountStar) {
            return std::nullopt;
        }
        std::size_t idx = 0;
        if (auto e = schema.resolve(a.qualifier, a.column, idx)) {
            return e;
        }
        if ((a.kind == AggKind::Sum || a.kind == AggKind::Avg) &&
            schema.cols[idx].type != Type::Int && schema.cols[idx].logical < 5) {  // F9f: allow 128-bit
            return std::string("SUM/AVG requires a numeric column (got TEXT column '" +
                               a.column + "')");
        }
        if (a.kind >= AggKind::ArrayAgg)  // ARRAY/JSON/STRING_AGG + BOOL_*/BIT_*
            return std::string("collecting/bitwise aggregates are not supported in a join yet");
        return std::nullopt;
    }

    // Compute one aggregate over a joined group, SKIPPING NULLs (the LEFT-join fill).
    // COUNT(*) counts rows; COUNT(col)/SUM/MIN/MAX/AVG count/aggregate present values
    // only. An all-NULL (or empty) group yields COUNT=0/SUM=0 and MIN/MAX/AVG NULL.
    [[nodiscard]] std::optional<std::string> compute_agg_joined(
        const AggExpr& a, const JoinSchema& schema,
        const std::vector<const std::vector<Datum>*>& rows, Datum& out) {
        if (a.kind == AggKind::CountStar) {
            out = Datum::make_int(static_cast<std::int64_t>(rows.size()));
            return std::nullopt;
        }
        std::size_t idx = 0;
        if (auto e = schema.resolve(a.qualifier, a.column, idx)) {
            return e;
        }
        const Type ty = schema.cols[idx].type;
        // Collect the present (non-NULL) values.
        std::vector<const Datum*> present;
        for (const auto* rp : rows) {
            const Datum& d = (*rp)[idx];
            if (!d.is_null) {
                present.push_back(&d);
            }
        }
        if (a.kind == AggKind::Count) {
            out = Datum::make_int(static_cast<std::int64_t>(present.size()));
            return std::nullopt;
        }
        if (present.empty()) {
            // No present value: SUM=0; MIN/MAX/AVG = NULL (typed).
            if (a.kind == AggKind::Sum) {
                out = Datum::make_int(0);
            } else {
                out = Datum::make_null(ty);
            }
            return std::nullopt;
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            Datum best = *present.front();
            for (const Datum* d : present) {
                const int c = cmp_datum(*d, best);
                if ((a.kind == AggKind::Min && c < 0) ||
                    (a.kind == AggKind::Max && c > 0)) {
                    best = *d;
                }
            }
            out = best;
            return std::nullopt;
        }
        // SUM/AVG over a UINT256 column across a join — accumulate in u256 (checked).
        if (present.front()->logical == 13) {
            u256 acc{};
            for (const Datum* d : present) {
                u256 nx;
                if (u256_add(acc, u256_decode(d->s), nx)) return "UINT256 overflow in SUM";
                acc = nx;
            }
            if (a.kind == AggKind::Avg) {
                u256 q, rem;
                (void)u256_divmod(acc, u256_from_u64(present.size()), q, rem);
                acc = q;
            }
            out = Datum::make_text(u256_encode(acc));
            out.logical = 13;
            return std::nullopt;
        }
        // F9f: SUM/AVG over a 128-bit column (the present Datums carry the INT128/DECIMAL128 tag).
        if (present.front()->logical == 5 || present.front()->logical == 6) {
            __int128 acc = 0;
            for (const Datum* d : present) {
                if (__builtin_add_overflow(acc, Datum::decode_i128(d->s), &acc))
                    return "integer overflow in SUM";
            }
            if (a.kind == AggKind::Avg) acc /= static_cast<__int128>(present.size());
            out = Datum::make_text(Datum::encode_i128(acc));
            out.logical = present.front()->logical;
            out.scale = present.front()->scale;
            return std::nullopt;
        }
        std::int64_t sum = 0;
        for (const Datum* d : present) {
            if (add_ovf(sum, d->i, sum)) return "integer overflow in SUM";  // F9d
        }
        if (a.kind == AggKind::Sum) {
            out = Datum::make_int(sum);
            return std::nullopt;
        }
        const std::int64_t n = static_cast<std::int64_t>(present.size());
        out = Datum::make_int(n == 0 ? 0 : sum / n);  // AVG truncates toward zero
        return std::nullopt;
    }

    // HAVING over a joined group: like eval_pred_joined but an aggregate operand folds
    // over the group (compute_agg_joined). A column operand reads the group key.
    [[nodiscard]] std::optional<std::string> eval_having_joined(
        const Predicate& p, std::int32_t node, const JoinSchema& schema,
        const std::vector<const std::vector<Datum>*>& rows, bool& truth) {
        if (node < 0) {
            truth = true;
            return std::nullopt;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        switch (n.kind) {
            case PredNodeKind::And: {
                bool l = false;
                if (auto e = eval_having_joined(p, n.left, schema, rows, l)) return e;
                if (!l) { truth = false; return std::nullopt; }
                bool r = false;
                if (auto e = eval_having_joined(p, n.right, schema, rows, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Or: {
                bool l = false;
                if (auto e = eval_having_joined(p, n.left, schema, rows, l)) return e;
                if (l) { truth = true; return std::nullopt; }
                bool r = false;
                if (auto e = eval_having_joined(p, n.right, schema, rows, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Not: {
                bool c = false;
                if (auto e = eval_having_joined(p, n.left, schema, rows, c)) return e;
                truth = !c;
                return std::nullopt;
            }
            case PredNodeKind::IsNull:
            case PredNodeKind::InList:
            case PredNodeKind::Exists:
                // v4: IS NULL / subqueries are not supported inside HAVING (rejected at
                // parse time for subqueries). Fail-closed if one ever reaches here.
                return std::string(
                    "IS NULL / subqueries are not supported in HAVING");
            case PredNodeKind::Cmp:
                break;
        }
        Datum lhs;
        if (n.operand == OperandKind::Agg) {
            if (auto e = compute_agg_joined(n.agg, schema, rows, lhs)) {
                return e;
            }
        } else {
            std::size_t li = 0;
            if (auto e = schema.resolve(n.qualifier, n.column, li)) {
                return e;
            }
            lhs = rows.empty() ? Datum{} : (*rows.front())[li];
        }
        Datum rhs = n.literal;  // HAVING compares an aggregate/grouped col to a literal
        if (lhs.is_null || rhs.is_null) {
            truth = false;
            return std::nullopt;
        }
        if (lhs.type != rhs.type) {
            return std::string("type mismatch in HAVING: comparing ") +
                   type_name(lhs.type) + " to " + type_name(rhs.type);
        }
        truth = leaf_truth(n.op, lhs, rhs);
        return std::nullopt;
    }

    // Compute the COLUMN-NEED MASK for a single-table SELECT: which schema columns
    // the query actually references, so the scan can decode ONLY those (lazy decode).
    // A column is needed iff it appears in: the projection (SELECT list / star), the
    // WHERE predicate, GROUP BY, an aggregate target, or ORDER BY. Unknown column
    // names are simply ignored here (a later resolution step reports them as errors
    // fail-closed) — this mask is a pure DECODE hint and never changes results: any
    // column the pipeline later reads is included, and the PK is always decodable from
    // the key. Conservative by construction (over-include is safe; under-include of a
    // read column is impossible because every read site is enumerated below).
    // K1 perf: mark the columns a scalar expression references into `need`. Returns false
    // when the reference set cannot be bounded (CASE — WHEN predicates are not walked here —
    // or a column that does not resolve): the caller then decodes every column (fail-safe).
    [[nodiscard]] static bool expr_mark_cols(const Expr& e, const Table& t,
                                             std::vector<bool>& need) {
        if (e.kind == ExprKind::Case) return false;
        if (e.kind == ExprKind::Col) {
            const auto idx = t.column_index(e.column);
            if (!idx) return false;
            need[*idx] = true;
            return true;
        }
        if (e.left && !expr_mark_cols(*e.left, t, need)) return false;
        if (e.right && !expr_mark_cols(*e.right, t, need)) return false;
        for (const auto& a2 : e.args)
            if (a2 && !expr_mark_cols(*a2, t, need)) return false;
        return true;
    }

    [[nodiscard]] static std::vector<bool> needed_columns(const Table& t,
                                                          const SelectStmt& sel) {
        std::vector<bool> need(t.columns.size(), false);
        const auto mark = [&](const std::string& col) {
            if (const auto idx = t.column_index(col)) {
                need[*idx] = true;
            }
        };
        // SELECT * needs every column; bail out to all-true.
        if (sel.star) {
            return std::vector<bool>(t.columns.size(), true);
        }
        // Projection / ORDER BY: the SELECT-list columns + aggregate targets.
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Column) {
                mark(item.column);
            } else if (item.kind == SelectItemKind::Expr ||
                       item.kind == SelectItemKind::Window) {
                // A1/C3: an expression or window function may touch any column — decode them all.
                return std::vector<bool>(t.columns.size(), true);
            } else if (item.agg.kind != AggKind::CountStar) {
                mark(item.agg.column);  // COUNT(*) needs no column
            }
        }
        for (const std::string& gc : sel.group_by) {
            mark(gc);
        }
        for (const OrderKey& k : sel.order_by) {
            // K1.2/K1 perf: an ORDER BY expression marks exactly the columns it references
            // (a k-NN over a wide table must not decode every column per row). A shape the
            // walker cannot bound (CASE — its WHEN predicates are not walked, or an unknown
            // column) falls back to decode-all, the previous behavior.
            if (k.expr) {
                if (!expr_mark_cols(*k.expr, t, need)) {
                    return std::vector<bool>(t.columns.size(), true);
                }
                continue;
            }
            mark(k.column);
        }
        // WHERE + HAVING predicate leaves (column operands + column rhs).
        const auto mark_pred = [&](const Predicate& p) {
            for (const PredNode& n : p.nodes) {
                // v4: IsNull / InList reference a column too (not just Cmp leaves). A NULL
                // test / IN-probe MUST decode its column, so include those node kinds.
                if (n.kind == PredNodeKind::IsNull ||
                    n.kind == PredNodeKind::InList) {
                    mark(n.column);
                    continue;
                }
                if (n.kind != PredNodeKind::Cmp) {
                    continue;
                }
                if (n.operand == OperandKind::Column) {
                    mark(n.column);
                } else if (n.agg.kind != AggKind::CountStar) {
                    mark(n.agg.column);
                }
                if (n.rhs_is_column) {
                    mark(n.rhs_column);
                }
            }
        };
        // J1: an expression LHS (`a + b = 10`, `doc->>'k' = 'v'`) may touch any column. We do not
        // walk the Expr tree here — decode every column when one is present (correct, rarely hot).
        const auto has_expr_operand = [](const Predicate& p) {
            for (const PredNode& n : p.nodes) {
                if ((n.kind == PredNodeKind::Cmp || n.kind == PredNodeKind::IsNull ||
                     n.kind == PredNodeKind::InList) &&
                    n.operand == OperandKind::Expr) {
                    return true;
                }
            }
            return false;
        };
        if (has_expr_operand(sel.filter) || has_expr_operand(sel.having)) {
            return std::vector<bool>(t.columns.size(), true);
        }
        mark_pred(sel.filter);
        mark_pred(sel.having);
        // A per-aggregate FILTER (WHERE ...) predicate references columns too — decode them.
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate && item.agg.filter && item.agg.filter->present()) {
                if (has_expr_operand(*item.agg.filter)) return std::vector<bool>(t.columns.size(), true);
                mark_pred(*item.agg.filter);
            }
        }
        // The PK is needed by ORDER BY's PK tie-break (apply_order_by) AND is cheap to
        // decode from the key; always include it so the deterministic tie-break holds.
        need[t.pk_index] = true;
        return need;
    }

    // Dispatch the SELECT to a Query<L> of the AST level (the D5 dispatch point).
    // `pk_fast` selects the point/range fast path over the full scan. Returns an
    // error string on a bad level parameter, else fills `kvs`.
    [[nodiscard]] std::optional<std::string> run_select_at_level(
        const Table& t, const SelectStmt& sel, bool pk_fast,
        std::vector<storage::KeyValue>& kvs) {
        switch (sel.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                build_read_steps(t, sel, pk_fast, q);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::Snapshot: {
                Query<Snapshot> q = snapshot_query(sel.snapshot_version);
                build_read_steps(t, sel, pk_fast, q);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q = bounded_query(sel.max_lag);
                build_read_steps(t, sel, pk_fast, q);
                // replica_lag modeled as 0 here (no replica lag injected by SQL);
                // the contract stays exact and call-site-visible.
                collect(db_.run(q, /*replica_lag=*/0), kvs);
                return std::nullopt;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q = ryw_query(sel.session);
                build_read_steps(t, sel, pk_fast, q);
                collect(db_.run(q, /*replica_lag=*/0, /*session_last_write=*/tip_),
                        kvs);
                return std::nullopt;
            }
        }
        return std::string("unsupported consistency level");
    }

    // Compose the read steps onto the typed query builder. PLANNER CHOICE:
    //   pk_fast + Eq      -> a POINT get of the encoded key (a point read).
    //   pk_fast + Between -> a half-open scan [encode(lo), encode(hi)++) so the
    //                        inclusive upper bound is covered (range over the
    //                        order-preserving PK).
    //   otherwise         -> a FULL SCAN over the table's contiguous key namespace
    //                        (the general predicate is then applied as a row filter).
    // The fast path is purely an optimization: the row filter in (2) would give the
    // SAME result over a full scan, but the point/range scan avoids reading the whole
    // table when the WHERE is exactly a PK eq / PK BETWEEN.
    template <typename L>
    static void build_read_steps(const Table& t, const SelectStmt& sel, bool pk_fast,
                                 Query<L>& q) {
        if (pk_fast && sel.where == SelectWhereKind::Eq) {
            q.get(encode_key(t, sel.eq_value));
        } else if (pk_fast && sel.where == SelectWhereKind::Between) {
            const Key lo = encode_key(t, sel.lo_value);
            Key hi = encode_key(t, sel.hi_value);
            hi.push_back('\0');  // make BETWEEN's inclusive upper bound half-open
            q.scan(lo, hi);
        } else {
            q.scan(table_prefix(t.id), table_prefix_end(t.id));
        }
    }

    // A chosen secondary-index access plan: WHICH index + the COL value range to scan.
    // `eq` is the equality value when `is_eq`; otherwise [lo, hi] is the inclusive
    // BETWEEN range. The residual predicate (the whole filter) is applied afterwards.
    struct IndexPlan {
        const Index* index = nullptr;
        bool is_eq = false;
        Datum eq;   // is_eq (single leading-column equality; legacy single-col path)
        Datum lo;   // BETWEEN lower (inclusive)
        Datum hi;   // BETWEEN upper (inclusive)
        // I1: a COMPOSITE eq prefix — equalities matched on a leading prefix of the index columns
        // (size >= 1). When non-empty, the scan ranges over this whole prefix (the residual WHERE
        // still runs over the fetched rows, so an unmatched suffix is filtered there).
        std::vector<Datum> eq_prefix;
    };

    // THE SHARED ACCESS-PATH CHOICE. Both the executor (exec_select) AND the plan builder
    // (build_plan_single, for EXPLAIN) call this single function, so the plan EXPLAIN shows
    // is EXACTLY the plan that runs. Phase 2 makes this COST-BASED (statistics-driven); today
    // it is the rule-based choice extracted verbatim from the old inline logic (byte-identical).
    struct AccessPath {
        enum class Kind : std::uint8_t { PkPoint, PkRange, Index, IndexMerge, Seq } kind = Kind::Seq;
        IndexPlan index;   // valid iff kind == Index or IndexMerge (first index)
        IndexPlan index2;  // I7: the SECOND index of an IndexMerge (PK-set intersection)
        [[nodiscard]] bool pk_fast() const {
            return kind == Kind::PkPoint || kind == Kind::PkRange;
        }
    };

    // COST MODEL (PERF_PLAN Phase 2). Deterministic integer estimates from row_count + simple
    // selectivity heuristics (no histogram yet): an eq term is assumed SELECTIVE (~rowcount/100,
    // ≥1), a range ~30%. Cost units are "row touches": a seq scan touches every row (N); an
    // index scan touches log2(N) + matches·kFetch (a point-get per match). The planner picks
    // the cheaper of {index, seq} for a non-PK filter — so a NON-selective index (matches ≈ N)
    // correctly loses to a seq scan, the classic cost-based win the old "always index" rule
    // missed. PK access is always cheapest (point=1, range=estimate). Same rows either way
    // (an access path is transparent — sql_index proves idx==scan), so this is conformance-safe.
    static std::size_t ilog2(std::size_t n) {
        std::size_t b = 0;
        while (n > 1) { n >>= 1U; ++b; }
        return b;
    }
    std::size_t est_eq_matches(std::size_t n) const { return n / 100 + 1; }
    // I6: eq selectivity from ANALYZE's n_distinct (matches ~= n / n_distinct); fall back to the
    // fixed 1% guess when stats are absent.
    std::size_t est_eq_matches_col(const Table& t, std::size_t col, std::size_t n) const {
        if (col < t.col_stats.size() && t.col_stats[col].n_distinct > 0)
            return std::max<std::size_t>(1, n / static_cast<std::size_t>(t.col_stats[col].n_distinct));
        return est_eq_matches(n);
    }
    std::size_t est_range_matches(std::size_t n) const { return (n * 3) / 10 + 1; }

    // Range selectivity from per-column min/max (Phase 2 stats) when available: the fraction
    // of the observed [min,max] the query's [lo,hi] covers, times row_count. Falls back to the
    // 30% heuristic when there are no INT stats. This is what fixes the visibly-wrong range
    // estimate (e.g. BETWEEN 0 AND 1900 over [0,2000) is ~95%, not 30%).
    std::size_t est_range_rows(const Table& t, std::size_t col, const Datum& lo_d,
                               const Datum& hi_d) const {
        if (col < t.col_stats.size() && t.col_stats[col].seen && lo_d.type == Type::Int &&
            hi_d.type == Type::Int) {
            const Table::ColStat& cs = t.col_stats[col];
            const std::int64_t span = cs.hi - cs.lo;
            if (span <= 0) {
                return t.row_count == 0 ? 1 : t.row_count;  // single observed value
            }
            const std::int64_t lo = lo_d.i > cs.lo ? lo_d.i : cs.lo;
            const std::int64_t hi = hi_d.i < cs.hi ? hi_d.i : cs.hi;
            if (hi < lo) {
                return 1;
            }
            const double frac = static_cast<double>(hi - lo + 1) /
                                static_cast<double>(span + 1);
            const auto r = static_cast<std::size_t>(frac * static_cast<double>(t.row_count));
            return r < 1 ? 1 : r;
        }
        return est_range_matches(t.row_count);
    }

    AccessPath choose_access_path(const SelectStmt& sel, const Table& t) {
        AccessPath ap;
        // F1: the single-column PK point/range fast path is invalid for a COMPOSITE PK (the key
        // needs ALL pk columns) — fall through to a full scan + filter.
        const bool pk_fast = !t.composite_pk() && sel.where != SelectWhereKind::None &&
                             sel.where_column == t.pk().name && predicate_is_pure_pk(sel, t);
        if (pk_fast && sel.where == SelectWhereKind::Eq) {
            ap.kind = AccessPath::Kind::PkPoint;
        } else if (pk_fast && sel.where == SelectWhereKind::Between) {
            ap.kind = AccessPath::Kind::PkRange;
        } else if (!pk_fast && sel.filter.present()) {
            if (auto plan = choose_index_access(sel, t)) {
                const std::size_t n = t.row_count;
                // J3: a GIN containment lookup is per-ELEMENT; the array column's whole-array n_distinct
                // stat is meaningless for it, so use the generic eq selectivity (a containment scan is a
                // selective access — and is always transparent: the residual `= ANY` re-checks each row).
                std::size_t matches =
                    plan->index->gin ? est_eq_matches(n)
                    : plan->is_eq    ? est_eq_matches_col(t, plan->index->column, n)
                                     : est_range_rows(t, plan->index->column, plan->lo, plan->hi);
                // I1: each extra equality in a composite prefix multiplies selectivity (fewer rows).
                if (plan->is_eq && plan->eq_prefix.size() > 1)
                    matches = std::max<std::size_t>(1, matches >> (plan->eq_prefix.size() - 1));
                constexpr std::size_t kFetch = 3;  // point-get cost per matched row
                const std::size_t idx_cost = ilog2(n) + matches * kFetch;
                const std::size_t seq_cost = n;
                // Use the index only when it is cheaper than a full scan (or the table is
                // tiny / stats absent — preserve the historical index preference there).
                if (n <= 1 || idx_cost < seq_cost) {
                    ap.kind = AccessPath::Kind::Index;
                    ap.index = *plan;
                }
            }
            // I7: INDEX MERGE — two single-column eq indexes on different columns intersected; cheaper
            // than either alone when both are selective. Only when it beats the chosen single path.
            if (auto m = choose_index_merge(sel, t)) {
                const std::size_t n = t.row_count;
                const std::size_t m1 = est_eq_matches_col(t, m->first.index->column, n);
                const std::size_t m2 = est_eq_matches_col(t, m->second.index->column, n);
                const std::size_t isect = std::max<std::size_t>(1, (m1 * m2) / std::max<std::size_t>(1, n));
                const std::size_t merge_cost = 2 * ilog2(n) + (m1 + m2) +
                                               isect * 3;  // scan both indexes + fetch the intersection
                const std::size_t cur_cost = ap.kind == AccessPath::Kind::Index
                                                 ? ilog2(n) + est_path_rows(ap, t) * 3
                                                 : n;  // seq otherwise
                if (n > 1 && merge_cost < cur_cost) {
                    ap.kind = AccessPath::Kind::IndexMerge;
                    ap.index = m->first;
                    ap.index2 = m->second;
                }
            }
        }
        return ap;
    }

    // I7: find two `col = literal` AND-conjuncts on DIFFERENT columns, each with its own single-column
    // index, for an index-merge intersection. Deterministic (first two by leaf order).
    [[nodiscard]] std::optional<std::pair<IndexPlan, IndexPlan>> choose_index_merge(
        const SelectStmt& sel, const Table& t) {
        std::vector<std::int32_t> leaves;
        gather_and_leaves(sel.filter, sel.filter.root, leaves);
        std::vector<IndexPlan> eqs;
        for (const std::int32_t li : leaves) {
            const PredNode& n = sel.filter.nodes[static_cast<std::size_t>(li)];
            if (n.kind != PredNodeKind::Cmp || n.operand != OperandKind::Column || !n.qualifier.empty() ||
                n.rhs_is_column || n.rhs_is_subquery || n.literal.is_null || n.op != CmpOp::Eq)
                continue;
            const auto col = t.column_index(n.column);
            if (!col || *col == t.pk_index || n.literal.type != t.columns[*col].type) continue;
            const Index* ix = t.index_for_column(*col);
            // a SINGLE-column index on the leading column (a composite is the I1 prefix path instead).
            if (ix == nullptr || ix->columns.size() != 1 || ix->columns[0] != *col || ix->gin ||
                ix->ivfflat || ix->hnsw || ix->bm25 || !query_implies_partial(sel, t, *ix))
                continue;
            bool dup = false;
            for (const IndexPlan& p : eqs) if (p.index->column == *col) { dup = true; break; }
            if (dup) continue;
            IndexPlan p;
            p.index = ix;
            p.is_eq = true;
            p.eq = n.literal;
            p.eq_prefix.push_back(n.literal);
            eqs.push_back(p);
            if (eqs.size() == 2) {
                const std::size_t n = t.row_count;  // fetch from the MORE selective index first
                if (est_eq_matches_col(t, eqs[1].index->column, n) <
                    est_eq_matches_col(t, eqs[0].index->column, n))
                    std::swap(eqs[0], eqs[1]);
                return std::make_pair(eqs[0], eqs[1]);
            }
        }
        return std::nullopt;
    }

    // Estimated rows OUT of a single-table access path (for EXPLAIN's estimated-vs-actual).
    std::size_t est_path_rows(const AccessPath& ap, const Table& t) const {
        switch (ap.kind) {
            case AccessPath::Kind::PkPoint:
                return 1;
            case AccessPath::Kind::PkRange:
                return est_range_matches(t.row_count);
            case AccessPath::Kind::Index: {
                if (ap.index.index->gin)  // J3: per-element containment — generic eq selectivity
                    return est_eq_matches(t.row_count);
                if (!ap.index.is_eq)
                    return est_range_rows(t, ap.index.index->column, ap.index.lo, ap.index.hi);
                std::size_t m = est_eq_matches_col(t, ap.index.index->column, t.row_count);  // I6
                if (ap.index.eq_prefix.size() > 1)  // I1: composite prefix selectivity
                    m = std::max<std::size_t>(1, m >> (ap.index.eq_prefix.size() - 1));
                return m;
            }
            case AccessPath::Kind::IndexMerge: {  // I7: intersection ~ the smaller of the two
                const std::size_t m1 = est_eq_matches_col(t, ap.index.index->column, t.row_count);
                const std::size_t m2 = est_eq_matches_col(t, ap.index2.index->column, t.row_count);
                return std::max<std::size_t>(1, std::min(m1, m2));
            }
            case AccessPath::Kind::Seq:
                return t.row_count;
        }
        return t.row_count;
    }

    // Scan the WHERE predicate for an `indexed_col = v` or `indexed_col BETWEEN a AND b`
    // term usable as an index access path. We look at the predicate ROOT: a single Cmp
    // Eq leaf, OR a top-level AND whose conjuncts include such a term (possibly nested
    // left-deep — the parser builds AND left-associative). The term's column must (a)
    // have a secondary index, (b) compare a bare column to a LITERAL of the column's
    // TYPE. A BETWEEN lowers to `col >= lo AND col <= hi` (two Cmp leaves under an AND),
    // which we also recognize. Returns the first usable plan (deterministic left-to-
    // right scan) or nullopt. The PK fast path is checked FIRST by the caller, so a PK
    // term never reaches here (and the PK has no secondary index anyway).
    // I5: a PARTIAL index is sound for a query ONLY if the query's WHERE IMPLIES the index predicate
    // (so the index contains every row the query can match). Conservative + sound: every AND-conjunct
    // leaf of the index predicate must appear as an identical AND-conjunct of the query's WHERE
    // (same column, op, literal). An empty predicate (a full index) is always usable.
    [[nodiscard]] bool query_implies_partial(const SelectStmt& sel, const Table& t, const Index& ix) {
        if (ix.partial_src.empty()) return true;
        if (!sel.filter.present()) return false;
        const ParseResult pr = parse_sql("SELECT * FROM " + t.name + " WHERE " + ix.partial_src);
        if (!pr.ok()) return false;
        const Predicate& pp = pr.stmt().select.filter;
        std::vector<std::int32_t> pleaves, qleaves;
        gather_and_leaves(pp, pp.root, pleaves);
        gather_and_leaves(sel.filter, sel.filter.root, qleaves);
        auto same = [](const PredNode& a, const PredNode& b) {
            return a.kind == PredNodeKind::Cmp && b.kind == PredNodeKind::Cmp &&
                   a.operand == OperandKind::Column && b.operand == OperandKind::Column &&
                   !a.rhs_is_column && !b.rhs_is_column && !a.rhs_is_subquery && !b.rhs_is_subquery &&
                   a.column == b.column && a.op == b.op && a.qualifier.empty() && b.qualifier.empty() &&
                   a.literal == b.literal;
        };
        for (const std::int32_t pl : pleaves) {
            const PredNode& pn = pp.nodes[static_cast<std::size_t>(pl)];
            bool found = false;
            for (const std::int32_t ql : qleaves)
                if (same(pn, sel.filter.nodes[static_cast<std::size_t>(ql)])) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }

    // J2: structural equality of two scalar expression trees — used to match a query's `WHERE <expr>
    // = literal` against an EXPRESSION index's stored expression. Compares kind + the per-kind payload
    // recursively (column name/qualifier, literal value, operator, function name, CAST target, args,
    // CASE arms). Whitespace / parenthesisation differences in the original source are irrelevant
    // because both sides are compared as parsed trees.
    static bool expr_equal(const Expr* a, const Expr* b) {
        if (a == nullptr || b == nullptr) return a == b;
        if (a->kind != b->kind) return false;
        switch (a->kind) {
            case ExprKind::Lit:
                return a->lit == b->lit && a->lit.is_null == b->lit.is_null;
            case ExprKind::Col:
                return a->column == b->column && a->qualifier == b->qualifier;
            case ExprKind::Neg:
                return expr_equal(a->left.get(), b->left.get());
            case ExprKind::Bin:
                return a->op == b->op && expr_equal(a->left.get(), b->left.get()) &&
                       expr_equal(a->right.get(), b->right.get());
            case ExprKind::Func:
                if (a->func != b->func || a->cast_type != b->cast_type ||
                    a->args.size() != b->args.size())
                    return false;
                for (std::size_t i = 0; i < a->args.size(); ++i)
                    if (!expr_equal(a->args[i].get(), b->args[i].get())) return false;
                return true;
            case ExprKind::Array:
                if (a->args.size() != b->args.size()) return false;
                for (std::size_t i = 0; i < a->args.size(); ++i)
                    if (!expr_equal(a->args[i].get(), b->args[i].get())) return false;
                return true;
            case ExprKind::Subscript:
                return expr_equal(a->left.get(), b->left.get()) &&
                       expr_equal(a->right.get(), b->right.get());
            case ExprKind::Case: {
                if (a->case_when.size() != b->case_when.size() ||
                    a->case_then.size() != b->case_then.size() ||
                    static_cast<bool>(a->case_else) != static_cast<bool>(b->case_else))
                    return false;
                for (std::size_t i = 0; i < a->case_then.size(); ++i)
                    if (!expr_equal(a->case_then[i].get(), b->case_then[i].get())) return false;
                return !a->case_else || expr_equal(a->case_else.get(), b->case_else.get());
            }
        }
        return false;
    }

    // J3: is `e` a CONSTANT expression (no column reference)? A constant LHS of `<const> = ANY(arr)`
    // is a single value the GIN index can look up; a column LHS is row-dependent and cannot.
    static bool expr_is_constant(const Expr* e) {
        if (e == nullptr) return true;
        if (e->kind == ExprKind::Col) return false;
        if (e->kind == ExprKind::Case) return false;  // its WHEN predicates may touch columns — conservative
        if (e->left && !expr_is_constant(e->left.get())) return false;
        if (e->right && !expr_is_constant(e->right.get())) return false;
        for (const auto& a : e->args) if (!expr_is_constant(a.get())) return false;
        for (const auto& w : e->case_then) if (!expr_is_constant(w.get())) return false;
        if (e->case_else && !expr_is_constant(e->case_else.get())) return false;
        return true;
    }

    // J2: find an EXPRESSION index whose stored expression structurally equals `qe` (the query's LHS
    // expression). The index is usable for `qe = literal` only when the index is not partial-or-the
    // -query-implies-it. Returns the index (or null).
    [[nodiscard]] const Index* match_expr_index(const SelectStmt& sel, const Table& t,
                                                const Expr* qe) {
        if (qe == nullptr) return nullptr;
        for (const Index& ix : t.indexes) {
            if (ix.expr_src.empty()) continue;
            if (!query_implies_partial(sel, t, ix)) continue;
            std::shared_ptr<Expr> ie;
            if (parse_index_expr(t, ix.expr_src, ie)) continue;  // defensive (validated at create)
            if (expr_equal(qe, ie.get())) return &ix;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<IndexPlan> choose_index_access(const SelectStmt& sel,
                                                              const Table& t) {
        const Predicate& p = sel.filter;
        // Collect the top-level AND-conjunct leaf nodes (flatten left-deep ANDs).
        std::vector<std::int32_t> leaves;
        gather_and_leaves(p, p.root, leaves);
        // First pass: an indexed equality (most selective).
        for (const std::int32_t li : leaves) {
            const PredNode& n = p.nodes[static_cast<std::size_t>(li)];
            if (n.kind != PredNodeKind::Cmp || n.operand != OperandKind::Column ||
                !n.qualifier.empty() || n.rhs_is_column || n.rhs_is_subquery ||
                n.literal.is_null || n.op != CmpOp::Eq) {
                continue;  // v4: a NULL/subquery RHS is never an index point lookup
            }
            const auto col = t.column_index(n.column);
            if (!col || *col == t.pk_index || n.literal.type != t.columns[*col].type) {
                continue;
            }
            // I5: pick the first USABLE index whose LEADING column is *col — a partial index that the
            // query doesn't imply is skipped (a full index on the same column can still serve).
            const Index* ix = nullptr;
            for (const Index& cand : t.indexes)
                if (!cand.columns.empty() && cand.columns[0] == *col && !cand.gin &&
                    !cand.ivfflat && !cand.hnsw && !cand.bm25 &&
                    query_implies_partial(sel, t, cand)) { ix = &cand; break; }  // special kinds excluded
            if (ix == nullptr) {
                continue;
            }
            IndexPlan plan;
            plan.index = ix;
            plan.is_eq = true;
            plan.eq = n.literal;
            // I1: extend the equality across a leading PREFIX of a composite index — for each next
            // index column, look for a matching `col = literal` conjunct; stop at the first miss.
            plan.eq_prefix.push_back(n.literal);
            for (std::size_t k = 1; k < ix->columns.size(); ++k) {
                const std::string& cname = t.columns[ix->columns[k]].name;
                bool matched = false;
                for (const std::int32_t lj : leaves) {
                    const PredNode& m = p.nodes[static_cast<std::size_t>(lj)];
                    if (m.kind == PredNodeKind::Cmp && m.operand == OperandKind::Column &&
                        m.qualifier.empty() && !m.rhs_is_column && !m.rhs_is_subquery &&
                        !m.literal.is_null && m.op == CmpOp::Eq && m.column == cname &&
                        m.literal.type == t.columns[ix->columns[k]].type) {
                        plan.eq_prefix.push_back(m.literal);
                        matched = true;
                        break;
                    }
                }
                if (!matched) break;
            }
            return plan;
        }
        // J2: an EXPRESSION-index equality — `WHERE <expr> = literal` where <expr> matches an index's
        // stored expression. The lookup value is the literal (it must be the index's physical type;
        // the entry was built by encoding the SAME-typed evaluated value). The residual WHERE still
        // re-checks `<expr> = literal` over the fetched rows, so the result is byte-identical to a scan.
        for (const std::int32_t li : leaves) {
            const PredNode& n = p.nodes[static_cast<std::size_t>(li)];
            if (n.kind != PredNodeKind::Cmp || n.operand != OperandKind::Expr ||
                n.rhs_is_column || n.rhs_is_subquery || n.literal.is_null ||
                n.op != CmpOp::Eq || n.any_quant || n.all_quant) {
                continue;
            }
            const Index* ix = match_expr_index(sel, t, n.expr.get());
            if (ix == nullptr || n.literal.type != ix->expr_type) {
                continue;
            }
            IndexPlan plan;
            plan.index = ix;
            plan.is_eq = true;
            plan.eq = n.literal;
            plan.eq_prefix.push_back(n.literal);
            return plan;
        }
        // J3: an array CONTAINMENT lookup — `<const> = ANY(arr_col)` over a GIN-indexed array column.
        // The GIN index has one entry per element, so looking up the constant value yields exactly the
        // rows whose array holds it. The residual WHERE still re-checks `= ANY` per fetched row, so the
        // result is byte-identical to a scan.
        for (const std::int32_t li : leaves) {
            const PredNode& n = p.nodes[static_cast<std::size_t>(li)];
            if (n.kind != PredNodeKind::Cmp || n.op != CmpOp::Eq || !n.any_quant || n.all_quant ||
                n.operand != OperandKind::Expr || !n.rhs_is_column || !expr_is_constant(n.expr.get())) {
                continue;
            }
            const auto acol = t.column_index(n.rhs_column);
            if (!acol || t.columns[*acol].logical != 7) continue;  // the ANY operand must be an ARRAY column
            const Index* ix = nullptr;
            for (const Index& cand : t.indexes)
                if (cand.gin && !cand.columns.empty() && cand.columns[0] == *acol &&
                    query_implies_partial(sel, t, cand)) { ix = &cand; break; }
            if (ix == nullptr) continue;
            // Evaluate the constant LHS to the element value to look up (no row needed — it is constant).
            Datum v;
            const std::vector<Datum> dummy(t.columns.size());
            if (n.expr == nullptr || eval_expr(*n.expr, t, dummy, v)) continue;  // un-evaluable => scan
            if (v.is_null || v.type != t.columns[*acol].elem_type) continue;
            IndexPlan plan;
            plan.index = ix;
            plan.is_eq = true;
            plan.eq = v;
            plan.eq_prefix.push_back(v);
            return plan;
        }
        // Second pass: an indexed BETWEEN (col >= lo AND col <= hi over ONE column).
        for (std::size_t a = 0; a < leaves.size(); ++a) {
            const PredNode& na = p.nodes[static_cast<std::size_t>(leaves[a])];
            if (na.kind != PredNodeKind::Cmp || na.operand != OperandKind::Column ||
                !na.qualifier.empty() || na.rhs_is_column || na.rhs_is_subquery ||
                na.literal.is_null || na.op != CmpOp::Ge) {
                continue;
            }
            const auto col = t.column_index(na.column);
            if (!col || *col == t.pk_index || na.literal.type != t.columns[*col].type) {
                continue;
            }
            const Index* ix = t.index_for_column(*col);
            if (ix == nullptr || ix->hash || ix->gin || ix->ivfflat || ix->hnsw || ix->bm25 ||
                !query_implies_partial(sel, t, *ix)) {  // hash/GIN/vector/BM25; I5 partial
                continue;
            }
            // Find a matching `col <= hi` conjunct on the SAME column.
            for (std::size_t b = 0; b < leaves.size(); ++b) {
                if (b == a) {
                    continue;
                }
                const PredNode& nb = p.nodes[static_cast<std::size_t>(leaves[b])];
                if (nb.kind == PredNodeKind::Cmp &&
                    nb.operand == OperandKind::Column && nb.qualifier.empty() &&
                    !nb.rhs_is_column && nb.op == CmpOp::Le && nb.column == na.column &&
                    nb.literal.type == t.columns[*col].type) {
                    IndexPlan plan;
                    plan.index = ix;
                    plan.is_eq = false;
                    plan.lo = na.literal;
                    plan.hi = nb.literal;
                    return plan;
                }
            }
        }
        return std::nullopt;
    }

    // Flatten a left-deep AND tree into its conjunct LEAF node indices. A non-AND root
    // is itself the single "leaf". (OR/NOT subtrees are NOT flattened — an indexed term
    // under an OR is not safely usable as the sole access path, so we treat the whole
    // OR/NOT node as an opaque leaf the index pass ignores.)
    static void gather_and_leaves(const Predicate& p, std::int32_t node,
                                  std::vector<std::int32_t>& out) {
        if (node < 0) {
            return;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        if (n.kind == PredNodeKind::And) {
            gather_and_leaves(p, n.left, out);
            gather_and_leaves(p, n.right, out);
        } else {
            out.push_back(node);
        }
    }

    // Execute a chosen index access: range-scan the index prefix for the matching col
    // range to collect the (col, PK) entries, then POINT-GET each row + decode. The
    // residual predicate (the whole WHERE) is applied by the caller in step (2), so
    // this only narrows WHICH rows are fetched. Reads run at the SELECT's D5 level.
    // I2: can the chosen index's scan order SATISFY the query's ORDER BY (so the sort is skipped)?
    // True when: a plain projection (no agg/group/distinct); an eq-prefix index plan; every covered
    // column is NOT NULL (so the index covers EVERY matching row — a NULL would be missing); and the
    // ORDER BY keys are exactly the index columns AFTER the eq-prefix, contiguous, ASC, by name. The
    // index entry is (cols asc, pk asc), which matches ORDER BY <cols> with the pipeline's PK
    // tie-break — byte-identical to the sorted full-scan result (the cross-check proves it).
    // Returns 0 (no), 1 (FORWARD — index scan order already satisfies ORDER BY), or 2 (REVERSE — a
    // descending scan satisfies it; only allowed when the result has NO TIES so the reversed
    // PK-tie-break is harmless: the index is UNIQUE and the eq-prefix + ORDER BY span the WHOLE index,
    // making every scanned tuple unique).
    [[nodiscard]] static int order_by_matches_index(const SelectStmt& sel, const IndexPlan& plan,
                                                    const Table& t) {
        if (sel.has_aggregates || !sel.group_by.empty() || sel.distinct || sel.order_by.empty())
            return 0;
        if (!plan.is_eq || plan.index == nullptr) return 0;
        const Index& ix = *plan.index;
        for (const std::size_t c : ix.columns) if (t.columns[c].nullable) return 0;
        const std::size_t base = plan.eq_prefix.empty() ? 1 : plan.eq_prefix.size();
        if (base + sel.order_by.size() > ix.columns.size()) return 0;
        bool all_asc = true, all_desc = true;
        for (std::size_t i = 0; i < sel.order_by.size(); ++i) {
            const OrderKey& k = sel.order_by[i];
            if (k.expr) return 0;  // K1.2: an expression key never matches an index order
            if (k.position != 0 || !k.qualifier.empty() || k.nulls != NullsOrder::Default) return 0;
            if (k.column != t.columns[ix.columns[base + i]].name) return 0;
            if (k.descending) all_asc = false; else all_desc = false;
        }
        if (all_asc) return 1;
        // I7: a uniform DESC needs a UNIQUE index fully covered by eq-prefix + ORDER BY (no ties).
        if (all_desc && ix.unique && base + sel.order_by.size() == ix.columns.size()) return 2;
        return 0;
    }

    // I7: collect the PK SET of an eq index plan's entries (no row fetch) — for an index-merge.
    [[nodiscard]] std::optional<std::string> index_pk_set(const Table& t, const SelectStmt& sel,
                                                          const IndexPlan& plan,
                                                          std::set<std::string>& out) {
        Key lo = index_prefix(t.id, plan.index->id);
        if (plan.eq_prefix.empty()) put_index_col(lo, plan.eq);
        else for (const Datum& d : plan.eq_prefix) put_index_col(lo, d);
        const Key hi = key_successor(lo);
        std::vector<storage::KeyValue> kvs;
        if (auto e = run_index_scan_at_level(sel, lo, hi, kvs)) return e;
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) continue;
            out.insert(encode_pk(decode_index_entry_pk(t, plan.index->id, kv.first)));
        }
        return std::nullopt;
    }
    // I7: INDEX MERGE — fetch rows of the more-selective index, keep those whose PK is also in the
    // second index's PK set (the AND intersection). The residual WHERE still runs, so correctness ==
    // a full scan (the indexed==full-scan cross-check proves it).
    [[nodiscard]] std::optional<std::string> read_via_index_merge(
        const Table& t, const SelectStmt& sel, const IndexPlan& p1, const IndexPlan& p2,
        const std::vector<bool>& need, std::vector<std::vector<Datum>>& rows_out) {
        // Intersect the two PK sets FIRST (index entries only, no row fetch), then fetch ONLY the
        // intersection — so the merge touches far fewer rows than either index alone.
        std::set<std::string> set1, set2;
        if (auto e = index_pk_set(t, sel, p1, set1)) return e;
        if (auto e = index_pk_set(t, sel, p2, set2)) return e;
        std::vector<std::pair<Datum, std::vector<Datum>>> fetched;
        for (const std::string& pkenc : set1) {
            if (set2.count(pkenc) == 0) continue;
            const Datum pk = decode_pk(t, table_prefix(t.id) + pkenc);
            if (t.columnar) {
                if (auto row = read_columnar_row(t, pk)) fetched.emplace_back(pk, std::move(*row));
                continue;
            }
            const Key rkey = encode_key(t, pk);
            const ReadResult rv = point_get_at_level(sel, rkey);
            if (!rv.has_value() || is_tombstone(*rv)) continue;
            fetched.emplace_back(pk, decode_row_projected(t, rkey, *rv, need));
        }
        // Emit PK-ascending for byte-identity with the full-scan path (a std::set of encode_pk keys
        // is already PK-ascending, so this preserves it).
        for (auto& [pk, row] : fetched) { (void)pk; rows_out.push_back(std::move(row)); }
        return std::nullopt;
    }

    // K1.3: IVFFLAT approximate k-NN. Matches the pgvector idiom
    //   SELECT ... FROM t ORDER BY vec_col <-> <const vector> LIMIT k [OFFSET m]
    // (no WHERE / GROUP BY / DISTINCT / aggregates / window / UNNEST) over an ivfflat-indexed
    // VECTOR column: probe the `probes` nearest centroid lists, compute EXACT distances from
    // the entry payloads, keep the top k+m by (distance, pk), then fetch + project those rows.
    // Returns nullopt when the shape doesn't match (exact fallback); a malformed query vector
    // is a clean failure. With probes >= lists the probed set is EVERY entry, so the result is
    // EXACTLY the brute-force scan's — the differential gate in sql_vector_test relies on it.
    // K1.3d: the ANN shape matcher, shared by the executor (ivfflat_try_knn) AND the EXPLAIN
    // plan builder (build_plan_single) so what EXPLAIN shows is exactly what runs.
    struct IvfMatch {
        const Index* ix = nullptr;   // nullptr = the query is NOT the ANN shape
        const Expr* vecside = nullptr;  // the constant query-vector expression
        std::uint8_t want_op = 0;       // 0 L2 / 1 cosine / 2 inner-product
        std::string op_name;            // the ORDER BY spelling (for errors/plan text)
    };
    [[nodiscard]] IvfMatch ivfflat_match(const Table& t, const SelectStmt& sel) const {
        if (sel.has_aggregates || !sel.group_by.empty() || sel.distinct || sel.filter.present() ||
            sel.having.present() || !sel.has_limit || sel.order_by.size() != 1 || gs_mode_) {
            return {};
        }
        const OrderKey& okey = sel.order_by[0];
        if (!okey.expr || okey.descending || okey.nulls != NullsOrder::Default) return {};
        for (const SelectItem& it : sel.items) {
            if (it.kind == SelectItemKind::Window) return {};
            if (it.kind == SelectItemKind::Expr && it.expr && it.expr->kind == ExprKind::Func &&
                it.expr->func == "UNNEST") {
                return {};
            }
        }
        const Expr& e = *okey.expr;
        if (e.kind != ExprKind::Func || e.args.size() != 2) return {};
        // K1.3c: the ORDER BY operator selects the required index OPERATOR CLASS.
        std::uint8_t want_op = 0;
        if (e.func == "<->" || e.func == "L2_DISTANCE" || e.func == "EUCLIDEAN_DISTANCE") {
            want_op = 0;  // vector_l2_ops
        } else if (e.func == "<=>" || e.func == "COSINE_DISTANCE") {
            want_op = 1;  // vector_cosine_ops
        } else if (e.func == "<#>") {
            want_op = 2;  // vector_ip_ops (ASC over the NEGATIVE inner product = max dot)
        } else {
            return {};
        }
        const Expr* colside = nullptr;
        const Expr* vecside = nullptr;
        for (int s = 0; s < 2; ++s) {
            const Expr* a = e.args[static_cast<std::size_t>(s)].get();
            const Expr* b = e.args[static_cast<std::size_t>(1 - s)].get();
            if (a != nullptr && a->kind == ExprKind::Col && b != nullptr && !expr_has_col(*b)) {
                colside = a;
                vecside = b;
                break;
            }
        }
        if (colside == nullptr) return {};
        if (!colside->qualifier.empty() && colside->qualifier != t.name) return {};
        const auto cidx = t.column_index(colside->column);
        if (!cidx) return {};
        for (const Index& cand : t.indexes) {
            if ((cand.ivfflat || cand.hnsw) && !cand.columns.empty() && cand.columns[0] == *cidx &&
                cand.vec_op == want_op && (cand.hnsw || !cand.centroids.empty())) {
                return IvfMatch{&cand, vecside, want_op, e.func};
            }
        }
        return {};
    }

    // K2.4: the HYBRID top-k path — Reciprocal Rank Fusion of the vector k-NN and the
    // BM25 rankings in ONE query:
    //   SELECT ... ORDER BY rrf_score(vec_col, '[qvec]', text_col, 'qtext') DESC LIMIT k
    // Both legs run through the EXISTING index-backed top-k paths (ivfflat_try_knn /
    // bm25_try_topk) as synthetic pk-only sub-selects at depth max(60, 3*(k+offset));
    // fusion: score(d) = sum over legs of 1/(60 + rank_d) (the standard RRF, k=60), and
    // the final order (score DESC, pk ASC) is total + deterministic because both leg
    // rankings are. Requires an IVFFLAT index on the vector column AND a BM25 index on
    // the text column — the flagship "hybrid search in one SQL query" shape.
    [[nodiscard]] std::optional<ExecResult> rrf_try_topk(const Table& t, const SelectStmt& sel) {
        if (sel.has_aggregates || !sel.group_by.empty() || sel.distinct || sel.filter.present() ||
            sel.having.present() || !sel.has_limit || sel.order_by.size() != 1 || gs_mode_) {
            return std::nullopt;
        }
        if (sel.level != Level::StrictSerializable || sel.snapshot_version != kNoSeq) {
            return std::nullopt;
        }
        const OrderKey& okey = sel.order_by[0];
        if (!okey.expr || !okey.descending || okey.nulls != NullsOrder::Default) return std::nullopt;
        const Expr& e = *okey.expr;
        if (e.kind != ExprKind::Func || e.func != "RRF_SCORE" || e.args.size() != 4) {
            return std::nullopt;
        }
        for (int a2 = 0; a2 < 4; ++a2) {
            if (!e.args[static_cast<std::size_t>(a2)]) return std::nullopt;
        }
        if (e.args[0]->kind != ExprKind::Col || e.args[2]->kind != ExprKind::Col ||
            expr_has_col(*e.args[1]) || expr_has_col(*e.args[3])) {
            return std::nullopt;
        }
        for (const SelectItem& it : sel.items) {
            if (it.kind == SelectItemKind::Window) return std::nullopt;
            if (it.kind == SelectItemKind::Expr && it.expr && it.expr->kind == ExprKind::Func &&
                (it.expr->func == "UNNEST" || it.expr->func == "RRF_SCORE")) {
                return std::nullopt;
            }
        }
        const std::size_t want = static_cast<std::size_t>(
            std::max<std::int64_t>(0, sel.limit) + std::max<std::int64_t>(0, sel.offset));
        const std::size_t depth = std::max<std::size_t>(60, 3 * want);
        // Build the two pk-only leg sub-selects and run them through the index paths.
        const auto leg = [&](const char* fn, const std::shared_ptr<Expr>& colref,
                             const std::shared_ptr<Expr>& query, bool desc,
                             bool is_bm25) -> std::optional<std::vector<Key>> {
            SelectStmt sub;
            sub.table = sel.table;
            sub.level = Level::StrictSerializable;
            SelectItem it;
            it.kind = SelectItemKind::Column;
            it.column = t.pk().name;
            it.label = t.pk().name;
            sub.items.push_back(std::move(it));
            OrderKey k2;
            auto fx = std::make_shared<Expr>();
            fx->kind = ExprKind::Func;
            fx->func = fn;
            fx->args = {colref, query};
            k2.expr = fx;
            k2.descending = desc;
            sub.order_by.push_back(std::move(k2));
            sub.has_limit = true;
            sub.limit = static_cast<std::int64_t>(depth);
            const auto r2 = is_bm25 ? bm25_try_topk(t, sub) : ivfflat_try_knn(t, sub);
            if (!r2 || !r2->ok) return std::nullopt;  // missing index / leg error -> no fusion
            std::vector<Key> pks;
            pks.reserve(r2->rows.size());
            for (const ResultRow& row : r2->rows) pks.push_back(encode_pk(row.cells[0].second));
            return pks;
        };
        const auto vleg = leg("<->", e.args[0], e.args[1], /*desc=*/false, /*is_bm25=*/false);
        if (!vleg) return std::nullopt;
        const auto tleg = leg("BM25_SCORE", e.args[2], e.args[3], /*desc=*/true, /*is_bm25=*/true);
        if (!tleg) return std::nullopt;
        constexpr double kRrf = 60.0;
        std::map<Key, double> fused;  // pk bytes -> RRF score (deterministic iteration)
        for (std::size_t rank = 0; rank < vleg->size(); ++rank)
            fused[(*vleg)[rank]] += 1.0 / (kRrf + static_cast<double>(rank + 1));
        for (std::size_t rank = 0; rank < tleg->size(); ++rank)
            fused[(*tleg)[rank]] += 1.0 / (kRrf + static_cast<double>(rank + 1));
        struct FHit {
            double s;
            Key pk;
        };
        std::vector<FHit> hits;
        hits.reserve(fused.size());
        for (const auto& [pk, s2] : fused) hits.push_back(FHit{s2, pk});
        const auto better = [](const FHit& a, const FHit& b) {
            const int c = Datum::real_cmp(a.s, b.s);
            if (c != 0) return c > 0;
            return a.pk < b.pk;
        };
        if (hits.size() > want) {
            std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(want),
                              hits.end(), better);
            hits.resize(want);
        } else {
            std::sort(hits.begin(), hits.end(), better);
        }
        ExecResult r;
        for (const FHit& h : hits) {
            const Key rkey = table_prefix(t.id) + h.pk;
            std::vector<Datum> row;
            bool have = false;
            if (t.columnar) {
                auto rr = read_columnar_row(t, decode_pk(t, rkey));
                if (rr) {
                    row = std::move(*rr);
                    have = true;
                }
            } else {
                (void)db_.engine().scan_visit(
                    storage::Range{rkey, key_successor(rkey), /*hi_unbounded=*/false},
                    db_.live_snap_seq(), [&](const Key&, const Value& v2) {
                        row = decode_row(t, rkey, v2);
                        have = true;
                    });
            }
            if (!have) continue;
            ResultRow out;
            if (sel.star) {
                for (std::size_t i = 0; i < t.columns.size(); ++i) {
                    if (t.columns[i].dropped) continue;
                    out.cells.emplace_back(t.columns[i].name, row[i]);
                }
            } else {
                for (const SelectItem& item : sel.items) {
                    Datum d2;
                    if (item.kind == SelectItemKind::Expr) {
                        if (auto err = eval_expr(*item.expr, t, row, d2)) {
                            return ExecResult::failure(*err);
                        }
                    } else {
                        const auto idx2 = t.column_index(item.column);
                        if (!idx2) return ExecResult::failure("unknown column '" + item.column + "'");
                        d2 = row[*idx2];
                    }
                    out.cells.emplace_back(item.label, d2);
                }
            }
            r.rows.push_back(std::move(out));
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    // K2: the BM25 top-k path — `SELECT ... ORDER BY bm25_score(col, 'query') DESC LIMIT k`
    // (no WHERE/GROUP BY/DISTINCT/aggregates) over a BM25-indexed TEXT column at the
    // Strict live level. Scores the classic BM25 (k1=1.2, b=0.75) from the postings:
    // one scan_visit per DISTINCT query term, df = the posting count, idf =
    // ln(1 + (N-df+0.5)/(df+0.5)), accumulated per doc; ranked by (score DESC, pk ASC) —
    // total and deterministic. Every non-matching shape returns nullopt (generic path).
    [[nodiscard]] std::optional<ExecResult> bm25_try_topk(const Table& t, const SelectStmt& sel) {
        if (sel.has_aggregates || !sel.group_by.empty() || sel.distinct || sel.filter.present() ||
            sel.having.present() || !sel.has_limit || sel.order_by.size() != 1 || gs_mode_) {
            return std::nullopt;
        }
        if (sel.level != Level::StrictSerializable || sel.snapshot_version != kNoSeq) {
            return std::nullopt;
        }
        const OrderKey& okey = sel.order_by[0];
        if (!okey.expr || !okey.descending || okey.nulls != NullsOrder::Default) return std::nullopt;
        const Expr& e = *okey.expr;
        if (e.kind != ExprKind::Func || e.func != "BM25_SCORE" || e.args.size() != 2) {
            return std::nullopt;
        }
        const Expr* colside = e.args[0] && e.args[0]->kind == ExprKind::Col ? e.args[0].get() : nullptr;
        const Expr* qside = e.args[1].get();
        if (colside == nullptr || qside == nullptr || expr_has_col(*qside)) return std::nullopt;
        if (!colside->qualifier.empty() && colside->qualifier != t.name) return std::nullopt;
        const auto cidx = t.column_index(colside->column);
        if (!cidx) return std::nullopt;
        const Index* ix = nullptr;
        for (const Index& cand : t.indexes) {
            if (cand.bm25 && !cand.columns.empty() && cand.columns[0] == *cidx) {
                ix = &cand;
                break;
            }
        }
        if (ix == nullptr) return std::nullopt;
        for (const SelectItem& it : sel.items) {  // no window/UNNEST/score projection
            if (it.kind == SelectItemKind::Window) return std::nullopt;
            if (it.kind == SelectItemKind::Expr && it.expr) {
                if (it.expr->kind == ExprKind::Func &&
                    (it.expr->func == "UNNEST" || it.expr->func == "BM25_SCORE")) {
                    return std::nullopt;
                }
            }
        }
        Datum qd;
        const std::vector<Datum> dummy(t.columns.size());
        if (eval_expr(*qside, t, dummy, qd)) return std::nullopt;
        if (qd.is_null || qd.type != Type::Text) return std::nullopt;
        std::uint32_t qdl = 0;
        const std::map<std::string, std::uint32_t> qterms = bm25_tokens(qd.s, qdl);

        std::uint32_t ndocs = 0;
        std::uint64_t total_len = 0;
        static const std::vector<std::pair<Key, Value>> kNoBatch;
        if (const auto sv = bm25_get(bm25_stats_key(t.id, ix->id), kNoBatch)) {
            bm25_stats_dec(*sv, ndocs, total_len);
        }
        ExecResult r;
        if (ndocs == 0 || qterms.empty()) {  // empty corpus/query -> empty result
            r.affected = 0;
            return r;
        }
        const double avgdl = static_cast<double>(total_len) / ndocs;
        constexpr double kK1 = 1.2, kB = 0.75;
        std::map<Key, double> score;  // pk bytes -> accumulated score (deterministic order)
        std::size_t scanned = 0;
        for (const auto& [term, qcnt] : qterms) {
            (void)qcnt;  // classic BM25 ignores query-side tf for short queries
            const Key pfx = bm25_term_prefix(t.id, ix->id, term);
            std::vector<std::pair<Key, std::pair<std::uint32_t, std::uint32_t>>> posts;
            const bool visited = db_.engine().scan_visit(
                storage::Range{pfx, key_successor(pfx), /*hi_unbounded=*/false},
                db_.live_snap_seq(), [&](const Key& k2, const Value& v2) {
                    if (v2.size() < 8 || k2.size() <= pfx.size()) return;
                    posts.emplace_back(k2.substr(pfx.size()),
                                       std::make_pair(Datum::get_be32_(v2, 0),
                                                      Datum::get_be32_(v2, 4)));
                });
            if (!visited) return std::nullopt;  // vlog-active engine: no BM25 fast path
            scanned += posts.size();
            if (posts.empty()) continue;
            const double df = static_cast<double>(posts.size());
            const double idf = std::log(1.0 + (ndocs - df + 0.5) / (df + 0.5));
            for (const auto& [pk, p2] : posts) {
                const double tf = p2.first, dl2 = p2.second;
                score[pk] += idf * (tf * (kK1 + 1.0)) /
                             (tf + kK1 * (1.0 - kB + kB * dl2 / avgdl));
            }
        }
        struct Hit2 {
            double s;
            Key pk;
        };
        std::vector<Hit2> hits;
        hits.reserve(score.size());
        for (const auto& [pk, sc] : score) hits.push_back(Hit2{sc, pk});
        const auto better = [](const Hit2& a, const Hit2& b) {
            const int c = Datum::real_cmp(a.s, b.s);
            if (c != 0) return c > 0;  // higher score first
            return a.pk < b.pk;        // pk tie-break, total + deterministic
        };
        const std::size_t want = static_cast<std::size_t>(
            std::max<std::int64_t>(0, sel.limit) + std::max<std::int64_t>(0, sel.offset));
        if (hits.size() > want) {
            std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(want),
                              hits.end(), better);
            hits.resize(want);
        } else {
            std::sort(hits.begin(), hits.end(), better);
        }
        for (const Hit2& h : hits) {
            const Key rkey = table_prefix(t.id) + h.pk;
            std::vector<Datum> row;
            bool have = false;
            if (t.columnar) {
                auto rr = read_columnar_row(t, decode_pk(t, rkey));
                if (rr) {
                    row = std::move(*rr);
                    have = true;
                }
            } else {
                (void)db_.engine().scan_visit(
                    storage::Range{rkey, key_successor(rkey), /*hi_unbounded=*/false},
                    db_.live_snap_seq(), [&](const Key&, const Value& v2) {
                        row = decode_row(t, rkey, v2);
                        have = true;
                    });
            }
            if (!have) continue;
            ResultRow out;
            if (sel.star) {
                for (std::size_t i = 0; i < t.columns.size(); ++i) {
                    if (t.columns[i].dropped) continue;
                    out.cells.emplace_back(t.columns[i].name, row[i]);
                }
            } else {
                for (const SelectItem& item : sel.items) {
                    Datum d2;
                    if (item.kind == SelectItemKind::Expr) {
                        if (auto err = eval_expr(*item.expr, t, row, d2)) {
                            return ExecResult::failure(*err);
                        }
                    } else {
                        const auto idx2 = t.column_index(item.column);
                        if (!idx2) return ExecResult::failure("unknown column '" + item.column + "'");
                        d2 = row[*idx2];
                    }
                    out.cells.emplace_back(item.label, d2);
                }
            }
            r.rows.push_back(std::move(out));
        }
        if (plan_stats_ != nullptr) {
            plan_stats_->scanned = scanned;
            plan_stats_->after_filter = r.rows.size();
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    [[nodiscard]] std::optional<ExecResult> ivfflat_try_knn(const Table& t, const SelectStmt& sel) {
        const IvfMatch m = ivfflat_match(t, sel);
        if (m.ix == nullptr) return std::nullopt;
        const Index* ix = m.ix;
        const Expr* vecside = m.vecside;
        const std::uint8_t want_op = m.want_op;
        const std::string& fname = m.op_name;

        // Evaluate the CONSTANT query vector (no column refs — any row works as context).
        Datum qd;
        const std::vector<Datum> dummy(t.columns.size());
        if (eval_expr(*vecside, t, dummy, qd)) return std::nullopt;  // un-evaluable => exact path
        if (qd.type != Type::Text || qd.is_null) return std::nullopt;
        std::vector<Datum> qelems;
        if (qd.logical == 7 || qd.logical == 15) {
            qelems = Datum::decode_array(qd.s);
        } else if (qd.logical == 0) {
            if (parse_vector_literal(qd.s, qelems)) return std::nullopt;
        } else {
            return std::nullopt;
        }
        std::vector<double> qv;
        qv.reserve(qelems.size());
        for (const Datum& el : qelems) {
            if (el.is_null) return std::nullopt;
            qv.push_back(el.is_real() ? el.real_value() : static_cast<double>(el.i));
        }
        std::size_t dim = 0;
        std::vector<std::vector<double>> cents;
        if (ix->ivfflat) {
            ivf_decode_centroids(ix->centroids, dim, cents);
            if (cents.empty()) return std::nullopt;
        } else {
            dim = t.columns[ix->columns[0]].max_len;  // K1.4: HNSW — dim from the column
        }
        if (qv.size() != dim) {
            return ExecResult::failure(fname + " requires vectors of equal length");
        }

        // The Strict-level LIVE path may use the synchronous storage seams (probe cache,
        // scan_visit point-gets); every other level reads through the leveled pipeline.
        const bool live_strict =
            sel.level == Level::StrictSerializable && sel.snapshot_version == kNoSeq;
        // K1 perf: a candidate is (distance, pk BYTES) only — the pk Datum is decoded for
        // the k+offset WINNERS after the cut, not for every scored entry.
        struct Cand {
            double dist = 0.0;
            Key pk_bytes;
        };
        std::vector<Cand> cands;
        std::size_t scanned = 0;
        const std::size_t want = static_cast<std::size_t>(
            std::max<std::int64_t>(0, sel.limit) + std::max<std::int64_t>(0, sel.offset));

        if (ix->hnsw) {
            // K1.4: greedy-descend from the entry point to level 0, then one ef-wide beam
            // search there (ef = max(hnsw.ef_search, k)). Zombies steer but are filtered.
            HnswIO io;
            io.t = &t;
            io.ix = ix;
            io.sel = &sel;  // leveled reads — AT SNAPSHOT sees the graph as of that Seq
            const auto entry = hnsw_get(io, hnsw_entry_key(t.id, ix->id));
            if (entry && !entry->empty()) {
                const std::uint8_t etop = static_cast<std::uint8_t>((*entry)[0]);
                std::vector<std::string> eps{entry->substr(1)};
                for (int lc = etop; lc >= 1; --lc) {
                    const auto w = hnsw_search_layer(io, qv, eps, 1, static_cast<std::uint8_t>(lc));
                    if (!w.empty()) eps = {w.front().second};
                }
                const std::size_t ef = std::max<std::size_t>(want, hnsw_ef_search_);
                const auto w0 = hnsw_search_layer(io, qv, eps, ef, 0);
                if (!io.err.empty()) return ExecResult::failure(io.err);
                for (const auto& [d, pkb] : w0) {
                    HnswNode n;
                    if (!hnsw_node(io, pkb, n, nullptr) || n.deleted) continue;  // zombie
                    cands.push_back(Cand{d, pkb});
                }
                scanned = io.visited;
            }
        } else {
        // Rank the lists by centroid distance in ASSIGNMENT space (raw for L2, direction for
        // cosine/IP — the same space the entries were bucketed in); probe the nearest `probes`.
        const std::vector<double> qav = ivf_assign_vec(want_op, qv);
        std::vector<std::pair<double, std::size_t>> ranked;
        ranked.reserve(cents.size());
        for (std::size_t c = 0; c < cents.size(); ++c) {
            double d2 = 0.0;
            for (std::size_t d = 0; d < dim; ++d) {
                const double x = cents[c][d] - qav[d];
                d2 += x * x;
            }
            ranked.emplace_back(d2, c);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        // K1.3: `SET ivfflat.probes = n` overrides the index's own default for this session.
        const std::uint32_t probes = ivfflat_probes_ != 0 ? ivfflat_probes_ : ix->probes;
        const std::size_t nprobe =
            std::min<std::size_t>(std::max<std::uint32_t>(1, probes), ranked.size());

        // Gather candidates from the probed lists (exact distance from the entry payload).
        // The entry key is prefix ++ put_index_col(INT list) [9 bytes] ++ encode_pk(pk):
        // the pk BYTES are the raw suffix — no Datum decode + re-encode per candidate.
        const std::size_t pk_off = index_prefix(t.id, ix->id).size() + 9;
        // EXACT opclass distance over the RAW payload — the SAME accumulation order as the
        // scalar distance kernel, so the value is IEEE-identical to the exact path's and
        // the probes=lists gate holds per opclass. Fused fast path; the generic decode
        // covers any other payload shape.
        const std::function<void(const Key&, const Value&)> score_entry =
            [&](const Key& ekey, const Value& payload) {
                ++scanned;
                Cand c;
                if (!ivf_score_fast(payload, qv, want_op, c.dist)) {
                    const std::vector<double> v = ivf_payload_doubles(payload);
                    const std::size_t vn = std::min(dim, v.size());
                    double dot = 0.0, sq = 0.0, nu = 0.0, nv = 0.0;  // shared 4-lane kernel
                    vec_accum4(vn, qv, [&v](std::size_t k) { return v[k]; }, dot, sq, nu, nv);
                    c.dist = vec_finish(want_op, dot, sq, nu, nv);
                }
                c.pk_bytes = ekey.size() > pk_off ? ekey.substr(pk_off) : Key{};
                cands.push_back(std::move(c));
            };
        // K1 perf (memory-bound fix): the Strict-level live probe scores from the
        // CONTIGUOUS per-list cache — dense double blocks stream sequentially instead of
        // reading ~600-byte payload strings scattered across the heap. The cache holds
        // the SAME doubles the payloads decode to and is erased by any index maintenance,
        // so the scored values are bit-identical to the scan path's. Built lazily here
        // (one scan_visit pass over the index) when absent.
        const IvfProbeCache* cache = nullptr;
        if (live_strict) {
            const std::uint64_t ck = ivf_cache_key(t.id, ix->id);
            auto cit = ivf_probe_cache_.find(ck);
            if (cit == ivf_probe_cache_.end()) {
                IvfProbeCache fresh;
                fresh.dim = dim;
                fresh.list_vecs.resize(ix->lists);
                fresh.list_f32.resize(ix->lists);
                fresh.list_norm.resize(ix->lists);
                fresh.list_pks.resize(ix->lists);
                bool ok = true;
                std::vector<double> tmp;
                for (std::uint32_t l = 0; l < ix->lists && ok; ++l) {
                    Key llo = index_prefix(t.id, ix->id);
                    put_index_col(llo, Datum::make_int(static_cast<std::int64_t>(l)));
                    ok = db_.engine().scan_visit(
                        storage::Range{llo, key_successor(llo), /*hi_unbounded=*/false},
                        db_.live_snap_seq(), [&](const Key& k2, const Value& val) {
                            if (!ivf_doubles_fast(val, tmp)) tmp = ivf_payload_doubles(val);
                            tmp.resize(dim, 0.0);  // defensive; coerced payloads match dim
                            auto& flat = fresh.list_vecs[l];
                            flat.insert(flat.end(), tmp.begin(), tmp.end());
                            auto& f32 = fresh.list_f32[l];
                            for (const double d2 : tmp) f32.push_back(static_cast<float>(d2));
                            fresh.list_norm[l].push_back(
                                static_cast<float>(std::sqrt(vec_nv4(tmp))));
                            fresh.list_pks[l].push_back(
                                k2.size() > pk_off ? k2.substr(pk_off) : Key{});
                        });
                }
                if (ok) cit = ivf_probe_cache_.emplace(ck, std::move(fresh)).first;
            }
            if (cit != ivf_probe_cache_.end() && cit->second.dim == dim) {
                cache = &cit->second;
            }
        }
        const double qnv = want_op == 1 ? vec_nv4(qv) : 0.0;  // cosine: query norm, once
        if (cache != nullptr) {
            // ---- Phase 1 (K1 perf): FLOAT32 prune over every probed list — half the
            // bytes, twice the lanes. Prune scores are approximate and only ever used to
            // pick a provably sufficient WINDOW (below); the RESULT comes from the exact
            // double re-rank, so it is identical to the pure-double path no matter how
            // the f32 rounding falls (and thus across platforms/binaries too).
            const std::vector<float> qf(qv.begin(), qv.end());
            float qnf = 0.0F;
            for (const float x : qf) qnf += x * x;
            qnf = std::sqrt(qnf);
            // K1 perf (SoA prune): scores land in ONE flat float array; (list, count)
            // pairs recover each score's position — no 12-byte per-candidate records.
            std::vector<float> ps;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> plists;
            plists.reserve(nprobe);
            for (std::size_t p = 0; p < nprobe; ++p) {
                const std::size_t list = ranked[p].second;
                if (list >= cache->list_pks.size()) continue;
                const float* f = cache->list_f32[list].data();
                const std::size_t n2 = cache->list_pks[list].size();
                ps.reserve(ps.size() + n2);
                plists.emplace_back(static_cast<std::uint32_t>(list),
                                    static_cast<std::uint32_t>(n2));
                for (std::size_t i = 0; i < n2; ++i, f += dim) {
                    float s;
                    if (want_op == 0) {
                        float l0 = 0, l1 = 0, l2 = 0, l3 = 0;
                        std::size_t k = 0;
                        for (; k + 4 <= dim; k += 4) {
                            const float a0 = f[k] - qf[k], a1 = f[k + 1] - qf[k + 1];
                            const float a2 = f[k + 2] - qf[k + 2], a3 = f[k + 3] - qf[k + 3];
                            l0 += a0 * a0; l1 += a1 * a1; l2 += a2 * a2; l3 += a3 * a3;
                        }
                        for (; k < dim; ++k) { const float a = f[k] - qf[k]; l0 += a * a; }
                        s = (l0 + l1) + (l2 + l3);  // pruned as SQUARED L2 (monotone)
                    } else {
                        float d0 = 0, d1 = 0, d2 = 0, d3 = 0, u0 = 0, u1 = 0, u2 = 0, u3 = 0;
                        std::size_t k = 0;
                        for (; k + 4 <= dim; k += 4) {
                            d0 += f[k] * qf[k]; d1 += f[k + 1] * qf[k + 1];
                            d2 += f[k + 2] * qf[k + 2]; d3 += f[k + 3] * qf[k + 3];
                            u0 += f[k] * f[k]; u1 += f[k + 1] * f[k + 1];
                            u2 += f[k + 2] * f[k + 2]; u3 += f[k + 3] * f[k + 3];
                        }
                        for (; k < dim; ++k) { d0 += f[k] * qf[k]; u0 += f[k] * f[k]; }
                        const float dotf = (d0 + d1) + (d2 + d3);
                        if (want_op == 1) {
                            const float denom = std::sqrt((u0 + u1) + (u2 + u3)) * qnf;
                            s = denom == 0.0F ? 1.0F : 1.0F - dotf / denom;
                        } else {
                            s = -dotf;
                        }
                    }
                    ps.push_back(s);
                }
            }
            scanned += ps.size();
            // The want-th smallest f32 prune score (the window pivot) via a RUNNING
            // bounded max-heap — no second scores copy, no nth_element pass.
            float kth = std::numeric_limits<float>::infinity();
            if (want > 0 && ps.size() > want) {
                std::vector<float> heap;
                heap.reserve(want);
                for (const float s : ps) {
                    if (heap.size() < want) {
                        heap.push_back(s);
                        std::push_heap(heap.begin(), heap.end());
                    } else if (s < heap.front()) {
                        std::pop_heap(heap.begin(), heap.end());
                        heap.back() = s;
                        std::push_heap(heap.begin(), heap.end());
                    }
                }
                kth = heap.front();
            }
            // ---- Phase 2: EXACT double re-rank of the window. The margins over-cover
            // the worst-case f32 error, so the window provably contains the true
            // top-`want` set: L2 prunes non-negative squared sums (no cancellation —
            // relative error <= ~(dim+4)*2^-24, margined at 2^-14 ~ 7x slack + an
            // absolute floor); cosine distance lies in [0,2] with absolute error
            // <= ~1e-6 (margin 1e-4); inner-product error scales with |u||v| (margin
            // 1e-4 * (|u|*|q| + 1)). Any candidate outside its margin cannot beat the
            // true k-th, so dropping it cannot change the exact top-k. Scores are read
            // back POSITIONALLY from the flat array (SoA — no per-candidate records).
            std::size_t pos = 0;
            for (const auto& [list32, cnt] : plists) {
                const std::size_t list = list32;
                const double* base = cache->list_vecs[list].data();
                for (std::uint32_t i = 0; i < cnt; ++i) {
                    const float s = ps[pos++];
                    bool in_window = true;
                    if (kth != std::numeric_limits<float>::infinity()) {
                        if (want_op == 0) {
                            in_window = s <= kth * (1.0F + 6.1e-5F) + 1e-30F;
                        } else if (want_op == 1) {
                            in_window = s <= kth + 1e-4F;
                        } else {
                            in_window =
                                s <= kth + 1e-4F * (cache->list_norm[list][i] * qnf + 1.0F);
                        }
                    }
                    if (!in_window) continue;
                    const double* fp = base + static_cast<std::size_t>(i) * dim;
                    const auto gx = [fp](std::size_t k) { return fp[k]; };
                    Cand c;
                    if (want_op == 0) {
                        c.dist = std::sqrt(vec_sq4(dim, qv, gx));
                    } else if (want_op == 1) {
                        double dot = 0.0, nu = 0.0;
                        vec_dot_nu4(dim, qv, gx, dot, nu);
                        const double denom = std::sqrt(nu) * std::sqrt(qnv);
                        c.dist = denom == 0.0 ? 1.0 : 1.0 - dot / denom;
                    } else {
                        c.dist = -vec_dot4(dim, qv, gx);
                    }
                    c.pk_bytes = cache->list_pks[list][i];
                    cands.push_back(std::move(c));
                }
            }
        }
        for (std::size_t p = 0; cache == nullptr && p < nprobe; ++p) {
            const std::size_t list = ranked[p].second;
            Key lo = index_prefix(t.id, ix->id);
            put_index_col(lo, Datum::make_int(static_cast<std::int64_t>(list)));
            const Key hi = key_successor(lo);
            // Strict-level fallback without a cache: visit synchronously inside storage;
            // any other level takes the regular leveled scan. Entries + scores identical.
            if (live_strict &&
                db_.engine().scan_visit(storage::Range{lo, hi, /*hi_unbounded=*/false},
                                        db_.live_snap_seq(), score_entry)) {
                continue;
            }
            std::vector<storage::KeyValue> kvs;
            if (auto err = run_index_scan_at_level(sel, lo, hi, kvs)) {
                return ExecResult::failure(*err);
            }
            for (const storage::KeyValue& ikv : kvs) {
                if (is_tombstone(ikv.second)) continue;
                score_entry(ikv.first, ikv.second);
            }
        }
        }  // end IVFFLAT probe branch
        // Total deterministic order — (distance, PK bytes) — matching the exact path's
        // ORDER BY distance with its PK tie-break (real_cmp keeps NaN total). The
        // comparator is TOTAL, so a partial sort of the k+offset winners selects and
        // orders exactly the same rows a full stable sort would (K1 perf: no full sort
        // of thousands of candidates for a LIMIT-k query).
        const auto cand_less = [](const Cand& a, const Cand& b) {
            const int c = Datum::real_cmp(a.dist, b.dist);
            if (c != 0) return c < 0;
            return a.pk_bytes < b.pk_bytes;
        };
        if (cands.size() > want) {
            std::partial_sort(cands.begin(),
                              cands.begin() + static_cast<std::ptrdiff_t>(want), cands.end(),
                              cand_less);
            cands.resize(want);
        } else {
            std::sort(cands.begin(), cands.end(), cand_less);
        }

        // Fetch + project the winners in distance order (already final — no sort needed).
        // The pk Datum is decoded HERE, for the <= k+offset winners only. K1 perf: on the
        // Strict-level live path the row point-get goes through the synchronous scan_visit
        // seam (a one-key range) instead of the full Query pipeline — the k winner fetches
        // were the dominant remaining per-query cost; values and MVCC semantics identical
        // (same snapshot Seq a Query<Strict> resolves to, tombstones filtered inside).
        ExecResult r;
        for (const Cand& c : cands) {
            const Key rkey = table_prefix(t.id) + c.pk_bytes;
            std::vector<Datum> row;
            bool have = false;
            if (t.columnar) {
                auto rr = read_columnar_row(t, decode_pk(t, rkey));
                if (rr) {
                    row = std::move(*rr);
                    have = true;
                }
            } else {
                bool visited = false;
                if (live_strict) {
                    visited = db_.engine().scan_visit(
                        storage::Range{rkey, key_successor(rkey), /*hi_unbounded=*/false},
                        db_.live_snap_seq(), [&](const Key&, const Value& v2) {
                            row = decode_row(t, rkey, v2);
                            have = true;
                        });
                }
                if (!visited) {
                    const ReadResult rv = point_get_at_level(sel, rkey);
                    if (rv.has_value() && !is_tombstone(*rv)) {
                        row = decode_row(t, rkey, *rv);
                        have = true;
                    }
                }
            }
            if (!have) continue;  // entry with no live row (defensive)
            ResultRow out;
            if (sel.star) {
                for (std::size_t i = 0; i < t.columns.size(); ++i) {
                    if (t.columns[i].dropped) continue;
                    out.cells.emplace_back(t.columns[i].name, row[i]);
                }
            } else {
                for (const SelectItem& item : sel.items) {
                    Datum d;
                    if (item.kind == SelectItemKind::Expr) {
                        if (auto err = eval_expr(*item.expr, t, row, d)) {
                            return ExecResult::failure(*err);
                        }
                    } else {
                        const auto idx = t.column_index(item.column);
                        if (!idx) return ExecResult::failure("unknown column '" + item.column + "'");
                        d = row[*idx];
                    }
                    out.cells.emplace_back(item.label, d);
                }
            }
            r.rows.push_back(std::move(out));
        }
        if (plan_stats_ != nullptr) {
            plan_stats_->scanned = scanned;
            plan_stats_->after_filter = r.rows.size();
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
    }

    [[nodiscard]] std::optional<std::string> read_via_index(
        const Table& t, const SelectStmt& sel, const IndexPlan& plan,
        const std::vector<bool>& need, std::vector<std::vector<Datum>>& rows_out,
        bool preserve_index_order = false, bool covering = false, bool reverse_index_order = false) {
        // (a) The index range [lo, hi): col-ascending entries for the matching values.
        Key lo = index_prefix(t.id, plan.index->id);
        Key hi;
        if (plan.is_eq) {
            // I1: encode the whole matched eq PREFIX (>=1 column) — the range covers every PK suffix
            // under that exact leading-prefix tuple.
            if (plan.eq_prefix.empty()) put_index_col(lo, plan.eq);
            else for (const Datum& d : plan.eq_prefix) put_index_col(lo, d);
            hi = key_successor(lo);
        } else {
            put_index_col(lo, plan.lo);
            Key hi_pref = index_prefix(t.id, plan.index->id);
            put_index_col(hi_pref, plan.hi);
            hi = key_successor(hi_pref);  // inclusive upper bound (col <= hi)
        }
        std::vector<storage::KeyValue> index_kvs;
        if (auto err = run_index_scan_at_level(sel, lo, hi, index_kvs)) {
            return err;
        }
        // (b) For each live index entry, decode the PK from the entry key and POINT-GET
        // the row. A tombstoned index entry (a stale slot a DROP/UPDATE retired) is
        // skipped; a point-get that misses (the row was deleted but the index entry is
        // momentarily a tombstone — never happens within one committed snapshot since
        // maintenance is atomic) is skipped fail-safe. PK-ascending is NOT guaranteed by
        // the index (it is COL-ascending); the pipeline re-sorts via ORDER BY / the PK
        // tie-break, and the no-ORDER-BY case is handled below.
        std::vector<std::pair<Datum, std::vector<Datum>>> fetched;  // (pk, row)
        for (const storage::KeyValue& ikv : index_kvs) {
            if (is_tombstone(ikv.second)) {
                continue;
            }
            const Datum pk = decode_index_entry_pk(t, plan.index->id, ikv.first);
            if (covering) {  // I3: index-only — reconstruct the row from the entry, no table fetch
                fetched.emplace_back(pk, decode_index_entry_full(t, *plan.index, ikv.first));
                continue;
            }
            if (t.columnar) {
                // Columnar: assemble the row from its column families (point-gets).
                auto row = read_columnar_row(t, pk);
                if (!row) {
                    continue;  // index entry with no live row (defensive)
                }
                fetched.emplace_back(pk, std::move(*row));
                continue;
            }
            const Key rkey = encode_key(t, pk);
            const ReadResult rv = point_get_at_level(sel, rkey);
            if (!rv.has_value() || is_tombstone(*rv)) {
                continue;  // index entry with no live row (defensive; atomicity prevents)
            }
            fetched.emplace_back(pk, decode_row_projected(t, rkey, *rv, need));
        }
        // (c) Emit rows in PK-ASCENDING order so the index path is byte-identical to the
        // full-scan path (which yields PK-ascending) BEFORE the pipeline's ORDER BY /
        // DISTINCT / aggregation. The conformance gate compares the two — same rows,
        // same order. Sort by the order-preserving PK key bytes (total + deterministic).
        // I2: when the caller will SKIP the ORDER BY because the index already yields the
        // requested order, KEEP the index (column-ascending, PK-tie-broken) scan order instead.
        if (reverse_index_order)
            std::reverse(fetched.begin(), fetched.end());  // I7: DESC over a unique full-cover index
        else if (!preserve_index_order)
            std::stable_sort(fetched.begin(), fetched.end(),
                             [](const auto& x, const auto& y) {
                                 return encode_pk(x.first) < encode_pk(y.first);
                             });
        rows_out.reserve(fetched.size());
        for (auto& [pk, row] : fetched) {
            (void)pk;
            rows_out.push_back(std::move(row));
        }
        return std::nullopt;
    }

    // I3: reconstruct a ROW from an index ENTRY (covered columns + PK) WITHOUT a table fetch — used
    // for an index-only (covering) scan. Each covered column is decoded from its order-preserving
    // token (INT: sign + be64^msb; TEXT-backed: unescape 0x00 0x01 up to the 0x00 0x00 terminator);
    // the PK is decoded from the suffix. Non-covered columns stay default (the caller guarantees the
    // query needs only covered columns + the PK).
    [[nodiscard]] static std::vector<Datum> decode_index_entry_full(const Table& t, const Index& ix,
                                                                    const Key& entry) {
        std::vector<Datum> row(t.columns.size());
        std::size_t off = index_prefix(t.id, ix.id).size();
        // J2: an expression index covers no column, so this covering-decode is never selected for one;
        // guard anyway — the PK still follows the (here unused) expression-value token.
        if (!ix.expr_src.empty()) {
            row[t.pk_index] = decode_index_entry_pk(t, ix.id, entry);
            tag_logical_cols(t, row);
            return row;
        }
        for (const std::size_t cidx : ix.columns) {
            if (t.columns[cidx].type == Type::Int) {
                std::uint64_t bits = 0;
                for (int b = 1; b < 9; ++b) bits = (bits << 8) | static_cast<unsigned char>(entry[off + b]);
                bits ^= 0x8000000000000000ULL;
                row[cidx] = Datum::make_int(static_cast<std::int64_t>(bits));
                off += 9;
            } else {
                std::string s;
                while (off + 1 < entry.size()) {
                    if (entry[off] == '\0') {
                        if (entry[off + 1] == '\0') { off += 2; break; }
                        s.push_back('\0'); off += 2;  // escaped 0x00 0x01
                    } else {
                        s.push_back(entry[off]); off += 1;
                    }
                }
                row[cidx] = Datum::make_text(std::move(s));
            }
        }
        row[t.pk_index] = decode_pk(t, table_prefix(t.id) + entry.substr(off));
        tag_logical_cols(t, row);
        return row;
    }
    // I3: does `ix` cover every column the query NEEDS (so the row can be served from the index)?
    static bool index_covers_need(const Index& ix, const std::vector<bool>& need,
                                  std::size_t pk_index) {
        // J2/J3: an expression index covers no stored column; a GIN entry holds a single ARRAY
        // element, not the array — neither can serve a covering (index-only) read.
        if (!ix.expr_src.empty() || ix.gin || ix.ivfflat || ix.hnsw || ix.bm25) return false;
        for (std::size_t c = 0; c < need.size(); ++c) {
            if (!need[c] || c == pk_index) continue;
            bool in_ix = false;
            for (const std::size_t ic : ix.columns) if (ic == c) { in_ix = true; break; }
            if (!in_ix) return false;
        }
        return true;
    }

    // Decode the PK datum from an index ENTRY key. The entry is
    // index_prefix ++ encode_index_col(col) ++ encode_pk(pk); skip the prefix, then the
    // self-delimiting col token, leaving the PK suffix which decodes per the PK type.
    [[nodiscard]] static Datum decode_index_entry_pk(const Table& t,
                                                     std::uint32_t index_id,
                                                     const Key& entry) {
        const std::size_t prefix_len = index_prefix(t.id, index_id).size();
        std::size_t off = prefix_len;
        const Index* ixp = nullptr;
        for (const Index& ix : t.indexes) if (ix.id == index_id) { ixp = &ix; break; }
        // The leading key tokens are either the covered columns' values (one each, possibly composite)
        // or — for a J2 EXPRESSION index (columns empty) — the single evaluated expression value.
        const auto skip_token = [&](Type ty) {
            if (ty == Type::Int) {
                off += 9;  // put_pk_int width (sign byte + be64)
                return;
            }
            while (off + 1 < entry.size()) {  // escaped TEXT token up to the 0x00 0x00 terminator
                if (entry[off] == '\0') {
                    if (entry[off + 1] == '\0') { off += 2; break; }
                    off += 2;  // escaped 0x00 0x01
                } else {
                    off += 1;
                }
            }
        };
        if (ixp->ivfflat) {
            skip_token(Type::Int);  // K1.3: the leading token is the INT list id, not the column
        } else if (ixp->gin) {
            skip_token(t.columns[ixp->columns[0]].elem_type);  // J3: one array-element token
        } else if (!ixp->expr_src.empty()) {
            skip_token(ixp->expr_type);  // J2: one expression-value token
        } else {
            for (const std::size_t cidx : ixp->columns) skip_token(t.columns[cidx].type);  // I1: composite
        }
        // The remaining suffix is encode_pk(pk). Reuse decode_pk by reconstructing a
        // full row key: table_prefix ++ suffix (decode_pk strips the 6-byte prefix).
        const std::string pk_suffix = entry.substr(off);
        const Key row_key = table_prefix(t.id) + pk_suffix;
        return decode_pk(t, row_key);
    }

    // Index range scan at the SELECT's D5 level. Mirrors run_select_at_level but reads
    // the index key namespace [lo, hi). Returns the raw index KV entries.
    [[nodiscard]] std::optional<std::string> run_index_scan_at_level(
        const SelectStmt& sel, const Key& lo, const Key& hi,
        std::vector<storage::KeyValue>& kvs) {
        switch (sel.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                q.scan(lo, hi);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::Snapshot: {
                Query<Snapshot> q = snapshot_query(sel.snapshot_version);
                q.scan(lo, hi);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q = bounded_query(sel.max_lag);
                q.scan(lo, hi);
                collect(db_.run(q, /*replica_lag=*/0), kvs);
                return std::nullopt;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q = ryw_query(sel.session);
                q.scan(lo, hi);
                collect(db_.run(q, /*replica_lag=*/0, /*session_last_write=*/tip_), kvs);
                return std::nullopt;
            }
        }
        return std::string("unsupported consistency level");
    }

    // A point-get of one row key at the SELECT's D5 level (the index path's per-PK row
    // fetch). Returns the committed value (incl. a tombstone) or nullopt if absent.
    [[nodiscard]] ReadResult point_get_at_level(const SelectStmt& sel, const Key& key) {
        std::vector<storage::KeyValue> kvs;
        switch (sel.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                q.get(key);
                collect(db_.run(q), kvs);
                break;
            }
            case Level::Snapshot: {
                Query<Snapshot> q = snapshot_query(sel.snapshot_version);
                q.get(key);
                collect(db_.run(q), kvs);
                break;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q = bounded_query(sel.max_lag);
                q.get(key);
                collect(db_.run(q, /*replica_lag=*/0), kvs);
                break;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q = ryw_query(sel.session);
                q.get(key);
                collect(db_.run(q, /*replica_lag=*/0, /*session_last_write=*/tip_), kvs);
                break;
            }
        }
        for (const storage::KeyValue& kv : kvs) {
            if (kv.first == key) {
                return kv.second;
            }
        }
        return std::nullopt;
    }

    // The fast path requires the PK WHERE's literal to match the PK column TYPE (a
    // TEXT literal compared to an INT PK is a general filter, never the encoded
    // range), AND the predicate to be PURELY that PK comparison (no extra AND/OR).
    static bool predicate_is_pure_pk(const SelectStmt& sel, const Table& t) {
        if (sel.where == SelectWhereKind::Eq) {
            if (sel.eq_value.type != t.pk().type) {
                return false;
            }
        } else if (sel.where == SelectWhereKind::Between) {
            if (sel.lo_value.type != t.pk().type ||
                sel.hi_value.type != t.pk().type) {
                return false;
            }
        } else {
            return false;
        }
        // The whole filter must be EXACTLY the recognized PK shape: a single Cmp
        // (Eq) or a top-level And of two Cmps (Between). The parser only sets the
        // fast-path fields in those shapes, so the filter's node count tells us
        // whether anything ELSE was AND/OR'd on.
        const Predicate& p = sel.filter;
        if (!p.present()) {
            return false;
        }
        if (sel.where == SelectWhereKind::Eq) {
            return p.nodes.size() == 1;
        }
        return p.nodes.size() == 3;  // exactly Cmp,Cmp,And from the BETWEEN sugar
    }

    // Flatten a QueryResult (a point and/or a range) into KV pairs. A point read
    // yields one KV iff present; a scan yields its rows (already key-ascending).
    // Takes the QueryResult BY RVALUE (every caller passes a db_.run(...) temporary) so
    // the range rows are MOVED into kvs, not copied — db_.run already moved the storage
    // scan vector into rr.rows, so this keeps the whole read path move-only (no per-row
    // string copy from the scan result into the SQL layer).
    static void collect(QueryResult&& qr, std::vector<storage::KeyValue>& kvs) {
        for (PointResult& p : qr.points) {
            if (p.value.has_value()) {
                kvs.emplace_back(std::move(p.key), std::move(*p.value));
            }
        }
        for (RangeResult& rr : qr.ranges) {
            if (kvs.empty()) {
                kvs = std::move(rr.rows);  // the common single-range case: steal the vector
            } else {
                kvs.reserve(kvs.size() + rr.rows.size());
                for (storage::KeyValue& kv : rr.rows) {
                    kvs.push_back(std::move(kv));
                }
            }
        }
    }

    // ========================================================================
    // v2 PIPELINE HELPERS — predicate eval, aggregation, distinct/order/limit.
    // ========================================================================

    // One GROUP: the member rows (pointers into the post-WHERE row vector, which
    // OUTLIVES the grouping) + the group-key datums (constant across the group).
    struct Group {
        std::vector<const std::vector<Datum>*> rows;
        std::vector<Datum> key_datums;
    };

    // A stable group-key field encoding (type tag + bytes) so the ordered map keys
    // groups deterministically and INT/TEXT never collide.
    static std::string group_key_field(const Datum& d) {
        std::string out;
        out.push_back(static_cast<char>(d.type));
        // A NULL group key is DISTINCT from any present value (all NULLs of a column
        // group together; a hash-join NULL key never matches, handled before this).
        out.push_back(d.is_null ? '\x01' : '\x00');
        if (d.is_null) {
            return out;
        }
        if (d.type == Type::Int) {
            const std::uint64_t bits =
                static_cast<std::uint64_t>(d.i) ^ 0x8000000000000000ULL;
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((bits >> shift) & 0xFF));
            }
        } else {
            out += d.s;
        }
        return out;
    }

    static const std::vector<Datum>& dummy_row() {
        static const std::vector<Datum> kEmpty;
        return kEmpty;
    }

    // The grouped-column value for output: a grouped column's value is constant
    // across the group, so read it off any member row (or key_datums by position).
    static Datum grouped_column_value(const std::vector<std::size_t>& gcols,
                                      const Group& grp, std::size_t col_idx) {
        for (std::size_t k = 0; k < gcols.size(); ++k) {
            if (gcols[k] == col_idx) {
                return grp.key_datums[k];
            }
        }
        // Unreachable (validated to be a grouped column), but be total.
        return grp.rows.empty() ? Datum{} : (*grp.rows.front())[col_idx];
    }

    // Compare two same-type datums, returning -1/0/1. (Cross-type never reaches here:
    // a column has one type; a literal is type-checked against it before compare.)
    // NULL ORDERING (ORDER BY over a LEFT-join result): a NULL sorts FIRST (before any
    // present value) ascending; two NULLs are equal. Predicate eval never reaches here
    // with a NULL (it short-circuits NULL comparisons to false), so this only affects
    // the deterministic ORDER BY / tie-break of joined output.
    // K1: compare a SoA column cell against a literal == cmp_datum(cc.at(r), lit), but WITHOUT
    // constructing a Datum (no per-row string copy for a TEXT / 128-bit column — the hot filter
    // path). Filter columns are never arrays (try_extract_conjuncts rejects logical 7), so a TEXT
    // compare is a plain lexicographic byte compare, exactly like cmp_datum's non-array branch.
    static int cmp_cell(const ColumnChunk& cc, std::uint32_t r, const Datum& lit) {
        if (cc.nulls[r] || lit.is_null) {
            if (cc.nulls[r] && lit.is_null) return 0;
            return cc.nulls[r] ? -1 : 1;  // NULLs first (matches cmp_datum)
        }
        if (cc.type == Type::Int) {
            const std::int64_t x = cc.ints[r];
            return x < lit.i ? -1 : (x > lit.i ? 1 : 0);
        }
        const std::string& s = cc.texts[r];
        return s < lit.s ? -1 : (s > lit.s ? 1 : 0);
    }

    static int cmp_datum(const Datum& a, const Datum& b) {
        if (a.is_null || b.is_null) {
            if (a.is_null && b.is_null) return 0;
            return a.is_null ? -1 : 1;  // NULL < any present value (NULLs first)
        }
        // F12: arrays compare ELEMENT-WISE (then shorter < longer), like Postgres — not a raw byte
        // compare (the count-prefixed payload wouldn't order correctly).
        if (a.logical == 7 && b.logical == 7) {
            const std::vector<Datum> ea = Datum::decode_array(a.s), eb = Datum::decode_array(b.s);
            for (std::size_t k = 0; k < ea.size() && k < eb.size(); ++k) {
                const int c = cmp_datum(ea[k], eb[k]);
                if (c != 0) return c;
            }
            if (ea.size() < eb.size()) return -1;
            if (ea.size() > eb.size()) return 1;
            return 0;
        }
        // F14: REAL (logical=14) — decode the 8-byte doubles and compare numerically (a raw byte
        // compare of the bit pattern mis-orders negatives). NaN sorts greatest (real_cmp).
        if (a.logical == 14 && b.logical == 14) {
            return Datum::real_cmp(Datum::decode_double(a.s), Datum::decode_double(b.s));
        }
        if (a.type == Type::Int) {
            if (a.i < b.i) return -1;
            if (a.i > b.i) return 1;
            return 0;
        }
        if (a.s < b.s) return -1;
        if (a.s > b.s) return 1;
        return 0;
    }

    static bool apply_cmp(CmpOp op, int c) {
        switch (op) {
            case CmpOp::Eq: return c == 0;
            case CmpOp::Ne: return c != 0;
            case CmpOp::Lt: return c < 0;
            case CmpOp::Le: return c <= 0;
            case CmpOp::Gt: return c > 0;
            case CmpOp::Ge: return c >= 0;
            case CmpOp::Like: return false;  // never reached: leaf_truth handles Like before apply_cmp
            case CmpOp::Contains: return false;  // never reached: leaf_truth handles @> before apply_cmp
        }
        return false;
    }

    // B1: SQL LIKE pattern match. `%` matches any run of chars (including empty), `_` matches exactly
    // one char; every other char is literal (no ESCAPE clause — out of scope). Iterative greedy
    // backtracking, O(|s|*|pat|) worst case. Pure function of the two strings.
    static bool like_match(const std::string& s, const std::string& pat) {
        std::size_t si = 0, pi = 0, star = std::string::npos, ss = 0;
        while (si < s.size()) {
            if (pi < pat.size() && (pat[pi] == '_' || pat[pi] == s[si])) {
                ++si;
                ++pi;
            } else if (pi < pat.size() && pat[pi] == '%') {
                star = pi++;
                ss = si;
            } else if (star != std::string::npos) {
                pi = star + 1;
                si = ++ss;
            } else {
                return false;
            }
        }
        while (pi < pat.size() && pat[pi] == '%') {
            ++pi;
        }
        return pi == pat.size();
    }

    // JSON containment (`@>`): does `doc` deeply contain `sub`? Objects: every key of `sub` is present
    // in `doc` with a contained value. Arrays: every element of `sub` is contained by some element of
    // `doc`; a scalar/object `sub` is contained by an array `doc` if some element contains it. Scalars
    // match when canonically equal. Recursive, pure function (mirrors PostgreSQL jsonb `@>`).
    static bool json_contained(const json::JVal& doc, const json::JVal& sub) {
        if (doc.kind == json::JVal::Obj && sub.kind == json::JVal::Obj) {
            for (const auto& [k, sv] : sub.obj) {
                const json::JVal* dv = nullptr;
                for (const auto& en : doc.obj)
                    if (en.key == k) { dv = &en.val; break; }
                if (dv == nullptr || !json_contained(*dv, sv)) return false;
            }
            return true;
        }
        if (doc.kind == json::JVal::Arr && sub.kind == json::JVal::Arr) {
            for (const json::JVal& se : sub.arr) {
                bool found = false;
                for (const json::JVal& de : doc.arr)
                    if (json_contained(de, se)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
        if (doc.kind == json::JVal::Arr) {  // a scalar / object contained in some array element
            for (const json::JVal& de : doc.arr)
                if (json_contained(de, sub)) return true;
            return false;
        }
        if (doc.kind != sub.kind) return false;  // scalars: canonical-equality
        std::string a, b;
        json::serialize(doc, a);
        json::serialize(sub, b);
        return a == b;
    }
    // Render one column value as a JSON value (for JSON_AGG). NULL => null; a JSON value embeds as-is;
    // plain INT / DECIMAL / 128-bit render as JSON numbers; everything else (TEXT/DATE/TIME/UUID/ENUM/
    // ARRAY) renders as a JSON string. The result is fed through parse+serialize for canonical form.
    static std::string datum_to_json(const Datum& d) {
        if (d.is_null) return "null";
        if (d.logical == 11) return d.s;  // already canonical JSON
        if (d.type == Type::Int && d.logical == 0) return std::to_string(d.i);
        if (d.logical == 1 || d.logical == 5 || d.logical == 6) return d.render();  // DECIMAL / 128-bit number
        std::string out;
        json::escape_into(d.render(), out);  // a JSON string (quoted + escaped)
        return out;
    }
    static bool json_contains(const std::string& doc, const std::string& sub) {
        json::JVal d, s;
        std::size_t pd = 0, ps = 0;
        if (!json::parse_value(doc, pd, d) || !json::parse_value(sub, ps, s)) return false;
        return json_contained(d, s);
    }

    // Evaluate ONE comparison leaf (lhs OP rhs) to a bool — the single place LIKE / @> diverge from the
    // 3-way ordered comparators. Both operands are already NULL-checked + type-equal by the caller.
    static bool leaf_truth(CmpOp op, const Datum& lhs, const Datum& rhs) {
        if (op == CmpOp::Like) {
            return lhs.type == Type::Text && rhs.type == Type::Text && like_match(lhs.s, rhs.s);
        }
        if (op == CmpOp::Contains) {  // @> JSON containment (both operands are JSON text)
            return lhs.type == Type::Text && rhs.type == Type::Text && json_contains(lhs.s, rhs.s);
        }
        return apply_cmp(op, cmp_datum(lhs, rhs));
    }

    // Try to flatten the WHERE predicate into a pure conjunction of `not_null_col <op>
    // literal` terms. Returns false (→ interpreter fallback) for ANYTHING else: OR / NOT /
    // IS NULL / IN / EXISTS / a scalar-subquery RHS / a column-vs-column RHS / a qualified
    // column / a nullable column (3VL) / a type-mismatched or NULL literal. The conservative
    // conditions guarantee the flat apply is BYTE-IDENTICAL to eval_pred.
    bool try_extract_conjuncts(const Predicate& f, std::int32_t idx, const Table& t,
                               std::vector<VecTerm>& out) const {
        if (idx < 0 || static_cast<std::size_t>(idx) >= f.nodes.size()) {
            return false;
        }
        const PredNode& n = f.nodes[static_cast<std::size_t>(idx)];
        if (n.kind == PredNodeKind::And) {
            return try_extract_conjuncts(f, n.left, t, out) &&
                   try_extract_conjuncts(f, n.right, t, out);
        }
        if (n.kind != PredNodeKind::Cmp || n.operand != OperandKind::Column ||
            n.rhs_is_column || n.rhs_is_subquery || !n.qualifier.empty() ||
            n.literal.is_null || n.op == CmpOp::Like ||
            n.op == CmpOp::Contains) {  // B1: LIKE / @> are not zone/mask-vectorizable
            return false;
        }
        const auto ci = t.column_index(n.column);
        if (!ci || t.columns[*ci].nullable) {
            return false;
        }
        if (t.columns[*ci].logical == 7) {  // F12: arrays compare element-wise — not zone/byte vectorizable
            return false;
        }
        // F9b/F9c: a literal compared against a DECIMAL/DATE/TIMESTAMP/UUID column must be coerced to
        // the column's stored representation (scaled int / days / secs / canonical string) BEFORE the
        // zone/mask compares against raw stored values. A coercion failure falls back to eval_pred
        // (which reports the error).
        Datum lit = n.literal;
        if (t.columns[*ci].logical != 0) {
            Datum cv;
            if (coerce(t.columns[*ci], lit, cv, /*for_write=*/false)) return false;
            lit = cv;
        }
        if (lit.type != t.columns[*ci].type) {
            return false;
        }
        out.push_back(VecTerm{*ci, n.op, lit});
        return true;
    }

    // ========================================================================
    // v4: SUBQUERY EVALUATION. A subquery is LOWERED by running its inner SELECT through
    // the SAME exec_select pipeline (no new query surface) and applying the predicate to
    // its result. UNCORRELATED ONLY (FLAG): the inner SELECT does NOT see the outer row;
    // it is evaluated ONCE and its result reused for every outer row (correct + cheap for
    // an uncorrelated subquery; a correlated subquery referencing an outer column would
    // resolve that column as unknown => a clean error, never a wrong answer). The inner
    // SELECT reads the SAME committed store at the SAME D5 level the outer statement runs.
    // ========================================================================

    // B2: flip a comparison operator (for swapping LHS/RHS during correlation rewriting).
    static CmpOp flip_op(CmpOp op) {
        switch (op) {
            case CmpOp::Lt: return CmpOp::Gt;
            case CmpOp::Le: return CmpOp::Ge;
            case CmpOp::Gt: return CmpOp::Lt;
            case CmpOp::Ge: return CmpOp::Le;
            default: return op;  // Eq/Ne/Like are symmetric (Like rarely correlated)
        }
    }

    // B2 CORRELATED subqueries: produce a copy of the inner SELECT with every reference to an OUTER
    // column (one that is NOT a column of the inner table but IS a column of the outer table)
    // substituted by the OUTER ROW's value as a literal. Called per outer row, so the inner query is
    // re-evaluated against the current outer binding. Handles a correlation appearing on either side
    // of a comparison (`inner.x = outer.y` or `outer.y = inner.x`).
    [[nodiscard]] SelectStmt correlate_subquery(const SelectStmt& sub, const Table& outer_t,
                                                const std::vector<Datum>& outer_row) {
        SelectStmt s = sub;
        const std::string inner_name =
            !s.table.empty() ? s.table : (s.from.empty() ? std::string() : s.from[0].table);
        const Table* inner = catalog_.find(inner_name);
        if (inner == nullptr || !s.filter.present()) {
            return s;
        }
        auto outer_val = [&](const std::string& col) -> std::optional<Datum> {
            if (inner->column_index(col)) return std::nullopt;  // an inner column — not correlated
            const auto oi = outer_t.column_index(col);
            if (!oi) return std::nullopt;
            return outer_row[*oi];
        };
        for (PredNode& n : s.filter.nodes) {
            if (n.kind != PredNodeKind::Cmp) continue;
            if (n.rhs_is_column) {
                if (auto v = outer_val(n.rhs_column)) {  // RHS is an outer column
                    n.literal = *v;
                    n.rhs_is_column = false;
                    continue;
                }
            }
            if (n.operand == OperandKind::Column) {
                if (auto v = outer_val(n.column)) {  // LHS is an outer column
                    if (n.rhs_is_column) {           // ... and the inner column is on the RHS — swap
                        n.column = n.rhs_column;
                        n.rhs_is_column = false;
                        n.literal = *v;
                        n.op = flip_op(n.op);
                    }
                }
            }
        }
        return s;
    }

    // The collected values of an IN/scalar subquery's single output column + whether any
    // were NULL (load-bearing for NOT IN's three-valued logic).
    struct SubColumn {
        std::vector<Datum> values;  // the present (non-NULL) values, in result order
        bool has_null = false;      // a NULL appeared in the subquery's column
    };

    // Run an uncorrelated subquery and extract its SINGLE output column. Errors if the
    // subquery projects more than one column (a scalar/IN subquery is single-column).
    [[nodiscard]] std::optional<std::string> run_sub_column(const SelectStmt& sub,
                                                            SubColumn& out) {
        const ExecResult r = exec_select(sub);
        if (!r.ok) {
            return std::string("subquery error: " + r.error);
        }
        for (const ResultRow& row : r.rows) {
            if (row.cells.size() != 1) {
                return std::string(
                    "subquery must return exactly ONE column (got " +
                    std::to_string(row.cells.size()) + ")");
            }
            const Datum& d = row.cells.front().second;
            if (d.is_null) {
                out.has_null = true;
            } else {
                out.values.push_back(d);
            }
        }
        return std::nullopt;
    }

    // Run a SCALAR subquery: it MUST return exactly one row / one column. >1 row is an
    // ERROR (like real SQL). 0 rows => the scalar is NULL (the outer comparison is then
    // UNKNOWN => false). Fills `is_null` + `value`.
    [[nodiscard]] std::optional<std::string> run_scalar_sub(const SelectStmt& sub,
                                                            bool& is_null, Datum& value) {
        const ExecResult r = exec_select(sub);
        if (!r.ok) {
            return std::string("subquery error: " + r.error);
        }
        if (r.rows.size() > 1) {
            return std::string(
                "scalar subquery returned " + std::to_string(r.rows.size()) +
                " rows (a scalar subquery must return at most one row)");
        }
        if (r.rows.empty()) {
            is_null = true;  // 0 rows => NULL scalar
            return std::nullopt;
        }
        if (r.rows.front().cells.size() != 1) {
            return std::string("scalar subquery must return exactly ONE column");
        }
        const Datum& d = r.rows.front().cells.front().second;
        is_null = d.is_null;
        value = d;
        return std::nullopt;
    }

    // Run an EXISTS subquery: TRUE iff it returns >=1 row (any shape). Never UNKNOWN.
    [[nodiscard]] std::optional<std::string> run_exists_sub(const SelectStmt& sub,
                                                            bool& exists) {
        const ExecResult r = exec_select(sub);
        if (!r.ok) {
            return std::string("subquery error: " + r.error);
        }
        exists = !r.rows.empty();
        return std::nullopt;
    }

    // Apply the IN / NOT IN membership test under SQL three-valued logic, given the
    // probe Datum + the subquery column. Returns the COLLAPSED truth (UNKNOWN => false):
    //   IN:      TRUE iff probe equals some present value; if not, UNKNOWN if a NULL was
    //            present (=> false), else FALSE.
    //   NOT IN:  the negation under three-valued logic: TRUE iff probe equals NO present
    //            value AND no NULL was present; if probe matches a present value => FALSE;
    //            if no match but a NULL was present => UNKNOWN (=> false). (This is the
    //            load-bearing NOT-IN-with-NULL rule the conformance teeth check.)
    // A NULL probe is itself UNKNOWN => false for both IN and NOT IN.
    [[nodiscard]] static std::optional<std::string> apply_in(const Datum& probe,
                                                             bool is_not,
                                                             const SubColumn& sub,
                                                             bool& truth) {
        if (probe.is_null) {
            truth = false;  // NULL IN/NOT IN anything is UNKNOWN
            return std::nullopt;
        }
        bool matched = false;
        for (const Datum& v : sub.values) {
            if (v.type != probe.type) {
                return std::string(
                    "type mismatch in IN: probe is " + std::string(type_name(probe.type)) +
                    ", subquery column is " + type_name(v.type));
            }
            if (cmp_datum(v, probe) == 0) {
                matched = true;
                break;
            }
        }
        if (!is_not) {
            // IN: present-match => true; else UNKNOWN-if-null (false) else false.
            truth = matched;  // (no-match + null => still false, same collapsed value)
            return std::nullopt;
        }
        // NOT IN: a present match => false; no match + a NULL => UNKNOWN => false;
        // no match + no NULL => true.
        truth = (!matched && !sub.has_null);
        return std::nullopt;
    }

    // Evaluate a predicate node into `truth`. `group` is non-null for HAVING (so an
    // Agg operand resolves to the group's aggregate); for a WHERE row filter it is
    // null and `row` is the candidate row. Returns an error string on a type
    // mismatch / unknown column / aggregate misuse (fail-closed, never UB).
    [[nodiscard]] std::optional<std::string> eval_pred(
        const Predicate& p, std::int32_t node, const Table& t,
        const std::vector<Datum>& row, const Group* group, bool& truth) {
        if (node < 0) {
            truth = true;  // absent predicate == always true
            return std::nullopt;
        }
        const PredNode& n = p.nodes[static_cast<std::size_t>(node)];
        switch (n.kind) {
            case PredNodeKind::And: {
                bool l = false;
                if (auto e = eval_pred(p, n.left, t, row, group, l)) return e;
                if (!l) { truth = false; return std::nullopt; }
                bool r = false;
                if (auto e = eval_pred(p, n.right, t, row, group, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Or: {
                bool l = false;
                if (auto e = eval_pred(p, n.left, t, row, group, l)) return e;
                if (l) { truth = true; return std::nullopt; }
                bool r = false;
                if (auto e = eval_pred(p, n.right, t, row, group, r)) return e;
                truth = r;
                return std::nullopt;
            }
            case PredNodeKind::Not: {
                bool c = false;
                if (auto e = eval_pred(p, n.left, t, row, group, c)) return e;
                truth = !c;
                return std::nullopt;
            }
            case PredNodeKind::IsNull: {
                // v4: <col> IS [NOT] NULL — the only predicate ever TRUE for a NULL.
                // Valid in WHERE (group==null, `row` is the candidate); not in HAVING.
                if (group != nullptr) {
                    return std::string("IS NULL is not supported in HAVING");
                }
                bool null = false;
                if (n.operand == OperandKind::Expr) {  // J1: <expr> IS [NOT] NULL
                    if (!n.expr) return std::string("empty expression operand in IS NULL");
                    Datum v;
                    if (auto e = eval_expr(*n.expr, t, row, v)) return e;
                    null = v.is_null;
                } else {
                    const auto idx = t.column_index(n.column);
                    if (!idx) {
                        return std::string("unknown column '" + n.column + "' in table '" +
                                           t.name + "'");
                    }
                    null = row[*idx].is_null;
                }
                truth = n.is_not ? !null : null;
                return std::nullopt;
            }
            case PredNodeKind::InList: {
                // v4: <col> [NOT] IN (SELECT ...) — uncorrelated subquery membership.
                if (group != nullptr) {
                    return std::string("IN subqueries are not supported in HAVING");
                }
                const auto idx = t.column_index(n.column);
                if (!idx) {
                    return std::string("unknown column '" + n.column + "' in table '" +
                                       t.name + "'");
                }
                SubColumn sub;
                const SelectStmt cs = correlate_subquery(*n.subquery, t, row);  // B2
                if (auto e = run_sub_column(cs, sub)) return e;
                return apply_in(row[*idx], n.is_not, sub, truth);
            }
            case PredNodeKind::Exists: {
                // v4/B2: [NOT] EXISTS (SELECT ...) — existence test (correlated per outer row).
                if (group != nullptr) {
                    return std::string("EXISTS subqueries are not supported in HAVING");
                }
                bool ex = false;
                const SelectStmt cs = correlate_subquery(*n.subquery, t, row);  // B2
                if (auto e = run_exists_sub(cs, ex)) return e;
                truth = n.is_not ? !ex : ex;
                return std::nullopt;
            }
            case PredNodeKind::Cmp:
                break;
        }
        // A comparison leaf: resolve the left operand to a Datum.
        Datum lhs;
        if (n.operand == OperandKind::Agg) {
            if (group == nullptr) {
                return std::string("an aggregate may only appear in HAVING / the "
                                   "SELECT list, not in WHERE");
            }
            if (auto e = compute_agg(n.agg, t, *group, lhs)) {
                return e;
            }
        } else if (n.operand == OperandKind::Expr) {
            // J1: a scalar-expression LHS (a+b, doc->>'k', UPPER(x)) evaluated per row.
            if (!n.expr) {
                return std::string("empty expression operand in predicate");
            }
            if (auto e = eval_expr(*n.expr, t, row, lhs)) {
                return e;
            }
        } else {
            const auto idx = t.column_index(n.column);
            if (!idx) {
                return std::string("unknown column '" + n.column + "' in table '" +
                                   t.name + "'");
            }
            lhs = row[*idx];
        }
        // F12: `lhs <op> ANY|ALL (<array>)` — test the op against every element.
        if (n.any_quant || n.all_quant) {
            Datum arr = n.literal;
            if (n.rhs_is_column) {
                const auto ai = t.column_index(n.rhs_column);
                if (!ai) return "unknown column '" + n.rhs_column + "' in ANY/ALL";
                arr = row[*ai];
            }
            if (lhs.is_null || arr.is_null || arr.logical != 7) { truth = false; return std::nullopt; }
            const std::vector<Datum> elems = Datum::decode_array(arr.s);
            bool any = false, all = true;
            for (const Datum& el : elems) {
                const bool m = !el.is_null && apply_cmp(n.op, cmp_datum(lhs, el));
                any = any || m;
                all = all && m;
            }
            truth = n.any_quant ? any : all;  // empty: ANY=false, ALL=true
            return std::nullopt;
        }
        // v4: the RHS may be a SCALAR SUBQUERY (col <op> (SELECT agg)). Resolve it to a
        // Datum (NULL if the subquery returned 0 rows; an error if it returned >1 row).
        Datum rhs;
        if (n.rhs_is_subquery) {
            bool snull = false;
            const SelectStmt cs = correlate_subquery(*n.subquery, t, row);  // B2
            if (auto e = run_scalar_sub(cs, snull, rhs)) return e;
            if (snull) {
                truth = false;  // comparison with a NULL scalar is UNKNOWN
                return std::nullopt;
            }
        } else {
            rhs = n.literal;
        }
        // F9b: a literal compared against a DECIMAL/DATE/TIMESTAMP column is given as a string
        // ('2000-01-01') or whole number; coerce it to the column's INT representation so the
        // comparison is the (deterministic) raw-int compare. (LHS already carries the logical
        // tag from decode.)
        if (n.operand == OperandKind::Column && !n.rhs_is_subquery && !n.rhs_is_column &&
            !rhs.is_null) {
            const auto cidx = t.column_index(n.column);
            if (cidx && t.columns[*cidx].logical != 0) {
                Datum cv;
                if (auto e = coerce(t.columns[*cidx], rhs, cv, /*for_write=*/false)) return e;
                rhs = cv;
            }
        }
        // J1: an expression LHS carrying a logical tag (e.g. ts + INTERVAL, a DECIMAL sum) needs
        // its literal RHS coerced to the same logical representation, mirroring the column path.
        if (n.operand == OperandKind::Expr && !n.rhs_is_subquery && !n.rhs_is_column &&
            !rhs.is_null && lhs.logical != 0) {
            Column tmp;
            tmp.type = lhs.type;
            tmp.logical = lhs.logical;
            tmp.scale = lhs.scale;
            Datum cv;
            if (auto e = coerce(tmp, rhs, cv, /*for_write=*/false)) return e;
            rhs = cv;
        }
        // NULL operand => UNKNOWN => false (three-valued logic collapsed at filter).
        if (lhs.is_null || rhs.is_null) {
            truth = false;
            return std::nullopt;
        }
        if (lhs.type != rhs.type) {
            return std::string("type mismatch in predicate: comparing ") +
                   type_name(lhs.type) + " to " + type_name(rhs.type);
        }
        truth = leaf_truth(n.op, lhs, rhs);
        return std::nullopt;
    }

    // Validate every aggregate's target column exists + has the right type (SUM/AVG
    // require INT; COUNT/MIN/MAX accept any). Errors fail-closed before execution.
    [[nodiscard]] std::optional<std::string> validate_aggs(const SelectStmt& sel,
                                                           const Table& t) {
        // Check items + HAVING aggregates.
        for (const SelectItem& item : sel.items) {
            if (item.kind == SelectItemKind::Aggregate) {
                if (auto e = validate_one_agg(item.agg, t)) return e;
            }
        }
        for (const PredNode& n : sel.having.nodes) {
            if (n.kind == PredNodeKind::Cmp && n.operand == OperandKind::Agg) {
                if (auto e = validate_one_agg(n.agg, t)) return e;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> validate_one_agg(const AggExpr& a,
                                                              const Table& t) {
        if (a.kind == AggKind::CountStar) {
            return std::nullopt;
        }
        const auto idx = t.column_index(a.column);
        if (!idx) {
            return std::string("unknown column '" + a.column +
                               "' in aggregate over table '" + t.name + "'");
        }
        // F9f: SUM/AVG over INT, or over a 128-bit column (INT128/DECIMAL128, logical 5/6). A plain
        // TEXT / UUID column is still an error.
        if ((a.kind == AggKind::Sum || a.kind == AggKind::Avg) &&
            t.columns[*idx].type != Type::Int && t.columns[*idx].logical < 5) {
            return std::string("SUM/AVG requires a numeric column (got TEXT column '" +
                               a.column + "')");
        }
        return std::nullopt;
    }

    // Compute one aggregate over a group. AVG(INT) truncates toward zero. MIN/MAX
    // work for INT (numeric) + TEXT (lexicographic).
    //
    // v4 NULL SEMANTICS (mirrors compute_agg_joined exactly): COUNT(*) counts EVERY row
    // (incl. NULL-valued ones). COUNT(col) / SUM / MIN / MAX / AVG SKIP NULLs (aggregate
    // over the PRESENT values only). A group with NO present value for the aggregated
    // column yields COUNT=0, SUM=0, and MIN/MAX/AVG = typed NULL. The synthetic
    // ungrouped-over-empty group yields COUNT=0, SUM=0, MIN/MAX/AVG NULL.
    [[nodiscard]] std::optional<std::string> compute_agg(const AggExpr& a,
                                                         const Table& t,
                                                         const Group& grp, Datum& out) {
        // FILTER (WHERE ...): keep only the rows that pass, then aggregate them without the filter.
        if (a.filter && a.filter->present()) {
            Group fg;
            for (const auto* rp : grp.rows) {
                bool truth = false;
                if (auto e = eval_pred(*a.filter, a.filter->root, t, *rp, /*group=*/nullptr, truth))
                    return e;
                if (truth) fg.rows.push_back(rp);
            }
            AggExpr nf = a;
            nf.filter.reset();
            return compute_agg(nf, t, fg, out);
        }
        if (a.kind == AggKind::CountStar) {
            out = Datum::make_int(static_cast<std::int64_t>(grp.rows.size()));
            return std::nullopt;
        }
        const auto idx = t.column_index(a.column);
        if (!idx) {
            return std::string("unknown column '" + a.column + "' in aggregate");
        }
        const std::size_t ci = *idx;
        const Type ty = t.columns[ci].type;
        // F12: ARRAY_AGG — collect the group's values (in scan order, INCLUDING NULLs, Postgres
        // semantics) into an array whose element type is the column's.
        if (a.kind == AggKind::ArrayAgg) {
            std::vector<Datum> elems;
            elems.reserve(grp.rows.size());
            for (const auto* rp : grp.rows) elems.push_back((*rp)[ci]);
            out = Datum::make_text(
                Datum::encode_array(t.columns[ci].logical, t.columns[ci].scale, elems));
            out.logical = 7;
            return std::nullopt;
        }
        // JSON_AGG — collect the group's values (scan order, NULLs included as JSON null) into a JSON
        // array. Built then re-parsed + canonically re-serialized so it byte-matches a stored JSON value.
        if (a.kind == AggKind::JsonAgg) {
            std::string js = "[";
            for (std::size_t i = 0; i < grp.rows.size(); ++i) {
                if (i != 0) js += ",";
                js += datum_to_json((*grp.rows[i])[ci]);
            }
            js += "]";
            json::JVal v;
            std::size_t jp = 0;
            if (!json::parse_value(js, jp, v)) return std::string("JSON_AGG produced invalid JSON");
            std::string canon;
            json::serialize(v, canon);
            out = Datum::make_text(std::move(canon));
            out.logical = 11;  // JSON
            return std::nullopt;
        }
        // STRING_AGG / GROUP_CONCAT — join the group's non-NULL values (scan order) with the
        // separator. Each value is rendered like a projected cell. NULLs are skipped (PG);
        // an all-NULL/empty group yields SQL NULL (like the other value-collecting aggregates).
        if (a.kind == AggKind::StringAgg) {
            std::string s;
            bool any = false;
            for (const auto* rp : grp.rows) {
                const Datum& d = (*rp)[ci];
                if (d.is_null) continue;
                if (any) s += a.delim;
                s += (d.type == Type::Text ? d.s : d.render());
                any = true;
            }
            out = any ? Datum::make_text(std::move(s)) : Datum::make_null(Type::Text);
            return std::nullopt;
        }
        // Collect the PRESENT (non-NULL) values of the aggregated column.
        std::vector<const Datum*> present;
        for (const auto* rp : grp.rows) {
            const Datum& d = (*rp)[ci];
            if (!d.is_null) {
                present.push_back(&d);
            }
        }
        // BOOL_AND / BOOL_OR / BIT_AND / BIT_OR — fold the non-NULL INT values (0 = false);
        // an all-NULL / empty group yields SQL NULL (PostgreSQL semantics).
        if (a.kind == AggKind::BoolAnd || a.kind == AggKind::BoolOr ||
            a.kind == AggKind::BitAnd || a.kind == AggKind::BitOr) {
            if (ty != Type::Int) return std::string("BOOL_*/BIT_* aggregate requires an INT column");
            if (present.empty()) { out = Datum::make_null(Type::Int); return std::nullopt; }
            std::int64_t acc = (a.kind == AggKind::BitAnd || a.kind == AggKind::BoolAnd) ? ~0LL : 0LL;
            bool ba = true, bo = false;
            for (const Datum* p : present) {
                acc = (a.kind == AggKind::BitAnd) ? (acc & p->i)
                    : (a.kind == AggKind::BitOr)  ? (acc | p->i)
                                                  : acc;
                if (p->i == 0) ba = false; else bo = true;
            }
            if (a.kind == AggKind::BoolAnd) out = Datum::make_int(ba ? 1 : 0);
            else if (a.kind == AggKind::BoolOr) out = Datum::make_int(bo ? 1 : 0);
            else out = Datum::make_int(acc);
            return std::nullopt;
        }
        // C1: COUNT/SUM/AVG(DISTINCT col) — keep only the FIRST occurrence of each distinct value
        // (deterministic: dedup by the order-preserving value encoding). COUNT(DISTINCT) then counts
        // distinct values, SUM/AVG aggregate over them. (NULLs are already excluded; MIN/MAX never
        // carry DISTINCT — the parser only allows it for COUNT/SUM/AVG.)
        if (a.distinct) {
            std::vector<const Datum*> uniq;
            std::set<std::string> seen;
            for (const Datum* d : present) {
                if (seen.insert(group_key_field(*d)).second) {
                    uniq.push_back(d);
                }
            }
            present.swap(uniq);
        }
        if (a.kind == AggKind::Count) {
            out = Datum::make_int(static_cast<std::int64_t>(present.size()));
            return std::nullopt;
        }
        if (grp.rows.empty()) {
            // The SYNTHETIC ungrouped-over-empty group (zero member rows): SUM=0 and
            // MIN/MAX/AVG render as 0 — the conventional empty-aggregate rendering we
            // pinned pre-v4 (kept byte-stable so the existing aggregates gate holds).
            out = Datum::make_int(0);
            return std::nullopt;
        }
        if (present.empty()) {
            // A NON-empty group whose aggregated column is ALL NULL: SUM=0; MIN/MAX/AVG
            // = typed NULL (the SQL three-valued result for an all-NULL aggregate).
            if (a.kind == AggKind::Sum)
                out = (t.columns[ci].logical == 14) ? Datum::make_real(0.0) : Datum::make_int(0);  // F14
            else
                out = Datum::make_null(ty);
            return std::nullopt;
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            Datum best = *present.front();
            for (const Datum* d : present) {
                const int c = cmp_datum(*d, best);
                if ((a.kind == AggKind::Min && c < 0) ||
                    (a.kind == AggKind::Max && c > 0)) {
                    best = *d;
                }
            }
            out = best;
            return std::nullopt;
        }
        // SUM / AVG over a UINT256 column — accumulate in u256 (checked); AVG truncates toward zero.
        if (t.columns[ci].logical == 13) {
            u256 acc{};
            for (const Datum* d : present) {
                u256 nx;
                if (u256_add(acc, u256_decode(d->s), nx)) return "UINT256 overflow in SUM";
                acc = nx;
            }
            if (a.kind == AggKind::Avg) {
                u256 q, rem;
                (void)u256_divmod(acc, u256_from_u64(present.size()), q, rem);
                acc = q;
            }
            out = Datum::make_text(u256_encode(acc));
            out.logical = 13;
            return std::nullopt;
        }
        // F14: SUM / AVG over a REAL column — fold in IEEE-754 double (MIN/MAX handled above via
        // cmp_datum). AVG divides by the present (non-NULL) count, like every other numeric type.
        if (t.columns[ci].logical == 14) {
            double acc = 0.0;
            for (const Datum* d : present) acc += d->real_value();
            if (a.kind == AggKind::Avg) acc /= static_cast<double>(present.size());  // present non-empty
            out = Datum::make_real(acc);
            return std::nullopt;
        }
        // F9f: SUM / AVG over a 128-bit column (INT128/DECIMAL128) — accumulate in __int128 (checked).
        // AVG truncates toward zero. The result keeps the column's logical/scale tag.
        if (t.columns[ci].logical == 5 || t.columns[ci].logical == 6) {
            __int128 acc = 0;
            for (const Datum* d : present) {
                if (__builtin_add_overflow(acc, Datum::decode_i128(d->s), &acc))
                    return "integer overflow in SUM";
            }
            if (a.kind == AggKind::Avg) acc /= static_cast<__int128>(present.size());  // present non-empty
            out = Datum::make_text(Datum::encode_i128(acc));
            out.logical = t.columns[ci].logical;
            out.scale = t.columns[ci].scale;
            return std::nullopt;
        }
        // SUM / AVG over INT (validated INT in validate_one_agg). F9d: checked accumulation.
        std::int64_t sum = 0;
        for (const Datum* d : present) {
            if (add_ovf(sum, d->i, sum)) return "integer overflow in SUM";
        }
        if (a.kind == AggKind::Sum) {
            out = Datum::make_int(sum);
            tag_decimal(t.columns[ci], out);  // F9b: SUM keeps the column's DECIMAL scale
            return std::nullopt;
        }
        // AVG: integer truncation toward zero (C++ / divides truncates toward zero).
        const std::int64_t n = static_cast<std::int64_t>(present.size());
        out = Datum::make_int(n == 0 ? 0 : sum / n);
        tag_decimal(t.columns[ci], out);  // F9b
        return std::nullopt;
    }

    // F9b: stamp a fresh aggregate result with the source column's DECIMAL scale (DATE/TIMESTAMP
    // SUM/AVG are nonsensical so they are left raw; MIN/MAX already carry the row's tag verbatim).
    static void tag_decimal(const Column& col, Datum& d) {
        if (col.logical == 1 && d.type == Type::Int && !d.is_null) {
            d.logical = 1;
            d.scale = col.scale;
        }
    }

    // (5) DISTINCT — drop duplicate OUTPUT rows, keeping the first occurrence. Stable
    // (preserves the pre-distinct order). A row's identity is its rendered cells.
    static void apply_distinct(const SelectStmt& sel, std::vector<ResultRow>& rows) {
        if (!sel.distinct) {
            return;
        }
        std::vector<ResultRow> kept;
        std::map<std::string, bool> seen;
        for (auto& row : rows) {
            std::string sig;
            for (const auto& [label, d] : row.cells) {
                sig.push_back(static_cast<char>(d.type));
                sig.push_back(d.is_null ? '\x01' : '\x00');  // NULL distinct from data
                sig += d.render();
                sig.push_back('\x1f');  // a field separator unlikely in data
            }
            if (seen.emplace(sig, true).second) {
                kept.push_back(std::move(row));
            }
        }
        rows = std::move(kept);
    }

    // (6) ORDER BY for the NON-aggregate path: keys reference table columns. We
    // tie-break by the PK so the order is TOTAL + byte-deterministic (V-RKV1). The
    // PK is always in the output? Not necessarily — so we look the key up by the
    // output LABEL first, else by the underlying table column on each row via a
    // stable index map. Here every row carries its full projection labels.
    [[nodiscard]] std::optional<std::string> apply_order_by(
        const SelectStmt& sel, const Table& t, std::vector<ResultRow>& rows) {
        if (sel.order_by.empty()) {
            return std::nullopt;
        }
        // Resolve each ORDER BY column to an output-cell label. The key MUST be a
        // projected/output column (we order over the SELECT output, like SQL when a
        // bare column name matches the output). For a non-aggregate SELECT the cells
        // are labelled by column name, so a table column is found by its name.
        for (const OrderKey& k : sel.order_by) {
            if (k.expr) continue;  // K1.2: expression key — sorted via its hidden cell
            if (k.position == 0 && !has_label(rows, k.column) && !t.column_index(k.column)) {
                return std::string("unknown ORDER BY column '" + k.column + "'");
            }
        }
        // Stable sort with a total comparator (ORDER BY keys, then PK tie-break).
        const std::string pk_label = t.pk().name;
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ResultRow& x, const ResultRow& y) {
                             for (std::size_t ki = 0; ki < sel.order_by.size(); ++ki) {
                                 const OrderKey& k = sel.order_by[ki];
                                 int dir = 0;
                                 if (k.expr) {  // K1.2: read the hidden expression cell
                                     if (order_key_less_label(x, y, ob_label(ki), k, dir)) return true;
                                 } else if (order_key_less(x, y, k, dir)) {
                                     return true;
                                 }
                                 if (dir != 0) return false;
                             }
                             // Tie-break by PK (ascending) for a TOTAL order.
                             const int c = cmp_by_label(x, y, pk_label);
                             return c < 0;
                         });
        return std::nullopt;
    }

    // ORDER BY for the AGGREGATE path: keys reference output labels (grouped columns
    // or aggregate labels like "COUNT(*)"). Tie-break by the full rendered row so the
    // order is total + deterministic.
    [[nodiscard]] std::optional<std::string> apply_order_by_labels(
        const SelectStmt& sel, std::vector<ResultRow>& rows) {
        if (sel.order_by.empty()) {
            return std::nullopt;
        }
        for (const OrderKey& k : sel.order_by) {
            if (k.expr) {  // K1.2: only the single-table scalar SELECT path evaluates these
                return std::string("ORDER BY expression is not supported in this context");
            }
            if (k.position == 0 && !has_label(rows, k.column) && !has_agg_label(sel, k.column)) {
                return std::string("ORDER BY '" + k.column +
                                   "' must reference a grouped column or an aggregate "
                                   "in the SELECT list");
            }
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ResultRow& x, const ResultRow& y) {
                             for (const OrderKey& k : sel.order_by) {
                                 int dir = 0;
                                 if (order_key_less(x, y, k, dir)) return true;
                                 if (dir != 0) return false;  // strictly greater on this key
                             }
                             return render_row(x) < render_row(y);  // total tie-break
                         });
        return std::nullopt;
    }

    // G3: compare two rows on ONE ORDER BY key, honoring NULLS FIRST/LAST. Returns true iff x<y on
    // this key; sets `dir` to 0 only when the key is a TIE (continue to the next key). A NULL is
    // placed FIRST/LAST per k.nulls; Default = NULL is the smallest value (FIRST under ASC, LAST
    // under DESC) — byte-identical to the pre-G3 cmp_datum behavior.
    static bool order_key_less(const ResultRow& x, const ResultRow& y, const OrderKey& k, int& dir) {
        if (k.position > 0) {  // G4: ORDER BY <n> — the n-th output column
            const std::size_t i = static_cast<std::size_t>(k.position - 1);
            const std::string lbl = i < x.cells.size() ? x.cells[i].first : std::string();
            return order_key_less_label(x, y, lbl, k, dir);
        }
        return order_key_less_label(x, y, k.column, k, dir);
    }
    // As above but the cell is looked up by an explicit `label` (the joined path resolves a key to
    // a qualified output label like "a.x"); direction + NULLS come from `k`.
    static bool order_key_less_label(const ResultRow& x, const ResultRow& y,
                                     const std::string& label, const OrderKey& k, int& dir) {
        const Datum* dx = cell(x, label);
        const Datum* dy = cell(y, label);
        if (dx == nullptr || dy == nullptr) {
            dir = 0;
            return false;  // label missing on this row — treat as a tie (pre-G3 cmp_by_label==0)
        }
        const bool xn = dx->is_null, yn = dy->is_null;
        if (xn || yn) {
            if (xn && yn) {
                dir = 0;
                return false;
            }
            bool nulls_first;
            if (k.nulls == NullsOrder::First) {
                nulls_first = true;
            } else if (k.nulls == NullsOrder::Last) {
                nulls_first = false;
            } else {
                nulls_first = !k.descending;  // Default: matches cmp_datum (NULL smallest) + dir flip
            }
            const bool x_first = xn;  // the NULL side
            dir = 1;                  // not a tie
            return nulls_first ? x_first : !x_first;
        }
        const int c = cmp_datum(*dx, *dy);
        dir = c;
        if (c == 0) return false;
        return k.descending ? (c > 0) : (c < 0);
    }

    // ORDER BY for the JOINED AGGREGATE path: an ORDER BY key may be spelled QUALIFIED
    // (a.col) while the output label is the qualified spelling, OR bare. Resolve each
    // key to the label the SELECT list used (qualified spelling first, then bare, then
    // an aggregate label like COUNT(*)). Tie-break by the full rendered row.
    [[nodiscard]] std::optional<std::string> apply_order_by_labels_joined(
        const SelectStmt& sel, std::vector<ResultRow>& rows) {
        if (sel.order_by.empty()) {
            return std::nullopt;
        }
        std::vector<std::string> keys;
        keys.reserve(sel.order_by.size());
        for (const OrderKey& k : sel.order_by) {
            if (k.expr) {  // K1.2: only the single-table scalar SELECT path evaluates these
                return std::string("ORDER BY expression is not supported in this context");
            }
            const std::string spelling =
                k.qualifier.empty() ? k.column : k.qualifier + "." + k.column;
            if (has_label(rows, spelling) || has_agg_label(sel, spelling)) {
                keys.push_back(spelling);
            } else if (has_label(rows, k.column) || has_agg_label(sel, k.column)) {
                keys.push_back(k.column);
            } else {
                return std::string("ORDER BY '" + spelling +
                                   "' must reference a grouped column or an aggregate "
                                   "in the SELECT list");
            }
        }
        const std::vector<OrderKey>& ob = sel.order_by;
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ResultRow& x, const ResultRow& y) {
                             for (std::size_t i = 0; i < ob.size(); ++i) {
                                 int dir = 0;
                                 if (order_key_less_label(x, y, keys[i], ob[i], dir)) return true;
                                 if (dir != 0) return false;  // G3: honor NULLS FIRST/LAST
                             }
                             return render_row(x) < render_row(y);  // total tie-break
                         });
        return std::nullopt;
    }

    static bool has_label(const std::vector<ResultRow>& rows, const std::string& l) {
        if (rows.empty()) {
            return true;  // empty result: nothing to order, accept any key name
        }
        for (const auto& [label, d] : rows.front().cells) {
            (void)d;
            if (label == l) {
                return true;
            }
        }
        return false;
    }

    static bool has_agg_label(const SelectStmt& sel, const std::string& l) {
        for (const SelectItem& item : sel.items) {
            if (item.label == l) {
                return true;
            }
        }
        return false;
    }

    // Compare two output rows by a named cell. A row missing the label sorts as if
    // its value were absent (it never happens for a validated key, but stay total).
    static int cmp_by_label(const ResultRow& x, const ResultRow& y,
                            const std::string& label) {
        const Datum* dx = cell(x, label);
        const Datum* dy = cell(y, label);
        if (dx == nullptr || dy == nullptr) {
            return 0;
        }
        return cmp_datum(*dx, *dy);
    }

    static const Datum* cell(const ResultRow& r, const std::string& label) {
        for (const auto& [l, d] : r.cells) {
            if (l == label) {
                return &d;
            }
        }
        return nullptr;
    }

    static std::string render_row(const ResultRow& r) {
        std::string s;
        for (const auto& [l, d] : r.cells) {
            (void)l;
            s.push_back(static_cast<char>(d.type));
            s.push_back(d.is_null ? '\x01' : '\x00');
            s += d.render();
            s.push_back('\x1f');
        }
        return s;
    }

    // (7) LIMIT/OFFSET — slice the final ordered output deterministically.
    static void apply_limit(const SelectStmt& sel, std::vector<ResultRow>& rows) {
        if (!sel.has_limit && sel.offset == 0) {
            return;
        }
        const std::size_t off =
            std::min<std::size_t>(static_cast<std::size_t>(sel.offset), rows.size());
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(off));
        if (sel.has_limit && static_cast<std::size_t>(sel.limit) < rows.size()) {
            rows.resize(static_cast<std::size_t>(sel.limit));
        }
    }

    // INCREMENTAL WRITE PATH (the O(N) write fix; was O(N^2)).
    //
    // OLD model: each write re-submitted the WHOLE accumulated write-log as one batch
    // (so a read-modify-write body saw prior committed state through the executor's
    // store, which starts EMPTY per submit) and then re-PRIMED the entire history into
    // the read store. Both are O(committed-writes) PER statement => O(N^2) to build N
    // rows. The executor + re-prime were paying to reconstruct, every time, state the
    // verified PersistentStore already holds incrementally.
    //
    // NEW model: the read-modify-write DECISION (dup-PK detect / decode+set / tombstone
    // a present row) runs in the Engine over the VERIFIED READ PATH — read_committed()
    // is a strict point-get against the LIVE committed store (exactly what a SELECT
    // WHERE pk = v reads). The Engine then commits ONE pure-writer txn through the SAME
    // verified executor (so the write-set is produced by the verified surface, not
    // hand-rolled) and applies its committed write-set INCREMENTALLY via the verified
    // Database::apply_committed (one WAL'd apply, no whole-history rebuild). Each write
    // is now O(1) amortized + one point read — O(N) (point) / O(N log N) (engine MVCC)
    // to build N rows. The conformance gate proves the result is byte-identical: the
    // read goes through the same Query<Strict> surface and the write through the same
    // executor + the verified incremental apply.

    // Strict point-get of a key against the LIVE committed store (the read path a
    // SELECT WHERE pk = v takes). Returns the committed value (incl. a tombstone
    // marker) or nullopt if absent. Pure function of the committed history + key.
    [[nodiscard]] ReadResult read_committed(const Key& key) {
        // G1: read-your-writes — inside a transaction, the latest buffered write for `key` shadows
        // the committed store (so dup-PK / UPDATE / DELETE see uncommitted changes within the txn).
        if (in_txn_) {
            const auto it = txn_overlay_.find(key);  // latest buffered write (K3.4: O(log n))
            if (it != txn_overlay_.end()) {
                return it->second;
            }
        }
        Query<Strict> q;
        q.get(key);
        const QueryResult qr = db_.run(q);
        for (const PointResult& p : qr.points) {
            if (p.key == key) {
                return p.value;
            }
        }
        return std::nullopt;
    }

    // Commit a SET OF precomputed key->value writes (the statement's already-decided
    // effect — the row write PLUS its secondary-index entry writes) ATOMICALLY in ONE
    // txn through the verified executor, then apply its committed write-set
    // incrementally to the live store. ATOMIC INDEX MAINTENANCE: the row and every
    // index entry land in the SAME committed write-set, so the index can NEVER diverge
    // from the table after a committed write (no torn/stale index — the durability +
    // atomicity is the existing verified txn path). The body is a PURE writer (no read)
    // — V-DET-USER holds — so a single-txn batch over the empty executor store is
    // correct (the read-modify-write decision was already made over the live store).
    // Submit one write batch as a single txn to `target` and apply its committed writes.
    // Returns the target's new tip Seq. `nosync` defers the fsync (group commit). Used for
    // BOTH the data store (db_) and the separate catalog store (catalog_db_) — see commit_writes
    // / persist_schema. Factoring this out is what lets the catalog live in its OWN Seq line.
    Seq commit_batch(Database& target, const std::vector<std::pair<Key, Value>>& kvs, bool nosync,
                     bool blind = false) {
        TxnFn fn;
        fn.id = next_txn_id_++;
        // A txn body NEVER reads via ctx (it writes pre-computed values), so the declared strict
        // reads on the write keys only provide write-write conflict detection for CONCURRENT txns.
        // For a BLIND bulk overwrite (a columnar flush: it rewrites whole blocks + tombstones the
        // delta from an already-consistent snapshot) under the single-threaded SQL engine there is no
        // concurrent writer, so declaring N reads is pure O(N log N) overhead — skip it. The write
        // versioning (a new committed prefix) is unchanged, so MVCC / recovery are unaffected.
        if (!blind) {
            std::vector<txn::Read> decl;
            decl.reserve(kvs.size());
            for (const auto& [k, v] : kvs) {
                (void)v;
                decl.push_back(declare::strict(k));
            }
            fn.declared = std::move(decl);
        }
        const std::vector<std::pair<Key, Value>> writes = kvs;
        fn.body = [writes](TxnContext& ctx) {
            for (const auto& [k, v] : writes) {
                ctx.write(k, v);
            }
        };

        txn::ExecConfig cfg;  // defaults: max_retry=2, replica_lag=0
        const SubmitResult sr = target.submit(fn, cfg);
        Seq t = target.tip();
        for (const txn::CommitInfo& c : sr.commits) {
            if (c.status == txn::Status::Committed && !c.writes_committed.empty()) {
                t = nosync ? target.apply_committed_nosync(c.writes_committed)
                           : target.apply_committed(c.writes_committed);
            }
        }
        return t;
    }

    void commit_writes(const std::vector<std::pair<Key, Value>>& kvs, bool blind = false) {
        // G1: inside a transaction, BUFFER the writes (read-your-writes via read_committed) and apply
        // them all atomically at COMMIT; ROLLBACK discards them. (DDL writes the catalog directly via
        // commit_batch and is not buffered — DDL inside a txn auto-commits.)
        if (in_txn_) {
            for (const auto& kv : kvs) {
                txn_writes_.push_back(kv);
                txn_overlay_[kv.first] = kv.second;  // keep the read-your-writes index hot
            }
            return;
        }
        // GROUP COMMIT: defer the fsync when the caller (wire::Server, net-backed) will sync()
        // once for the whole burst — the SQL-write analogue of the keyed path. acked==durable
        // holds because the caller withholds the SqlResult ack until its sync.
        // `blind` skips the (here-redundant) per-write-key read declaration for a bulk blind
        // overwrite (a columnar flush) — O(N) instead of O(N log N) read validations.
        tip_ = commit_batch(db_, kvs, /*nosync=*/group_commit_, blind);
    }

    // Convenience: a single-key write (back-compat for the index-only DDL paths).
    void commit_write(const Key& key, const Value& value) {
        commit_writes({{key, value}});
    }

    Catalog catalog_;
    Database db_;
    Database catalog_db_;       // SEPARATE store for schema records (own Seq line / WAL) — keeps
                                // DDL out of the data MVCC version line (see persist_schema).
    bool catalog_durable_ = false;  // true ⇒ catalog_db_ is disk-backed + recovered on restart.
    Seq tip_ = 0;
    std::uint64_t next_txn_id_ = 1;
    PlanStats* plan_stats_ = nullptr;  // non-null during an EXPLAIN ANALYZE run
    bool vectorize_ = true;            // vectorized filter fast path (test-toggleable)
    bool last_join_index_nl_ = false;  // I4: set when a join took the index nested-loop path
    bool columnar_default_ = false;    // new tables use the columnar layout when set
    bool group_commit_ = false;        // defer write fsync; caller sync()s the burst once
    bool gs_mode_ = false;             // C2: inside a GROUPING SETS sub-run (NULL-out non-set cols)
    bool soa_overflow_ = false;        // K1: a 128-bit SoA SUM overflowed => bail to the row path (errors)
    bool in_txn_ = false;              // G1: inside an explicit BEGIN..COMMIT transaction
    std::vector<std::pair<Key, Value>> txn_writes_;  // G1: buffered DML writes (committed at COMMIT)
    // K3.4 perf: the LATEST buffered value per key — read_committed's read-your-writes
    // lookup was a LINEAR walk of txn_writes_ per in-txn read (every INSERT's dup-PK
    // check), turning an N-statement transaction into O(N^2). The map mirrors the
    // vector (last write wins) and is rebuilt from the vector prefix on ROLLBACK TO
    // SAVEPOINT (rare); the vector stays the source of ORDER for COMMIT/savepoints.
    std::map<Key, Value> txn_overlay_;
    // G6: SAVEPOINT stack — (name, txn_writes_ length when the savepoint was set). ROLLBACK TO
    // truncates txn_writes_ back to that length (undoing writes since the savepoint) and drops
    // savepoints established after it; RELEASE just forgets the savepoint (keeps the writes).
    std::vector<std::pair<std::string, std::size_t>> savepoints_;
    std::set<std::string> expanding_views_;  // H1: view names on the current expansion stack (cycle guard)
    std::map<std::string, std::string> matviews_;  // materialized view (qualified name) -> refreshable source SQL
    std::uint64_t auto_flush_rows_ = 0;  // auto-flush a columnar table past this delta (0=off)

    // W3.1 RESOURCE GOVERNANCE — per-statement query memory accounting. A DETERMINISTIC
    // byte counter (charged from row/cell byte sizes, never from the allocator) so the SAME
    // query + SAME limit yields the SAME error on every replica — replicated statement
    // execution stays byte-identical. max_query_mem_ = 0 disables the limit (the default;
    // conformance runs with it off, so the accounting is inert and results are unchanged).
    // Charged at intermediate MATERIALIZATION points (derived tables / CTEs / views), the
    // common unbounded-intermediate OOM vector, where the code path already returns an error.
    std::size_t max_query_mem_ = 0;      // per-statement cap in bytes (0 = unlimited)
    std::size_t query_mem_used_ = 0;     // bytes charged so far in the current top-level statement
    int stmt_depth_ = 0;                 // >0 while inside a top-level exec() (reset guard)

    // K1.3: SET ivfflat.probes — session override of each IVFFLAT index's own probes default
    // (0 = no override). pgvector's query-time recall knob.
    std::uint32_t ivfflat_probes_ = 0;
    // K1.4: SET hnsw.ef_search — the HNSW level-0 beam width (pgvector default 40).
    std::uint32_t hnsw_ef_search_ = 40;
    // K1 perf: content-keyed memo of parsed '[x,y,z]' vector text literals (the constant
    // query vector of a k-NN would otherwise be strtod-parsed on EVERY row). Pure cache —
    // same text always yields the same doubles; bounded at 64 entries.
    std::unordered_map<std::string, std::vector<double>> vec_lit_cache_;
    // K1 perf (the memory-bound fix): a CONTIGUOUS probe cache per IVFFLAT index — each
    // list's live vectors as one dense double block + the parallel pk-byte suffixes. The
    // probe then streams sequential memory instead of ~600-byte payload strings scattered
    // across the heap (the profiled floor). DERIVED data: contents are a deterministic
    // function of the live index (rebuilt via scan_visit), node-local, never replicated,
    // and the scored doubles are the SAME doubles — results stay bit-identical. Any
    // maintenance write to the index (insert/update/delete/truncate paths all go through
    // index_writes_for_row) erases the entry; DROP/CREATE INDEX erase it too. Only the
    // Strict-level live path consults it — AT SNAPSHOT reads the index as of its Seq.
    struct IvfProbeCache {
        std::size_t dim = 0;
        std::vector<std::vector<double>> list_vecs;  // per list: n*dim flattened doubles
        std::vector<std::vector<float>> list_f32;    // per list: n*dim floats (prune pass)
        std::vector<std::vector<float>> list_norm;   // per list: n vector norms (dot margins)
        std::vector<std::vector<Key>> list_pks;      // per list: n pk-byte suffixes
    };
    std::map<std::uint64_t, IvfProbeCache> ivf_probe_cache_;  // key: table_id<<32 | index_id
    static std::uint64_t ivf_cache_key(std::uint32_t tid, std::uint32_t iid) {
        return (static_cast<std::uint64_t>(tid) << 32) | iid;
    }

    // W9.2: parse cache (sql text -> parsed AST). Always valid (parsing is catalog-independent).
    static constexpr std::size_t kParseCacheCap = 1024;
    std::unordered_map<std::string, Statement> parse_cache_;

    // W3.2 CANCELLATION seam. An externally-owned flag (the prod reactor's statement-timeout
    // deadline timer, or a PG CancelRequest handler, sets it; a test sets it deterministically).
    // Null => no cancellation (the default; zero overhead + deterministic in sim). The SQL engine
    // polls it at statement entry, before each intermediate materialization, and at coarse row-loop
    // boundaries — cooperative cancellation, so a long-running query aborts with a deterministic
    // "query canceled" error. Read with a plain load (no explicit memory_order — eventual
    // visibility is fine for a cancel poll; the forbidden-call lint permits a bare std::atomic).
    const std::atomic<bool>* cancel_ = nullptr;

    // Decoded-block + zone-map cache, tagged by the table's flush generation (blocks change
    // only on flush). Perf-only: a pure function of the committed blocks, so results +
    // determinism are unchanged. Cleared on recover().
    struct ChunkCacheEntry {
        std::uint64_t gen = 0;
        std::vector<ColumnChunk> chunks;
    };
    std::map<std::uint64_t, ChunkCacheEntry> chunk_cache_;  // key = (table_id<<20 | col_id)
    // Cached full-column flat concat (all chunks of a column merged, pk-asc). The GROUP BY /
    // aggregate path decodes whole columns; without this it re-concatenated them from the cached
    // chunks on EVERY query (a fresh 8MB/Mrow build). Pure function of the committed blocks, so
    // flush_gen-tagged + cleared on recover like chunk_cache_ — results + determinism unchanged.
    struct ConcatCacheEntry {
        std::uint64_t gen = 0;
        ColumnChunk col;
    };
    std::map<std::uint64_t, ConcatCacheEntry> concat_cache_;  // key = (table_id<<20 | col_id)
    // A2 — dictionary-encoded TEXT column cache (struct declared above col_text_dict_cached).
    std::map<std::uint64_t, TextDictEntry> text_dict_cache_;
    struct ZoneCacheEntry {
        std::uint64_t gen = 0;
        std::vector<std::vector<ColZone>> zones;
    };
    std::map<std::uint32_t, ZoneCacheEntry> zone_cache_;  // key = table_id

    // Morsel-parallel executor (null = serial default). Only ever wraps independent per-partition
    // folds whose merge is order-fixed, so the result stays deterministic.
    IParallelExecutor* parallel_executor_ = nullptr;
    // Below this row count a parallel split costs more (thread dispatch) than it saves.
    static constexpr std::int64_t kParallelMinRows = 50000;
    // emit_all parallelises the per-group FOLD ACROSS GROUPS. With only a handful of groups (a
    // low-cardinality GROUP BY served by the fast serial dict path) there is nothing to balance
    // and the concurrent folds only add dispatch overhead (groupby_region residual ~40%). Fold
    // groups in parallel only when there are enough of them; the group-BUILD pass still
    // parallelises independently (par_group), so few-group GROUP BY keeps that win.
    static constexpr std::size_t kParallelMinGroups = 64;
};

}  // namespace lockstep::query::sql
