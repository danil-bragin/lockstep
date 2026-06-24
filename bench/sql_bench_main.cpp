// sql_bench_main.cpp — SQL SURFACE micro-benchmark DRIVER (a BASELINE, not an
// optimization target). It measures the per-query cost of the SQL v2 pipeline:
//   PARSE (recursive descent) + PLAN (lowering decision) + EXECUTE (scan/get/range
//   over the verified query::Database + the in-memory filter/group/aggregate/sort).
//
// HONEST SCOPE — WHAT THIS MEASURES:
//   * It is an IN-MEMORY benchmark over the query layer (no disk I/O on the read
//     path; the engine primes a read store). So it measures the SQL layer's CPU cost
//     (tokenize + parse + plan + decode rows + filter/group/aggregate/sort), NOT
//     storage throughput (that is bench_main.cpp / PERF_BASELINE.md).
//   * TIME IS MEASURED EXTERNALLY (this TU is forbidden-lint scanned: NO std::chrono,
//     NO wall-clock here). The driver runs a FIXED, deterministic amount of work
//     (`iters` repetitions of each query over an N-row table) and prints the work
//     done + a result CHECKSUM. WRAP IT WITH A WALL-CLOCK TIMER to get latency:
//        /usr/bin/time -p ./lockstep_sql_bench_driver
//     or the recorded numbers in bench/PERF_BASELINE.md (the SQL section). Dividing
//     the measured wall by the printed (iters × query-count) gives per-op latency;
//     the checksum proves the work was REAL (the optimizer cannot elide it) and
//     DETERMINISTIC (same build => same checksum => same work, run to run).
//
// DETERMINISM: a pure function of (N, iters). No clock, no <random> (a hand-rolled
// SplitMix seeds the row data), no threads. Two runs print byte-identical output.
//
// USAGE:
//   sql_bench_driver               — the full baseline (N=500, iters tuned per shape)
//   sql_bench_driver --smoke       — a tiny deterministic run (the gate's smoke ctest)
//   sql_bench_driver --rows N       — override the table size
//   sql_bench_driver --iters K      — override the per-shape repetition count
//
// WRITE-PATH NOTE (the SQL-optimize stage FIXED the dominant O(N^2)): the v1 write
// path used to re-submit the WHOLE accumulated write-log as one batch per INSERT AND
// re-prime the full committed history each statement — O(committed) per write, so
// O(N^2) to build N rows (observed: N=2000 ~18s). The SQL Engine now applies writes
// INCREMENTALLY: the read-modify-write decision runs in the Engine over the verified
// read path (a strict point-get of the live committed store), then ONE pure-writer
// txn commits through the verified executor + Database::apply_committed lands it
// incrementally (no whole-history rebuild). Build is now ~O(N) of SQL work (observed:
// N=2000 ~0.04s, ~460x). A RESIDUAL near-quadratic remains in storage (the WalEngine
// memtable does a LINEAR scan over keys per get/insert — WalEngine.hpp find/
// versions_for, "a binary search would do; a linear scan is fine + clear") — that is a
// PROTECTED storage cost, pre-existing, affecting all workloads, FLAGGED not changed
// here. The default N stays 500 so the (now cheap) build never dominates the per-query
// numbers. See bench/PERF_BASELINE.md (the SQL-optimize section) for the before/after.
//
// --build-only times JUST the build (the write path); --only <substr> isolates one
// query shape for external per-shape timing. Everything is BOUNDED (N capped).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/sql/Parser.hpp>

namespace {

using namespace lockstep::query::sql;

// Deterministic SplitMix64 (NOT a std::*_engine — integer mixing only).
class SplitMix {
public:
    explicit SplitMix(std::uint64_t seed) noexcept : s_(seed) {}
    std::uint64_t next() noexcept {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) noexcept { return n == 0 ? 0 : (next() % n); }

private:
    std::uint64_t s_;
};

// Fold an ExecResult into a running checksum so the optimizer can NOT elide the
// query (the work is observable) + the checksum proves determinism across runs.
std::uint64_t fold(std::uint64_t acc, const ExecResult& r) {
    acc = acc * 1000003ULL + (r.ok ? 1u : 0u);
    acc = acc * 1000003ULL + r.affected;
    for (const ResultRow& row : r.rows) {
        for (const auto& [label, d] : row.cells) {
            for (const char c : label) {
                acc = acc * 131ULL + static_cast<unsigned char>(c);
            }
            const std::string rendered = d.render();
            for (const char c : rendered) {
                acc = acc * 131ULL + static_cast<unsigned char>(c);
            }
        }
    }
    return acc;
}

// Build an N-row table emp(id INT PK, dept TEXT, sal INT, age INT, deptid INT, bio TEXT)
// deterministically, PLUS a join table dpt(did INT PK, region TEXT) keyed by an INT dept
// id (0..4), so a 2-table equi-join `emp.deptid = dpt.did` exercises the HASH-JOIN path
// over N rows. emp gets an extra INT `deptid` column (0..4) that equi-joins to dpt.did,
// and a WIDE TEXT `bio` payload column (~64 bytes) the projected/filtered SELECT shapes
// DO NOT reference — so the lazy/projected decode (skip the unreferenced wide field)
// is measurable: a `SELECT id` over emp must SKIP copying every row's bio.
SqlEngine build_table(std::uint64_t n) {
    SqlEngine eng;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, deptid INT, bio TEXT, "
        "PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE dpt (did INT, region TEXT, PRIMARY KEY (did))");
    const char* regions[] = {"north", "south", "east", "west", "central"};
    for (std::int64_t d = 0; d < 5; ++d) {
        (void)eng.exec("INSERT INTO dpt (did, region) VALUES (" + std::to_string(d) +
                       ", '" + regions[d] + "')");
    }
    SplitMix rng(0x5EED1234ULL);
    const char* depts[] = {"eng", "sales", "ops", "hr", "legal"};
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint64_t dx = rng.below(5);
        const std::string dept = depts[dx];
        const std::int64_t sal = static_cast<std::int64_t>(rng.below(1000));
        const std::int64_t age = static_cast<std::int64_t>(rng.below(50)) + 18;
        // A deterministic ~64-byte payload (no rng-dependent length: fixed width so the
        // checksum is stable + the skipped-bytes win is uniform across rows).
        std::string bio = "bio-" + std::to_string(i) + "-";
        bio.resize(64, static_cast<char>('a' + static_cast<char>(dx)));
        (void)eng.exec("INSERT INTO emp (id, dept, sal, age, deptid, bio) VALUES (" +
                       std::to_string(i) + ", '" + dept + "', " + std::to_string(sal) +
                       ", " + std::to_string(age) + ", " + std::to_string(dx) + ", '" +
                       bio + "')");
    }
    return eng;
}

// Build the same N-row table, then CREATE secondary indexes on `sal` (INT) and `dept`
// (TEXT). The indexed-WHERE bench shapes then take the index access path (range-scan
// the index for the matching col range -> point-get -> residual filter) instead of the
// O(N) full scan — so a side-by-side index-vs-scan timing shows the access-path win.
SqlEngine build_indexed_table(std::uint64_t n) {
    SqlEngine eng = build_table(n);
    (void)eng.exec("CREATE INDEX idx_sal ON emp (sal)");    // backfills N rows
    (void)eng.exec("CREATE INDEX idx_dept ON emp (dept)");
    return eng;
}

// One benchmark shape: a label + the SQL + how many times to repeat it.
struct Shape {
    std::string label;
    std::string sql;
    std::uint64_t iters;
};

// Run a shape `iters` times, folding the result into a checksum. Returns the
// checksum (proves real, deterministic work) — wall time is measured EXTERNALLY.
std::uint64_t run_shape(SqlEngine& eng, const Shape& sh) {
    std::uint64_t acc = 1469598103934665603ULL;  // FNV offset basis (seed)
    for (std::uint64_t k = 0; k < sh.iters; ++k) {
        acc = fold(acc, eng.exec(sh.sql));
    }
    return acc;
}

// A PARSE-ONLY micro-shape (tokenize + recursive descent, no plan/exec) so the
// report separates parse cost from execute cost.
std::uint64_t run_parse_only(const std::string& sql, std::uint64_t iters) {
    std::uint64_t acc = 1469598103934665603ULL;
    for (std::uint64_t k = 0; k < iters; ++k) {
        ParseResult pr = parse_sql(sql);
        acc = acc * 1000003ULL + (pr.ok() ? 1u : 0u);
        if (pr.ok()) {
            acc = acc * 131ULL + pr.stmt().select.items.size();
            acc = acc * 131ULL + pr.stmt().select.filter.nodes.size();
        }
    }
    return acc;
}

std::vector<Shape> full_shapes(std::uint64_t n) {
    // Iteration counts are tuned so each shape does comparable total work (a point
    // get is cheap => many iters; a full-scan GROUP BY is expensive => fewer).
    return {
        {"point   SELECT id=PK (point get)",
         "SELECT id, dept, sal FROM emp WHERE id = " + std::to_string(n / 2),
         20000},
        {"range   SELECT PK BETWEEN (range scan)",
         "SELECT id, dept FROM emp WHERE id BETWEEN " + std::to_string(n / 4) +
             " AND " + std::to_string(3 * n / 4),
         2000},
        {"filter  full scan + ANY-col predicate",
         "SELECT id, sal FROM emp WHERE sal > 500 AND dept = 'eng'", 2000},
        {"project full scan, 1 narrow col (skip wide bio)",
         "SELECT id FROM emp", 2000},
        {"projfilt full scan + filter, skip wide bio",
         "SELECT id, sal FROM emp WHERE sal > 500", 2000},
        {"order   full scan + ORDER BY + LIMIT",
         "SELECT id, sal FROM emp ORDER BY sal DESC LIMIT 10", 1000},
        {"groupby full scan + GROUP BY + 5 aggs",
         "SELECT dept, COUNT(*), SUM(sal), MIN(sal), MAX(sal), AVG(sal) FROM emp "
         "GROUP BY dept",
         1000},
        {"having  GROUP BY + HAVING + ORDER BY",
         "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 1 "
         "ORDER BY dept",
         1000},
        {"distinct full scan + DISTINCT + ORDER BY",
         "SELECT DISTINCT dept FROM emp ORDER BY dept", 1000},
        {"join    2-table equi-join (hash join)",
         "SELECT emp.id, dpt.region FROM emp JOIN dpt ON emp.deptid = dpt.did", 1000},
        {"joingrp join + GROUP BY + aggregate",
         "SELECT dpt.region, COUNT(*), AVG(emp.sal) FROM emp "
         "JOIN dpt ON emp.deptid = dpt.did GROUP BY dpt.region ORDER BY dpt.region",
         1000},
    };
}

// INDEX-vs-SCAN shapes: a SELECTIVE WHERE on the indexed `sal` column. The SAME query
// is timed twice — once against the indexed engine (index access path: O(log N + matches))
// and once against the plain engine (full scan + filter: O(N)). With the index the work
// per query is proportional to the (few) matching rows; the scan reads all N. Run with
// the indexed engine for the "idx" variants and the plain engine for the "scan" variants.
std::vector<Shape> index_eq_shape(std::uint64_t /*n*/) {
    // sal in [0,1000); an eq match averages ~N/1000 rows (selective).
    return {{"sal=500", "SELECT id, sal FROM emp WHERE sal = 500", 20000}};
}
std::vector<Shape> index_range_shape(std::uint64_t /*n*/) {
    // A narrow range (~4% of the value space) — selective vs the full scan.
    return {{"sal BETWEEN 480 AND 520",
             "SELECT id, sal FROM emp WHERE sal BETWEEN 480 AND 520", 5000}};
}

std::vector<Shape> smoke_shapes(std::uint64_t n) {
    return {
        {"point", "SELECT id FROM emp WHERE id = " + std::to_string(n / 2), 50},
        {"groupby",
         "SELECT dept, COUNT(*), AVG(sal) FROM emp GROUP BY dept ORDER BY dept", 20},
    };
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    bool build_only = false;
    std::string only;  // run ONLY shapes whose label contains this substring
    std::uint64_t n = 500;
    std::uint64_t iters_override = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--build-only") == 0) {
            build_only = true;  // measure ONLY the N-row table build (the write path)
        } else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];  // isolate one shape for external per-shape timing
        } else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            n = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters_override =
                static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (smoke) {
        n = 64;
    }
    if (n == 0) {
        n = 1;
    }
    // BOUNDED: cap N so an accidental O(N^2) write path cannot hang the host (the
    // build-N curve sweep tops out at 8000; a generous cap leaves room without
    // letting a regression run unbounded).
    constexpr std::uint64_t kMaxRows = 200000;
    if (n > kMaxRows) {
        n = kMaxRows;
    }

    // --build-only: time JUST the table build (the WRITE path), print a row checksum
    // proving the rows were really written. Used to chart the build-N curve (the
    // O(N^2) -> O(N) write fix) WITHOUT the per-query scan cost mixed in.
    if (build_only) {
        SqlEngine beng = build_table(n);
        const ExecResult all = beng.exec("SELECT id, dept, sal, age, deptid FROM emp");
        std::uint64_t cs = fold(1469598103934665603ULL, all);
        std::printf("BUILD-ONLY rows=%llu built=%llu checksum=%016llx\n",
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(all.rows.size()),
                    static_cast<unsigned long long>(cs));
        return 0;
    }

    SqlEngine eng = build_table(n);
    std::vector<Shape> shapes = smoke ? smoke_shapes(n) : full_shapes(n);
    if (iters_override != 0) {
        for (Shape& s : shapes) {
            s.iters = iters_override;
        }
    }

    // INDEX-vs-SCAN comparison (the secondary-index perf path). Build a SECOND engine
    // WITH indexes on sal/dept; run the selective indexed-WHERE shapes against BOTH the
    // indexed engine (index access path) and the plain engine (full scan + filter), so
    // the external timer can compare per-op latency. Each line is --only-isolatable
    // (label "idx " vs "scan"). The index reads ~matches rows; the scan reads all N.
    SqlEngine ieng = build_indexed_table(n);
    std::vector<std::pair<std::string, std::pair<SqlEngine*, Shape>>> cmp;
    if (!smoke) {
        for (const Shape& s : index_eq_shape(n)) {
            cmp.push_back({"idx  eq   " + s.label, {&ieng, s}});
            cmp.push_back({"scan eq   " + s.label, {&eng, s}});
        }
        for (const Shape& s : index_range_shape(n)) {
            cmp.push_back({"idx  rng  " + s.label, {&ieng, s}});
            cmp.push_back({"scan rng  " + s.label, {&eng, s}});
        }
    }
    if (iters_override != 0) {
        for (auto& c : cmp) {
            c.second.second.iters = iters_override;
        }
    }

    std::printf("====================================================================\n");
    std::printf(" Lockstep SQL micro-bench (v2 surface) — parse/plan/execute BASELINE%s\n",
                smoke ? "  [SMOKE]" : "");
    std::printf(" rows=%llu  (in-memory over query::Database; CPU-bound, NO disk I/O)\n",
                static_cast<unsigned long long>(n));
    std::printf(" Time is measured EXTERNALLY (this TU is wall-clock-free / lint-clean):\n");
    std::printf("   /usr/bin/time -p %s   then divide wall by total iters.\n",
                argv[0]);
    std::printf(" Each shape prints its iters + a result checksum (proves REAL,\n");
    std::printf(" DETERMINISTIC work the optimizer cannot elide).\n");
    std::printf("====================================================================\n");

    // PARSE-ONLY baseline (a representative GROUP BY query) — isolates parser cost.
    const std::string parse_sql_str =
        "SELECT dept, COUNT(*), SUM(sal) FROM emp WHERE sal > 100 GROUP BY dept "
        "HAVING COUNT(*) > 1 ORDER BY dept DESC LIMIT 5";
    const std::uint64_t parse_iters = smoke ? 100 : 100000;
    const std::uint64_t pcs = run_parse_only(parse_sql_str, parse_iters);
    std::printf(" %-38s iters=%-8llu checksum=%016llx\n", "PARSE-ONLY (rich query)",
                static_cast<unsigned long long>(parse_iters),
                static_cast<unsigned long long>(pcs));
    std::printf("--------------------------------------------------------------------\n");

    std::uint64_t total_iters = parse_iters;
    for (const Shape& sh : shapes) {
        if (!only.empty() && sh.label.find(only) == std::string::npos) {
            continue;  // --only: skip non-matching shapes (per-shape isolation timing)
        }
        const std::uint64_t cs = run_shape(eng, sh);
        total_iters += sh.iters;
        std::printf(" %-38s iters=%-8llu checksum=%016llx\n", sh.label.c_str(),
                    static_cast<unsigned long long>(sh.iters),
                    static_cast<unsigned long long>(cs));
    }
    // INDEX-vs-SCAN comparison block (selective WHERE on the indexed sal column).
    if (!cmp.empty()) {
        std::printf("------ INDEX vs FULL-SCAN (selective WHERE on indexed sal) --------\n");
        for (const auto& [label, eng_shape] : cmp) {
            if (!only.empty() && label.find(only) == std::string::npos) {
                continue;
            }
            const std::uint64_t cs = run_shape(*eng_shape.first, eng_shape.second);
            total_iters += eng_shape.second.iters;
            std::printf(" %-38s iters=%-8llu checksum=%016llx\n", label.c_str(),
                        static_cast<unsigned long long>(eng_shape.second.iters),
                        static_cast<unsigned long long>(cs));
        }
    }

    std::printf("--------------------------------------------------------------------\n");
    std::printf(" total query executions (excl. parse-only) + parse iters = %llu\n",
                static_cast<unsigned long long>(total_iters));
    std::printf(" Divide /usr/bin/time wall by the per-shape iters for per-op latency.\n");
    return 0;
}
