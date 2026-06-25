// sql_parallel_agg_test.cpp — MORSEL-PARALLELISM determinism gate. The columnar scalar-aggregate
// fast path (ungrouped + unfiltered SUM/COUNT/MIN/MAX/AVG) folds its chunks across worker threads
// via an injected query::IParallelExecutor (ProdParallelExecutor here). Parallelism is a wall-
// clock optimization ONLY: the partials are merged in a FIXED chunk order, so the RESULT must be
// byte-identical to the serial fold — for every worker count, column type (int + text MIN/MAX),
// nullability, and table size (spanning the parallel-gate threshold and chunk boundaries).
//
// Method: build one columnar table, flush, then run each aggregate query (a) serial (no executor)
// and (b) parallel at W = 2,3,4,7,8,16 workers; assert identical rendered output. Re-run under a
// repeat loop so TSan in the Linux gate exercises the pool dispatch/merge for races. A real bug
// in the split/merge (wrong partition bounds, lost tail chunk, racy partial) breaks byte-equality.
//
// Threads live only in ProdParallelExecutor (providers/prod) — this file injects it, no std::thread
// token here. Linux-only build (mirrors the other prod-threaded gates).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/prod/ProdParallelExecutor.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;
using lockstep::prod::ProdParallelExecutor;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

std::string render(const ExecResult& r) {
    std::string out = r.ok ? "OK" : "ERR";
    if (!r.ok) return out + "(" + r.error + ")";
    out += " aff=" + std::to_string(r.affected);
    for (const ResultRow& row : r.rows) {
        out += " |";
        for (const auto& [label, d] : row.cells) out += " " + label + "=" + d.render();
    }
    return out;
}

// The ungrouped + unfiltered scalar aggregates that hit compute_agg_chunked (the parallel path).
const std::vector<std::string> kQueries = {
    "SELECT COUNT(*) FROM t",
    "SELECT SUM(amount) FROM t",
    "SELECT MIN(amount), MAX(amount) FROM t",
    "SELECT COUNT(amount), SUM(amount), MIN(amount), MAX(amount) FROM t",
    "SELECT MIN(region), MAX(region) FROM t",      // TEXT min/max (generic Datum path)
    "SELECT COUNT(opt), SUM(opt), MIN(opt), MAX(opt) FROM t",  // NULLABLE int (null branch)
    "SELECT COUNT(*), SUM(amount), MIN(region), MAX(opt) FROM t",  // mixed
};

// Build a columnar table of N rows, flush to blocks, return the engine. opt is NULL on every
// 4th row so the nullable-aggregate branch is exercised; region is one of 5 strings (TEXT
// min/max); amount is a scrambled int.
void load(SqlEngine& e, std::size_t n) {
    e.set_columnar_default(true);
    e.exec("CREATE TABLE t (id INT, amount INT NOT NULL, region TEXT NOT NULL, opt INT, "
           "PRIMARY KEY (id))");
    const char* regs[] = {"north", "south", "east", "west", "central"};
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t amount = static_cast<std::int64_t>((i * 2654435761ULL) % 100000);
        std::string sql = "INSERT INTO t (id, amount, region, opt) VALUES (" + std::to_string(i) +
                          ", " + std::to_string(amount) + ", '" + regs[i % 5] + "', " +
                          ((i % 4 == 0) ? std::string("NULL") : std::to_string((i * 7) % 5000)) +
                          ")";
        const ExecResult r = e.exec(sql);
        check(r.ok, "insert: " + render(r));
    }
    check(!e.flush_columnar("t").has_value(), "flush n=" + std::to_string(n));
}

// For one table size: serial baseline per query, then each worker count must match it byte-for-byte.
void run_size(std::size_t n) {
    SqlEngine e;
    load(e, n);

    std::vector<std::string> baseline;
    e.set_parallel_executor(nullptr);  // serial
    for (const std::string& q : kQueries) baseline.push_back(render(e.exec(q)));

    for (std::size_t w : std::vector<std::size_t>{2, 3, 4, 7, 8, 16}) {
        ProdParallelExecutor ex(w);
        e.set_parallel_executor(&ex);
        for (std::size_t qi = 0; qi < kQueries.size(); ++qi) {
            const std::string got = render(e.exec(kQueries[qi]));
            check(got == baseline[qi],
                  "n=" + std::to_string(n) + " w=" + std::to_string(w) + " q=[" + kQueries[qi] +
                      "] serial=[" + baseline[qi] + "] parallel=[" + got + "]");
        }
        e.set_parallel_executor(nullptr);
    }
}

}  // namespace

int main() {
    // Sizes spanning: empty, tiny (serial-gated, < kParallelMinRows), and large (parallel,
    // multiple chunks, non-divisible-by-worker-count splits to stress partition bounds).
    for (std::size_t n : std::vector<std::size_t>{0, 1, 100, 49999, 50000, 50001, 123457, 300000}) {
        run_size(n);
    }
    // Repeat the largest a few times so TSan sees many pool dispatch/merge cycles.
    for (int rep = 0; rep < 3; ++rep) run_size(200003);

    if (g_fail) {
        std::printf("sql_parallel_agg_test: FAILED\n");
        return 1;
    }
    std::printf("sql_parallel_agg_test: OK\n");
    return 0;
}
