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
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/sql/Ast.hpp>
#include <lockstep/query/sql/Catalog.hpp>
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
        const std::uint32_t table_id = t->id;
        const std::size_t pk_index = t->pk_index;

        // BACKFILL: scan every live row and write its index entry. Read through the
        // verified scan path (a full table scan), then commit one index-entry write per
        // row through the verified executor (one txn per row — atomic, ordered).
        if (t->columnar) {
            // Columnar: enumerate live PKs from the pk-column family, assemble each row
            // from its column families (point-gets), then write its index entry.
            std::vector<storage::KeyValue> anchor;
            {
                Query<Strict> q;
                q.scan(col_prefix(table_id, static_cast<std::uint32_t>(pk_index)),
                       col_prefix_end(table_id, static_cast<std::uint32_t>(pk_index)));
                collect(db_.run(q), anchor);
            }
            for (const storage::KeyValue& kv : anchor) {
                if (is_tombstone(kv.second)) {
                    continue;
                }
                const Datum pk = decode_pk_from_col_key(*t, kv.first);
                const auto row = read_columnar_row(*t, pk);
                if (!row) {
                    continue;
                }
                const Key ikey =
                    encode_index_entry(table_id, ix, (*row)[ix.column], (*row)[pk_index]);
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

    // Emit the storage write(s) that MATERIALISE a row. Row mode: one {row_key, value}.
    // Columnar: one {col_key, col_value} per column (incl. the PK column, whose family
    // doubles as the existence anchor). The caller commits them in ONE batch, so the
    // row is atomic regardless of layout (the durable core's write batch is the unit).
    void emit_row_writes(const Table& t, const std::vector<Datum>& row,
                         std::vector<std::pair<Key, Value>>& writes) const {
        const Datum& pk = row[t.pk_index];
        if (t.columnar) {
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                writes.emplace_back(col_key(t.id, static_cast<std::uint32_t>(c), pk),
                                    encode_col_value(row[c]));
            }
        } else {
            writes.emplace_back(encode_key(t, pk), encode_value(t, row));
        }
    }

    // Emit the tombstone write(s) that RETIRE a row (one per column family in columnar).
    void emit_row_tombstones(const Table& t, const Datum& pk,
                             std::vector<std::pair<Key, Value>>& writes) const {
        if (t.columnar) {
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                writes.emplace_back(col_key(t.id, static_cast<std::uint32_t>(c), pk),
                                    tombstone_marker());
            }
        } else {
            writes.emplace_back(encode_key(t, pk), tombstone_marker());
        }
    }

    // Read a columnar row by PK (one point-get per column family). nullopt if no live
    // family entry exists (row absent/deleted). PK is taken from the arg (authoritative).
    // Used by UPDATE/DELETE (need the full old row for index upkeep) + the index fetch.
    [[nodiscard]] std::optional<std::vector<Datum>> read_columnar_row(const Table& t,
                                                                      const Datum& pk) {
        std::vector<Datum> row(t.columns.size());
        bool any = false;
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            const ReadResult v =
                read_committed(col_key(t.id, static_cast<std::uint32_t>(c), pk));
            if (!v.has_value() || is_tombstone(*v)) {
                continue;
            }
            row[c] = decode_col_value(t.columns[c].type, *v);
            any = true;
        }
        if (!any) {
            return std::nullopt;
        }
        row[t.pk_index] = pk;  // authoritative PK from the key
        return row;
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

    // Build the post-scan `rows` for a COLUMNAR table: scan the pk-column family for the
    // live PK list (+ order), then scan EACH needed column family and zip by position
    // (families are pk-aligned — every live row writes every family at the same commit,
    // so the i-th live entry of every family is the same row). Only NEEDED columns are
    // scanned: the projection-pushdown win (a wide unreferenced column's family is never
    // read). Needed columns are decoded into rows_out; unneeded stay Datum{}.
    // `pk_between` (with sel.lo_value/hi_value) restricts every family scan to the PK
    // sub-range [lo, hi] — the columnar PK-fast BETWEEN path (the pk suffix is order-
    // preserving, so a family's [col_prefix++pk_lo, col_prefix++pk_hi++) covers it).
    [[nodiscard]] std::optional<std::string> columnar_build_rows(
        const Table& t, const SelectStmt& sel, const std::vector<bool>& need,
        std::vector<std::vector<Datum>>& rows_out, bool pk_between = false) {
        auto fam_lo = [&](std::uint32_t c) {
            Key k = col_prefix(t.id, c);
            if (pk_between) {
                k += encode_pk(sel.lo_value);
            }
            return k;
        };
        auto fam_hi = [&](std::uint32_t c) {
            if (pk_between) {
                Key k = col_prefix(t.id, c) + encode_pk(sel.hi_value);
                k.push_back('\0');  // inclusive upper bound -> half-open
                return k;
            }
            return col_prefix_end(t.id, c);
        };
        const std::uint32_t pkc = static_cast<std::uint32_t>(t.pk_index);
        std::vector<storage::KeyValue> anchor;  // pk-column family enumerates live rows
        if (auto e = scan_range_at_level(sel, fam_lo(pkc), fam_hi(pkc), anchor)) {
            return e;
        }
        std::vector<Datum> pks;
        pks.reserve(anchor.size());
        for (const storage::KeyValue& kv : anchor) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            pks.push_back(decode_pk_from_col_key(t, kv.first));
        }
        std::vector<std::vector<Datum>> rows(pks.size(),
                                             std::vector<Datum>(t.columns.size()));
        if (need[t.pk_index]) {
            for (std::size_t i = 0; i < pks.size(); ++i) {
                rows[i][t.pk_index] = pks[i];
            }
        }
        for (std::size_t c = 0; c < t.columns.size(); ++c) {
            if (c == t.pk_index || !need[c]) {
                continue;
            }
            std::vector<storage::KeyValue> fam;
            if (auto e = scan_range_at_level(sel, fam_lo(static_cast<std::uint32_t>(c)),
                                             fam_hi(static_cast<std::uint32_t>(c)), fam)) {
                return e;
            }
            std::size_t i = 0;
            for (const storage::KeyValue& kv : fam) {
                if (is_tombstone(kv.second)) {
                    continue;
                }
                if (i >= rows.size()) {
                    return std::string("columnar family misaligned (extra column entry)");
                }
                rows[i][c] = decode_col_value(t.columns[c].type, kv.second);
                ++i;
            }
            if (i != rows.size()) {
                return std::string("columnar family misaligned (missing column entry)");
            }
        }
        rows_out = std::move(rows);
        return std::nullopt;
    }

    ExecResult exec_insert(const InsertStmt& ins) {
        const Table* t = catalog_.find(ins.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + ins.table + "'");
        }
        // v4: columns may be OMITTED. A named column is set (with type checking + NULL
        // re-typing); an omitted column defaults to NULL iff it is NULLABLE, else the
        // INSERT is rejected (a NOT NULL column REQUIRES a value). The PK is NOT NULL, so
        // omitting it is always an error.
        std::vector<Datum> row(t->columns.size());
        std::vector<bool> set(t->columns.size(), false);
        for (std::size_t k = 0; k < ins.columns.size(); ++k) {
            const auto idx = t->column_index(ins.columns[k]);
            if (!idx) {
                return ExecResult::failure("unknown column '" + ins.columns[k] +
                                           "' in table '" + ins.table + "'");
            }
            if (set[*idx]) {
                return ExecResult::failure("column '" + ins.columns[k] +
                                           "' specified more than once");
            }
            Datum d;
            if (auto e = coerce(t->columns[*idx], ins.values[k], d)) {
                return ExecResult::failure(*e);
            }
            row[*idx] = d;
            set[*idx] = true;
        }
        for (std::size_t c = 0; c < t->columns.size(); ++c) {
            if (set[c]) {
                continue;
            }
            if (!t->columns[c].nullable) {
                return ExecResult::failure(
                    "INSERT omits NOT NULL column '" + t->columns[c].name +
                    "' (provide a value)");
            }
            row[c] = Datum::make_null(t->columns[c].type);  // omitted nullable => NULL
        }
        const Datum& pk = row[t->pk_index];

        // INCREMENTAL write path (see commit_write): the read-modify-write DECISION
        // (dup-PK detect) runs in the Engine over the VERIFIED read path (the live
        // committed store), so we never re-submit the whole prior write-log. The
        // committed state is probed via the existence key (the row key, or — in columnar
        // mode — the pk-column family anchor); the resulting writes commit through the
        // verified executor + apply incrementally.
        const ReadResult existing = read_committed(existence_key(*t, pk));
        if (existing.has_value() && !is_tombstone(*existing)) {
            return ExecResult::failure("duplicate primary key in table '" + ins.table +
                                       "' (row already exists)");
        }
        // ATOMIC: the row materialisation (one KV in row mode, one per column family in
        // columnar) + every secondary-index entry commit in ONE txn (the index/columns
        // can never lag the row after a committed INSERT).
        std::vector<std::pair<Key, Value>> writes;
        emit_row_writes(*t, row, writes);
        index_writes_for_row(*t, row, /*tombstone=*/false, writes);
        commit_writes(writes);
        if (Table* mt = catalog_.find_mut(ins.table)) {
            ++mt->row_count;  // a committed INSERT adds exactly one new row (dup PK is rejected)
            // Grow per-column INT min/max (skip NULLs). Stats may lag a DELETE of an extreme —
            // acceptable for a cost estimate (a slightly-wide range under-estimates selectivity).
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
        ExecResult r;
        r.affected = 1;
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
            if (mt->row_count > 0) {
                --mt->row_count;  // a committed DELETE removes exactly one present row
            }
        }
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

    ExecResult exec_select(const SelectStmt& sel) {
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

        // No aggregates: resolve the plain projection (validate columns exist).
        std::vector<std::size_t> proj;
        std::vector<std::string> labels;
        if (sel.star) {
            for (std::size_t i = 0; i < t->columns.size(); ++i) {
                proj.push_back(i);
                labels.push_back(t->columns[i].name);
            }
        } else {
            for (const SelectItem& item : sel.items) {
                // (an aggregate here is impossible — has_aggregates would be set)
                const auto idx = t->column_index(item.column);
                if (!idx) {
                    return ExecResult::failure("unknown column '" + item.column +
                                               "' in table '" + sel.table + "'");
                }
                proj.push_back(*idx);
                labels.push_back(item.label);
            }
        }

        // Build the projected output rows (still PK-ascending from the scan).
        ExecResult r;
        for (const auto& row : rows) {
            ResultRow out;
            for (std::size_t k = 0; k < proj.size(); ++k) {
                out.cells.emplace_back(labels[k], row[proj[k]]);
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

    ExecResult exec_select_joined(const SelectStmt& sel) {
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
                    }
                }
            }
            if (!matched && je.kind == JoinKind::Left) {
                std::vector<Datum> jr = ljr;
                place_null(schema, rt.alias, jr);
                out.push_back(std::move(jr));
            }
        }
        return std::nullopt;
    }

    // Nested-loop: for each left row, scan every right row, evaluate the full ON
    // predicate (CROSS == always true). LEFT NULL-fills a left row with no match.
    [[nodiscard]] std::optional<std::string> nested_loop_join(
        const JoinEntry& je, const JoinSchema& schema,
        const std::vector<std::vector<Datum>>& left, const ScannedTable& rt,
        std::vector<std::vector<Datum>>& out) {
        for (const auto& ljr : left) {
            bool matched = false;
            for (const auto& rrow : rt.rows) {
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
                }
            }
            if (!matched && je.kind == JoinKind::Left) {
                std::vector<Datum> jr = ljr;
                place_null(schema, rt.alias, jr);
                out.push_back(std::move(jr));
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
        truth = apply_cmp(n.op, cmp_datum(lhs, rhs));
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
                                 const int c = cmp_by_label(x, y, keys[i]);
                                 if (c != 0) {
                                     return ob[i].descending ? (c > 0) : (c < 0);
                                 }
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
        truth = apply_cmp(n.op, cmp_datum(lhs, rhs));
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
        }
        return false;
    }

    // VECTORIZED FILTER (Phase 3) — a single comparison term `col <op> literal`.
    struct VecTerm {
        std::size_t col = 0;
        CmpOp op = CmpOp::Eq;
        Datum lit;
    };

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
            n.literal.is_null) {
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
        truth = apply_cmp(n.op, cmp_datum(lhs, rhs));
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
                                 const int c = cmp_by_label(x, y, k.column);
                                 if (c != 0) {
                                     return k.descending ? (c > 0) : (c < 0);
                                 }
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
                                 const int c = cmp_by_label(x, y, k.column);
                                 if (c != 0) {
                                     return k.descending ? (c > 0) : (c < 0);
                                 }
                             }
                             return render_row(x) < render_row(y);  // total tie-break
                         });
        return std::nullopt;
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
                                 const int c = cmp_by_label(x, y, keys[i]);
                                 if (c != 0) {
                                     return ob[i].descending ? (c > 0) : (c < 0);
                                 }
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
    void commit_writes(const std::vector<std::pair<Key, Value>>& kvs) {
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
        const SubmitResult sr = db_.submit(fn, cfg);
        for (const txn::CommitInfo& c : sr.commits) {
            if (c.status == txn::Status::Committed && !c.writes_committed.empty()) {
                tip_ = db_.apply_committed(c.writes_committed);
            }
        }
    }

    // Convenience: a single-key write (back-compat for the index-only DDL paths).
    void commit_write(const Key& key, const Value& value) {
        commit_writes({{key, value}});
    }

    Catalog catalog_;
    Database db_;
    Seq tip_ = 0;
    std::uint64_t next_txn_id_ = 1;
    PlanStats* plan_stats_ = nullptr;  // non-null during an EXPLAIN ANALYZE run
    bool vectorize_ = true;            // vectorized filter fast path (test-toggleable)
    bool columnar_default_ = false;    // new tables use the columnar layout when set
};

}  // namespace lockstep::query::sql
