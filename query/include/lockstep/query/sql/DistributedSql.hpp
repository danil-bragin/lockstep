#pragma once

// DistributedSql — a scatter-gather coordinator over M shard SqlEngines (the distributed-SQL
// milestone, ⭐2). Single-node SQL-over-wire is the per-shard foundation; this fans a statement
// across shards and MERGES the results so a sharded table answers like one logical table:
//   * CREATE / CREATE INDEX / DROP INDEX  -> BROADCAST to every shard (each holds the schema).
//   * INSERT / UPDATE / DELETE / point SELECT (WHERE pk = v) -> ROUTE to the one shard owning the
//     key (FNV/multiplicative hash of the PK value % M).
//   * scan / aggregate SELECT -> SCATTER to all shards, then GATHER+MERGE: scalar + GROUP BY
//     aggregates recombine by kind (COUNT/COUNT(*) -> Σ, SUM -> Σ, MIN -> min, MAX -> max), the
//     final groups re-sorted by the group key to match single-node order.
//
// Scope: distributed AVG (needs sum+count, not the averaged value) and distributed JOIN are
// rejected (a larger follow-on). A plain projection scan's GLOBAL row order needs a merge-sort
// (each shard is locally pk-ordered) — also a follow-on; aggregates + point ops are exact here.
//
// Verified by tests/sql_distributed_test.cpp: the SAME workload over M shards yields results
// BYTE-IDENTICAL to one SqlEngine holding all the rows.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/sql/Parser.hpp>

namespace lockstep::query::sql {

// A shard the coordinator can run a statement on — IN-PROCESS (a local SqlEngine) or OVER THE WIRE
// (a wire connection to a remote SqlEngine). The coordinator is transport-agnostic: it routes +
// scatters + merges over this interface, so the SAME scatter-gather logic serves a single-process
// test and a real multi-node cluster (the wire SqlResult now ships the SELECT rows).
struct ISqlShard {
    virtual ~ISqlShard() = default;
    virtual ExecResult exec(const std::string& sql) = 0;
};

// In-process shard: a local SqlEngine.
class EngineSqlShard final : public ISqlShard {
public:
    explicit EngineSqlShard(SqlEngine* e) : eng_(e) {}
    ExecResult exec(const std::string& sql) override { return eng_->exec(sql); }

private:
    SqlEngine* eng_;
};

class DistributedSql {
public:
    explicit DistributedSql(std::vector<ISqlShard*> shards) : shards_(std::move(shards)) {}

    // Mark `table` REPLICATED: it lives in FULL on every shard (writes broadcast instead of route by
    // PK hash). A small dimension declared replicated lets a star JOIN run ENTIRELY on each shard
    // (fact-shard ⋈ the local full dim -> partial group aggregates), which the coordinator merges —
    // the broadcast-dim alternative to the pre-aggregate pushdown. Better when the join key has far
    // more distinct values than the GROUP BY (per-shard partials = #groups, not #keys). Declare
    // BEFORE inserting the table's rows so every row broadcasts. The FACT stays sharded.
    void set_replicated(const std::string& table) { replicated_.insert(table); }

    ExecResult exec(const std::string& sql) {
        ParseResult pr = parse_sql(sql);
        if (!pr.ok()) {
            return shards_.front()->exec(sql);  // let a shard produce the canonical parse error
        }
        const Statement& st = pr.stmt();
        switch (st.kind) {
            case StmtKind::Create:
                pk_[st.create.table] = st.create.pk_column;
                create_sql_[st.create.table] = sql;  // remembered to rebuild a local copy for JOINs
                return broadcast(sql);
            case StmtKind::CreateIndex:
            case StmtKind::DropIndex:
            case StmtKind::DropTable:
            case StmtKind::Truncate:  // E2: a DDL/all-rows op -> every shard
            case StmtKind::CreateSchema:  // E4
            case StmtKind::DropSchema:
            case StmtKind::SetSearchPath:
            case StmtKind::Alter:
            case StmtKind::Begin:
            case StmtKind::Commit:
            case StmtKind::Rollback:
                return broadcast(sql);
            case StmtKind::Insert:
                return route_or_broadcast(sql, st.insert.table, pk_value_of(st.insert));
            case StmtKind::Update:
                return route_or_broadcast(sql, st.update.table, st.update.where_value);
            case StmtKind::Delete:
                return route_or_broadcast(sql, st.del.table, st.del.where_value);
            case StmtKind::Select:
                return exec_select(sql, st.select);
            case StmtKind::ShowTables:  // E5: introspection -> any shard (schema is replicated)
            case StmtKind::Describe:
                return shards_.front()->exec(sql);
            case StmtKind::Analyze:  // I6: recompute stats on every shard
                return broadcast(sql);
        }
        return ExecResult::failure("distributed: unsupported statement");
    }

private:
    [[nodiscard]] std::size_t shard_of(const Datum& key) const {
        std::uint64_t h = 1469598103934665603ULL;
        if (key.type == Type::Int) {
            h = static_cast<std::uint64_t>(key.i) * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
        } else {
            for (const char c : key.s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ULL;
            }
        }
        return static_cast<std::size_t>(h % shards_.size());
    }

    [[nodiscard]] Datum pk_value_of(const InsertStmt& ins) const {
        const auto it = pk_.find(ins.table);
        if (it != pk_.end()) {
            for (std::size_t i = 0; i < ins.columns.size(); ++i) {
                if (ins.columns[i] == it->second) return ins.values[i];
            }
        }
        return Datum::make_int(0);  // PK is required + named in practice; fall back to shard 0
    }

    ExecResult broadcast(const std::string& sql) {
        ExecResult last;
        for (ISqlShard* s : shards_) last = s->exec(sql);
        return last;  // identical across shards (same DDL) — return any
    }

    ExecResult route(const std::string& sql, const Datum& key) {
        return shards_[shard_of(key)]->exec(sql);
    }

    // A replicated table's write goes to EVERY shard (full copy on each); a sharded table's write is
    // routed to the one owning shard by PK hash.
    ExecResult route_or_broadcast(const std::string& sql, const std::string& table, const Datum& key) {
        if (replicated_.count(table) != 0) return broadcast(sql);
        return route(sql, key);
    }

    // True iff every NON-fact table in the join is replicated and the fact (from[0]) is sharded — the
    // precondition for the broadcast-dim join (each shard's fact-slice joined with its full dim copies
    // yields a disjoint partition of the global join; the partial group aggregates then merge).
    [[nodiscard]] bool broadcast_join_ok(const SelectStmt& sel) const {
        if (!sel.is_join() || sel.from.empty()) return false;
        if (replicated_.count(sel.from[0].table) != 0) return false;  // fact must be sharded
        for (std::size_t i = 1; i < sel.from.size(); ++i)
            if (replicated_.count(sel.from[i].table) == 0) return false;
        return true;
    }

    ExecResult exec_select(const std::string& sql, const SelectStmt& sel) {
        // JOIN, or a non-aggregate scan that needs GLOBAL ORDER (ORDER BY): GATHER every involved
        // table's rows from all shards into a fresh local engine and run the ORIGINAL query there —
        // the engine's verified JOIN / ORDER BY logic produces a result byte-identical to a single
        // node. (Baseline broadcast-style distributed JOIN; a shuffle/co-located join is a perf
        // follow-on.) Aggregates + unordered scans + point reads stay on the efficient
        // scatter-merge path below.
        if (sel.is_join()) {
            // BROADCAST-DIM JOIN: when every dim is replicated (full copy on each shard), each shard
            // joins its fact-slice with its local dims and computes PARTIAL group aggregates; the
            // coordinator merges them (Σ/min/max by group key). The dim was broadcast at write time;
            // only per-shard #group partials cross the network. Aggregate-only, no HAVING/ORDER BY
            // (a shard's local HAVING / order can't decide the global result). DISTINCT/AVG are
            // rejected by merge_aggregate (same as the scatter path). Else: the pre-aggregate
            // pushdown, else gather-and-run.
            if (broadcast_join_ok(sel) && sel.has_aggregates && !sel.having.present() &&
                sel.order_by.empty()) {
                std::vector<ExecResult> parts;
                parts.reserve(shards_.size());
                for (ISqlShard* s : shards_) {
                    parts.push_back(s->exec(sql));
                    if (!parts.back().ok) return parts.back();
                }
                ++broadcast_joins_;
                return merge_aggregate(parts, sel);
            }
            // PUSHDOWN star-schema JOIN+aggregate (co-located shuffle): aggregate the LARGE fact by
            // the join key ON EACH SHARD (no join, no dim there), merge the small per-key partials,
            // then join the (tiny) small dim + roll up by the dim group — so the fact is NEVER
            // gathered. Falls back to gather-and-run otherwise.
            if (auto pushed = try_star_join_pushdown(sel)) { ++pushdowns_; return std::move(*pushed); }
        }
        if (sel.is_join() || (!sel.has_aggregates && !sel.order_by.empty())) {
            std::vector<std::string> tables;
            if (sel.is_join()) {
                for (const JoinEntry& j : sel.from) tables.push_back(j.table);
            } else {
                tables.push_back(sel.table);
            }
            return gather_and_run(sql, tables);
        }
        if (sel.where == SelectWhereKind::Eq) {
            return route(sql, sel.eq_value);  // point read -> the owning shard
        }
        // Single-table GROUP BY + HAVING: a shard applying HAVING to its LOCAL partial groups is
        // wrong for a group split across shards (the global aggregate decides, not the partial).
        // Gather the table and run single-node — byte-identical, incl any HAVING error.
        if (sel.has_aggregates && sel.having.present()) {
            return gather_and_run(sql, {sel.table});
        }
        // DISTINCT aggregate: a per-shard partial can't be merged (a value on two shards would be
        // counted/summed twice). Ship the DISTINCT (group, value) tuples and dedup globally (a true
        // shuffle) for the common single-DISTINCT shape; gather-and-run (correct) for anything else.
        if (sel.has_aggregates && has_distinct_agg(sel)) {
            if (auto shuffled = try_distinct_shuffle(sel)) {
                ++distinct_shuffles_;
                return std::move(*shuffled);
            }
            return gather_and_run(sql, {sel.table});
        }
        std::vector<ExecResult> parts;
        parts.reserve(shards_.size());
        for (ISqlShard* s : shards_) {
            parts.push_back(s->exec(sql));
            if (!parts.back().ok) return parts.back();  // propagate the first shard error
        }
        if (sel.has_aggregates) return merge_aggregate(parts, sel);
        return merge_scan(parts, sel);  // unordered projection scan: concat (order unspecified)
    }

    [[nodiscard]] static bool has_distinct_agg(const SelectStmt& sel) {
        for (const SelectItem& it : sel.items) {
            if (it.kind != SelectItemKind::Column && it.agg.distinct) return true;
        }
        return false;
    }

    // GLOBAL-DISTINCT SHUFFLE for a single-table COUNT(DISTINCT col) with GROUP BY: scatter the
    // DISTINCT (group-cols, value) tuples, UNION them at the coordinator (cross-shard dedup), then
    // count the per-group distinct value SET. Only distinct tuples cross the network — a value
    // present on K shards ships K times (deduped here) instead of every row. Byte-identical to
    // single-node COUNT(DISTINCT) (which SKIPS NULL — we skip null values too).
    //
    // Returns nullopt (caller falls back to gather-and-run, correct for every shape) unless it fits:
    // single table, GROUP BY present, items are GROUP BY cols + EXACTLY ONE COUNT(DISTINCT col), no
    // other aggregate, no WHERE, no HAVING. SUM/AVG(DISTINCT) and scalar (no GROUP BY) go to gather
    // (they need typed-NULL / int-only handling the coordinator can't prove here).
    std::optional<ExecResult> try_distinct_shuffle(const SelectStmt& sel) {
        if (sel.filter.present() || sel.having.present()) return std::nullopt;
        if (sel.group_by.empty()) return std::nullopt;  // scalar -> gather (empty-table rendering)
        std::vector<std::string> gcols;
        for (const std::string& gc : sel.group_by) {
            std::string q;
            std::string c;
            split_qual(gc, q, c);
            gcols.push_back(c);
        }
        const AggExpr* da = nullptr;
        for (const SelectItem& it : sel.items) {
            if (it.kind == SelectItemKind::Column) {
                bool grouped = false;
                for (const std::string& g : gcols) if (g == it.column) grouped = true;
                if (!grouped) return std::nullopt;
            } else {
                const AggExpr& a = it.agg;
                if (!a.distinct || a.kind != AggKind::Count || a.column.empty()) return std::nullopt;
                if (da != nullptr) return std::nullopt;  // >1 distinct aggregate -> gather
                da = &a;
            }
        }
        if (da == nullptr) return std::nullopt;
        // Scatter the distinct (gcols, col) tuples (each shard's local DISTINCT via GROUP BY).
        std::string proj;
        std::string group;
        for (const std::string& g : gcols) { proj += g + ", "; group += g + ", "; }
        proj += da->column;
        group += da->column;
        const std::string tuple_sql =
            "SELECT " + proj + " FROM " + sel.table + " GROUP BY " + group;
        struct Grp { std::vector<Datum> gkey; std::map<std::string, char> vals; };
        std::map<std::string, Grp> groups;
        for (ISqlShard* s : shards_) {
            ExecResult part = s->exec(tuple_sql);
            if (!part.ok) return std::nullopt;
            for (const ResultRow& row : part.rows) {
                if (row.cells.size() != gcols.size() + 1) return std::nullopt;
                std::string gk;
                std::vector<Datum> gkey;
                gkey.reserve(gcols.size());
                for (std::size_t j = 0; j < gcols.size(); ++j) {
                    gk += row.cells[j].second.render();
                    gk.push_back('\x1f');
                    gkey.push_back(row.cells[j].second);
                }
                const Datum& val = row.cells[gcols.size()].second;
                Grp& grp = groups[gk];
                if (grp.gkey.empty()) grp.gkey = std::move(gkey);
                if (!val.is_null) grp.vals.emplace(val.render(), 0);  // COUNT(DISTINCT) SKIPS NULL
            }
        }
        std::vector<const Grp*> ordered;
        ordered.reserve(groups.size());
        for (const auto& [k, g] : groups) { (void)k; ordered.push_back(&g); }
        std::sort(ordered.begin(), ordered.end(),
                  [](const Grp* a, const Grp* b) { return key_less(a->gkey, b->gkey); });
        ExecResult r;
        for (const Grp* g : ordered) {
            ResultRow out;
            for (const SelectItem& it : sel.items) {
                if (it.kind == SelectItemKind::Column) {
                    std::size_t gi = 0;
                    for (std::size_t k = 0; k < gcols.size(); ++k)
                        if (gcols[k] == it.column) { gi = k; break; }
                    out.cells.emplace_back(it.label, g->gkey[gi]);
                } else {
                    out.cells.emplace_back(it.label,
                                           Datum::make_int(static_cast<std::int64_t>(g->vals.size())));
                }
            }
            r.rows.push_back(std::move(out));
        }
        r.affected = r.rows.size();
        return r;
    }

    static std::string agg_name(AggKind k) {
        switch (k) {
            case AggKind::Sum: return "SUM";
            case AggKind::Min: return "MIN";
            case AggKind::Max: return "MAX";
            default: return "COUNT";  // CountStar / Count
        }
    }

    // CO-LOCATED SHUFFLE for a star-schema JOIN+aggregate (1 FACT + N DIMS). Each shard aggregates
    // its FACT rows BY THE COMPOSITE JOIN KEY (all the foreign keys; no join / no dim there); the
    // small per-key partials merge; then each (tiny) dim is gathered + joined + the result rolled up
    // by the dim group columns. The large fact is NEVER gathered. Supports WHERE (split fact/per-dim),
    // HAVING (coordinator post-filter), and AVG (SUM/COUNT split). Returns nullopt (fall back to
    // gather-and-run) unless the shape fits: >=2 tables, every dim an INNER equi-join to the FACT
    // (fact.fk = dim.key) — a dim joining ANOTHER dim (snowflake chain) bails; GROUP BY only DIM
    // columns; aggregates only over the FACT (or COUNT(*)); no DISTINCT / ARRAY_AGG / JSON_AGG.
    // Byte-identical to single-node (the aggregates roll up exactly); the conformance gate checks.
    std::optional<ExecResult> try_star_join_pushdown(const SelectStmt& sel) {
        const std::size_t nt = sel.from.size();
        if (nt < 2 || !sel.has_aggregates || sel.group_by.empty()) return std::nullopt;
        const std::string& fa = sel.from[0].alias;  // fact (streamed/large)

        // Collect the dims (from[1..]). Each must be an INNER equi-join fact.fk = dim.key; an ON
        // that references another dim (snowflake) or a non-equi shape -> bail to gather.
        struct Dim {
            std::string alias;
            std::string table;
            std::string fk_col;   // FACT column joining this dim
            std::string key_col;  // this dim's key column
            std::vector<std::string> gcols;  // this dim's GROUP BY columns (in GROUP BY order)
            std::map<std::string, std::vector<Datum>> by_key;  // key-render -> gcol datums (gcols order)
        };
        std::vector<Dim> dims;
        for (std::size_t i = 1; i < nt; ++i) {
            const JoinEntry& je = sel.from[i];
            if (je.kind != JoinKind::Inner || !je.on.present() || je.on.root < 0) return std::nullopt;
            const PredNode& on = je.on.nodes[static_cast<std::size_t>(je.on.root)];
            if (on.kind != PredNodeKind::Cmp || on.op != CmpOp::Eq ||
                on.operand != OperandKind::Column || !on.rhs_is_column) {
                return std::nullopt;
            }
            Dim d;
            d.alias = je.alias;
            d.table = je.table;
            if (on.qualifier == fa && on.rhs_qualifier == d.alias) {
                d.fk_col = on.column;
                d.key_col = on.rhs_column;
            } else if (on.qualifier == d.alias && on.rhs_qualifier == fa) {
                d.fk_col = on.rhs_column;
                d.key_col = on.column;
            } else {
                return std::nullopt;  // ON not fact<->this-dim (snowflake / cross-dim) -> gather
            }
            dims.push_back(std::move(d));
        }

        // Map each GROUP BY column to its dim; build the global group-col order (== GROUP BY order).
        std::vector<std::pair<std::string, std::string>> group_cols;  // (qualifier, column), gkey order
        std::vector<std::pair<std::size_t, std::size_t>> gkey_src;    // (dim index, pos in dim.gcols)
        for (const std::string& gc : sel.group_by) {
            std::string q;
            std::string c;
            split_qual(gc, q, c);
            std::size_t di = dims.size();
            for (std::size_t k = 0; k < dims.size(); ++k) if (dims[k].alias == q) { di = k; break; }
            if (di == dims.size()) return std::nullopt;  // group col not a known dim -> bail
            gkey_src.emplace_back(di, dims[di].gcols.size());
            dims[di].gcols.push_back(c);
            group_cols.emplace_back(q, c);
        }
        auto gkey_pos_of = [&](const std::string& q, const std::string& c) -> std::int64_t {
            for (std::size_t i = 0; i < group_cols.size(); ++i)
                if (group_cols[i].second == c && (q.empty() || group_cols[i].first == q))
                    return static_cast<std::int64_t>(i);
            return -1;
        };

        // Plan each SELECT item; build the fact-side aggregate list (in SELECT order). An AVG output
        // expands to TWO fact-side aggregates (SUM + COUNT of the column) merged independently and
        // divided at the end — SUM/COUNT both skip NULL, so SUM/COUNT == single-node AVG exactly.
        struct Plan {
            bool is_col = false;
            bool is_avg = false;
            std::size_t gpos = 0;          // is_col: index into the global group key
            std::size_t agg_pos = 0;       // non-AVG: index of the single fact aggregate
            std::size_t avg_sum_pos = 0;   // AVG: index of the pushed SUM(col)
            std::size_t avg_cnt_pos = 0;   // AVG: index of the pushed COUNT(col)
        };
        std::vector<Plan> plans;
        std::vector<int> agg_kinds;  // fact-side aggregate kinds (== order in fact_sql after the fks)
        std::string agg_sql;
        for (const SelectItem& item : sel.items) {
            Plan p;
            if (item.kind == SelectItemKind::Column) {
                const std::int64_t gp = gkey_pos_of(item.qualifier, item.column);
                if (gp < 0) return std::nullopt;  // a selected column that isn't a dim group column
                p.is_col = true;
                p.gpos = static_cast<std::size_t>(gp);
            } else {
                const AggExpr& a = item.agg;
                // Only additive/min/max/avg kinds merge from partials; DISTINCT and the collection
                // aggregates (ARRAY_AGG/JSON_AGG) cannot — fall back to gather for those.
                if (a.distinct || a.kind == AggKind::ArrayAgg || a.kind == AggKind::JsonAgg) {
                    return std::nullopt;
                }
                if (a.kind == AggKind::Avg) {
                    if (a.qualifier != fa || a.column.empty()) return std::nullopt;  // AVG over a fact col
                    agg_sql += ", SUM(" + a.column + "), COUNT(" + a.column + ")";
                    p.is_avg = true;
                    p.avg_sum_pos = agg_kinds.size();
                    agg_kinds.push_back(static_cast<int>(AggKind::Sum));
                    p.avg_cnt_pos = agg_kinds.size();
                    agg_kinds.push_back(static_cast<int>(AggKind::Count));
                } else {
                    std::string colpart = "*";
                    if (a.kind != AggKind::CountStar) {
                        if (a.qualifier != fa) return std::nullopt;  // aggregate must be over the fact
                        colpart = a.column;
                    }
                    agg_sql += ", " + agg_name(a.kind) + "(" + colpart + ")";
                    p.agg_pos = agg_kinds.size();
                    agg_kinds.push_back(static_cast<int>(a.kind));
                }
            }
            plans.push_back(p);
        }
        if (agg_kinds.empty()) return std::nullopt;

        // HAVING: plan its aggregates onto the fact side and resolve each leaf; applied as a
        // coordinator post-filter on the rolled-up groups (below). Bails to gather on an unsafe shape.
        std::map<std::int32_t, std::size_t> hv_agg_at;
        std::map<std::int32_t, std::size_t> hv_key_at;
        if (sel.having.present() &&
            !plan_having(sel.having, fa, group_cols, agg_kinds, agg_sql, hv_agg_at, hv_key_at)) {
            return std::nullopt;
        }

        // WHERE PUSHDOWN: split the filter into a fact-side fragment and a per-dim fragment. Fact
        // predicates filter rows BEFORE the per-shard aggregate (row-local, commutes with the PK
        // partition); dim predicates filter that gathered dim (inner join drops the unmatched fact
        // partials). A filter that doesn't split cleanly falls back to gather-and-run.
        std::vector<std::string> dim_aliases;
        dim_aliases.reserve(dims.size());
        for (const Dim& d : dims) dim_aliases.push_back(d.alias);
        std::string fact_where;
        std::vector<std::string> dim_wheres(dims.size());
        if (sel.filter.present() &&
            !split_where(sel.filter, fa, dim_aliases, fact_where, dim_wheres)) {
            return std::nullopt;
        }

        // FACT-SIDE per-composite-key aggregate, pushed to every shard; merge the per-key partials.
        std::string fk_list;
        for (std::size_t i = 0; i < dims.size(); ++i) {
            if (i != 0) fk_list += ", ";
            fk_list += dims[i].fk_col;
        }
        const std::string fact_sql = "SELECT " + fk_list + agg_sql + " FROM " + sel.from[0].table +
                                     (fact_where.empty() ? "" : " WHERE " + fact_where) +
                                     " GROUP BY " + fk_list;
        struct FP { std::vector<Datum> fks; std::vector<Datum> aggs; };
        std::map<std::string, FP> per;  // composite-fk-render -> partial
        for (ISqlShard* s : shards_) {
            ExecResult part = s->exec(fact_sql);
            if (!part.ok) return std::nullopt;  // shard can't run it -> fall back
            for (const ResultRow& row : part.rows) {
                if (row.cells.size() != dims.size() + agg_kinds.size()) return std::nullopt;
                std::string ck;
                std::vector<Datum> fks;
                fks.reserve(dims.size());
                for (std::size_t j = 0; j < dims.size(); ++j) {
                    ck += row.cells[j].second.render();
                    ck.push_back('\x1f');
                    fks.push_back(row.cells[j].second);
                }
                auto it = per.find(ck);
                if (it == per.end()) {
                    std::vector<Datum> av;
                    av.reserve(agg_kinds.size());
                    for (std::size_t i = 0; i < agg_kinds.size(); ++i)
                        av.push_back(row.cells[dims.size() + i].second);
                    per.emplace(ck, FP{std::move(fks), std::move(av)});
                } else {
                    for (std::size_t i = 0; i < agg_kinds.size(); ++i)
                        combine(it->second.aggs[i], row.cells[dims.size() + i].second, agg_kinds[i]);
                }
            }
        }
        // Gather each (small) DIM, applying its dim-side WHERE so only surviving keys join. A dim row
        // with a NULL key can never inner-join (NULL = anything is UNKNOWN) — skip it.
        for (std::size_t di = 0; di < dims.size(); ++di) {
            Dim& d = dims[di];
            const std::string dim_sql = "SELECT * FROM " + d.table +
                                        (dim_wheres[di].empty() ? "" : " WHERE " + dim_wheres[di]);
            for (ISqlShard* s : shards_) {
                ExecResult part = s->exec(dim_sql);
                if (!part.ok) return std::nullopt;
                for (const ResultRow& row : part.rows) {
                    std::string keyr;
                    bool key_null = true;
                    std::vector<Datum> gvals(d.gcols.size());
                    for (const auto& [label, dv] : row.cells) {
                        if (label == d.key_col) { keyr = dv.render(); key_null = dv.is_null; }
                        for (std::size_t k = 0; k < d.gcols.size(); ++k)
                            if (label == d.gcols[k]) { gvals[k] = dv; break; }
                    }
                    if (!key_null) d.by_key[keyr] = std::move(gvals);
                }
            }
        }
        // ROLL UP: each merged fact partial -> look up every dim by its fk -> assemble the global
        // group key -> combine the aggregates. A miss in ANY dim drops the partial (inner join).
        struct G { std::vector<Datum> gkey; std::vector<Datum> aggs; };
        std::map<std::string, G> groups;
        for (const auto& [ck, fp] : per) {
            (void)ck;
            std::vector<const std::vector<Datum>*> dvals(dims.size(), nullptr);
            bool matched = true;
            for (std::size_t di = 0; di < dims.size(); ++di) {
                const auto it = dims[di].by_key.find(fp.fks[di].render());
                if (it == dims[di].by_key.end()) { matched = false; break; }
                dvals[di] = &it->second;
            }
            if (!matched) continue;
            std::vector<Datum> gkey;
            gkey.reserve(group_cols.size());
            for (const auto& [di, pos] : gkey_src) gkey.push_back((*dvals[di])[pos]);
            std::string gk;
            for (const Datum& d : gkey) { gk += d.render(); gk.push_back('\x1f'); }
            auto git = groups.find(gk);
            if (git == groups.end()) {
                groups.emplace(gk, G{std::move(gkey), fp.aggs});
            } else {
                for (std::size_t i = 0; i < agg_kinds.size(); ++i)
                    combine(git->second.aggs[i], fp.aggs[i], agg_kinds[i]);
            }
        }
        // Order the groups by the group key (matches single-node GROUP BY order).
        std::vector<const G*> ordered;
        ordered.reserve(groups.size());
        for (const auto& [k, g] : groups) { (void)k; ordered.push_back(&g); }
        std::sort(ordered.begin(), ordered.end(),
                  [](const G* a, const G* b) { return key_less(a->gkey, b->gkey); });
        ExecResult r;
        for (const G* g : ordered) {
            if (sel.having.present()) {
                bool keep = false;
                if (!eval_having(sel.having, sel.having.root, g->aggs, g->gkey, hv_agg_at, hv_key_at,
                                 keep)) {
                    return std::nullopt;  // unresolved/type-mismatch HAVING — gather reproduces it
                }
                if (!keep) continue;  // group filtered out by HAVING
            }
            ResultRow out;
            for (std::size_t i = 0; i < plans.size(); ++i) {
                const Plan& p = plans[i];
                const std::string& label = sel.items[i].label;  // match single-node output labels
                if (p.is_col) {
                    out.cells.emplace_back(label, g->gkey[p.gpos]);
                } else if (p.is_avg) {
                    // AVG = Σsum / Σcount (non-null), INT trunc toward zero (== single-node). A
                    // count==0 (all-NULL) group renders a TYPED NULL single-node; rather than
                    // reconstruct that exact null, fall back to gather (rare, still correct).
                    const std::int64_t cnt = g->aggs[p.avg_cnt_pos].i;
                    if (cnt == 0) return std::nullopt;
                    out.cells.emplace_back(label, Datum::make_int(g->aggs[p.avg_sum_pos].i / cnt));
                } else {
                    out.cells.emplace_back(label, g->aggs[p.agg_pos]);
                }
            }
            r.rows.push_back(std::move(out));
        }
        r.affected = r.rows.size();
        return r;
    }

    // The SQL text for a comparison op that re-parses to the same CmpOp, or nullptr for
    // an op we won't push down (LIKE/Contains — pattern/JSON semantics we don't re-serialize).
    [[nodiscard]] static const char* cmp_op_sql(CmpOp op) {
        switch (op) {
            case CmpOp::Eq: return "=";
            case CmpOp::Ne: return "!=";
            case CmpOp::Lt: return "<";
            case CmpOp::Le: return "<=";
            case CmpOp::Gt: return ">";
            case CmpOp::Ge: return ">=";
            case CmpOp::Like:
            case CmpOp::Contains:
                return nullptr;
        }
        return nullptr;
    }

    // Split a star-join WHERE into a fact-side fragment and a per-dim fragment so each can be
    // pushed to its table independently. Returns false (caller falls back to gather) on ANY
    // shape that does not split cleanly into single-table simple comparisons ANDed together:
    //   * a non-AND/non-Cmp node (OR / NOT / IS NULL / IN / EXISTS) — can't safely route,
    //   * a Cmp whose LHS is not a plain column (Agg / scalar Expr), or whose RHS is a column
    //     / subquery / ANY|ALL array, or a LIKE/Contains op,
    //   * a NULL / non-Int-non-Text literal (sql_literal may not round-trip it),
    //   * a leaf qualified by neither the fact nor any dim alias.
    // Each leaf goes WHOLE to ONE table's fragment (no leaf spans tables), so filtering each side
    // then inner-joining is byte-identical to filtering the joined rows (AND only).
    [[nodiscard]] static bool split_where(const Predicate& pred, const std::string& fa,
                                          const std::vector<std::string>& dim_aliases,
                                          std::string& fact_where,
                                          std::vector<std::string>& dim_wheres) {
        std::vector<std::int32_t> stack{pred.root};
        while (!stack.empty()) {
            const std::int32_t idx = stack.back();
            stack.pop_back();
            if (idx < 0 || static_cast<std::size_t>(idx) >= pred.nodes.size()) return false;
            const PredNode& n = pred.nodes[static_cast<std::size_t>(idx)];
            if (n.kind == PredNodeKind::And) {
                stack.push_back(n.left);
                stack.push_back(n.right);
                continue;
            }
            if (n.kind != PredNodeKind::Cmp) return false;
            if (n.operand != OperandKind::Column) return false;
            if (n.rhs_is_column || n.rhs_is_subquery || n.any_quant || n.all_quant) return false;
            if (n.literal.is_null ||
                (n.literal.type != Type::Int && n.literal.type != Type::Text)) {
                return false;
            }
            const char* ops = cmp_op_sql(n.op);
            if (ops == nullptr) return false;
            std::string* side = nullptr;
            if (n.qualifier == fa) {
                side = &fact_where;
            } else {
                for (std::size_t i = 0; i < dim_aliases.size(); ++i)
                    if (n.qualifier == dim_aliases[i]) { side = &dim_wheres[i]; break; }
            }
            if (side == nullptr) return false;  // unqualified / unknown table — can't route safely
            if (!side->empty()) *side += " AND ";
            *side += n.column + " " + ops + " " + sql_literal(n.literal);
        }
        return true;
    }

    // Compare two scalar datums under a CmpOp (the 6 ordered ops; INT numeric, TEXT lexical) —
    // the coordinator equivalent of the engine's leaf_truth for HAVING evaluation.
    [[nodiscard]] static bool datum_cmp(CmpOp op, const Datum& a, const Datum& b) {
        int c = 0;
        if (a.type == Type::Int) {
            c = (a.i < b.i) ? -1 : (a.i > b.i) ? 1 : 0;
        } else {
            const int sc = a.s.compare(b.s);
            c = (sc < 0) ? -1 : (sc > 0) ? 1 : 0;
        }
        switch (op) {
            case CmpOp::Eq: return c == 0;
            case CmpOp::Ne: return c != 0;
            case CmpOp::Lt: return c < 0;
            case CmpOp::Le: return c <= 0;
            case CmpOp::Gt: return c > 0;
            case CmpOp::Ge: return c >= 0;
            default: return false;
        }
    }

    // Plan a star-join HAVING for coordinator post-filtering. Every aggregate the HAVING references
    // is pushed onto the FACT side too (so each rolled-up group carries its value), and each Cmp leaf
    // is resolved to an aggregate position (agg_at) or a group-key index (key_at) into the global
    // group columns. Returns false (caller falls back to gather, which runs HAVING single-node —
    // byte-identical incl any error) on any shape that isn't pushdown-safe: a non-And/Or/Not/Cmp
    // node, a fact aggregate that is AVG / DISTINCT / a collection agg / not over the fact, a column
    // that is not a GROUP BY column, a column/subquery/quantifier RHS, a LIKE/Contains op, or a
    // NULL / non-Int-non-Text literal.
    [[nodiscard]] static bool plan_having(
        const Predicate& pred, const std::string& fa,
        const std::vector<std::pair<std::string, std::string>>& group_cols,
        std::vector<int>& agg_kinds, std::string& agg_sql,
        std::map<std::int32_t, std::size_t>& agg_at,
        std::map<std::int32_t, std::size_t>& key_at) {
        std::vector<std::int32_t> stack{pred.root};
        while (!stack.empty()) {
            const std::int32_t idx = stack.back();
            stack.pop_back();
            if (idx < 0 || static_cast<std::size_t>(idx) >= pred.nodes.size()) return false;
            const PredNode& n = pred.nodes[static_cast<std::size_t>(idx)];
            if (n.kind == PredNodeKind::And || n.kind == PredNodeKind::Or) {
                stack.push_back(n.left);
                stack.push_back(n.right);
                continue;
            }
            if (n.kind == PredNodeKind::Not) {
                stack.push_back(n.left);
                continue;
            }
            if (n.kind != PredNodeKind::Cmp) return false;
            if (n.rhs_is_column || n.rhs_is_subquery || n.any_quant || n.all_quant) return false;
            if (cmp_op_sql(n.op) == nullptr) return false;
            if (n.literal.is_null ||
                (n.literal.type != Type::Int && n.literal.type != Type::Text)) {
                return false;
            }
            if (n.operand == OperandKind::Agg) {
                const AggExpr& a = n.agg;
                if (a.distinct || a.kind == AggKind::Avg || a.kind == AggKind::ArrayAgg ||
                    a.kind == AggKind::JsonAgg) {
                    return false;
                }
                std::string colpart = "*";
                if (a.kind != AggKind::CountStar) {
                    if (a.qualifier != fa || a.column.empty()) return false;  // agg over the fact only
                    colpart = a.column;
                }
                agg_sql += ", " + agg_name(a.kind) + "(" + colpart + ")";
                agg_at[idx] = agg_kinds.size();
                agg_kinds.push_back(static_cast<int>(a.kind));
            } else if (n.operand == OperandKind::Column) {
                std::size_t gi = 0;
                bool found = false;
                for (std::size_t k = 0; k < group_cols.size(); ++k) {
                    if (group_cols[k].second == n.column &&
                        (n.qualifier.empty() || group_cols[k].first == n.qualifier)) {
                        gi = k;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;  // HAVING column must be a GROUP BY column
                key_at[idx] = gi;
            } else {
                return false;  // scalar Expr LHS
            }
        }
        return true;
    }

    // Evaluate a planned HAVING for one rolled-up group. `ok` is the truth; the bool return is
    // whether evaluation SUCCEEDED (false => an unresolved leaf / type mismatch => caller bails to
    // gather). Mirrors single-node HAVING: NULL operand => false; AND/OR short-circuit; NOT negates.
    [[nodiscard]] static bool eval_having(const Predicate& pred, std::int32_t node,
                                          const std::vector<Datum>& aggs,
                                          const std::vector<Datum>& gkey,
                                          const std::map<std::int32_t, std::size_t>& agg_at,
                                          const std::map<std::int32_t, std::size_t>& key_at,
                                          bool& ok) {
        if (node < 0) { ok = true; return true; }
        const PredNode& n = pred.nodes[static_cast<std::size_t>(node)];
        if (n.kind == PredNodeKind::And) {
            bool l = false;
            if (!eval_having(pred, n.left, aggs, gkey, agg_at, key_at, l)) return false;
            if (!l) { ok = false; return true; }
            return eval_having(pred, n.right, aggs, gkey, agg_at, key_at, ok);
        }
        if (n.kind == PredNodeKind::Or) {
            bool l = false;
            if (!eval_having(pred, n.left, aggs, gkey, agg_at, key_at, l)) return false;
            if (l) { ok = true; return true; }
            return eval_having(pred, n.right, aggs, gkey, agg_at, key_at, ok);
        }
        if (n.kind == PredNodeKind::Not) {
            bool c = false;
            if (!eval_having(pred, n.left, aggs, gkey, agg_at, key_at, c)) return false;
            ok = !c;
            return true;
        }
        Datum lhs;
        if (const auto ai = agg_at.find(node); ai != agg_at.end()) {
            if (ai->second >= aggs.size()) return false;
            lhs = aggs[ai->second];
        } else if (const auto ki = key_at.find(node); ki != key_at.end()) {
            if (ki->second >= gkey.size()) return false;
            lhs = gkey[ki->second];
        } else {
            return false;
        }
        const Datum& rhs = n.literal;
        if (lhs.is_null || rhs.is_null) { ok = false; return true; }
        if (lhs.type != rhs.type) return false;  // single-node errors here; gather reproduces it
        ok = datum_cmp(n.op, lhs, rhs);
        return true;
    }

    static void split_qual(const std::string& s, std::string& q, std::string& c) {
        const auto dot = s.find('.');
        if (dot == std::string::npos) { q.clear(); c = s; }
        else { q = s.substr(0, dot); c = s.substr(dot + 1); }
    }

    // Render a Datum back to a SQL literal that re-parses to the same value.
    [[nodiscard]] static std::string sql_literal(const Datum& d) {
        if (d.is_null) return "NULL";
        if (d.type == Type::Int) return std::to_string(d.i);
        std::string out = "'";
        for (const char c : d.s) {
            if (c == '\'') out += "''";  // SQL single-quote escaping
            else out.push_back(c);
        }
        out += "'";
        return out;
    }

    // GATHER each table's rows from all shards into a fresh local engine, then run the original
    // query there. Byte-identical to single-node (the local engine IS a single node over the union
    // of the rows). Tables must have a remembered CREATE.
    ExecResult gather_and_run(const std::string& original_sql,
                              const std::vector<std::string>& tables) {
        SqlEngine local;  // row-mode: result == columnar == reference, simpler
        for (const std::string& table : tables) {
            const auto it = create_sql_.find(table);
            if (it == create_sql_.end()) {
                return ExecResult::failure("distributed: no schema for table '" + table + "'");
            }
            const ExecResult cr = local.exec(it->second);
            if (!cr.ok) return cr;
            for (ISqlShard* s : shards_) {
                const ExecResult part = s->exec("SELECT * FROM " + table);
                if (!part.ok) return part;
                for (const ResultRow& row : part.rows) {
                    std::string cols;
                    std::string vals;
                    for (const auto& [label, d] : row.cells) {
                        cols += label + ",";
                        vals += sql_literal(d) + ",";
                    }
                    if (!cols.empty()) {
                        cols.pop_back();
                        vals.pop_back();
                    }
                    const ExecResult ins =
                        local.exec("INSERT INTO " + table + " (" + cols + ") VALUES (" + vals + ")");
                    if (!ins.ok) return ins;
                }
            }
        }
        return local.exec(original_sql);
    }

    // -1 == a group-key column; else the AggKind of that output item.
    [[nodiscard]] static std::vector<int> item_roles(const SelectStmt& sel) {
        std::vector<int> roles(sel.items.size(), -1);
        for (std::size_t i = 0; i < sel.items.size(); ++i) {
            if (sel.items[i].kind == SelectItemKind::Aggregate) {
                roles[i] = static_cast<int>(sel.items[i].agg.kind);
            }
        }
        return roles;
    }

    // Merge a per-shard partial `v` into the running `acc` for one aggregate. NULL-SAFE: a NULL
    // partial is "no value from this shard" (e.g. a shard whose join slice is empty for this group),
    // so it contributes 0 to COUNT/SUM and is skipped for MIN/MAX — exactly single-node NULL-skip
    // semantics. Without this, a NULL partial's default .i (0) would corrupt a MIN/MAX merge.
    static void combine(Datum& acc, const Datum& v, int kind) {
        const AggKind k = static_cast<AggKind>(kind);
        if (k == AggKind::CountStar || k == AggKind::Count || k == AggKind::Sum) {
            const std::int64_t add = v.is_null ? 0 : v.i;
            acc = Datum::make_int((acc.is_null ? 0 : acc.i) + add);
        } else if (k == AggKind::Min) {
            if (v.is_null) return;
            if (acc.is_null || v.i < acc.i) acc = v;
        } else if (k == AggKind::Max) {
            if (v.is_null) return;
            if (acc.is_null || v.i > acc.i) acc = v;
        }
    }

    // Order two group-key cell vectors as single-node GROUP BY does (INT numeric, TEXT lexical).
    [[nodiscard]] static bool key_less(const std::vector<Datum>& a, const std::vector<Datum>& b) {
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (a[i].type == Type::Int) {
                if (a[i].i != b[i].i) return a[i].i < b[i].i;
            } else {
                if (a[i].s != b[i].s) return a[i].s < b[i].s;
            }
        }
        return false;
    }

    ExecResult merge_aggregate(const std::vector<ExecResult>& parts, const SelectStmt& sel) {
        const std::vector<int> roles = item_roles(sel);
        for (const int r : roles) {
            if (r == static_cast<int>(AggKind::Avg)) {
                return ExecResult::failure(
                    "distributed: AVG across shards unsupported (needs SUM/COUNT split)");
            }
        }
        // C1: a DISTINCT aggregate cannot be merged from per-shard partials — a value present on two
        // shards would be counted/summed twice. Reject (like AVG) until a global-distinct shuffle.
        for (const SelectItem& it : sel.items) {
            if (it.kind != SelectItemKind::Column && it.agg.distinct) {
                return ExecResult::failure(
                    "distributed: DISTINCT aggregate across shards unsupported (needs a global shuffle)");
            }
        }
        struct Group {
            std::vector<Datum> key;  // the group-key column cells (for ordering)
            ResultRow row;           // the merged output row
        };
        std::map<std::string, Group> groups;  // keyed by rendered group key
        for (const ExecResult& p : parts) {
            for (const ResultRow& row : p.rows) {
                std::string gk;
                std::vector<Datum> keyd;
                for (std::size_t i = 0; i < roles.size(); ++i) {
                    if (roles[i] < 0) {
                        gk += row.cells[i].second.render();
                        gk.push_back('\x1f');
                        keyd.push_back(row.cells[i].second);
                    }
                }
                auto it = groups.find(gk);
                if (it == groups.end()) {
                    groups.emplace(gk, Group{std::move(keyd), row});
                } else {
                    for (std::size_t i = 0; i < roles.size(); ++i) {
                        if (roles[i] >= 0) {
                            combine(it->second.row.cells[i].second, row.cells[i].second, roles[i]);
                        }
                    }
                }
            }
        }
        std::vector<Group> ordered;
        ordered.reserve(groups.size());
        for (auto& [k, g] : groups) {
            (void)k;
            ordered.push_back(std::move(g));
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const Group& a, const Group& b) { return key_less(a.key, b.key); });
        ExecResult r;
        for (Group& g : ordered) r.rows.push_back(std::move(g.row));
        if (sel.has_limit) {
            const std::size_t lim = static_cast<std::size_t>(sel.limit < 0 ? 0 : sel.limit);
            if (r.rows.size() > lim) r.rows.resize(lim);
        }
        r.affected = r.rows.size();
        return r;
    }

    ExecResult merge_scan(const std::vector<ExecResult>& parts, const SelectStmt& sel) {
        ExecResult r;
        for (const ExecResult& p : parts) {
            for (const ResultRow& row : p.rows) r.rows.push_back(row);
        }
        if (sel.has_limit) {
            const std::size_t lim = static_cast<std::size_t>(sel.limit < 0 ? 0 : sel.limit);
            if (r.rows.size() > lim) r.rows.resize(lim);
        }
        r.affected = r.rows.size();
        return r;
    }

    std::vector<ISqlShard*> shards_;
    std::map<std::string, std::string> pk_;          // table -> PK column (for INSERT routing)
    std::map<std::string, std::string> create_sql_;  // table -> CREATE (to rebuild a local copy)
    std::set<std::string> replicated_;  // tables held in FULL on every shard (writes broadcast)
    std::size_t pushdowns_ = 0;  // star-schema pushdowns taken (vs the gather fallback)
    std::size_t distinct_shuffles_ = 0;  // global-distinct shuffles taken (vs the gather fallback)
    std::size_t broadcast_joins_ = 0;  // broadcast-dim joins taken (replicated dims, per-shard join)

public:
    [[nodiscard]] std::size_t pushdowns() const { return pushdowns_; }
    [[nodiscard]] std::size_t distinct_shuffles() const { return distinct_shuffles_; }
    [[nodiscard]] std::size_t broadcast_joins() const { return broadcast_joins_; }
};

}  // namespace lockstep::query::sql
