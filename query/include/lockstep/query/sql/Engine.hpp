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
        }
        return ExecResult::failure("unknown statement kind");
    }

    [[nodiscard]] const Catalog& catalog() const { return catalog_; }

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
        (void)catalog_.create(std::move(t));
        return ExecResult{};
    }

    // Coerce a parsed literal Datum to a column's declared type (type checking).
    // INT<->TEXT mismatch is an error (no implicit conversion in v1).
    [[nodiscard]] static std::optional<std::string> coerce(const Column& col,
                                                           const Datum& in,
                                                           Datum& out) {
        if (col.type != in.type) {
            return std::string("type mismatch for column '") + col.name +
                   "': expected " + type_name(col.type) + ", got " +
                   type_name(in.type);
        }
        out = in;
        return std::nullopt;
    }

    // --- INSERT ---------------------------------------------------------------
    ExecResult exec_insert(const InsertStmt& ins) {
        const Table* t = catalog_.find(ins.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + ins.table + "'");
        }
        // Every column must be provided exactly once (no defaults/NULL in v1).
        if (ins.columns.size() != t->columns.size()) {
            return ExecResult::failure(
                "INSERT must provide all " + std::to_string(t->columns.size()) +
                " columns in v1 (no defaults/NULL); got " +
                std::to_string(ins.columns.size()));
        }
        // Map named columns -> a row in schema order, with type checking.
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
        const Datum& pk = row[t->pk_index];
        const Key key = encode_key(*t, pk);
        const Value enc = encode_value(*t, row);

        // INCREMENTAL write path (see commit_write): the read-modify-write DECISION
        // (dup-PK detect) runs in the Engine over the VERIFIED read path (the live
        // committed store), so we never re-submit the whole prior write-log. The
        // committed state is read with read_committed(key); the resulting write (or
        // no-op) is committed through the verified executor + applied incrementally.
        const ReadResult existing = read_committed(key);
        if (existing.has_value() && !is_tombstone(*existing)) {
            return ExecResult::failure("duplicate primary key in table '" + ins.table +
                                       "' (row already exists)");
        }
        commit_write(key, enc);
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
        const Key key = encode_key(*t, pk);
        const std::size_t col = *set_idx;
        // INCREMENTAL: read the prior committed value over the verified read path,
        // decode + set the column in the Engine, then commit the new value.
        const ReadResult existing = read_committed(key);
        if (!existing.has_value() || is_tombstone(*existing)) {
            ExecResult r;
            r.affected = 0;  // no row to update
            return r;
        }
        std::vector<Datum> row = decode_row(*t, key, *existing);
        row[col] = sv;
        commit_write(key, encode_value(*t, row));
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
        const Key key = encode_key(*t, pk);
        // INCREMENTAL: read the prior committed value; if a live row exists, commit
        // a tombstone (the verified executor + incremental apply path do the write).
        const ReadResult existing = read_committed(key);
        if (!existing.has_value() || is_tombstone(*existing)) {
            ExecResult r;
            r.affected = 0;  // nothing to delete
            return r;
        }
        commit_write(key, tombstone_marker());
        ExecResult r;
        r.affected = 1;
        return r;
    }

    // --- SELECT (the v2 pipeline) --------------------------------------------
    ExecResult exec_select(const SelectStmt& sel) {
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
        const bool pk_fast =
            sel.where != SelectWhereKind::None && sel.where_column == t->pk().name &&
            predicate_is_pure_pk(sel, *t);
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
        const std::vector<bool> need = needed_columns(*t, sel);
        std::vector<std::vector<Datum>> rows;
        rows.reserve(kvs.size());
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            rows.push_back(decode_row_projected(*t, kv.first, kv.second, need));
        }

        // (2) WHERE — apply the general predicate as a row filter UNLESS the PK fast
        // path already enforced the entire WHERE (a pure-PK predicate). When the fast
        // path was NOT taken (full scan), the predicate must run here.
        if (sel.filter.present() && !pk_fast) {
            std::vector<std::vector<Datum>> kept;
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
            rows = std::move(kept);
        }

        // (3)/(4) GROUP + AGGREGATE + HAVING when the query has aggregates / GROUP BY.
        if (sel.has_aggregates || !sel.group_by.empty()) {
            return exec_aggregate(sel, *t, rows);
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
        if (n.rhs_is_column) {
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
    static void collect(const QueryResult& qr, std::vector<storage::KeyValue>& kvs) {
        for (const PointResult& p : qr.points) {
            if (p.value.has_value()) {
                kvs.emplace_back(p.key, *p.value);
            }
        }
        for (const RangeResult& rr : qr.ranges) {
            for (const storage::KeyValue& kv : rr.rows) {
                kvs.push_back(kv);
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
        if (lhs.type != n.literal.type) {
            return std::string("type mismatch in predicate: comparing ") +
                   type_name(lhs.type) + " to " + type_name(n.literal.type);
        }
        truth = apply_cmp(n.op, cmp_datum(lhs, n.literal));
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
    // work for INT (numeric) + TEXT (lexicographic). An empty group yields COUNT 0;
    // MIN/MAX/SUM/AVG over an empty group is impossible here (a group has >=1 row)
    // EXCEPT the synthetic ungrouped-over-empty case, where COUNT=0 and SUM=0 and
    // MIN/MAX/AVG render as 0 (the conventional empty-aggregate rendering we pin).
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
        if (a.kind == AggKind::Count) {
            // COUNT(col): every column is present (non-NULL) in this subset.
            out = Datum::make_int(static_cast<std::int64_t>(grp.rows.size()));
            return std::nullopt;
        }
        if (grp.rows.empty()) {
            // Only the synthetic ungrouped-empty group. SUM=0; MIN/MAX/AVG=0.
            out = Datum::make_int(0);
            return std::nullopt;
        }
        if (a.kind == AggKind::Min || a.kind == AggKind::Max) {
            Datum best = (*grp.rows.front())[ci];
            for (const auto* rp : grp.rows) {
                const Datum& d = (*rp)[ci];
                const int c = cmp_datum(d, best);
                if ((a.kind == AggKind::Min && c < 0) ||
                    (a.kind == AggKind::Max && c > 0)) {
                    best = d;
                }
            }
            out = best;
            return std::nullopt;
        }
        // SUM / AVG over INT (validated INT in validate_one_agg).
        std::int64_t sum = 0;
        for (const auto* rp : grp.rows) {
            sum += (*rp)[ci].i;
        }
        if (a.kind == AggKind::Sum) {
            out = Datum::make_int(sum);
            return std::nullopt;
        }
        // AVG: integer truncation toward zero (C++ / divides truncates toward zero).
        const std::int64_t n = static_cast<std::int64_t>(grp.rows.size());
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

    // Commit ONE precomputed key->value write (the statement's already-decided effect)
    // through the verified executor, then apply its committed write-set incrementally
    // to the live store. The txn body is a PURE writer (no read) — V-DET-USER holds —
    // so a single-txn batch over the empty executor store is correct (the decision was
    // already made by read_committed over the live store).
    void commit_write(const Key& key, const Value& value) {
        TxnFn fn;
        fn.id = next_txn_id_++;
        fn.declared = reads(declare::strict(key));
        fn.body = [key, value](TxnContext& ctx) { ctx.write(key, value); };

        txn::ExecConfig cfg;  // defaults: max_retry=2, replica_lag=0
        const SubmitResult sr = db_.submit(fn, cfg);
        for (const txn::CommitInfo& c : sr.commits) {
            if (c.status == txn::Status::Committed && !c.writes_committed.empty()) {
                tip_ = db_.apply_committed(c.writes_committed);
            }
        }
    }

    Catalog catalog_;
    Database db_;
    Seq tip_ = 0;
    std::uint64_t next_txn_id_ = 1;
};

}  // namespace lockstep::query::sql
