// lockstep_vector_v2.cpp — K1.5 dataset v2: SEPARATED points (1000 clusters x N/1000,
// per-dim jitter in [0,2) — pairwise distances ~6.5, no near-ties), probes SWEEP.
// Emits {"sys":"lockstep","probes":P,"ms_each":X,"recall":R} per sweep point, plus brute.
//   lockstep_vector_v2 N DIM K QUERIES LISTS
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
double elem(std::int64_t i, std::int64_t d) {
    const std::int64_t c = i % 1000;
    const std::int64_t base = (c * 37 + d * 11) % 8;
    const std::int64_t jit = (i * 2654435761LL + d * 40503LL) % 2000;
    return static_cast<double>(base) + static_cast<double>(jit) / 1000.0;
}
double qelem(int j, std::int64_t d) {
    const std::int64_t c = (static_cast<std::int64_t>(j) * 37) % 1000;
    const std::int64_t base = (c * 37 + d * 11) % 8;
    const std::int64_t off = (static_cast<std::int64_t>(j) * 13 + d * 7) % 100;
    return static_cast<double>(base) + static_cast<double>(off) / 100.0;
}
std::string vtext(std::int64_t i, int dim, bool query, int j) {
    std::string v = "[";
    for (int d = 0; d < dim; ++d) {
        if (d != 0) v += ",";
        v += Datum::render_double(query ? qelem(j, d) : elem(i, d));
    }
    return v + "]";
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
    const std::int64_t n = argc > 1 ? std::atoll(argv[1]) : 100000;
    const int dim = argc > 2 ? std::atoi(argv[2]) : 64;
    const int k = argc > 3 ? std::atoi(argv[3]) : 10;
    const int queries = argc > 4 ? std::atoi(argv[4]) : 20;
    const int lists = argc > 5 ? std::atoi(argv[5]) : 200;

    SqlEngine e;
    e.exec("CREATE TABLE docs (id INT, emb VECTOR(" + std::to_string(dim) +
           ") NOT NULL, PRIMARY KEY (id))");
    for (std::int64_t i = 0; i < n; i += 500) {
        std::string sql = "INSERT INTO docs (id,emb) VALUES ";
        for (std::int64_t j = i; j < i + 500 && j < n; ++j) {
            if (j > i) sql += ",";
            sql += "(" + std::to_string(j) + ",'" + vtext(j, dim, false, 0) + "')";
        }
        if (!e.exec(sql).ok) { std::fprintf(stderr, "load fail\n"); return 1; }
    }
    const auto knn = [&](int j) {
        return e.exec("SELECT id FROM docs ORDER BY emb <-> '" + vtext(0, dim, true, j) +
                      "' LIMIT " + std::to_string(k));
    };
    // Brute reference (no index yet).
    std::vector<std::vector<std::int64_t>> ref(static_cast<std::size_t>(queries));
    const auto tb = std::chrono::steady_clock::now();
    for (int j = 0; j < queries; ++j)
        for (const auto& row : knn(j).rows)
            ref[static_cast<std::size_t>(j)].push_back(row.cells[0].second.i);
    std::printf("{\"sys\":\"lockstep\",\"probes\":0,\"ms_each\":%.2f,\"recall\":1.0}\n",
                ms_since(tb) / queries);
    if (!e.exec("CREATE INDEX dix ON docs (emb) USING IVFFLAT WITH (lists = " +
                std::to_string(lists) + ", probes = 1)").ok) {
        std::fprintf(stderr, "index fail\n");
        return 1;
    }
    const int sweep[] = {1, 2, 5, 10, 20, 50};
    for (const int pr : sweep) {
        (void)e.exec("SET ivfflat.probes = " + std::to_string(pr));
        for (int j = 0; j < queries; ++j) (void)knn(j);  // warm (cache build on first)
        int hit = 0, total = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int j = 0; j < queries; ++j) {
            const ExecResult r = knn(j);
            for (const auto& row : r.rows) {
                ++total;
                for (const std::int64_t id : ref[static_cast<std::size_t>(j)])
                    if (id == row.cells[0].second.i) { ++hit; break; }
            }
        }
        std::printf("{\"sys\":\"lockstep\",\"probes\":%d,\"ms_each\":%.2f,\"recall\":%.4f}\n",
                    pr, ms_since(t0) / queries, total ? static_cast<double>(hit) / total : 0.0);
    }
    return 0;
}
