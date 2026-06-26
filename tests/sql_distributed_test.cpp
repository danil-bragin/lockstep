// sql_distributed_test.cpp — DISTRIBUTED SQL (⭐2) scatter-gather gate. A table sharded across M
// SqlEngines (via DistributedSql: CREATE broadcast, INSERT routed by PK hash, aggregate SELECT
// scattered + merged) must answer IDENTICALLY to ONE SqlEngine holding all the rows. Asserts, over
// a seeded workload, that the distributed result is BYTE-IDENTICAL to the single-node oracle for:
// scalar aggregates, GROUP BY (int + text key), a filtered aggregate, and a point SELECT — plus
// that the rows really were SPREAD across shards (the routing is non-trivial). Distributed AVG is
// rejected (asserted). Determinism: only the seed (inlined SplitMix). No clock/thread/rng.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lockstep/query/sql/DistributedSql.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

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

class SplitMix {
public:
    explicit SplitMix(std::uint64_t s) : s_(s) {}
    std::uint64_t next() {
        s_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
private:
    std::uint64_t s_;
};

const char* kDDL =
    "CREATE TABLE t (id INT, amount INT NOT NULL, cat INT NOT NULL, region TEXT NOT NULL, "
    "PRIMARY KEY (id))";

}  // namespace

int main() {
    constexpr std::size_t kShards = 4;
    constexpr std::size_t kRows = 2000;

    SqlEngine solo;
    solo.set_columnar_default(true);

    std::vector<SqlEngine> shards(kShards);
    std::vector<SqlEngine*> shard_ptrs;
    for (SqlEngine& s : shards) {
        s.set_columnar_default(true);
        shard_ptrs.push_back(&s);
    }
    DistributedSql dist(shard_ptrs);

    check(solo.exec(kDDL).ok, "solo create");
    check(dist.exec(kDDL).ok, "dist create (broadcast)");

    const char* regs[] = {"north", "south", "east", "west", "central"};
    SplitMix rng(12345);
    for (std::size_t i = 0; i < kRows; ++i) {
        const std::int64_t amount = static_cast<std::int64_t>(rng.next() % 1000);
        const std::int64_t cat = static_cast<std::int64_t>(i % 8);
        const std::string sql =
            "INSERT INTO t (id, amount, cat, region) VALUES (" + std::to_string(i) + ", " +
            std::to_string(amount) + ", " + std::to_string(cat) + ", '" + regs[i % 5] + "')";
        check(solo.exec(sql).ok, "solo insert");
        check(dist.exec(sql).ok, "dist insert (routed)");  // routed to one shard by PK hash
    }
    for (SqlEngine& s : shards) s.flush_columnar("t");
    solo.flush_columnar("t");

    // Rows really spread across shards (routing is non-trivial — not all on one shard).
    std::size_t nonempty = 0, spread_total = 0;
    for (SqlEngine& s : shards) {
        const ExecResult c = s.exec("SELECT COUNT(*) FROM t");
        const std::int64_t n = c.rows.empty() ? 0 : c.rows[0].cells[0].second.i;
        if (n > 0) ++nonempty;
        spread_total += static_cast<std::size_t>(n);
    }
    check(nonempty == kShards, "rows spread across ALL shards (got nonempty=" +
                                   std::to_string(nonempty) + "/" + std::to_string(kShards) + ")");
    check(spread_total == kRows, "no row lost/dup in routing (total=" +
                                     std::to_string(spread_total) + ")");

    // Distributed == single-node oracle, byte-for-byte.
    const std::vector<std::string> queries = {
        "SELECT COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM t",
        "SELECT cat, COUNT(*), SUM(amount), MIN(amount), MAX(amount) FROM t GROUP BY cat",
        "SELECT region, COUNT(*), SUM(amount) FROM t GROUP BY region",
        "SELECT COUNT(*), SUM(amount) FROM t WHERE amount > 500",
        "SELECT amount FROM t WHERE id = 1234",     // point read -> one shard
        "SELECT COUNT(*) FROM t WHERE amount > 999999",  // zero-match aggregate
    };
    for (const std::string& q : queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "distributed != solo for [" + q + "]\n  solo=[" + s + "]\n  dist=[" + d + "]");
    }

    // Distributed AVG is explicitly rejected (can't merge an averaged value across shards).
    const ExecResult avg = dist.exec("SELECT AVG(amount) FROM t");
    check(!avg.ok && avg.error.find("AVG") != std::string::npos,
          "distributed AVG rejected with a clear error");

    if (g_fail) {
        std::printf("sql_distributed_test: FAILED\n");
        return 1;
    }
    std::printf("sql_distributed_test: OK (distributed == single-node across %zu shards)\n", kShards);
    return 0;
}
