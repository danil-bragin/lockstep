// dist_join_bench.cpp — DISTRIBUTED STAR-JOIN: the co-located-shuffle pushdown vs the
// gather-the-fact baseline, on the SAME shards / data / query. The pushdown aggregates the large
// FACT by the join key ON EACH SHARD and ships only the per-key partials (cardinality = distinct
// keys) — the fact is NEVER gathered. The baseline (DistributedSql with the pushdown disabled)
// gathers the whole fact to the coordinator and runs the join there. Both produce a BYTE-IDENTICAL
// result; this measures the structural win.
//
//   build: c++ -std=c++23 -O2 -DNDEBUG -I<modules>/include dist_join_bench.cpp -o dist_join_bench
//   run:   ./dist_join_bench [FACT_ROWS=1000000] [SHARDS=8] [DISTINCT_FK=1000] [DIM_GROUPS=8]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/query/sql/DistributedSql.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    const std::uint64_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    const std::size_t M = argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 8;
    const std::uint64_t FK = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1000;  // distinct join keys
    const std::uint64_t G = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 8;       // dim group labels

    std::vector<SqlEngine> shards(M);
    std::vector<EngineSqlShard> wraps;
    wraps.reserve(M);
    for (SqlEngine& s : shards) { s.set_columnar_default(true); wraps.emplace_back(&s); }
    std::vector<ISqlShard*> ptrs;
    for (EngineSqlShard& w : wraps) ptrs.push_back(&w);
    DistributedSql dist(ptrs);

    dist.exec("CREATE TABLE fact (id INT, fk INT NOT NULL, amt INT NOT NULL, PRIMARY KEY (id))");
    dist.exec("CREATE TABLE dim (k INT, label TEXT NOT NULL, PRIMARY KEY (k))");
    // dim: FK distinct keys, each mapped to one of G coarse labels (the star's grouping column).
    for (std::uint64_t k = 0; k < FK; ++k) {
        dist.exec("INSERT INTO dim (k,label) VALUES (" + std::to_string(k) + ",'g" +
                  std::to_string(k % G) + "')");
    }
    std::printf("loading %llu fact rows across %zu shards (fk-cardinality %llu, %llu dim groups)...\n",
                (unsigned long long)N, M, (unsigned long long)FK, (unsigned long long)G);
    const auto l0 = Clock::now();
    for (std::uint64_t i = 0; i < N; ++i) {
        const std::uint64_t amt = (i * 2654435761ULL) % 1000;
        dist.exec("INSERT INTO fact (id,fk,amt) VALUES (" + std::to_string(i) + "," +
                  std::to_string(i % FK) + "," + std::to_string(amt) + ")");
    }
    for (SqlEngine& s : shards) s.flush_columnar("fact");
    std::printf("loaded in %.0f ms\n", ms(l0, Clock::now()));

    const std::string q =
        "SELECT dim.label, COUNT(*), SUM(fact.amt) FROM fact JOIN dim ON fact.fk = dim.k "
        "GROUP BY dim.label";

    auto render = [](const ExecResult& r) {
        std::string s;
        for (const ResultRow& row : r.rows) {
            for (const auto& [l, d] : row.cells) s += d.render() + " ";
            s += "| ";
        }
        return s;
    };

    // A — co-located-shuffle pushdown ON (fact aggregated per-key on each shard; never gathered).
    dist.set_pushdown_enabled(true);
    const auto a0 = Clock::now();
    const ExecResult rp = dist.exec(q);
    const double ms_push = ms(a0, Clock::now());

    // B — pushdown OFF: gather the whole fact to the coordinator and join there.
    dist.set_pushdown_enabled(false);
    const auto b0 = Clock::now();
    const ExecResult rg = dist.exec(q);
    const double ms_gather = ms(b0, Clock::now());

    const bool same = render(rp) == render(rg);
    // Rows crossing the coordinator: pushdown ships <= distinct-fk partials per shard; gather ships
    // every fact row. (Both also gather the tiny dim once.)
    const std::uint64_t shipped_push = FK * M;  // upper bound: per-(shard,fk) partials
    const std::uint64_t shipped_gather = N;

    std::printf("\n== distributed star-JOIN: shuffle pushdown vs gather-the-fact ==\n");
    std::printf("  query: %s\n", q.c_str());
    std::printf("  result byte-identical both ways: %s (%zu groups)\n", same ? "YES" : "NO!!",
                rp.rows.size());
    std::printf("  PUSHDOWN (fact never gathered) : %9.1f ms   ~%llu partial rows to coordinator\n",
                ms_push, (unsigned long long)shipped_push);
    std::printf("  GATHER   (whole fact gathered) : %9.1f ms   ~%llu fact rows to coordinator\n",
                ms_gather, (unsigned long long)shipped_gather);
    std::printf("  SPEEDUP  pushdown vs gather     : %6.1fx   (%.0fx less data shipped)\n",
                ms_gather / (ms_push > 0 ? ms_push : 1),
                static_cast<double>(shipped_gather) / static_cast<double>(shipped_push));
    return same ? 0 : 1;
}
