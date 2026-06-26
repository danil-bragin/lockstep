// lockstep_analytics.cpp — SQL ANALYTICS benchmark on the Lockstep COLUMNAR engine, for a
// head-to-head vs PostgreSQL (same deterministic data + same SQL, run under the same CPU pin).
//
// Loads N deterministic rows into a columnar table, flushes (compacts to SoA blocks), then
// times a fixed set of analytical queries (scan-aggregate, GROUP BY, filtered, zone-skip),
// each over `iters` repetitions. Emits one JSON line per query: {q, ms_total, iters, ms_each}.
// TIME IS WALL-CLOCK here (this is a perf comparison, not the sim's deterministic counters).
//
// The data formula MUST MATCH pg_load.sql so both systems aggregate identical values.
//   id INT PK = i;  uid = i%10000;  cat = i%8;  amount = (i*2654435761) mod 1000;
//   region = {north,south,east,west,central}[i%5];  ts = i (monotonic, for zone skipping).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <lockstep/prod/ProdParallelExecutor.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
std::int64_t amount_of(std::uint64_t i) {
    return static_cast<std::int64_t>((i * 2654435761ULL) % 1000ULL);
}
const char* region_of(std::uint64_t i) {
    static const char* r[] = {"north", "south", "east", "west", "central"};
    return r[i % 5];
}
double now_ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}
}  // namespace

int main(int argc, char** argv) {
    std::uint64_t N = 200000;
    int iters = 50;
    if (argc > 1) {
        N = std::strtoull(argv[1], nullptr, 10);
    }
    if (argc > 2) {
        iters = std::atoi(argv[2]);
    }
    std::size_t workers = 1;  // argv[3]: morsel-parallel worker threads (1 = serial)
    if (argc > 3) {
        workers = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
        if (workers < 1) workers = 1;
    }

    SqlEngine eng;
    eng.set_columnar_default(true);
    // Morsel parallelism: fold the scalar-aggregate fast path across `workers` cores. The pool
    // lives for the whole bench; the RESULT is byte-identical to serial (fixed-order merge).
    lockstep::prod::ProdParallelExecutor pexec(workers);
    if (workers > 1) {
        eng.set_parallel_executor(&pexec);
    }
    // Filter columns NOT NULL so the vectorized-aggregate + zone-skip fast path applies
    // (the vectorizable-conjunct extractor conservatively skips nullable columns).
    (void)eng.exec(
        "CREATE TABLE events (id INT, uid INT, cat INT NOT NULL, amount INT NOT NULL, "
        "region TEXT NOT NULL, ts INT NOT NULL, PRIMARY KEY (id))");

    auto t_load = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < N; ++i) {
        (void)eng.exec("INSERT INTO events (id, uid, cat, amount, region, ts) VALUES (" +
                       std::to_string(i) + ", " + std::to_string(i % 10000) + ", " +
                       std::to_string(i % 8) + ", " + std::to_string(amount_of(i)) + ", '" +
                       region_of(i) + "', " + std::to_string(i) + ")");
    }
    const double load_ms = now_ms_since(t_load);
    auto t_flush = std::chrono::steady_clock::now();
    (void)eng.flush_columnar("events");
    const double flush_ms = now_ms_since(t_flush);
    std::fprintf(stderr, "lockstep: loaded %llu rows in %.0fms, flush %.0fms\n",
                 (unsigned long long)N, load_ms, flush_ms);

    const std::uint64_t ts_hi = N - N / 20;  // top ~5% (zone skip ~19/20 chunks)
    struct Q { const char* name; std::string sql; };
    const Q qs[] = {
        {"scan_agg", "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM events"},
        {"groupby_cat", "SELECT cat, COUNT(*), SUM(amount) FROM events GROUP BY cat"},
        {"groupby_region", "SELECT region, COUNT(*), SUM(amount) FROM events GROUP BY region"},
        {"filtered_agg", "SELECT COUNT(*), SUM(amount) FROM events WHERE amount > 800"},
        {"zone_skip", "SELECT COUNT(*), SUM(amount) FROM events WHERE ts > " +
                          std::to_string(ts_hi)},
    };
    for (const Q& q : qs) {
        // WARM the lazy columnar decode (col_chunks built on first access) BEFORE timing — the
        // analytics shape is load-once/query-many, and the competitors are measured warm (Postgres
        // EXPLAIN ANALYZE min-of-5, DuckDB looped, ClickHouse 5 reps). Without this the one-time
        // cold block decode (~90ms for 1M rows) lands in the average and the fast steady-state
        // queries (scan_agg) read as ~3ms instead of their true ~0.2ms.
        for (int w = 0; w < 3; ++w) (void)eng.exec(q.sql);
        auto t0 = std::chrono::steady_clock::now();
        std::uint64_t chk = 0;
        for (int k = 0; k < iters; ++k) {
            chk += eng.exec(q.sql).rows.size();
        }
        const double ms = now_ms_since(t0);
        std::printf("{\"sys\":\"lockstep\",\"w\":%zu,\"q\":\"%s\",\"iters\":%d,\"ms_total\":%.2f,"
                    "\"ms_each\":%.4f,\"chk\":%llu}\n",
                    workers, q.name, iters, ms, ms / iters, (unsigned long long)chk);
    }
    return 0;
}
