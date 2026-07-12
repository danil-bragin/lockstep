// sql_bm25_test.cpp — K2: BM25 full-text search. USING BM25 posting index (term -> tf, dl
// + corpus stats) over one TEXT column; top-k via ORDER BY bm25_score(col,'q') DESC LIMIT k;
// MATCHES(col,'q') per-row predicate. DIFFERENTIAL GATE (K2.6): an INDEPENDENT reference
// BM25 (own tokenizer + formula, computed in the test over the raw corpus) must produce
// the SAME ranking as the engine's index path. Row + columnar + maintenance + restart.
#include <cmath>
#include <cstdio>
#include <map>
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
    v.reserve(r.rows.size());
    for (const auto& row : r.rows) v.push_back(row.cells[0].second.i);
    return v;
}

// --- the INDEPENDENT reference: own tokenizer + classic BM25 (k1=1.2, b=0.75) ---------
std::map<std::string, int> rtok(const std::string& s, int& dl) {
    std::map<std::string, int> tf;
    dl = 0;
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 2) { ++tf[cur]; ++dl; }
        cur.clear();
    };
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur.push_back(c);
        else flush();
    }
    flush();
    return tf;
}
std::vector<std::int64_t> ref_topk(const std::map<std::int64_t, std::string>& corpus,
                                   const std::string& query, std::size_t k) {
    int qdl = 0;
    const auto qt = rtok(query, qdl);
    const double n = static_cast<double>(corpus.size());
    double total = 0;
    std::map<std::int64_t, std::pair<std::map<std::string, int>, int>> docs;
    for (const auto& [id, body] : corpus) {
        int dl = 0;
        auto tf = rtok(body, dl);
        total += dl;
        docs[id] = {std::move(tf), dl};
    }
    const double avgdl = total / n;
    std::map<std::int64_t, double> score;
    for (const auto& [term, qc] : qt) {
        (void)qc;
        double df = 0;
        for (const auto& [id, d] : docs)
            if (d.first.count(term)) ++df;
        if (df == 0) continue;
        const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
        for (const auto& [id, d] : docs) {
            const auto it = d.first.find(term);
            if (it == d.first.end()) continue;
            const double tf = it->second, dl = d.second;
            score[id] += idf * (tf * 2.2) / (tf + 1.2 * (1.0 - 0.75 + 0.75 * dl / avgdl));
        }
    }
    std::vector<std::pair<double, std::int64_t>> hits;
    for (const auto& [id, s] : score) hits.emplace_back(s, id);
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < hits.size() && i < k; ++i) out.push_back(hits[i].second);
    return out;
}


// Reference RRF: fuse two reference rankings with 1/(60+rank), (score DESC, id ASC).
std::vector<std::int64_t> ref_rrf(const std::vector<std::int64_t>& a,
                                  const std::vector<std::int64_t>& b, std::size_t k) {
    std::map<std::int64_t, double> f;
    for (std::size_t i = 0; i < a.size(); ++i) f[a[i]] += 1.0 / (60.0 + i + 1);
    for (std::size_t i = 0; i < b.size(); ++i) f[b[i]] += 1.0 / (60.0 + i + 1);
    std::vector<std::pair<double, std::int64_t>> h;
    for (const auto& [id, s] : f) h.emplace_back(s, id);
    std::sort(h.begin(), h.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first > y.first;
        return x.second < y.second;
    });
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < h.size() && i < k; ++i) out.push_back(h[i].second);
    return out;
}
std::map<std::int64_t, std::string> corpus() {
    return {{1, "the quick brown fox jumps over the lazy dog"},
            {2, "a quick brown cat sits on the mat"},
            {3, "raft consensus makes replicated logs safe"},
            {4, "the raft raft raft protocol elects a leader"},
            {5, "database replication with raft consensus"},
            {6, "lazy evaluation of database queries"},
            {7, "brown bears eat quick salmon"},
            {8, "vector search inside a sql database"}};
}
void seed(SqlEngine& e) {
    e.exec("CREATE TABLE docs (id INT, body TEXT, PRIMARY KEY (id))");
    std::string ins = "INSERT INTO docs (id,body) VALUES ";
    bool first = true;
    for (const auto& [id, body] : corpus()) {
        if (!first) ins += ",";
        first = false;
        ins += "(" + std::to_string(id) + ",'" + body + "')";
    }
    e.exec(ins);
    e.exec("CREATE INDEX docs_fts ON docs (body) USING BM25");
}
void expect_topk(SqlEngine& e, const std::map<std::int64_t, std::string>& c,
                 const std::string& q, std::size_t k, const std::string& tag) {
    const ExecResult r = e.exec("SELECT id FROM docs ORDER BY bm25_score(body, '" + q +
                                "') DESC LIMIT " + std::to_string(k));
    check(r.ok && ids(r) == ref_topk(c, q, k), tag + " top-" + std::to_string(k) + " '" + q +
                                                   "' == reference BM25");
}
}  // namespace

int main() {
    std::printf("=== sql_bm25_test (K2 full-text) ===\n");
    for (bool columnar : {false, true}) {
        SqlEngine e;
        if (columnar) e.set_columnar_default(true);
        auto c = corpus();
        seed(e);
        if (columnar) e.flush_columnar("docs");
        const std::string tag = columnar ? "columnar " : "row ";

        // K2.6 differential gate: engine ranking == independent reference, several queries.
        expect_topk(e, c, "raft consensus", 3, tag);
        expect_topk(e, c, "quick brown", 4, tag);
        expect_topk(e, c, "database", 3, tag);
        expect_topk(e, c, "lazy dog", 2, tag);

        // MATCHES predicate (no index needed; AND semantics over terms).
        {
            const ExecResult r =
                e.exec("SELECT id FROM docs WHERE MATCHES(body, 'raft consensus') = 1 ORDER BY id");
            check(ids(r) == (std::vector<std::int64_t>{3, 5}), tag + "MATCHES AND-semantics");
        }
        // Maintenance: INSERT re-ranks; UPDATE re-tokenizes; DELETE drops from results.
        e.exec("INSERT INTO docs (id,body) VALUES (9, 'raft raft consensus consensus raft')");
        c[9] = "raft raft consensus consensus raft";
        expect_topk(e, c, "raft consensus", 3, tag + "post-INSERT ");
        e.exec("UPDATE docs SET body = 'gardening tips for spring' WHERE id = 4");
        c[4] = "gardening tips for spring";
        expect_topk(e, c, "raft consensus", 3, tag + "post-UPDATE ");
        e.exec("DELETE FROM docs WHERE id = 9");
        c.erase(9);
        expect_topk(e, c, "raft consensus", 3, tag + "post-DELETE ");


        // --- K2.4: hybrid RRF (vectors + BM25 in ONE query) ---
        e.exec("CREATE TABLE hyb (id INT, emb VECTOR(2) NOT NULL, body TEXT, PRIMARY KEY (id))");
        e.exec("INSERT INTO hyb (id,emb,body) VALUES "
               "(1,'[0,0]','raft consensus replication'), (2,'[0.2,0]','quick brown fox'), "
               "(3,'[5,5]','raft consensus protocol'), (4,'[0.1,0.1]','database queries'), "
               "(5,'[5,5.2]','lazy dog sleeps'), (6,'[0.3,0.2]','raft leader election')");
        if (columnar) e.flush_columnar("hyb");
        e.exec("CREATE INDEX hyb_v ON hyb (emb) USING IVFFLAT WITH (lists = 2, probes = 2)");
        e.exec("CREATE INDEX hyb_t ON hyb (body) USING BM25");
        {
            // Reference legs: exact vector ranking (engine exact path == gated) and the
            // reference BM25 over the hyb corpus; fuse with reference RRF.
            const ExecResult vr = e.exec(
                "SELECT id, l2_distance(emb,'[0,0]') AS d FROM hyb ORDER BY d LIMIT 60");
            std::map<std::int64_t, std::string> hc = {
                {1, "raft consensus replication"}, {2, "quick brown fox"},
                {3, "raft consensus protocol"},    {4, "database queries"},
                {5, "lazy dog sleeps"},            {6, "raft leader election"}};
            const auto want = ref_rrf(ids(vr), ref_topk(hc, "raft consensus", 60), 3);
            const ExecResult r = e.exec(
                "SELECT id FROM hyb ORDER BY rrf_score(emb, '[0,0]', body, 'raft consensus') "
                "DESC LIMIT 3");
            check(r.ok && ids(r) == want, tag + "hybrid RRF == reference fusion");
        }
        // Hybrid teeth: missing either index -> clean RRF_SCORE error (no silent scan).
        e.exec("CREATE TABLE hyb2 (id INT, emb VECTOR(2) NOT NULL, body TEXT, PRIMARY KEY (id))");
        e.exec("INSERT INTO hyb2 (id,emb,body) VALUES (1,'[0,0]','x y')");
        check(!e.exec("SELECT id FROM hyb2 ORDER BY rrf_score(emb,'[0,0]',body,'x') DESC LIMIT 1").ok,
              tag + "RRF without indexes is a clean error");

        // Teeth.
        check(!e.exec("CREATE INDEX bad1 ON docs (id) USING BM25").ok,
              tag + "BM25 on INT rejected");
        check(!e.exec("CREATE UNIQUE INDEX bad2 ON docs (body) USING BM25").ok,
              tag + "UNIQUE BM25 rejected");
        check(!e.exec("SELECT id, bm25_score(body,'raft') FROM docs").ok,
              tag + "projected bm25_score is a clean error");
    }

    // Durability: postings + stats recover; ranking identical; INSERT after restart works.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0xB2500ull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        { SqlEngine e(sched, data, sched, cat); seed(e); }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            auto c = corpus();
            expect_topk(e, c, "raft consensus", 3, "restart ");
            e.exec("INSERT INTO docs (id,body) VALUES (9, 'raft consensus everywhere')");
            c[9] = "raft consensus everywhere";
            expect_topk(e, c, "raft consensus", 3, "restart post-INSERT ");
        }
    }

    if (g_fail != 0) { std::printf("sql_bm25_test: FAILURES\n"); return 1; }
    std::printf("sql_bm25_test: ALL PASS\n");
    return 0;
}
