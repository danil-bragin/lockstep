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
                return route(sql, pk_value_of(st.insert));
            case StmtKind::Update:
                return route(sql, st.update.where_value);
            case StmtKind::Delete:
                return route(sql, st.del.where_value);
            case StmtKind::Select:
                return exec_select(sql, st.select);
            case StmtKind::ShowTables:  // E5: introspection -> any shard (schema is replicated)
            case StmtKind::Describe:
                return shards_.front()->exec(sql);
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

    ExecResult exec_select(const std::string& sql, const SelectStmt& sel) {
        // JOIN, or a non-aggregate scan that needs GLOBAL ORDER (ORDER BY): GATHER every involved
        // table's rows from all shards into a fresh local engine and run the ORIGINAL query there —
        // the engine's verified JOIN / ORDER BY logic produces a result byte-identical to a single
        // node. (Baseline broadcast-style distributed JOIN; a shuffle/co-located join is a perf
        // follow-on.) Aggregates + unordered scans + point reads stay on the efficient
        // scatter-merge path below.
        if (sel.is_join()) {
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
        std::vector<ExecResult> parts;
        parts.reserve(shards_.size());
        for (ISqlShard* s : shards_) {
            parts.push_back(s->exec(sql));
            if (!parts.back().ok) return parts.back();  // propagate the first shard error
        }
        if (sel.has_aggregates) return merge_aggregate(parts, sel);
        return merge_scan(parts, sel);  // unordered projection scan: concat (order unspecified)
    }

    static std::string agg_name(AggKind k) {
        switch (k) {
            case AggKind::Sum: return "SUM";
            case AggKind::Min: return "MIN";
            case AggKind::Max: return "MAX";
            default: return "COUNT";  // CountStar / Count
        }
    }

    // CO-LOCATED SHUFFLE for a star-schema JOIN+aggregate: each shard aggregates its FACT rows BY
    // THE JOIN KEY (no join / no dim needed there); the small per-key partials merge; then the tiny
    // dim is gathered + joined + the result rolled up by the dim group columns. The large fact is
    // NEVER gathered. Returns nullopt (fall back to gather-and-run) unless the shape fits: 2 tables,
    // INNER equi-join (fact.fk = dim.key), GROUP BY only DIM columns, aggregates only over the FACT
    // (or COUNT(*)) with additive/min/max kinds (no AVG), no WHERE/HAVING. Byte-identical to
    // single-node (the aggregates roll up exactly); the distributed conformance gate cross-checks.
    std::optional<ExecResult> try_star_join_pushdown(const SelectStmt& sel) {
        if (sel.from.size() != 2 || sel.from[1].kind != JoinKind::Inner) return std::nullopt;
        if (sel.filter.present() || sel.having.present() || !sel.has_aggregates) return std::nullopt;
        if (sel.group_by.empty()) return std::nullopt;
        if (!sel.from[1].on.present() || sel.from[1].on.root < 0) return std::nullopt;
        const PredNode& on = sel.from[1].on.nodes[static_cast<std::size_t>(sel.from[1].on.root)];
        if (on.kind != PredNodeKind::Cmp || on.op != CmpOp::Eq ||
            on.operand != OperandKind::Column || !on.rhs_is_column) {
            return std::nullopt;
        }
        const std::string& fa = sel.from[0].alias;  // fact (streamed/large)
        const std::string& da = sel.from[1].alias;  // dim (small)
        std::string fk_col;
        std::string dim_key_col;
        if (on.qualifier == fa && on.rhs_qualifier == da) {
            fk_col = on.column;
            dim_key_col = on.rhs_column;
        } else if (on.qualifier == da && on.rhs_qualifier == fa) {
            fk_col = on.rhs_column;
            dim_key_col = on.column;
        } else {
            return std::nullopt;
        }
        std::vector<std::string> dim_gcols;  // GROUP BY columns (must all be dim)
        for (const std::string& gc : sel.group_by) {
            std::string q;
            std::string c;
            split_qual(gc, q, c);
            if (q != da) return std::nullopt;
            dim_gcols.push_back(c);
        }
        // Plan each SELECT item; build the fact-side aggregate list (in SELECT order).
        struct Plan { bool is_col = false; std::string dim_col; std::size_t agg_pos = 0; };
        std::vector<Plan> plans;
        std::vector<int> agg_kinds;  // fact-side aggregate kinds (== order in fact_sql after fk)
        std::string agg_sql;
        for (const SelectItem& item : sel.items) {
            Plan p;
            if (item.kind == SelectItemKind::Column) {
                if (item.qualifier != da) return std::nullopt;
                bool grouped = false;
                for (const std::string& g : dim_gcols) if (g == item.column) grouped = true;
                if (!grouped) return std::nullopt;
                p.is_col = true;
                p.dim_col = item.column;
            } else {
                const AggExpr& a = item.agg;
                if (a.kind == AggKind::Avg) return std::nullopt;  // needs sum+count split
                std::string colpart = "*";
                if (a.kind != AggKind::CountStar) {
                    if (a.qualifier != fa) return std::nullopt;  // aggregate must be over the fact
                    colpart = a.column;
                }
                agg_sql += ", " + agg_name(a.kind) + "(" + colpart + ")";
                p.agg_pos = agg_kinds.size();
                agg_kinds.push_back(static_cast<int>(a.kind));
            }
            plans.push_back(p);
        }
        if (agg_kinds.empty()) return std::nullopt;

        // FACT-SIDE per-key aggregate, pushed to every shard; merge the (small) per-key partials.
        const std::string fact_sql = "SELECT " + fk_col + agg_sql + " FROM " + sel.from[0].table +
                                     " GROUP BY " + fk_col;
        std::map<std::string, std::vector<Datum>> per_fk;  // fk-render -> [agg datums]
        for (ISqlShard* s : shards_) {
            ExecResult part = s->exec(fact_sql);
            if (!part.ok) return std::nullopt;  // shard can't run it -> fall back
            for (const ResultRow& row : part.rows) {
                if (row.cells.size() != agg_kinds.size() + 1) return std::nullopt;
                const std::string k = row.cells[0].second.render();
                auto it = per_fk.find(k);
                if (it == per_fk.end()) {
                    std::vector<Datum> av;
                    av.reserve(agg_kinds.size());
                    for (std::size_t i = 0; i < agg_kinds.size(); ++i) av.push_back(row.cells[i + 1].second);
                    per_fk.emplace(k, std::move(av));
                } else {
                    for (std::size_t i = 0; i < agg_kinds.size(); ++i)
                        combine(it->second[i], row.cells[i + 1].second, agg_kinds[i]);
                }
            }
        }
        // Gather the small DIM (key-render -> the dim group-column datums).
        std::map<std::string, std::vector<Datum>> dim_by_key;
        for (ISqlShard* s : shards_) {
            ExecResult part = s->exec("SELECT * FROM " + sel.from[1].table);
            if (!part.ok) return std::nullopt;
            for (const ResultRow& row : part.rows) {
                std::string keyr;
                std::vector<Datum> gvals;
                gvals.reserve(dim_gcols.size());
                for (const auto& [label, d] : row.cells) {
                    if (label == dim_key_col) keyr = d.render();
                    for (const std::string& g : dim_gcols)
                        if (label == g) { gvals.push_back(d); break; }
                }
                if (!keyr.empty() || true) dim_by_key[keyr] = std::move(gvals);
            }
        }
        // ROLL UP: each merged per-key partial -> its dim group -> combine the aggregates.
        struct G { std::vector<Datum> gkey; std::vector<Datum> aggs; };
        std::map<std::string, G> groups;
        for (const auto& [fkr, av] : per_fk) {
            const auto dit = dim_by_key.find(fkr);
            if (dit == dim_by_key.end()) continue;  // inner join: no dim match
            std::string gk;
            for (const Datum& d : dit->second) { gk += d.render(); gk.push_back('\x1f'); }
            auto git = groups.find(gk);
            if (git == groups.end()) {
                groups.emplace(gk, G{dit->second, av});
            } else {
                for (std::size_t i = 0; i < agg_kinds.size(); ++i)
                    combine(git->second.aggs[i], av[i], agg_kinds[i]);
            }
        }
        // Order the groups by the dim group key (matches single-node GROUP BY order).
        std::vector<const G*> ordered;
        ordered.reserve(groups.size());
        for (const auto& [k, g] : groups) { (void)k; ordered.push_back(&g); }
        std::sort(ordered.begin(), ordered.end(),
                  [](const G* a, const G* b) { return key_less(a->gkey, b->gkey); });
        ExecResult r;
        for (const G* g : ordered) {
            ResultRow out;
            for (std::size_t i = 0; i < plans.size(); ++i) {
                const Plan& p = plans[i];
                const std::string& label = sel.items[i].label;  // match single-node output labels
                if (p.is_col) {
                    std::size_t gi = 0;
                    for (std::size_t k = 0; k < dim_gcols.size(); ++k)
                        if (dim_gcols[k] == p.dim_col) { gi = k; break; }
                    out.cells.emplace_back(label, g->gkey[gi]);
                } else {
                    out.cells.emplace_back(label, g->aggs[p.agg_pos]);
                }
            }
            r.rows.push_back(std::move(out));
        }
        r.affected = r.rows.size();
        return r;
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

    static void combine(Datum& acc, const Datum& v, int kind) {
        const AggKind k = static_cast<AggKind>(kind);
        if (k == AggKind::CountStar || k == AggKind::Count || k == AggKind::Sum) {
            acc = Datum::make_int(acc.i + v.i);
        } else if (k == AggKind::Min) {
            if (v.i < acc.i) acc = v;
        } else if (k == AggKind::Max) {
            if (v.i > acc.i) acc = v;
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
    std::size_t pushdowns_ = 0;  // star-schema pushdowns taken (vs the gather fallback)

public:
    [[nodiscard]] std::size_t pushdowns() const { return pushdowns_; }
};

}  // namespace lockstep::query::sql
