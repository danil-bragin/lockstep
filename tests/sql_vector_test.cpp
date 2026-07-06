// sql_vector_test.cpp — K1: vector search over REAL[] embeddings.
//
// Distance / similarity functions over two equal-length numeric vectors return a REAL:
//   L2_DISTANCE (Euclidean), COSINE_DISTANCE (1 - cosine similarity), INNER_PRODUCT (dot).
// Brute-force k-NN is `SELECT ..., l2_distance(emb, ARRAY[...]) AS d ... ORDER BY d LIMIT k`.
// Built on the F14 REAL type + the generic ARRAY codec — no separate vector storage. Durable,
// and correct under the columnar engine too.

#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
std::vector<std::int64_t> ids(const ExecResult& r) {
    std::vector<std::int64_t> v;
    for (const auto& row : r.rows) v.push_back(row.cells[0].second.i);
    return v;
}
double d0(const ExecResult& r) {
    return (r.ok && r.rows.size() == 1) ? r.rows[0].cells[0].second.real_value() : -1;
}
void seed(SqlEngine& e) {
    e.exec("CREATE TABLE docs (id INT, emb REAL[] NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO docs (id,emb) VALUES "
           "(1, ARRAY[1.0,0.0,0.0]), (2, ARRAY[0.0,1.0,0.0]), "
           "(3, ARRAY[0.9,0.1,0.0]), (4, ARRAY[0.0,0.0,1.0])");
}
}  // namespace

int main() {
    std::printf("=== sql_vector_test (K1 vector search) ===\n");

    for (bool columnar : {false, true}) {
        SqlEngine e;
        if (columnar) e.set_columnar_default(true);
        seed(e);
        if (columnar) e.flush_columnar("docs");
        const std::string tag = columnar ? "columnar " : "row ";

        // Exact distances to the query vector [1,0,0].
        check(d0(e.exec("SELECT l2_distance(emb, ARRAY[1.0,0.0,0.0]) FROM docs WHERE id = 1")) == 0.0,
              tag + "L2 to itself = 0");
        // id 3 = [0.9,0.1,0.0]: L2 = sqrt(0.01 + 0.01) = sqrt(0.02) ~= 0.1414.
        {
            const double d = d0(e.exec("SELECT l2_distance(emb, ARRAY[1.0,0.0,0.0]) FROM docs WHERE id = 3"));
            check(d > 0.1414 && d < 0.1415, tag + "L2 id3 ~= 0.1414");
        }
        // Cosine distance: identical direction = 0, orthogonal = 1.
        check(d0(e.exec("SELECT cosine_distance(emb, ARRAY[1.0,0.0,0.0]) FROM docs WHERE id = 1")) == 0.0,
              tag + "cosine to itself = 0");
        check(d0(e.exec("SELECT cosine_distance(emb, ARRAY[1.0,0.0,0.0]) FROM docs WHERE id = 2")) == 1.0,
              tag + "cosine orthogonal = 1");
        // Inner product: [0.9,0.1,0]·[1,1,1] = 1.0.
        check(d0(e.exec("SELECT inner_product(emb, ARRAY[1.0,1.0,1.0]) FROM docs WHERE id = 3")) == 1.0,
              tag + "inner_product id3 = 1.0");

        // Brute-force k-NN: the 2 nearest to [1,0,0] are id 1 (itself) then id 3 (0.9,0.1,0).
        const ExecResult knn = e.exec(
            "SELECT id, l2_distance(emb, ARRAY[1.0,0.0,0.0]) AS d FROM docs ORDER BY d LIMIT 2");
        check(ids(knn) == (std::vector<std::int64_t>{1, 3}), tag + "k-NN (k=2) -> id 1, 3");

        // Cosine k-NN gives the same top-2 ordering here.
        const ExecResult cknn = e.exec(
            "SELECT id, cosine_distance(emb, ARRAY[1.0,0.0,0.0]) AS d FROM docs ORDER BY d LIMIT 2");
        check(ids(cknn) == (std::vector<std::int64_t>{1, 3}), tag + "cosine k-NN -> id 1, 3");

        // Dimension mismatch is a clean error.
        check(!e.exec("SELECT l2_distance(emb, ARRAY[1.0,0.0]) FROM docs WHERE id = 1").ok,
              tag + "dimension mismatch errors");
    }

    // Durability: embeddings recover; k-NN still works after a restart.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x7EC70ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        { SqlEngine e(sched, data, sched, cat); seed(e); }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            const ExecResult knn = e.exec(
                "SELECT id, l2_distance(emb, ARRAY[1.0,0.0,0.0]) AS d FROM docs ORDER BY d LIMIT 2");
            check(ids(knn) == (std::vector<std::int64_t>{1, 3}), "k-NN after restart -> id 1, 3");
        }
    }

    if (g_fail != 0) { std::printf("sql_vector_test: FAILURES\n"); return 1; }
    std::printf("sql_vector_test: ALL PASS\n");
    return 0;
}
