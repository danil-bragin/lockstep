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

    // Oracle is ROW-MODE (the reference layout — columnar == row-mode == reference for the agg
    // queries; row-mode also gives a correct JOIN oracle, whereas a flushed-vs-unflushed columnar
    // JOIN is a separate columnar quirk out of scope here). Shards stay COLUMNAR (the distributed
    // path is exercised over the columnar layout).
    SqlEngine solo;

    std::vector<SqlEngine> shards(kShards);
    std::vector<EngineSqlShard> wraps;
    wraps.reserve(kShards);
    for (SqlEngine& s : shards) {
        s.set_columnar_default(true);
        wraps.emplace_back(&s);
    }
    std::vector<ISqlShard*> shard_ptrs;
    for (EngineSqlShard& w : wraps) shard_ptrs.push_back(&w);
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
    for (SqlEngine& s : shards) s.flush_columnar("t");  // shards columnar; solo is row-mode

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
        // Single-table GROUP BY + HAVING: a group spans shards, so HAVING must apply to the GLOBAL
        // aggregate (the coordinator gathers + runs single-node, not per-shard local HAVING).
        "SELECT cat, SUM(amount) FROM t GROUP BY cat HAVING SUM(amount) > 120000",
        "SELECT region, COUNT(*) FROM t GROUP BY region HAVING COUNT(*) > 380",
    };
    for (const std::string& q : queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "distributed != solo for [" + q + "]\n  solo=[" + s + "]\n  dist=[" + d + "]");
    }

    // ---- B4: a second sharded table + a JOIN + a GLOBAL-ORDER scan (gather + local execute) ----
    const char* dimDDL =
        "CREATE TABLE dim (cat INT, label TEXT NOT NULL, PRIMARY KEY (cat))";
    check(solo.exec(dimDDL).ok, "solo create dim");
    check(dist.exec(dimDDL).ok, "dist create dim");
    const char* labels[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    for (int cat = 0; cat < 8; ++cat) {
        const std::string ins = "INSERT INTO dim (cat, label) VALUES (" + std::to_string(cat) +
                                ", '" + labels[cat] + "')";
        check(solo.exec(ins).ok, "solo dim insert");
        check(dist.exec(ins).ok, "dist dim insert (routed)");
    }
    const std::vector<std::string> b4_queries = {
        // distributed JOIN (gather both tables, run locally) — fact group col => gather path
        "SELECT t.region, dim.label, COUNT(*) FROM t JOIN dim ON t.cat = dim.cat "
        "GROUP BY t.region, dim.label",
        // distributed GLOBAL-ORDER scan (gather the table, ORDER BY locally)
        "SELECT id, amount FROM t ORDER BY amount, id LIMIT 12",
        // STAR-SCHEMA PUSHDOWN (co-located shuffle): all-dim GROUP BY + fact aggregates => the fact
        // is aggregated per-key ON THE SHARDS, never gathered. Must equal the single-node oracle.
        "SELECT dim.label, COUNT(*), SUM(t.amount), MIN(t.amount), MAX(t.amount) "
        "FROM t JOIN dim ON t.cat = dim.cat GROUP BY dim.label",
        // PUSHDOWN + WHERE on the FACT (filters fact rows BEFORE the per-shard aggregate).
        "SELECT dim.label, COUNT(*), SUM(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "WHERE t.amount > 400 GROUP BY dim.label",
        // PUSHDOWN + WHERE on the DIM (filters the gathered dim; inner join drops unmatched fact).
        "SELECT dim.label, COUNT(*), MAX(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "WHERE dim.label < 'd' GROUP BY dim.label",
        // PUSHDOWN + WHERE spanning BOTH sides (ANDed) — each leaf routed to its own table.
        "SELECT dim.label, COUNT(*), SUM(t.amount), MIN(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "WHERE t.amount >= 200 AND t.amount <= 800 AND dim.label != 'b' GROUP BY dim.label",
        // PUSHDOWN + AVG (fact col) — expands to pushed SUM+COUNT, divided at the coordinator.
        "SELECT dim.label, AVG(t.amount) FROM t JOIN dim ON t.cat = dim.cat GROUP BY dim.label",
        // PUSHDOWN + AVG mixed with other aggregates AND a WHERE.
        "SELECT dim.label, AVG(t.amount), COUNT(*), MAX(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "WHERE t.amount > 300 GROUP BY dim.label",
        // PUSHDOWN + HAVING on a SELECTed aggregate (coordinator post-filter on rolled-up groups).
        "SELECT dim.label, SUM(t.amount) FROM t JOIN dim ON t.cat = dim.cat GROUP BY dim.label "
        "HAVING SUM(t.amount) > 100000",
        // PUSHDOWN + HAVING on an aggregate NOT in the SELECT list (pushed for HAVING only).
        "SELECT dim.label, MAX(t.amount) FROM t JOIN dim ON t.cat = dim.cat GROUP BY dim.label "
        "HAVING COUNT(*) > 240",
        // PUSHDOWN + WHERE + HAVING together.
        "SELECT dim.label, COUNT(*), SUM(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "WHERE t.amount >= 100 GROUP BY dim.label HAVING SUM(t.amount) > 80000 AND COUNT(*) >= 100",
    };
    const std::size_t pd_before = dist.pushdowns();
    for (const std::string& q : b4_queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "B4 distributed != solo for [" + q + "]\n  solo=[" + s + "]\n  dist=[" + d +
                          "]");
    }
    check(dist.pushdowns() - pd_before >= 9,
          "all 9 star-schema queries (base + 3 WHERE + 2 AVG + 3 HAVING) took the pushdown path, "
          "not the gather fallback (got " + std::to_string(dist.pushdowns() - pd_before) + ")");

    // ---- MULTI-DIM STAR (1 fact + 2 dims): the fact is aggregated by the COMPOSITE join key, each
    // dim gathered + joined, rolled up by group cols spanning BOTH dims ----
    const char* rgDDL = "CREATE TABLE rg (region TEXT, zone TEXT NOT NULL, PRIMARY KEY (region))";
    check(solo.exec(rgDDL).ok, "solo create rg");
    check(dist.exec(rgDDL).ok, "dist create rg");
    const char* zones[] = {"N", "S", "E", "W", "C"};
    for (int i = 0; i < 5; ++i) {
        const std::string ins = "INSERT INTO rg (region, zone) VALUES ('" + std::string(regs[i]) +
                                "', '" + zones[i] + "')";
        check(solo.exec(ins).ok, "solo rg insert");
        check(dist.exec(ins).ok, "dist rg insert");
    }
    const std::size_t md_before = dist.pushdowns();
    const std::vector<std::string> md_queries = {
        // 2-dim star: GROUP BY columns from BOTH dims, aggregates over the fact.
        "SELECT dim.label, rg.zone, COUNT(*), SUM(t.amount) FROM t JOIN dim ON t.cat = dim.cat "
        "JOIN rg ON t.region = rg.region GROUP BY dim.label, rg.zone",
        // 2-dim star + WHERE on the fact AND each dim + AVG + HAVING (full stack over a snowflake-ish
        // multi-dim shape; still a pure star — every dim joins the fact).
        "SELECT rg.zone, dim.label, AVG(t.amount), COUNT(*) FROM t JOIN dim ON t.cat = dim.cat "
        "JOIN rg ON t.region = rg.region WHERE t.amount > 100 AND dim.label != 'h' AND rg.zone != 'C' "
        "GROUP BY rg.zone, dim.label HAVING COUNT(*) > 20",
    };
    for (const std::string& q : md_queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "multi-dim distributed != solo for [" + q + "]\n  solo=[" + s + "]\n  dist=[" +
                          d + "]");
    }
    check(dist.pushdowns() - md_before == 2,
          "both multi-dim (2-dim) star queries took the pushdown path (got " +
              std::to_string(dist.pushdowns() - md_before) + ")");

    // ---- DISTINCT-aggregate GLOBAL SHUFFLE: COUNT(DISTINCT) merged across shards ----
    const std::size_t ds_before = dist.distinct_shuffles();
    const std::vector<std::string> distinct_queries = {
        // COUNT(DISTINCT col) + GROUP BY -> the global-distinct SHUFFLE (ships distinct tuples).
        "SELECT cat, COUNT(DISTINCT region) FROM t GROUP BY cat",
        "SELECT region, COUNT(DISTINCT cat) FROM t GROUP BY region",
        // SUM(DISTINCT) and scalar COUNT(DISTINCT) -> gather fallback (still byte-identical).
        "SELECT cat, SUM(DISTINCT amount) FROM t GROUP BY cat",
        "SELECT COUNT(DISTINCT region) FROM t",
    };
    for (const std::string& q : distinct_queries) {
        const std::string s = render(solo.exec(q));
        const std::string d = render(dist.exec(q));
        check(s == d, "DISTINCT distributed != solo for [" + q + "]\n  solo=[" + s + "]\n  dist=[" +
                          d + "]");
    }
    check(dist.distinct_shuffles() - ds_before == 2,
          "the 2 COUNT(DISTINCT)+GROUP BY queries took the shuffle path; SUM(DISTINCT)/scalar "
          "gathered (got " + std::to_string(dist.distinct_shuffles() - ds_before) + ")");

    // ---- BROADCAST-DIM JOIN: a REPLICATED dim joined per-shard, partial aggregates merged ----
    {
        SqlEngine bsolo;
        std::vector<SqlEngine> bshards(kShards);
        std::vector<EngineSqlShard> bwraps;
        bwraps.reserve(kShards);
        for (SqlEngine& s : bshards) {
            s.set_columnar_default(true);
            bwraps.emplace_back(&s);
        }
        std::vector<ISqlShard*> bptrs;
        for (EngineSqlShard& w : bwraps) bptrs.push_back(&w);
        DistributedSql bdist(bptrs);
        bdist.set_replicated("d");  // dim replicated -> writes broadcast -> per-shard join is valid
        const char* fddl = "CREATE TABLE f (id INT, k INT NOT NULL, v INT NOT NULL, PRIMARY KEY (id))";
        const char* dddl = "CREATE TABLE d (k INT, lbl TEXT NOT NULL, PRIMARY KEY (k))";
        check(bsolo.exec(fddl).ok && bsolo.exec(dddl).ok, "bsolo create");
        check(bdist.exec(fddl).ok && bdist.exec(dddl).ok, "bdist create");
        for (int kk = 0; kk < 4; ++kk) {
            const std::string ins = "INSERT INTO d (k, lbl) VALUES (" + std::to_string(kk) + ", '" +
                                    labels[kk] + "')";
            check(bsolo.exec(ins).ok && bdist.exec(ins).ok, "d insert");
        }
        SplitMix br(99);
        for (int i = 0; i < 400; ++i) {
            const std::string ins = "INSERT INTO f (id, k, v) VALUES (" + std::to_string(i) + ", " +
                                    std::to_string(i % 4) + ", " +
                                    std::to_string(br.next() % 500) + ")";
            check(bsolo.exec(ins).ok && bdist.exec(ins).ok, "f insert");
        }
        for (SqlEngine& s : bshards) s.flush_columnar("f");
        // The replicated dim is FULL on every shard (4 rows each); the fact is spread.
        std::size_t dim_total = 0;
        std::size_t fact_nonempty = 0;
        for (SqlEngine& s : bshards) {
            const ExecResult dc = s.exec("SELECT COUNT(*) FROM d");
            dim_total += static_cast<std::size_t>(dc.rows[0].cells[0].second.i);
            const ExecResult fc = s.exec("SELECT COUNT(*) FROM f");
            if (fc.rows[0].cells[0].second.i > 0) ++fact_nonempty;
        }
        check(dim_total == kShards * 4, "replicated dim is FULL on every shard (got " +
                                            std::to_string(dim_total) + ")");
        check(fact_nonempty == kShards, "fact spread across shards");
        const std::size_t bj = bdist.broadcast_joins();
        const std::vector<std::string> bq = {
            "SELECT d.lbl, COUNT(*), SUM(f.v) FROM f JOIN d ON f.k = d.k GROUP BY d.lbl",
            "SELECT d.lbl, MIN(f.v), MAX(f.v) FROM f JOIN d ON f.k = d.k GROUP BY d.lbl",
        };
        for (const std::string& q : bq) {
            const std::string ss = render(bsolo.exec(q));
            const std::string dd = render(bdist.exec(q));
            check(ss == dd, "broadcast-dim join != solo for [" + q + "]\n  solo=[" + ss +
                                "]\n  dist=[" + dd + "]");
        }
        check(bdist.broadcast_joins() - bj == 2,
              "both broadcast-dim joins took the per-shard-join path (got " +
                  std::to_string(bdist.broadcast_joins() - bj) + ")");
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
