// Same corpus/embeddings as run_mem0.py, engine driven IN-PROCESS (mem0's shape):
// splits the MCP subprocess+JSON transport tax from the actual engine cost.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <lockstep/query/sql/Engine.hpp>
using namespace lockstep::query::sql;
using Clock = std::chrono::steady_clock;
static const int NW = 84;
static std::string words[NW];
static std::string word_at(long long i, long long j) {
    unsigned long long h = (i * 2654435761ULL + j * 40503ULL + 0x9E3779B9ULL) & 0xFFFFFFFFULL;
    h ^= h >> 13;
    return words[h % NW];
}
static std::string mem_text(long long i) {
    std::string s = "note" + std::to_string(i);
    for (int j = 0; j < 8; ++j) s += " " + word_at(i, j);
    return s;
}
static std::string emb(const std::string& text) {  // same 32-dim FNV BoW, normalized
    double v[32] = {0};
    std::string w;
    auto flush = [&] {
        if (w.empty()) return;
        unsigned long long h = 2166136261ULL;
        for (char c : w) h = ((h ^ static_cast<unsigned char>(c)) * 16777619ULL) & 0xFFFFFFFFULL;
        v[h % 32] += 1.0;
        w.clear();
    };
    for (char c : text) { if (c == ' ') flush(); else w += static_cast<char>(std::tolower(c)); }
    flush();
    double n = 0; for (double x : v) n += x * x; n = n > 0 ? std::sqrt(n) : 1.0;
    std::string s = "[";
    for (int k = 0; k < 32; ++k) { if (k) s += ','; s += std::to_string(v[k] / n); }
    return s + "]";
}
int main() {
    for (int i = 0; i < NW; ++i) words[i] = i < 64 ? "w" + std::string(i < 10 ? "0" : "") + std::to_string(i)
        : std::vector<std::string>{"raft","election","timeout","quorum","snapshot","compaction","vector","recall","bm25","hybrid","deploy","pipeline","conformance","gate","agent","memory","cursor","exactly","once","replica"}[i - 64];
    SqlEngine e;
    e.set_trace_enabled(false);
    e.exec("CREATE TABLE agent_memory (id INT AUTO_INCREMENT, kind TEXT, content TEXT NOT NULL, embedding VECTOR(32), PRIMARY KEY (id))");
    e.exec("CREATE INDEX am_txt ON agent_memory (content) USING BM25");
    e.exec("CREATE INDEX am_vec ON agent_memory (embedding) USING IVFFLAT WITH (lists = 8, probes = 8)");
    const int N = 2000, Q = 200;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        const std::string c = mem_text(i);
        if (!e.exec("INSERT INTO agent_memory (content, kind, embedding) VALUES ('" + c + "', 'fact', '" + emb(c) + "')").ok) { std::printf("BAD\n"); return 1; }
    }
    auto t1 = Clock::now();
    int hits = 0;
    auto t2 = Clock::now();
    for (int qi = 0; qi < Q; ++qi) {
        const long long i = qi * 10;
        std::string q;
        for (int j : {0, 2, 4, 6}) q += (q.empty() ? "" : " ") + word_at(i, j);
        auto r = e.exec("SELECT content FROM agent_memory ORDER BY rrf_score(embedding, '" + emb(q) + "', content, '" + q + "') DESC LIMIT 5");
        for (auto& row : r.rows) if (row.cells[0].second.s == mem_text(i)) { ++hits; break; }
    }
    auto t3 = Clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0; };
    std::printf("in-process: add %.0f ops/s | search %.0f q/s | recall@5 %.3f\n",
                N / ms(t0, t1) * 1000, Q / ms(t2, t3) * 1000, hits / double(Q));
    return 0;
}
