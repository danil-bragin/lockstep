// lockstep_vector.cpp — K1.5: vector k-NN bench, the Lockstep side. Loads N deterministic
// DIM-d vectors (the EXACT integer formula run_vector.sh reproduces in SQL for pgvector),
// then measures: brute-force k-NN (no index), IVFFLAT build, IVFFLAT k-NN, and recall@k of
// the index result against this engine's own brute-force reference.
//
//   lockstep_vector N DIM K QUERIES LISTS PROBES
//
// Emits one JSON line per metric: {"sys":"lockstep","q":"...","ms_each":X} plus a recall line.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
// Element (i, d) of the deterministic data set — INTEGER math only, so psql's
// generate_series reproduces it bit-for-bit: cluster base + a hash jitter in [0, 0.2).
double elem(std::int64_t i, std::int64_t d) {
    const std::int64_t c = i % 100;
    const std::int64_t base = (c * 31 + d * 7) % 20;
    const std::int64_t jit = (i * 2654435761LL + d * 40503LL) % 1000;
    return static_cast<double>(base) + static_cast<double>(jit) / 5000.0;
}
std::string vec_text(std::int64_t i, int dim) {
    std::string v = "[";
    for (int d = 0; d < dim; ++d) {
        if (d != 0) v += ",";
        v += Datum::render_double(elem(i, d));
    }
    return v + "]";
}
// Query j: the center of cluster (j*13)%100, offset +0.05 per element.
std::string query_text(int j, int dim) {
    const std::int64_t c = (static_cast<std::int64_t>(j) * 13) % 100;
    std::string v = "[";
    for (int d = 0; d < dim; ++d) {
        if (d != 0) v += ",";
        v += Datum::render_double(static_cast<double>((c * 31 + d * 7) % 20) + 0.05);
    }
    return v + "]";
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}
}  // namespace

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 100000;
    const int dim = argc > 2 ? std::atoi(argv[2]) : 64;
    const int k = argc > 3 ? std::atoi(argv[3]) : 10;
    const int queries = argc > 4 ? std::atoi(argv[4]) : 20;
    const int lists = argc > 5 ? std::atoi(argv[5]) : 200;
    const int probes = argc > 6 ? std::atoi(argv[6]) : 10;

    SqlEngine e;
    e.exec("CREATE TABLE docs (id INT, emb VECTOR(" + std::to_string(dim) +
           ") NOT NULL, PRIMARY KEY (id))");
    const auto tload = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < n; i += 500) {
        std::string sql = "INSERT INTO docs (id,emb) VALUES ";
        for (std::int64_t j = i; j < i + 500 && j < n; ++j) {
            if (j > i) sql += ",";
            sql += "(" + std::to_string(j) + ",'" + vec_text(j, dim) + "')";
        }
        if (!e.exec(sql).ok) {
            std::fprintf(stderr, "load failed at %lld\n", static_cast<long long>(i));
            return 1;
        }
    }
    std::fprintf(stderr, "loaded %lld rows in %.0f ms\n", static_cast<long long>(n),
                 ms_since(tload));

    const auto run_knn = [&](int j) {
        return e.exec("SELECT id FROM docs ORDER BY emb <-> '" + query_text(j, dim) +
                      "' LIMIT " + std::to_string(k));
    };
    // Brute force (no index yet): reference ids + timing (each query once — the scan cost
    // dominates and is stable; matches the min-of-warm-runs Postgres side closely enough).
    std::vector<std::vector<std::int64_t>> ref(static_cast<std::size_t>(queries));
    const auto tb = std::chrono::steady_clock::now();
    for (int j = 0; j < queries; ++j) {
        const ExecResult r = run_knn(j);
        if (!r.ok) { std::fprintf(stderr, "brute failed: %s\n", r.error.c_str()); return 1; }
        for (const auto& row : r.rows) ref[static_cast<std::size_t>(j)].push_back(row.cells[0].second.i);
    }
    const double brute_ms = ms_since(tb) / queries;
    std::printf("{\"sys\":\"lockstep\",\"q\":\"knn_brute\",\"ms_each\":%.2f}\n", brute_ms);

    const auto tix = std::chrono::steady_clock::now();
    const ExecResult ix = e.exec("CREATE INDEX docs_ivf ON docs (emb) USING IVFFLAT WITH (lists = " +
                                 std::to_string(lists) + ", probes = " + std::to_string(probes) + ")");
    if (!ix.ok) { std::fprintf(stderr, "index build failed: %s\n", ix.error.c_str()); return 1; }
    std::printf("{\"sys\":\"lockstep\",\"q\":\"ivfflat_build\",\"ms_each\":%.0f}\n", ms_since(tix));

    // IVFFLAT k-NN: warm once, then time.
    for (int j = 0; j < queries; ++j) (void)run_knn(j);
    std::vector<std::vector<std::int64_t>> ann(static_cast<std::size_t>(queries));
    const auto ta = std::chrono::steady_clock::now();
    for (int j = 0; j < queries; ++j) {
        const ExecResult r = run_knn(j);
        for (const auto& row : r.rows) ann[static_cast<std::size_t>(j)].push_back(row.cells[0].second.i);
    }
    std::printf("{\"sys\":\"lockstep\",\"q\":\"knn_ivfflat\",\"ms_each\":%.2f}\n",
                ms_since(ta) / queries);

    int hit = 0, total = 0;
    for (int j = 0; j < queries; ++j) {
        for (const std::int64_t id : ref[static_cast<std::size_t>(j)]) {
            ++total;
            for (const std::int64_t a : ann[static_cast<std::size_t>(j)])
                if (a == id) { ++hit; break; }
        }
    }
    std::printf("{\"sys\":\"lockstep\",\"q\":\"recall_at_k\",\"ms_each\":%.4f}\n",
                total == 0 ? 0.0 : static_cast<double>(hit) / total);
    return 0;
}
