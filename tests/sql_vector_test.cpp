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
// K1.3: two well-separated clusters (A near the origin, B near [10,10]) + an IVFFLAT index
// with lists=2 — the deterministic k-means must split them, so probes=1 finds a whole cluster.
void seed_ann(SqlEngine& e) {
    e.exec("CREATE TABLE ann (id INT, emb VECTOR(2) NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO ann (id,emb) VALUES "
           "(1,'[0,0]'), (2,'[1,0]'), (3,'[0,1]'), (4,'[1,1]'), (5,'[0.5,0.5]'), "
           "(6,'[10,10]'), (7,'[11,10]'), (8,'[10,11]'), (9,'[11,11]'), (10,'[10.5,10.5]')");
    e.exec("CREATE INDEX ann_ivf ON ann (emb) USING IVFFLAT WITH (lists = 2, probes = 1)");
}
// K1.4: the same two spatial clusters, indexed by the deterministic HNSW graph.
void seed_hnsw(SqlEngine& e) {
    e.exec("CREATE TABLE annh (id INT, emb VECTOR(2) NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO annh (id,emb) VALUES "
           "(1,'[0,0]'), (2,'[1,0]'), (3,'[0,1]'), (4,'[1,1]'), (5,'[0.5,0.5]'), "
           "(6,'[10,10]'), (7,'[11,10]'), (8,'[10,11]'), (9,'[11,11]'), (10,'[10.5,10.5]')");
    e.exec("CREATE INDEX annh_ix ON annh (emb) USING HNSW WITH (m = 4, ef_construction = 32)");
}
// K1.3c: two DIRECTION clusters (along +x and along +y) with mixed magnitudes — the cosine
// opclass buckets by direction, which plain L2 clustering would scatter by magnitude.
void seed_annc(SqlEngine& e) {
    e.exec("CREATE TABLE annc (id INT, emb VECTOR(2) NOT NULL, PRIMARY KEY (id))");
    e.exec("INSERT INTO annc (id,emb) VALUES "
           "(1,'[1,0]'), (2,'[5,0.1]'), (3,'[20,1]'), (4,'[3,0.05]'), (5,'[9,0.4]'), "
           "(6,'[0,1]'), (7,'[0.1,7]'), (8,'[1,30]'), (9,'[0.05,2]'), (10,'[0.3,11]')");
    e.exec("CREATE INDEX annc_cos ON annc (emb vector_cosine_ops) USING IVFFLAT "
           "WITH (lists = 2, probes = 1)");
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

        // --- K1.3: IVFFLAT approximate k-NN index ---
        seed_ann(e);
        if (columnar) e.flush_columnar("ann");
        // probes=1 near cluster A. Distances from [0,0]: id1=0, id5~0.707, id2=id3=1 (PK breaks
        // the tie -> id2). The whole cluster lives in the probed list, so this matches exact.
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 3");
            check(ids(r) == (std::vector<std::int64_t>{1, 5, 2}), tag + "ivfflat k-NN cluster A");
        }
        {
            const ExecResult ref = e.exec(
                "SELECT id, l2_distance(emb,'[0,0]') AS d FROM ann ORDER BY d LIMIT 3");
            check(ids(ref) == (std::vector<std::int64_t>{1, 5, 2}),
                  tag + "brute-force reference agrees");
        }
        // Maintenance: INSERT lands in the right list; DELETE disappears; UPDATE moves the row.
        e.exec("INSERT INTO ann (id,emb) VALUES (11, '[0.1,0.1]')");
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{1, 11}), tag + "ivfflat INSERT maintained");
        }
        e.exec("DELETE FROM ann WHERE id = 11");
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{1, 5}), tag + "ivfflat DELETE maintained");
        }
        e.exec("UPDATE ann SET emb = '[10,10.1]' WHERE id = 1");  // id 1 moves to cluster B
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[10,10]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{6, 1}), tag + "ivfflat UPDATE moved the row");
        }
        e.exec("UPDATE ann SET emb = '[0,0]' WHERE id = 1");  // restore
        // Differential gate: probes = lists probes EVERY entry, so the index result must equal
        // the brute-force scan EXACTLY (same rows, same order — shared kernel + PK tie-break).
        e.exec("DROP INDEX ann_ivf ON ann");
        check(e.exec("CREATE INDEX ann_ivf2 ON ann (emb) USING IVFFLAT WITH (lists = 2, probes = 2)").ok,
              tag + "CREATE IVFFLAT probes=lists");
        {
            const ExecResult a = e.exec("SELECT id FROM ann ORDER BY emb <-> '[5,5]' LIMIT 10");
            const ExecResult b = e.exec(
                "SELECT id, l2_distance(emb,'[5,5]') AS d FROM ann ORDER BY d LIMIT 10");
            check(a.ok && b.ok && ids(a) == ids(b) && a.rows.size() == 10,
                  tag + "probes=lists == exact scan (differential gate)");
        }
        // A WHERE alongside the distance ORDER BY falls back to the exact path (still correct).
        {
            const ExecResult r = e.exec(
                "SELECT id FROM ann WHERE id <= 5 ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{1, 5}), tag + "WHERE + <-> exact fallback");
        }
        // K1.3d: EXPLAIN shows the ANN path (same matcher as the executor — the plan is honest).
        {
            const ExecResult r = e.exec("EXPLAIN SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 3");
            bool has = false;
            for (const auto& row : r.rows)
                if (row.cells[0].second.s.find("Ivfflat Scan") != std::string::npos) has = true;
            check(r.ok && has, tag + "EXPLAIN shows Ivfflat Scan");
        }
        // ...and a non-matching shape (WHERE present) plans as a plain scan, not ANN.
        {
            const ExecResult r = e.exec(
                "EXPLAIN SELECT id FROM ann WHERE id <= 5 ORDER BY emb <-> '[0,0]' LIMIT 3");
            bool has = false;
            for (const auto& row : r.rows)
                if (row.cells[0].second.s.find("Ivfflat Scan") != std::string::npos) has = true;
            check(r.ok && !has, tag + "EXPLAIN: WHERE shape is not ANN");
        }
        // K1.3b: SET ivfflat.probes overrides the index default (ann_ivf2 has probes=2) for
        // the session; 0 restores each index's own default.
        e.exec("SET ivfflat.probes = 1");
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 10");
            check(r.ok && r.rows.size() == 5, tag + "SET ivfflat.probes=1 probes one list");
        }
        e.exec("SET ivfflat.probes = 0");
        {
            const ExecResult r = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 10");
            check(r.ok && r.rows.size() == 10, tag + "SET ivfflat.probes=0 restores the default");
        }

        // --- K1.3c: cosine / inner-product operator classes ---
        seed_annc(e);
        if (columnar) e.flush_columnar("annc");
        // probes=1 towards +x finds the whole +x DIRECTION cluster regardless of magnitude,
        // and the ordering matches the exact scan (shared kernel, identical FP accumulation).
        {
            const ExecResult a = e.exec("SELECT id FROM annc ORDER BY emb <=> '[1,0]' LIMIT 5");
            const ExecResult b = e.exec(
                "SELECT id, cosine_distance(emb,'[1,0]') AS d FROM annc ORDER BY d LIMIT 5");
            check(a.ok && b.ok && ids(a) == ids(b) && a.rows.size() == 5,
                  tag + "cosine ANN == exact top-5");
        }
        // The L2 operator does NOT use the cosine index (opclass mismatch -> exact fallback).
        {
            const ExecResult r = e.exec("SELECT id FROM annc ORDER BY emb <-> '[1,0]' LIMIT 1");
            check(ids(r) == (std::vector<std::int64_t>{1}), tag + "<-> on a cosine index falls back");
        }
        // IP opclass: <#> ASC == max inner product; probes = lists == exact (differential gate).
        check(e.exec("CREATE INDEX annc_ip ON annc (emb vector_ip_ops) USING IVFFLAT "
                     "WITH (lists = 2, probes = 2)").ok,
              tag + "CREATE IP IVFFLAT");
        {
            const ExecResult a = e.exec("SELECT id FROM annc ORDER BY emb <#> '[1,0]' LIMIT 3");
            check(a.ok && ids(a) == (std::vector<std::int64_t>{3, 5, 2}),
                  tag + "IP ANN (probes=lists) top-3 by dot");
        }
        // Opclass teeth: an opclass without IVFFLAT and an unknown opclass are clean errors.
        check(!e.exec("CREATE INDEX bad_oc ON annc (emb vector_cosine_ops)").ok,
              tag + "opclass without IVFFLAT rejected");
        check(!e.exec("CREATE INDEX bad_oc2 ON annc (emb vector_bogus_ops) USING IVFFLAT").ok,
              tag + "unknown opclass rejected");

        // --- K1.4: deterministic HNSW graph index ---
        seed_hnsw(e);
        if (columnar) e.flush_columnar("annh");
        // ef_search (default 40) >= N explores the whole connected graph => EXACT top-k.
        {
            const ExecResult r = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 3");
            check(ids(r) == (std::vector<std::int64_t>{1, 5, 2}), tag + "hnsw k-NN cluster A");
        }
        {
            const ExecResult a = e.exec("SELECT id FROM annh ORDER BY emb <-> '[5,5]' LIMIT 10");
            const ExecResult b = e.exec(
                "SELECT id, l2_distance(emb,'[5,5]') AS d FROM annh ORDER BY d LIMIT 10");
            check(a.ok && b.ok && ids(a) == ids(b) && a.rows.size() == 10,
                  tag + "hnsw ef>=N == exact scan (differential gate)");
        }
        // EXPLAIN shows the graph path.
        {
            const ExecResult r = e.exec("EXPLAIN SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 3");
            bool has = false;
            for (const auto& row : r.rows)
                if (row.cells[0].second.s.find("Hnsw Scan") != std::string::npos) has = true;
            check(r.ok && has, tag + "EXPLAIN shows Hnsw Scan");
        }
        // Maintenance: INSERT links live; DELETE zombies (excluded, still traversable);
        // UPDATE re-links under the same PK (same deterministic level).
        e.exec("INSERT INTO annh (id,emb) VALUES (11, '[0.1,0.1]')");
        {
            const ExecResult r = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{1, 11}), tag + "hnsw INSERT maintained");
        }
        e.exec("DELETE FROM annh WHERE id = 11");
        {
            const ExecResult r = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{1, 5}), tag + "hnsw DELETE zombied");
        }
        {   // the zombie stays out of a full-graph result too
            const ExecResult r = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 20");
            check(r.ok && r.rows.size() == 10, tag + "hnsw zombie excluded from results");
        }
        e.exec("UPDATE annh SET emb = '[10,10.1]' WHERE id = 1");
        {
            const ExecResult r = e.exec("SELECT id FROM annh ORDER BY emb <-> '[10,10]' LIMIT 2");
            check(ids(r) == (std::vector<std::int64_t>{6, 1}), tag + "hnsw UPDATE re-linked");
        }
        e.exec("UPDATE annh SET emb = '[0,0]' WHERE id = 1");  // restore
        // SET hnsw.ef_search widens the beam; results stay exact-equal here.
        e.exec("SET hnsw.ef_search = 100");
        {
            const ExecResult a = e.exec("SELECT id FROM annh ORDER BY emb <-> '[5,5]' LIMIT 10");
            const ExecResult b = e.exec(
                "SELECT id, l2_distance(emb,'[5,5]') AS d FROM annh ORDER BY d LIMIT 10");
            check(a.ok && b.ok && ids(a) == ids(b), tag + "hnsw ef_search=100 == exact");
        }
        e.exec("SET hnsw.ef_search = 0");  // restore the default
        // A cosine-opclass HNSW graph over the direction clusters (annc from K1.3c).
        check(e.exec("CREATE INDEX annc_hnsw ON annc (emb vector_cosine_ops) USING HNSW "
                     "WITH (m = 4)").ok,
              tag + "CREATE cosine HNSW");
        {
            // Two cosine indexes exist (ivfflat annc_cos + hnsw annc_hnsw); the matcher takes
            // the FIRST in CREATE order — annc_cos — so drop it to exercise the graph.
            (void)e.exec("DROP INDEX annc_cos ON annc");
            const ExecResult a = e.exec("SELECT id FROM annc ORDER BY emb <=> '[1,0]' LIMIT 5");
            const ExecResult b = e.exec(
                "SELECT id, cosine_distance(emb,'[1,0]') AS d FROM annc ORDER BY d LIMIT 5");
            check(a.ok && b.ok && ids(a) == ids(b) && a.rows.size() == 5,
                  tag + "cosine HNSW == exact top-5");
        }
        // HNSW teeth.
        check(!e.exec("CREATE INDEX bad_h1 ON annh (id) USING HNSW").ok,
              tag + "HNSW on non-VECTOR rejected");
        check(!e.exec("CREATE UNIQUE INDEX bad_h2 ON annh (emb) USING HNSW").ok,
              tag + "UNIQUE HNSW rejected");
        e.exec("CREATE TABLE annhu (id INT, emb VECTOR, PRIMARY KEY (id))");
        check(!e.exec("CREATE INDEX bad_h3 ON annhu (emb) USING HNSW").ok,
              tag + "undimensioned VECTOR HNSW rejected");

        // Teeth: IVFFLAT demands a dimensioned VECTOR(n) column and rejects UNIQUE.
        check(!e.exec("CREATE INDEX bad_ivf ON ann (id) USING IVFFLAT").ok,
              tag + "IVFFLAT on non-VECTOR rejected");
        check(!e.exec("CREATE UNIQUE INDEX bad_ivf2 ON ann (emb) USING IVFFLAT").ok,
              tag + "UNIQUE IVFFLAT rejected");
        e.exec("CREATE TABLE annu (id INT, emb VECTOR, PRIMARY KEY (id))");
        check(!e.exec("CREATE INDEX bad_ivf3 ON annu (emb) USING IVFFLAT").ok,
              tag + "undimensioned VECTOR rejected");
    }

    // Durability: embeddings recover; k-NN still works after a restart.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x7EC70ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        { SqlEngine e(sched, data, sched, cat); seed(e); seed_vec(e); seed_ann(e); seed_annc(e); seed_hnsw(e); }
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
            // K1.3: the IVFFLAT index (centroids included) recovers with the catalog — the ANN
            // path works, and a fresh INSERT is bucketed by the RECOVERED centroids.
            const ExecResult aknn = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 3");
            check(ids(aknn) == (std::vector<std::int64_t>{1, 5, 2}), "ivfflat k-NN after restart");
            e.exec("INSERT INTO ann (id,emb) VALUES (11, '[0.1,0.1]')");
            const ExecResult aknn2 = e.exec("SELECT id FROM ann ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(aknn2) == (std::vector<std::int64_t>{1, 11}),
                  "ivfflat INSERT after restart uses recovered centroids");
            // K1.3c: the opclass (vec_op) + its direction-space centroids recover too.
            const ExecResult cknn2 = e.exec("SELECT id FROM annc ORDER BY emb <=> '[1,0]' LIMIT 5");
            const ExecResult cref = e.exec(
                "SELECT id, cosine_distance(emb,'[1,0]') AS d FROM annc ORDER BY d LIMIT 5");
            check(cknn2.ok && cref.ok && ids(cknn2) == ids(cref) && cknn2.rows.size() == 5,
                  "cosine ivfflat after restart == exact");
            // K1.4: the HNSW graph (KV records + entry point + knobs) recovers — k-NN is
            // exact again, and a fresh INSERT links into the RECOVERED graph.
            const ExecResult h1 = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 3");
            check(ids(h1) == (std::vector<std::int64_t>{1, 5, 2}), "hnsw k-NN after restart");
            e.exec("INSERT INTO annh (id,emb) VALUES (11, '[0.1,0.1]')");
            const ExecResult h2 = e.exec("SELECT id FROM annh ORDER BY emb <-> '[0,0]' LIMIT 2");
            check(ids(h2) == (std::vector<std::int64_t>{1, 11}),
                  "hnsw INSERT after restart links into the recovered graph");
        }
    }

    if (g_fail != 0) { std::printf("sql_vector_test: FAILURES\n"); return 1; }
    std::printf("sql_vector_test: ALL PASS\n");
    return 0;
}
