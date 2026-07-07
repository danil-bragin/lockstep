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
// K1.1: a dedicated VECTOR(3) table — the pgvector-style fixed-dimension type. Mixed literal
// forms: ARRAY[...] and the pgvector '[x,y,z]' text.
void seed_vec(SqlEngine& e) {
    e.exec("CREATE TABLE items (id INT, emb VECTOR(3) NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO items (id,emb) VALUES "
           "(1, '[1,0,0]'), (2, ARRAY[0.0,1.0,0.0]), (3, '[0.9, 0.1, 0]'), (4, '[0,0,1]')");
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

        // --- K1.1: the VECTOR(n) type ---
        seed_vec(e);
        if (columnar) e.flush_columnar("items");
        // Render is pgvector's '[x,y,z]' text form (canonical REAL payload).
        {
            const ExecResult r = e.exec("SELECT emb FROM items WHERE id = 1");
            check(r.ok && r.rows.size() == 1 && r.rows[0].cells[0].second.render() == "[1,0,0]",
                  tag + "VECTOR renders as [1,0,0]");
        }
        {
            const ExecResult r = e.exec("SELECT emb FROM items WHERE id = 3");
            check(r.ok && r.rows.size() == 1 && r.rows[0].cells[0].second.render() == "[0.9,0.1,0]",
                  tag + "VECTOR renders [0.9,0.1,0]");
        }
        // Distance functions accept a VECTOR column + a '[x,y,z]' text literal query vector.
        check(d0(e.exec("SELECT l2_distance(emb, '[1,0,0]') FROM items WHERE id = 1")) == 0.0,
              tag + "VECTOR L2 to itself = 0");
        check(d0(e.exec("SELECT cosine_distance(emb, '[1,0,0]') FROM items WHERE id = 2")) == 1.0,
              tag + "VECTOR cosine orthogonal = 1");
        // ...and an ARRAY[...] query vector against a VECTOR column.
        check(d0(e.exec("SELECT inner_product(emb, ARRAY[1.0,1.0,1.0]) FROM items WHERE id = 3")) == 1.0,
              tag + "VECTOR inner_product = 1.0");
        // k-NN over the VECTOR column.
        {
            const ExecResult knn = e.exec(
                "SELECT id, l2_distance(emb, '[1,0,0]') AS d FROM items ORDER BY d LIMIT 2");
            check(ids(knn) == (std::vector<std::int64_t>{1, 3}), tag + "VECTOR k-NN -> id 1, 3");
        }
        // The declared dimension is enforced at INSERT (both literal forms), NULL elements and
        // the empty vector are rejected, and a non-numeric element is a clean error.
        check(!e.exec("INSERT INTO items (id,emb) VALUES (10, '[1,0]')").ok,
              tag + "INSERT wrong-dim text literal rejected");
        check(!e.exec("INSERT INTO items (id,emb) VALUES (11, ARRAY[1.0,2.0])").ok,
              tag + "INSERT wrong-dim ARRAY rejected");
        check(!e.exec("INSERT INTO items (id,emb) VALUES (12, ARRAY[1.0,NULL,2.0])").ok,
              tag + "INSERT NULL element rejected");
        check(!e.exec("INSERT INTO items (id,emb) VALUES (13, '[]')").ok,
              tag + "INSERT empty vector rejected");
        check(!e.exec("INSERT INTO items (id,emb) VALUES (14, '[a,b,c]')").ok,
              tag + "INSERT non-numeric element rejected");
        // DESCRIBE shows the declared dimension.
        {
            const ExecResult r = e.exec("DESCRIBE items");
            bool found = false;
            for (const auto& row : r.rows)
                if (row.cells[0].second.s == "emb" && row.cells[1].second.s == "VECTOR(3)") found = true;
            check(found, tag + "DESCRIBE shows VECTOR(3)");
        }
        // --- K1.2: pgvector distance operators <-> / <#> / <=> + ORDER BY expression ---
        // Operator spelling == function spelling (same kernel).
        {
            const double d = d0(e.exec("SELECT emb <-> '[1,0,0]' AS d FROM items WHERE id = 3"));
            check(d > 0.1414 && d < 0.1415, tag + "<-> == l2_distance");
        }
        check(d0(e.exec("SELECT emb <=> '[1,0,0]' AS d FROM items WHERE id = 2")) == 1.0,
              tag + "<=> == cosine_distance");
        // <#> is pgvector's NEGATIVE inner product.
        check(d0(e.exec("SELECT emb <#> '[1,1,1]' AS d FROM items WHERE id = 3")) == -1.0,
              tag + "<#> == -inner_product");
        // The pgvector k-NN idiom: ORDER BY a distance EXPRESSION (not projected) + LIMIT.
        {
            const ExecResult knn = e.exec(
                "SELECT id FROM items ORDER BY emb <-> '[1,0,0]' LIMIT 2");
            check(ids(knn) == (std::vector<std::int64_t>{1, 3}), tag + "ORDER BY <-> LIMIT k");
        }
        // DESC + PK tie-break (id 2 and 4 are both at sqrt(2); the PK breaks the tie).
        {
            const ExecResult r = e.exec(
                "SELECT id FROM items ORDER BY emb <-> '[1,0,0]' DESC LIMIT 1");
            check(ids(r) == (std::vector<std::int64_t>{2}), tag + "ORDER BY <-> DESC");
        }
        // SELECT * + ORDER BY expression: the hidden sort cell is stripped from the output.
        {
            const ExecResult r = e.exec("SELECT * FROM items ORDER BY emb <-> '[1,0,0]' LIMIT 1");
            check(r.ok && r.rows.size() == 1 && r.rows[0].cells.size() == 2 &&
                      r.rows[0].cells[0].second.i == 1,
                  tag + "SELECT * ORDER BY <-> strips the hidden cell");
        }
        // A distance operator in WHERE (J1 expression operand).
        {
            const ExecResult r = e.exec(
                "SELECT id FROM items WHERE emb <-> '[1,0,0]' < 0.2 ORDER BY id");
            check(ids(r) == (std::vector<std::int64_t>{1, 3}), tag + "WHERE <-> < 0.2");
        }
        // '<=' is still '<=' (the <=> lexer change must not break it).
        {
            const ExecResult r = e.exec("SELECT id FROM items WHERE id <= 2 ORDER BY id");
            check(ids(r) == (std::vector<std::int64_t>{1, 2}), tag + "<= untouched");
        }
        // VECTOR[] (array of vectors) is rejected at parse.
        check(!e.exec("CREATE TABLE bad (id INT, v VECTOR(2)[], PRIMARY KEY (id))").ok,
              tag + "VECTOR[] rejected");
        // A dimension outside 1..16000 is rejected.
        check(!e.exec("CREATE TABLE bad2 (id INT, v VECTOR(0), PRIMARY KEY (id))").ok,
              tag + "VECTOR(0) rejected");
    }

    // Durability: embeddings recover; k-NN still works after a restart.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x7EC70ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        { SqlEngine e(sched, data, sched, cat); seed(e); seed_vec(e); }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            const ExecResult knn = e.exec(
                "SELECT id, l2_distance(emb, ARRAY[1.0,0.0,0.0]) AS d FROM docs ORDER BY d LIMIT 2");
            check(ids(knn) == (std::vector<std::int64_t>{1, 3}), "k-NN after restart -> id 1, 3");
            // K1.1: the VECTOR table recovers too — schema (dimension) + payload + render.
            const ExecResult r = e.exec("SELECT emb FROM items WHERE id = 1");
            check(r.ok && r.rows.size() == 1 && r.rows[0].cells[0].second.render() == "[1,0,0]",
                  "VECTOR renders after restart");
            check(!e.exec("INSERT INTO items (id,emb) VALUES (10, '[1,0]')").ok,
                  "VECTOR dim enforced after restart");
            const ExecResult vknn = e.exec(
                "SELECT id, l2_distance(emb, '[1,0,0]') AS d FROM items ORDER BY d LIMIT 2");
            check(ids(vknn) == (std::vector<std::int64_t>{1, 3}), "VECTOR k-NN after restart -> id 1, 3");
        }
    }

    if (g_fail != 0) { std::printf("sql_vector_test: FAILURES\n"); return 1; }
    std::printf("sql_vector_test: ALL PASS\n");
    return 0;
}
