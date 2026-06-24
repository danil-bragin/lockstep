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

        // Lower to a one-shot TxnFn: read the key (dup-PK detect), write if absent.
        // The body is a PURE function of its declared read (V-DET-USER preserved).
        TxnFn fn;
        fn.id = next_txn_id_++;
        fn.declared = reads(declare::strict(key));
        fn.body = [key, enc](TxnContext& ctx) {
            const ReadResult existing = ctx.read(key);
            if (existing.has_value() && !is_tombstone(*existing)) {
                return;  // a live row already occupies this PK (empty write == dup)
            }
            ctx.write(key, enc);
        };
        // Submit the WHOLE accumulated write-log so the executor's store carries
        // prior committed writes forward (a single-statement batch starts empty,
        // so the dup-detect read must run with the full history in one batch).
        const bool wrote = submit_write(std::move(fn));
        if (!wrote) {
            return ExecResult::failure("duplicate primary key in table '" + ins.table +
                                       "' (row already exists)");
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
        const Key key = encode_key(*t, pk);
        const Table tab = *t;
        const std::size_t col = *set_idx;
        TxnFn fn;
        fn.id = next_txn_id_++;
        fn.declared = reads(declare::strict(key));
        fn.body = [key, tab, col, sv](TxnContext& ctx) {
            const ReadResult existing = ctx.read(key);
            if (!existing.has_value() || is_tombstone(*existing)) {
                return;  // no row to update (empty write == 0 affected)
            }
            std::vector<Datum> row = decode_row(tab, key, *existing);
            row[col] = sv;
            ctx.write(key, encode_value(tab, row));
        };
        const bool wrote = submit_write(std::move(fn));
        ExecResult r;
        r.affected = wrote ? 1 : 0;
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
        TxnFn fn;
        fn.id = next_txn_id_++;
        fn.declared = reads(declare::strict(key));
        fn.body = [key](TxnContext& ctx) {
            const ReadResult existing = ctx.read(key);
            if (!existing.has_value() || is_tombstone(*existing)) {
                return;  // nothing to delete (empty write == 0 affected)
            }
            ctx.write(key, tombstone_marker());
        };
        const bool wrote = submit_write(std::move(fn));
        ExecResult r;
        r.affected = wrote ? 1 : 0;
        return r;
    }

    // --- SELECT (the v2 pipeline) --------------------------------------------
    ExecResult exec_select(const SelectStmt& sel) {
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

        // DECODE each KV -> a row, dropping tombstones.
        std::vector<std::vector<Datum>> rows;
        rows.reserve(kvs.size());
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            rows.push_back(decode_row(*t, kv.first, kv.second));
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
    static int cmp_datum(const Datum& a, const Datum& b) {
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

    // Submit a write statement as a one-shot txn. Each write goes through the
    // VERIFIED txn surface — but a single-statement batch starts against an EMPTY
    // executor store, so a read-modify-write body (dup-detect / update / delete)
    // would never see prior committed state. We therefore keep the WHOLE write-log
    // (every prior write-statement's TxnFn) and submit it as ONE ordered batch, so
    // the executor carries committed writes forward WITHIN the batch exactly as the
    // consensus seqLog would. The NEW statement is the LAST txn in the batch; its
    // observable `writes_committed` tells us the effect:
    //   non-empty => the statement wrote (INSERT persisted / UPDATE/DELETE matched)
    //   empty     => a no-op (INSERT dup-PK / UPDATE/DELETE missing row)
    // Returns true iff the new statement wrote (so the caller maps dup vs affected).
    // After the batch, the committed write-sets re-prime the read-path store so
    // subsequent SELECTs observe them (the verified Database::prime, byte-identical
    // to the re-execution model the conformance gate judges).
    bool submit_write(TxnFn fn) {
        write_log_.push_back(std::move(fn));
        txn::ExecConfig cfg;  // defaults: max_retry=2, replica_lag=0
        const SubmitResult sr = db_.submit(write_log_, cfg);

        // The new statement is the last submitted txn; find its commit (the batch is
        // in seqLog order, so the last Committed entry with our id is it). We added
        // it last, so the final commit corresponds to our statement.
        bool wrote = false;
        std::vector<txn::WriteSet> history;
        history.reserve(sr.commits.size());
        for (const txn::CommitInfo& c : sr.commits) {
            if (c.status == txn::Status::Committed && !c.writes_committed.empty()) {
                history.push_back(c.writes_committed);
            }
        }
        if (!sr.commits.empty()) {
            const txn::CommitInfo& last = sr.commits.back();
            wrote = (last.status == txn::Status::Committed) &&
                    !last.writes_committed.empty();
        }
        // If the new statement was a no-op (dup / missing), drop it from the log so a
        // failed INSERT does not bloat every future batch (the no-op is not part of
        // the committed history anyway).
        if (!wrote) {
            write_log_.pop_back();
        }
        tip_ = db_.prime(history);
        return wrote;
    }

    Catalog catalog_;
    Database db_;
    std::vector<TxnFn> write_log_;  // every committed write-statement's TxnFn, ordered
    Seq tip_ = 0;
    std::uint64_t next_txn_id_ = 1;
};

}  // namespace lockstep::query::sql
