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
// TOMBSTONE NOTE: the txn WriteSet models writes as key->value puts (no native del
// at this seam), so DELETE writes a reserved one-byte sentinel value the SELECT
// decode treats as "row absent" (it is filtered out of results). This keeps storage
// UNCHANGED while giving SQL DELETE the right observable semantics.
//
// DETERMINISM: pure function of (catalog, committed history, statement). No clock,
// no rng, no threads. The committed write-set history is replayed into the read
// store via the verified Database::prime — so a SELECT reads EXACTLY what the typed
// query surface would for the same KV writes (the conformance gate proves it).

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

    // --- SELECT ---------------------------------------------------------------
    ExecResult exec_select(const SelectStmt& sel) {
        const Table* t = catalog_.find(sel.table);
        if (t == nullptr) {
            return ExecResult::failure("unknown table '" + sel.table + "'");
        }
        // Resolve the projection (validate columns exist; * => all columns).
        std::vector<std::size_t> proj;
        if (sel.star) {
            for (std::size_t i = 0; i < t->columns.size(); ++i) {
                proj.push_back(i);
            }
        } else {
            for (const std::string& c : sel.columns) {
                const auto idx = t->column_index(c);
                if (!idx) {
                    return ExecResult::failure("unknown column '" + c +
                                               "' in table '" + sel.table + "'");
                }
                proj.push_back(*idx);
            }
        }
        // A WHERE on a non-PK column is OUT in v1 (only PK point/range supported).
        if (sel.where != SelectWhereKind::None &&
            sel.where_column != t->pk().name) {
            return ExecResult::failure(
                "SELECT WHERE must filter on the primary key '" + t->pk().name +
                "' in v1 (got '" + sel.where_column + "')");
        }

        // Lower the WHERE to typed get/scan/range and run at the chosen D5 LEVEL.
        // The D5 level stays CALL-SITE-VISIBLE: it is encoded in the AST's `level`
        // and dispatched to a Query<L> of the matching tag type (V-D5-SAFE).
        std::vector<storage::KeyValue> kvs;
        if (auto err = run_select_at_level(*t, sel, kvs)) {
            return ExecResult::failure(*err);
        }

        // DECODE each KV -> a row, FILTER tombstones, PROJECT the requested columns.
        ExecResult r;
        for (const storage::KeyValue& kv : kvs) {
            if (is_tombstone(kv.second)) {
                continue;
            }
            const std::vector<Datum> row = decode_row(*t, kv.first, kv.second);
            ResultRow out;
            for (const std::size_t ci : proj) {
                out.cells.emplace_back(t->columns[ci].name, row[ci]);
            }
            r.rows.push_back(std::move(out));
        }
        r.affected = r.rows.size();
        return r;
    }

    // Dispatch the SELECT to a Query<L> of the AST level (the D5 dispatch point).
    // Returns an error string on a bad level parameter, else fills `kvs`.
    [[nodiscard]] std::optional<std::string> run_select_at_level(
        const Table& t, const SelectStmt& sel, std::vector<storage::KeyValue>& kvs) {
        switch (sel.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                build_read_steps(t, sel, q);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::Snapshot: {
                Query<Snapshot> q = snapshot_query(sel.snapshot_version);
                build_read_steps(t, sel, q);
                collect(db_.run(q), kvs);
                return std::nullopt;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q = bounded_query(sel.max_lag);
                build_read_steps(t, sel, q);
                // replica_lag modeled as 0 here (no replica lag injected by SQL);
                // the contract stays exact and call-site-visible.
                collect(db_.run(q, /*replica_lag=*/0), kvs);
                return std::nullopt;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q = ryw_query(sel.session);
                build_read_steps(t, sel, q);
                collect(db_.run(q, /*replica_lag=*/0, /*session_last_write=*/tip_),
                        kvs);
                return std::nullopt;
            }
        }
        return std::string("unsupported consistency level");
    }

    // Compose the read steps for the SELECT's WHERE onto the typed query builder:
    //   Eq      -> a POINT get of the encoded key (a point read).
    //   Between -> a half-open scan [encode(lo), encode(hi)++) so the inclusive
    //              upper bound is covered (range over the order-preserving PK).
    //   None    -> a full scan over the table's contiguous key namespace.
    template <typename L>
    static void build_read_steps(const Table& t, const SelectStmt& sel, Query<L>& q) {
        if (sel.where == SelectWhereKind::Eq) {
            q.get(encode_key(t, sel.eq_value));
        } else if (sel.where == SelectWhereKind::Between) {
            const Key lo = encode_key(t, sel.lo_value);
            Key hi = encode_key(t, sel.hi_value);
            hi.push_back('\0');  // make BETWEEN's inclusive upper bound half-open
            q.scan(lo, hi);
        } else {
            q.scan(table_prefix(t.id), table_prefix_end(t.id));
        }
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
