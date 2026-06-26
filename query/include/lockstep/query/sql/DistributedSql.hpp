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
                return broadcast(sql);
            case StmtKind::Insert:
                return route(sql, pk_value_of(st.insert));
            case StmtKind::Update:
                return route(sql, st.update.where_value);
            case StmtKind::Delete:
                return route(sql, st.del.where_value);
            case StmtKind::Select:
                return exec_select(sql, st.select);
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
};

}  // namespace lockstep::query::sql
