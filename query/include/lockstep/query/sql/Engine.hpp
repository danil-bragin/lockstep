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
#include <cstdint>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/ParallelExecutor.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/sql/Ast.hpp>
#include <lockstep/query/sql/Catalog.hpp>
#include <lockstep/query/sql/ColumnBlock.hpp>
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
        // C7: rebuild the CATALOG from its durable schema records (reserved 0x01 namespace) in the
        // SEPARATE catalog store. Without this the recovered ROW/BLOCK data is uninterpretable after
        // a restart (the schema lived only in memory). Each record is a serialized Table re-registered
        // with its PERSISTED id (the data keys are namespaced by it).
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
            (void)catalog_.insert_recovered(std::move(t));
        }
    }

    // True iff the catalog store is disk-backed (4-arg ctor) — the caller then persists the
    // catalog disk's length alongside the data disk's and passes both to recover().
    [[nodiscard]] bool catalog_durable() const noexcept { return catalog_durable_; }

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

    static std::string serialize_schema(const Table& t) {
        std::string o;
        cat_put_s(o, t.name);
        cat_put_u32(o, t.id);
        cat_put_u32(o, static_cast<std::uint32_t>(t.pk_index));
        o.push_back(t.columnar ? 1 : 0);
        cat_put_u32(o, t.next_index_id);
        cat_put_s(o, std::to_string(t.next_auto_id));  // F6: persist the AUTO_INCREMENT counter
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
        }
        cat_put_u32(o, static_cast<std::uint32_t>(t.indexes.size()));
        for (const Index& ix : t.indexes) {
            cat_put_s(o, ix.name);
            cat_put_u32(o, ix.id);
            cat_put_u32(o, static_cast<std::uint32_t>(ix.column));
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
            t.columns.push_back(std::move(c));
        }
        const std::uint32_t ni = cat_get_u32(s, p);
        for (std::uint32_t i = 0; i < ni; ++i) {
            Index ix;
            ix.name = cat_get_s(s, p);
            ix.id = cat_get_u32(s, p);
            ix.column = cat_get_u32(s, p);
            t.indexes.push_back(std::move(ix));
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
            (void)commit_batch(catalog_db_, {{catalog_key(name), serialize_schema(*t)}},
                               /*nosync=*/false);
        }
    }

    // Parse + execute one SQL string. Parse errors surface as ExecResult::failure.
    ExecResult exec(const std::string& sql) {
        ParseResult pr = parse_sql(sql);
        if (!pr.ok()) {
            return ExecResult::failure(pr.error().render());
        }
        return exec(pr.stmt());
    }

    // Execute an already-parsed statement.
    ExecResult exec(const Statement& st) {
        switch (st.kind) {
            case StmtKind::Create:
                return exec_create(st.create);
            case StmtKind::Insert:
                return exec_insert(st.insert);
            case StmtKind::Update:
                return exec_update(st.update);
            case StmtKind::Delete:
                return exec_delete(st.del);
            case StmtKind::Select:
                return exec_select(st.select);
            case StmtKind::CreateIndex:
                return exec_create_index(st.create_index);
            case StmtKind::DropIndex:
                return exec_drop_index(st.drop_index);
            case StmtKind::DropTable:
                return exec_drop_table(st.drop_table);
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
        commit_writes(writes);  // ATOMIC: base + delta clear + overlay retire, one batch
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
        commit_writes(writes);
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
        commit_writes(writes);
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
    // --- CREATE TABLE ---------------------------------------------------------
    ExecResult exec_create(const CreateStmt& c) {
        if (catalog_.has(c.table)) {
            return ExecResult::failure("table '" + c.table + "' already exists");
        }
        Table t;
        t.name = c.table;
        t.columns = c.columns;
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
        // v4: the PRIMARY KEY column is ALWAYS NOT NULL (a NULL PK is meaningless and
        // could never be addressed by the order-preserving key encoding). Force it
        // regardless of any NOT NULL spelling in the DDL.
        t.columns[t.pk_index].nullable = false;
        t.col_stats.assign(t.columns.size(), Table::ColStat{});  // per-column stats (Phase 2)
        t.columnar = columnar_default_;  // columnar layout opt-in (engine default at CREATE)
        (void)catalog_.create(std::move(t));
        persist_schema(c.table);  // C7: durable schema record (survives a restart)
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
        Index ix;
        ix.name = ci.index;
        ix.id = t->next_index_id++;
        ix.column = *col;
        t->indexes.push_back(ix);
        persist_schema(ci.table);  // C7: the new index is part of the durable schema
        const std::uint32_t table_id = t->id;
        const std::size_t pk_index = t->pk_index;

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
            for (const std::vector<Datum>& row : rows) {
                const Key ikey =
                    encode_index_entry(table_id, ix, row[ix.column], row[pk_index]);
                commit_writes({{ikey, std::string{}}});
            }
            return ExecResult{};
        }
        std::vector<storage::KeyValue> kvs;
        {
            Query<Strict> q;
            q.scan(table_prefix(table_id), table_prefix_end(table_id));
            collect(db_.run(q), kvs);
        }
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            const std::vector<Datum> row = decode_row(*t, kv.first, kv.second);
            const Key ikey =
                encode_index_entry(table_id, ix, row[ix.column], row[pk_index]);
            commit_writes({{ikey, std::string{}}});
        }
        return ExecResult{};
    }

    // --- DROP INDEX <name> ON <table> -----------------------------------------
    // Remove the index from the catalog AND tombstone every one of its KV entries.
    ExecResult exec_drop_index(const DropIndexStmt& di) {
        Table* t = catalog_.find_mut(di.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + di.table + "'");
        }
        const Index* ixp = t->index_by_name(di.index);
        if (ixp == nullptr) {
            return ExecResult::failure("unknown index '" + di.index + "' on table '" +
                                       di.table + "'");
        }
        const Index ix = *ixp;  // copy before mutating the vector
        const std::uint32_t table_id = t->id;
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

    // DROP TABLE <t> (F8): forget the table (catalog) + durably TOMBSTONE its schema record (so a
    // restart does not resurrect it). The row/columnar/index/zone DATA under the table's monotonic
    // id is left orphaned-but-invisible (no query can name a dropped table; a re-CREATE gets a NEW
    // id) — the same no-space-reclaim model DROP INDEX uses. Unknown table => error.
    ExecResult exec_drop_table(const DropTableStmt& dt) {
        if (catalog_.find(dt.table) == nullptr) {
            return ExecResult::failure("unknown table '" + dt.table + "'");
        }
        // Durably retire the schema record in the SEPARATE catalog store (its own Seq line).
        (void)commit_batch(catalog_db_, {{catalog_key(dt.table), tombstone_marker()}},
                           /*nosync=*/false);
        catalog_.remove(dt.table);
        // Invalidate any decoded-block / zone caches that keyed on this table's flush_gen.
        chunk_cache_.clear();
        concat_cache_.clear();
        text_dict_cache_.clear();
        zone_cache_.clear();
        return ExecResult{};
    }

    // Coerce a parsed literal Datum to a column's declared type (type checking).
    // INT<->TEXT mismatch is an error (no implicit conversion in v1).
    [[nodiscard]] static std::optional<std::string> coerce(const Column& col,
                                                           const Datum& in,
                                                           Datum& out) {
        // v4: a NULL literal carries a placeholder type — re-type it to the column's
        // declared type and accept it iff the column is NULLABLE (fail-closed otherwise).
        if (in.is_null) {
            if (!col.nullable) {
                return std::string("NULL not allowed for NOT NULL column '") + col.name +
                       "'";
            }
            out = Datum::make_null(col.type);
            return std::nullopt;
        }
        if (col.type != in.type) {
            return std::string("type mismatch for column '") + col.name +
                   "': expected " + type_name(col.type) + ", got " +
                   type_name(in.type);
        }
        out = in;
        return std::nullopt;
    }

    // Append the index-entry writes for `row` (one per secondary index) to `out`.
    // `make` decides the value: a new entry (empty value) on INSERT/UPDATE-new, or a
    // tombstone on DELETE/UPDATE-old. The PK is read from row[pk_index].
    static void index_writes_for_row(const Table& t, const std::vector<Datum>& row,
                                     bool tombstone,
                                     std::vector<std::pair<Key, Value>>& out) {
        for (const Index& ix : t.indexes) {
            // v4: a NULL column value gets NO index entry (a NULL is never matched by an
            // `indexed_col = v` / BETWEEN lookup — comparison-with-NULL is UNKNOWN). So a
            // NULL row is simply absent from the index; the residual full-predicate (run
            // over point-got rows) still excludes it. This keeps the index == the table's
            // matchable rows. (UPDATE removes the OLD entry only if the old value was
            // non-NULL — symmetric, so a NULL<->value transition is maintained correctly.)
            if (row[ix.column].is_null) {
                continue;
            }
            const Key ikey =
                encode_index_entry(t.id, ix, row[ix.column], row[t.pk_index]);
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
            writes.emplace_back(encode_key(t, pk), encode_value(t, row));
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
                    case CmpOp::Like: break;  // LIKE is TEXT — never an INT zone term
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
                    case CmpOp::Like: break;  // LIKE handled in the general path, not zone-skipped
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
        for (auto& [k, row] : merged) {
            (void)k;
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
                for (std::uint32_t r = 0; r < ch.count; ++r) p.sum += ch.ints[r];
                p.n += ch.count;
            } else {
                for (std::uint32_t r = 0; r < ch.count; ++r) {
                    if (ch.nulls[r]) continue;
                    p.sum += ch.ints[r];
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
                    sm += x[r];
                    mn = std::min(mn, x[r]);
                    mx = std::max(mx, x[r]);
                }
                s.sum += sm;
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
                if (ty == Type::Int) s.sum += v.i;
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
            m.sum += p.sum;
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
                sum += cc.ints[r];
            }
            n = count;
        } else {
            for (std::uint32_t r = 0; r < count; ++r) {
                if (cc.nulls[r]) continue;
                sum += cc.ints[r];
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
            return any ? best : Datum::make_null(ty);
        }
        std::int64_t sum = 0;  // SUM / AVG over INT (validated INT)
        std::int64_t n = 0;
        for (const std::uint32_t r : idxs) {
            if (cc.nulls[r]) {
                continue;
            }
            sum += cc.ints[r];
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
                    break;  // LIKE is never an INT-vectorizable term (extractor rejects it)
            }
        }
        std::int64_t n = 0;
        for (std::uint32_t r = 0; r < count; ++r) n += mask[r];
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
        if (gcols.empty() && !has_filter && !has_having) {
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
                if (!apply_cmp(vt.op, cmp_datum(cols[vt.col]->at(rr), vt.lit))) {
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
            bool vec_ok = true;
            for (const VecTerm& vt : vterms) {
                if (t.columns[vt.col].type != Type::Int || t.columns[vt.col].nullable ||
                    vt.lit.type != Type::Int || vt.lit.is_null) {
                    vec_ok = false;
                    break;
                }
            }
            if (vec_ok) {
                for (const SelectItem& item : sel.items) {
                    if (item.kind != SelectItemKind::Aggregate) {
                        vec_ok = false;
                        break;
                    }
                    if (item.agg.kind == AggKind::CountStar) {
                        continue;
                    }
                    const auto ci = t.column_index(item.agg.column);
                    if (!ci || t.columns[*ci].type != Type::Int || t.columns[*ci].nullable) {
                        vec_ok = false;
                        break;
                    }
                }
            }
            if (vec_ok) {
                std::vector<std::uint8_t> mask;
                const std::int64_t nmatch = build_filter_mask(cols, count, vterms, mask);
                ResultRow out;
                for (const SelectItem& item : sel.items) {
                    out.cells.emplace_back(
                        item.label, compute_masked_agg(item.agg, t, cols, count, mask, nmatch));
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
        }
        ExecResult r;
        // Emit ONE group's result row (HAVING-filtered): aggregates fold via compute_agg_soa
        // (TIGHT per-column second pass; a 1-pass branchy running-accumulator measured SLOWER).
        // Appends to r.rows; returns an error string, or nullopt (emitted OR skipped by HAVING).
        // Compute ONE group's result row into `out` (HAVING-filtered via `keep`). Reads only
        // shared CONST state (cols / sel / catalog) + writes its own `out`, so groups can be
        // folded in PARALLEL across workers (compute_agg_soa / having_eval_soa mutate no engine
        // state). Returns an error string, or nullopt (keep=false => the group is dropped).
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
                                  parallel_executor_->workers() > 1 && refs.size() > 1 &&
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
            if (par_group) {
                build_groups_parallel(
                    tg, count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->texts[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_text(cols[gc]->texts[rr])};
                    });
            } else if (!use_survivors) {
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
            } else {
                hg = hash_group_collect<std::string>(  // open-addressing (A1) — sorted by key
                    count, has_filter, passes,
                    [&](std::uint32_t rr) { return cols[gc]->texts[rr]; },
                    [&](std::uint32_t rr) {
                        return std::vector<Datum>{Datum::make_text(cols[gc]->texts[rr])};
                    });
            }
            std::vector<GroupRef> refs;
            if (par_group) {
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

        // F2: UNIQUE constraint enforcement. Pre-scan the table once to collect the existing non-NULL
        // values of every UNIQUE column; staging then rejects a row that repeats one (existing OR
        // earlier in this batch). NULLs are allowed to repeat (SQL UNIQUE permits multiple NULLs).
        std::vector<std::size_t> unique_cols;
        for (std::size_t c = 0; c < t->columns.size(); ++c) {
            if (t->columns[c].unique && c != t->pk_index) unique_cols.push_back(c);
        }
        std::vector<std::set<std::string>> seen_unique(unique_cols.size());
        if (!unique_cols.empty()) {
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
                    return *e;
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
                // F6: an omitted AUTO_INCREMENT column gets the next monotonic id.
                if (t->columns[c].auto_increment) {
                    row[c] = Datum::make_int(auto_next++);
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
            const Key pk_key = encode_key(*t, pk);
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
                    if (auto e = coerce(t->columns[*ci], v, d)) return *e;
                    new_row[*ci] = d;
                }
                index_writes_for_row(*t, old_row, /*tombstone=*/true, writes);
                emit_row_writes(*t, new_row, writes);
                index_writes_for_row(*t, new_row, /*tombstone=*/false, writes);
                ++conflict_updated;
                return std::nullopt;
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
            if (auto_next != mt->next_auto_id) {
                mt->next_auto_id = auto_next;
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
        if (del.where_column != t->pk().name) {
            return ExecResult::failure(
                "DELETE WHERE must filter on the primary key '" + t->pk().name +
                "' in v1 (got '" + del.where_column + "')");
        }
        Datum pk;
        if (auto e = coerce(t->pk(), del.where_value, pk)) {
            return ExecResult::failure(*e);
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
            SeqScan, PkPointGet, PkRangeScan, IndexScan,  // access paths (leaves)
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
            case AccessPath::Kind::Index:
                node.kind = PlanNode::Kind::IndexScan;
                node.detail = "using " + ap.index.index->name +
                              (ap.index.is_eq ? " (= const)" : " (range)") + " on " + t.name;
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
        if (!sel.order_by.empty()) {
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

    ExecResult exec_select(const SelectStmt& sel) {
        if (sel.set_op != SetOp::None) {
            return exec_set_operation(sel);
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
        if (t->columnar && sel.has_aggregates && !agg_has_distinct(sel)) {
            if (auto fast = columnar_vectorized_agg(*t, sel)) {
                return std::move(*fast);
            }
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
        // Phase 3: pre-extract a vectorizable conjunctive filter ONCE. On a seq scan it FUSES
        // into the decode (decode-into-scratch + push only survivors → no per-row alloc for a
        // filtered-out row, the measured bottleneck). On the index path it applies post-fetch.
        std::vector<VecTerm> vterms;
        const bool vectorizable = sel.filter.present() && !pk_fast && vectorize_ &&
                                  try_extract_conjuncts(sel.filter, sel.filter.root, *t, vterms);
        bool filter_applied = false;
        if (used_index) {
            // Index scan: range-scan the index for the PKs, point-get each row; the full
            // predicate still runs as the residual filter in (2). (read_via_index assembles
            // columnar rows from their column families when t is columnar.)
            if (auto err = read_via_index(*t, sel, ap.index, need, rows)) {
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

        if (!used_index && !t->columnar) {
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
        ExecResult r;
        for (const auto& row : rows) {
            ResultRow out;
            if (sel.star) {
                for (std::size_t i = 0; i < t->columns.size(); ++i) {
                    out.cells.emplace_back(t->columns[i].name, row[i]);
                }
            } else {
                for (const SelectItem& item : sel.items) {
                    if (item.kind == SelectItemKind::Expr) {
                        Datum d;
                        if (auto e = eval_expr(*item.expr, *t, row, d)) {
                            return ExecResult::failure(*e);
                        }
                        out.cells.emplace_back(item.label, d);
                    } else {
                        const auto idx = t->column_index(item.column);
                        if (!idx) {
                            return ExecResult::failure("unknown column '" + item.column +
                                                       "' in table '" + sel.table + "'");
                        }
                        out.cells.emplace_back(item.label, row[*idx]);
                    }
                }
            }
            r.rows.push_back(std::move(out));
        }

        // (5) DISTINCT, (6) ORDER BY, (7) LIMIT/OFFSET.
        apply_distinct(sel, r.rows);
        if (auto e = apply_order_by(sel, *t, r.rows)) {
            return ExecResult::failure(*e);
        }
        apply_limit(sel, r.rows);
        r.affected = r.rows.size();
        return r;
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
                if (c.type != Type::Int) return "unary '-' requires an INT operand";
                out = Datum::make_int(-c.i);
                return std::nullopt;
            }
            case ExprKind::Bin: {
                Datum l, r;
                if (auto er = eval_expr(*e.left, t, row, l)) return er;
                if (auto er = eval_expr(*e.right, t, row, r)) return er;
                if (l.is_null || r.is_null) { out = Datum::make_null(Type::Int); return std::nullopt; }
                if (l.type != Type::Int || r.type != Type::Int)
                    return "arithmetic requires INT operands";
                std::int64_t v = 0;
                switch (e.op) {
                    case BinOp::Add: v = l.i + r.i; break;
                    case BinOp::Sub: v = l.i - r.i; break;
                    case BinOp::Mul: v = l.i * r.i; break;
                    case BinOp::Div:
                        if (r.i == 0) return "division by zero";
                        v = l.i / r.i;
                        break;
                    case BinOp::Mod:
                        if (r.i == 0) return "modulo by zero";
                        v = l.i % r.i;
                        break;
                }
                out = Datum::make_int(v);
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
        }
        return "unhandled expression";
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
            if (!grouped) {
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
                    // The grouped column's value is constant across the group; take
                    // it from key_datums (matched by position).
                    Datum d = grouped_column_value(gcols, grp, idx);
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
            schema.cols.push_back(JoinColumn{sel.from[0].alias, c.name, c.type});
        schema.alias_span.emplace(sel.from[0].alias, std::make_pair(std::size_t{0}, fnc));
        for (const Column& c : dim->columns)
            schema.cols.push_back(JoinColumn{sel.from[1].alias, c.name, c.type});
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
                if (p.kind == AggKind::Sum || p.kind == AggKind::Avg) acc.sum += v.i;
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
        if (auto fused = try_fused_join_aggregate(sel)) {
            return std::move(*fused);
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
                schema.cols.push_back(JoinColumn{je.alias, c.name, c.type});
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

        // (2) WHERE — filter the joined rows by the general predicate (three-valued:
        // a row is kept iff the predicate evaluates TRUE; NULL/UNKNOWN drops it).
        if (sel.filter.present()) {
            std::vector<std::vector<Datum>> kept;
            for (auto& jr : acc) {
                bool truth = false;
                if (auto e = eval_pred_joined(sel.filter, sel.filter.root, schema, jr,
                                              truth)) {
                    return ExecResult::failure(*e);
                }
                if (truth) {
                    kept.push_back(std::move(jr));
                }
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
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
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
        for (const auto& ljr : left) {
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
        for (const auto& ljr : left) {
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
            schema.cols[idx].type != Type::Int) {
            return std::string("SUM/AVG requires an INT column (got TEXT column '" +
                               a.column + "')");
        }
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
        std::int64_t sum = 0;
        for (const Datum* d : present) {
            sum += d->i;
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
            } else if (item.kind == SelectItemKind::Expr) {
                // A1: an expression may touch any column — decode them all (conservative + correct).
                return std::vector<bool>(t.columns.size(), true);
            } else if (item.agg.kind != AggKind::CountStar) {
                mark(item.agg.column);  // COUNT(*) needs no column
            }
        }
        for (const std::string& gc : sel.group_by) {
            mark(gc);
        }
        for (const OrderKey& k : sel.order_by) {
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
        mark_pred(sel.filter);
        mark_pred(sel.having);
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
        Datum eq;   // is_eq
        Datum lo;   // BETWEEN lower (inclusive)
        Datum hi;   // BETWEEN upper (inclusive)
    };

    // THE SHARED ACCESS-PATH CHOICE. Both the executor (exec_select) AND the plan builder
    // (build_plan_single, for EXPLAIN) call this single function, so the plan EXPLAIN shows
    // is EXACTLY the plan that runs. Phase 2 makes this COST-BASED (statistics-driven); today
    // it is the rule-based choice extracted verbatim from the old inline logic (byte-identical).
    struct AccessPath {
        enum class Kind : std::uint8_t { PkPoint, PkRange, Index, Seq } kind = Kind::Seq;
        IndexPlan index;  // valid iff kind == Index
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
        const bool pk_fast = sel.where != SelectWhereKind::None &&
                             sel.where_column == t.pk().name && predicate_is_pure_pk(sel, t);
        if (pk_fast && sel.where == SelectWhereKind::Eq) {
            ap.kind = AccessPath::Kind::PkPoint;
        } else if (pk_fast && sel.where == SelectWhereKind::Between) {
            ap.kind = AccessPath::Kind::PkRange;
        } else if (!pk_fast && sel.filter.present()) {
            if (auto plan = choose_index_access(sel, t)) {
                const std::size_t n = t.row_count;
                const std::size_t matches =
                    plan->is_eq ? est_eq_matches(n)
                                : est_range_rows(t, plan->index->column, plan->lo, plan->hi);
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
        }
        return ap;
    }

    // Estimated rows OUT of a single-table access path (for EXPLAIN's estimated-vs-actual).
    std::size_t est_path_rows(const AccessPath& ap, const Table& t) const {
        switch (ap.kind) {
            case AccessPath::Kind::PkPoint:
                return 1;
            case AccessPath::Kind::PkRange:
                return est_range_matches(t.row_count);
            case AccessPath::Kind::Index:
                return ap.index.is_eq
                           ? est_eq_matches(t.row_count)
                           : est_range_rows(t, ap.index.index->column, ap.index.lo, ap.index.hi);
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
            const Index* ix = t.index_for_column(*col);
            if (ix == nullptr) {
                continue;
            }
            IndexPlan plan;
            plan.index = ix;
            plan.is_eq = true;
            plan.eq = n.literal;
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
            if (ix == nullptr) {
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
    [[nodiscard]] std::optional<std::string> read_via_index(
        const Table& t, const SelectStmt& sel, const IndexPlan& plan,
        const std::vector<bool>& need, std::vector<std::vector<Datum>>& rows_out) {
        // (a) The index range [lo, hi): col-ascending entries for the matching values.
        Key lo = index_prefix(t.id, plan.index->id);
        Key hi;
        if (plan.is_eq) {
            put_index_col(lo, plan.eq);
            hi = key_successor(lo);  // all PK-suffixes under this exact col value
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

    // Decode the PK datum from an index ENTRY key. The entry is
    // index_prefix ++ encode_index_col(col) ++ encode_pk(pk); skip the prefix, then the
    // self-delimiting col token, leaving the PK suffix which decodes per the PK type.
    [[nodiscard]] static Datum decode_index_entry_pk(const Table& t,
                                                     std::uint32_t index_id,
                                                     const Key& entry) {
        const std::size_t prefix_len = index_prefix(t.id, index_id).size();
        std::size_t off = prefix_len;
        // Skip the self-delimiting col token. We need the indexed column's TYPE to skip
        // it; recover the type from the index id.
        Type ctype = Type::Int;
        for (const Index& ix : t.indexes) {
            if (ix.id == index_id) {
                ctype = t.columns[ix.column].type;
                break;
            }
        }
        if (ctype == Type::Int) {
            off += 9;  // put_pk_int width (sign byte + be64)
        } else {
            // Skip the escaped TEXT token up to the 0x00 0x00 terminator.
            while (off + 1 < entry.size()) {
                if (entry[off] == '\0') {
                    if (entry[off + 1] == '\0') {
                        off += 2;  // terminator
                        break;
                    }
                    off += 2;  // escaped 0x00 0x01
                } else {
                    off += 1;
                }
            }
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
    static int cmp_datum(const Datum& a, const Datum& b) {
        if (a.is_null || b.is_null) {
            if (a.is_null && b.is_null) return 0;
            return a.is_null ? -1 : 1;  // NULL < any present value (NULLs first)
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

    // Evaluate ONE comparison leaf (lhs OP rhs) to a bool — the single place LIKE diverges from the
    // 3-way ordered comparators. Both operands are already NULL-checked + type-equal by the caller.
    static bool leaf_truth(CmpOp op, const Datum& lhs, const Datum& rhs) {
        if (op == CmpOp::Like) {
            return lhs.type == Type::Text && rhs.type == Type::Text && like_match(lhs.s, rhs.s);
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
            n.literal.is_null || n.op == CmpOp::Like) {  // B1: LIKE is not zone/mask-vectorizable
            return false;
        }
        const auto ci = t.column_index(n.column);
        if (!ci || t.columns[*ci].nullable || n.literal.type != t.columns[*ci].type) {
            return false;
        }
        out.push_back(VecTerm{*ci, n.op, n.literal});
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
                const auto idx = t.column_index(n.column);
                if (!idx) {
                    return std::string("unknown column '" + n.column + "' in table '" +
                                       t.name + "'");
                }
                const bool null = row[*idx].is_null;
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
                if (auto e = run_sub_column(*n.subquery, sub)) return e;
                return apply_in(row[*idx], n.is_not, sub, truth);
            }
            case PredNodeKind::Exists: {
                // v4: [NOT] EXISTS (SELECT ...) — uncorrelated existence test.
                if (group != nullptr) {
                    return std::string("EXISTS subqueries are not supported in HAVING");
                }
                bool ex = false;
                if (auto e = run_exists_sub(*n.subquery, ex)) return e;
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
        } else {
            const auto idx = t.column_index(n.column);
            if (!idx) {
                return std::string("unknown column '" + n.column + "' in table '" +
                                   t.name + "'");
            }
            lhs = row[*idx];
        }
        // v4: the RHS may be a SCALAR SUBQUERY (col <op> (SELECT agg)). Resolve it to a
        // Datum (NULL if the subquery returned 0 rows; an error if it returned >1 row).
        Datum rhs;
        if (n.rhs_is_subquery) {
            bool snull = false;
            if (auto e = run_scalar_sub(*n.subquery, snull, rhs)) return e;
            if (snull) {
                truth = false;  // comparison with a NULL scalar is UNKNOWN
                return std::nullopt;
            }
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
        if ((a.kind == AggKind::Sum || a.kind == AggKind::Avg) &&
            t.columns[*idx].type != Type::Int) {
            return std::string("SUM/AVG requires an INT column (got TEXT column '" +
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
        // Collect the PRESENT (non-NULL) values of the aggregated column.
        std::vector<const Datum*> present;
        for (const auto* rp : grp.rows) {
            const Datum& d = (*rp)[ci];
            if (!d.is_null) {
                present.push_back(&d);
            }
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
            out = (a.kind == AggKind::Sum) ? Datum::make_int(0) : Datum::make_null(ty);
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
        // SUM / AVG over INT (validated INT in validate_one_agg).
        std::int64_t sum = 0;
        for (const Datum* d : present) {
            sum += d->i;
        }
        if (a.kind == AggKind::Sum) {
            out = Datum::make_int(sum);
            return std::nullopt;
        }
        // AVG: integer truncation toward zero (C++ / divides truncates toward zero).
        const std::int64_t n = static_cast<std::int64_t>(present.size());
        out = Datum::make_int(n == 0 ? 0 : sum / n);
        return std::nullopt;
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
            if (!has_label(rows, k.column) && !t.column_index(k.column)) {
                return std::string("unknown ORDER BY column '" + k.column + "'");
            }
        }
        // Stable sort with a total comparator (ORDER BY keys, then PK tie-break).
        const std::string pk_label = t.pk().name;
        std::stable_sort(rows.begin(), rows.end(),
                         [&](const ResultRow& x, const ResultRow& y) {
                             for (const OrderKey& k : sel.order_by) {
                                 int dir = 0;
                                 if (order_key_less(x, y, k, dir)) return true;
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
            if (!has_label(rows, k.column) && !has_agg_label(sel, k.column)) {
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
    Seq commit_batch(Database& target, const std::vector<std::pair<Key, Value>>& kvs, bool nosync) {
        TxnFn fn;
        fn.id = next_txn_id_++;
        std::vector<txn::Read> decl;
        decl.reserve(kvs.size());
        for (const auto& [k, v] : kvs) {
            (void)v;
            decl.push_back(declare::strict(k));
        }
        fn.declared = std::move(decl);
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

    void commit_writes(const std::vector<std::pair<Key, Value>>& kvs) {
        // GROUP COMMIT: defer the fsync when the caller (wire::Server, net-backed) will sync()
        // once for the whole burst — the SQL-write analogue of the keyed path. acked==durable
        // holds because the caller withholds the SqlResult ack until its sync.
        tip_ = commit_batch(db_, kvs, /*nosync=*/group_commit_);
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
    bool columnar_default_ = false;    // new tables use the columnar layout when set
    bool group_commit_ = false;        // defer write fsync; caller sync()s the burst once
    std::uint64_t auto_flush_rows_ = 0;  // auto-flush a columnar table past this delta (0=off)

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
};

}  // namespace lockstep::query::sql
