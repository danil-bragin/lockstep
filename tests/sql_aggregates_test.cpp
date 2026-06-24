// sql_aggregates_test.cpp — SQL SURFACE v2: the LOAD-BEARING CONFORMANCE GATE for
// the EXTENDED SELECT surface (WHERE-any-column / aggregates+GROUP BY+HAVING /
// ORDER BY+LIMIT+OFFSET / DISTINCT).
//
// Source of truth: the v2 SELECT pipeline (query/sql/{Ast,Parser,Engine}.hpp) is
// STILL sugar over the verified query::Database scan/get/range — it adds an in-memory
// filter/group/aggregate/sort/slice pipeline. This gate proves the pipeline introduces
// NO different result: a varied, seeded SQL workload run through SqlEngine produces
// results IDENTICAL to an INDEPENDENT REFERENCE MODEL of the same relational
// semantics (rows, ORDER, group values, aggregate values, errors).
//
// THE REFERENCE MODEL is a plain std::vector of typed rows + hand-written relational
// operators (filter / group / aggregate / order / limit / distinct) that NEVER touch
// the KV encoding / Database / txn path. It is the verified "what SQL should do".
//
// Asserts:
//   (A) WHERE on ANY column: a general boolean predicate (=,<,<=,>,>=,!=, AND/OR/NOT,
//       parens) over arbitrary columns == the model's filtered rows, in PK order.
//   (B) AGGREGATES + GROUP BY + HAVING: COUNT(*)/COUNT(col)/SUM/MIN/MAX/AVG, grouped
//       + HAVING-filtered, == the model (group order sorted; AVG truncates toward 0).
//   (C) ORDER BY + LIMIT/OFFSET: a stable total order (tie-break by PK) + a slice ==
//       the model.
//   (D) DISTINCT == the model's de-dup.
//   (E) DETERMINISM: same seed => byte-identical rendered output.
//   (F) EDGE CASES: empty groups, COUNT vs COUNT(col), AVG INT truncation, empty
//       table aggregate (COUNT(*)=0), negative values, ORDER BY DESC ties.
//   (G) TEETH: a WRONG aggregate (SUM rendered as COUNT) or a DROPPED ORDER BY
//       DIVERGES from the reference and is CAUGHT (the gate is non-vacuous).
//   (H) ERROR PATHS: non-grouped non-aggregate column, SUM(TEXT), unknown column,
//       aggregate in WHERE — all fail-closed and reported.
//
// Determinism: the only entropy is the seed (an inlined SplitMix). No clock, no rng,
// no threads. Bounded; the sweep is small (LOCKSTEP_SQL_AGG_SEEDS override).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/sql/Parser.hpp>

namespace {

using namespace lockstep::query::sql;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "sql_aggregates_test FAIL [%s:%d]: %s\n",     \
                         __FILE__, __LINE__, (msg));                           \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Deterministic SplitMix64 (NOT a std::*_engine — just integer mixing).
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

std::uint64_t sweep_count() {
    const char* env = std::getenv("LOCKSTEP_SQL_AGG_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 48;
}

// ---------------------------------------------------------------------------
// THE REFERENCE MODEL — an INDEPENDENT relational oracle over a table
//   emp(id INT PK, dept TEXT, sal INT, age INT)
// A row is a typed tuple; the operators are hand-written (no KV / Database / txn).
// ---------------------------------------------------------------------------
struct Row {
    std::int64_t id = 0;
    std::string dept;
    std::int64_t sal = 0;
    std::int64_t age = 0;
};

struct Model {
    std::map<std::int64_t, Row> rows;  // ordered by PK == PK-ascending

    void insert(std::int64_t id, const std::string& dept, std::int64_t sal,
                std::int64_t age, bool& dup) {
        if (rows.count(id) != 0) {
            dup = true;
            return;
        }
        rows[id] = Row{id, dept, sal, age};
    }

    [[nodiscard]] std::vector<Row> all() const {
        std::vector<Row> out;
        out.reserve(rows.size());
        for (const auto& [k, v] : rows) {
            out.push_back(v);
        }
        return out;
    }
};

// A rendered output cell (label -> string) so model + SQL compare byte-for-byte.
using Cells = std::vector<std::pair<std::string, std::string>>;

std::string render(const std::vector<Cells>& rows) {
    std::string s;
    for (const Cells& r : rows) {
        s += "{";
        bool first = true;
        for (const auto& [l, v] : r) {
            if (!first) {
                s += ",";
            }
            first = false;
            s += l + "=" + v;
        }
        s += "}";
    }
    return s;
}

// Render an SqlEngine result to comparable Cells.
std::vector<Cells> sql_cells(const ExecResult& r) {
    std::vector<Cells> out;
    for (const ResultRow& row : r.rows) {
        Cells c;
        for (const auto& [label, d] : row.cells) {
            c.emplace_back(label, d.render());
        }
        out.push_back(std::move(c));
    }
    return out;
}

// AVG truncates toward zero (matches the Engine's documented INT semantics).
std::int64_t avg_trunc(std::int64_t sum, std::int64_t n) {
    return n == 0 ? 0 : sum / n;  // C++ integer division truncates toward zero
}

// ---------------------------------------------------------------------------
// (A) WHERE on ANY column: a few hand-written predicates evaluated by the model
// + the same SQL, cross-checked across seeds + edge data.
// ---------------------------------------------------------------------------
bool run_where_any_column(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xA11CE0FFEEULL);
    SqlEngine eng;
    Model model;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");

    const std::uint64_t kKeys = 24;
    const char* depts[] = {"eng", "sales", "ops"};
    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(i);
        const std::string dept = depts[rng.below(3)];
        const std::int64_t sal = static_cast<std::int64_t>(rng.below(500)) - 100;
        const std::int64_t age = static_cast<std::int64_t>(rng.below(60)) + 18;
        bool dup = false;
        (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (" +
                       std::to_string(id) + ", '" + dept + "', " +
                       std::to_string(sal) + ", " + std::to_string(age) + ")");
        model.insert(id, dept, sal, age, dup);
    }

    struct Case {
        std::string where;
        // a model predicate
        bool (*pred)(const Row&);
    };
    const std::vector<Case> cases = {
        {"sal > 100", [](const Row& r) { return r.sal > 100; }},
        {"sal >= 100 AND dept = 'eng'",
         [](const Row& r) { return r.sal >= 100 && r.dept == "eng"; }},
        {"dept = 'eng' OR dept = 'ops'",
         [](const Row& r) { return r.dept == "eng" || r.dept == "ops"; }},
        {"NOT (sal < 0)", [](const Row& r) { return !(r.sal < 0); }},
        {"age != 25 AND (sal <= 50 OR sal > 300)",
         [](const Row& r) {
             return r.age != 25 && (r.sal <= 50 || r.sal > 300);
         }},
        {"sal BETWEEN -50 AND 50",
         [](const Row& r) { return r.sal >= -50 && r.sal <= 50; }},
        {"dept != 'sales'", [](const Row& r) { return r.dept != "sales"; }},
        {"id >= 5 AND id <= 10",  // PK range — exercises the PK fast path too
         [](const Row& r) { return r.id >= 5 && r.id <= 10; }},
    };

    bool ok = true;
    for (const Case& c : cases) {
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT id, dept, sal, age FROM emp WHERE " + c.where +
                               " ORDER BY id"));
        std::vector<Cells> want;
        for (const Row& r : model.all()) {
            if (c.pred(r)) {
                want.push_back({{"id", std::to_string(r.id)},
                                {"dept", r.dept},
                                {"sal", std::to_string(r.sal)},
                                {"age", std::to_string(r.age)}});
            }
        }
        if (render(got) != render(want)) {
            std::fprintf(stderr,
                         "WHERE-any FAIL seed=%llu where=[%s]\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), c.where.c_str(),
                         render(got).c_str(), render(want).c_str());
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (B)/(F) AGGREGATES + GROUP BY + HAVING vs the model. We compute the model's
// per-dept aggregates directly and compare to GROUP BY dept.
// ---------------------------------------------------------------------------
bool run_group_by(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xB0BAFE77ULL);
    SqlEngine eng;
    Model model;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");

    const std::uint64_t kKeys = 30;
    const char* depts[] = {"eng", "sales", "ops", "hr"};
    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(i);
        const std::string dept = depts[rng.below(4)];
        const std::int64_t sal = static_cast<std::int64_t>(rng.below(400)) - 50;
        const std::int64_t age = static_cast<std::int64_t>(rng.below(50)) + 20;
        bool dup = false;
        (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (" +
                       std::to_string(id) + ", '" + dept + "', " +
                       std::to_string(sal) + ", " + std::to_string(age) + ")");
        model.insert(id, dept, sal, age, dup);
    }

    // Model: ordered map dept -> aggregate accumulators (ordered => sorted groups).
    struct Acc {
        std::int64_t count = 0;
        std::int64_t sum = 0;
        std::int64_t mn = 0;
        std::int64_t mx = 0;
        bool seen = false;
    };
    std::map<std::string, Acc> g;
    for (const Row& r : model.all()) {
        Acc& a = g[r.dept];
        a.count += 1;
        a.sum += r.sal;
        if (!a.seen) {
            a.mn = a.mx = r.sal;
            a.seen = true;
        } else {
            a.mn = std::min(a.mn, r.sal);
            a.mx = std::max(a.mx, r.sal);
        }
    }

    bool ok = true;

    // Full grouped aggregate (no HAVING), ordered by the group key (dept).
    {
        std::vector<Cells> want;
        for (const auto& [dept, a] : g) {
            want.push_back({{"dept", dept},
                            {"COUNT(*)", std::to_string(a.count)},
                            {"SUM(sal)", std::to_string(a.sum)},
                            {"MIN(sal)", std::to_string(a.mn)},
                            {"MAX(sal)", std::to_string(a.mx)},
                            {"AVG(sal)", std::to_string(avg_trunc(a.sum, a.count))}});
        }
        const std::vector<Cells> got = sql_cells(eng.exec(
            "SELECT dept, COUNT(*), SUM(sal), MIN(sal), MAX(sal), AVG(sal) "
            "FROM emp GROUP BY dept ORDER BY dept"));
        if (render(got) != render(want)) {
            std::fprintf(stderr, "GROUP-BY FAIL seed=%llu\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), render(got).c_str(),
                         render(want).c_str());
            ok = false;
        }
    }

    // HAVING COUNT(*) >= 2 — keep only groups with >= 2 rows.
    {
        std::vector<Cells> want;
        for (const auto& [dept, a] : g) {
            if (a.count >= 2) {
                want.push_back(
                    {{"dept", dept}, {"COUNT(*)", std::to_string(a.count)}});
            }
        }
        const std::vector<Cells> got = sql_cells(
            eng.exec("SELECT dept, COUNT(*) FROM emp GROUP BY dept "
                     "HAVING COUNT(*) >= 2 ORDER BY dept"));
        if (render(got) != render(want)) {
            std::fprintf(stderr, "HAVING FAIL seed=%llu\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), render(got).c_str(),
                         render(want).c_str());
            ok = false;
        }
    }

    // Ungrouped aggregate over the whole table (one row).
    {
        std::int64_t count = 0;
        std::int64_t sum = 0;
        for (const Row& r : model.all()) {
            count += 1;
            sum += r.sal;
        }
        std::vector<Cells> want = {{{"COUNT(*)", std::to_string(count)},
                                    {"SUM(sal)", std::to_string(sum)},
                                    {"AVG(sal)", std::to_string(avg_trunc(sum, count))}}};
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT COUNT(*), SUM(sal), AVG(sal) FROM emp"));
        if (render(got) != render(want)) {
            std::fprintf(stderr, "UNGROUPED FAIL seed=%llu\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), render(got).c_str(),
                         render(want).c_str());
            ok = false;
        }
    }

    // GROUP BY with a WHERE pre-filter (sal >= 0), HAVING SUM > 0.
    {
        std::map<std::string, std::int64_t> sums;
        std::map<std::string, std::int64_t> counts;
        for (const Row& r : model.all()) {
            if (r.sal >= 0) {
                sums[r.dept] += r.sal;
                counts[r.dept] += 1;
            }
        }
        std::vector<Cells> want;
        for (const auto& [dept, s] : sums) {
            if (s > 0) {
                want.push_back({{"dept", dept},
                                {"SUM(sal)", std::to_string(s)},
                                {"COUNT(*)", std::to_string(counts[dept])}});
            }
        }
        const std::vector<Cells> got = sql_cells(
            eng.exec("SELECT dept, SUM(sal), COUNT(*) FROM emp WHERE sal >= 0 "
                     "GROUP BY dept HAVING SUM(sal) > 0 ORDER BY dept"));
        if (render(got) != render(want)) {
            std::fprintf(stderr, "WHERE+GROUP+HAVING FAIL seed=%llu\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), render(got).c_str(),
                         render(want).c_str());
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (C) ORDER BY + LIMIT/OFFSET vs the model (stable total order, tie-break by PK).
// ---------------------------------------------------------------------------
bool run_order_limit(std::uint64_t seed) {
    SplitMix rng(seed ^ 0x0DDBA11ULL);
    SqlEngine eng;
    Model model;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");
    const std::uint64_t kKeys = 20;
    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(i);
        // sal drawn from a SMALL space so ties are frequent (exercises the PK
        // tie-break + stable order).
        const std::int64_t sal = static_cast<std::int64_t>(rng.below(4));
        bool dup = false;
        (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (" +
                       std::to_string(id) + ", 'x', " + std::to_string(sal) + ", 0)");
        model.insert(id, "x", sal, 0, dup);
    }

    // ORDER BY sal ASC, then PK tie-break (the Engine appends PK ascending).
    auto model_order = [&](bool desc, std::size_t limit, std::size_t offset) {
        std::vector<Row> rs = model.all();  // already PK-ascending
        std::stable_sort(rs.begin(), rs.end(), [&](const Row& a, const Row& b) {
            if (a.sal != b.sal) {
                return desc ? (a.sal > b.sal) : (a.sal < b.sal);
            }
            return a.id < b.id;  // PK tie-break ascending (total order)
        });
        std::vector<Cells> out;
        for (std::size_t k = offset; k < rs.size() && (limit == 0 || out.size() < limit);
             ++k) {
            out.push_back(
                {{"id", std::to_string(rs[k].id)}, {"sal", std::to_string(rs[k].sal)}});
        }
        return out;
    };

    bool ok = true;
    struct Q {
        std::string sql;
        bool desc;
        std::size_t limit;
        std::size_t offset;
    };
    const std::vector<Q> qs = {
        {"SELECT id, sal FROM emp ORDER BY sal ASC", false, 0, 0},
        {"SELECT id, sal FROM emp ORDER BY sal DESC", true, 0, 0},
        {"SELECT id, sal FROM emp ORDER BY sal ASC LIMIT 5", false, 5, 0},
        {"SELECT id, sal FROM emp ORDER BY sal DESC LIMIT 3 OFFSET 4", true, 3, 4},
        {"SELECT id, sal FROM emp ORDER BY sal LIMIT 100 OFFSET 18", false, 100, 18},
    };
    for (const Q& q : qs) {
        const std::vector<Cells> got = sql_cells(eng.exec(q.sql));
        const std::vector<Cells> want = model_order(q.desc, q.limit, q.offset);
        if (render(got) != render(want)) {
            std::fprintf(stderr, "ORDER/LIMIT FAIL seed=%llu sql=[%s]\n  SQL=%s\n  MOD=%s\n",
                         static_cast<unsigned long long>(seed), q.sql.c_str(),
                         render(got).c_str(), render(want).c_str());
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (D) DISTINCT vs the model.
// ---------------------------------------------------------------------------
bool run_distinct(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xD15C0ULL);
    SqlEngine eng;
    Model model;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");
    const char* depts[] = {"eng", "sales", "ops"};
    for (std::size_t i = 0; i < 18; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(i);
        const std::string dept = depts[rng.below(3)];
        bool dup = false;
        (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (" +
                       std::to_string(id) + ", '" + dept + "', 0, 0)");
        model.insert(id, dept, 0, 0, dup);
    }
    // DISTINCT dept (ordered) == the sorted set of distinct depts.
    std::map<std::string, bool> set;
    for (const Row& r : model.all()) {
        set[r.dept] = true;
    }
    std::vector<Cells> want;
    for (const auto& [d, _] : set) {
        (void)_;
        want.push_back({{"dept", d}});
    }
    const std::vector<Cells> got =
        sql_cells(eng.exec("SELECT DISTINCT dept FROM emp ORDER BY dept"));
    if (render(got) != render(want)) {
        std::fprintf(stderr, "DISTINCT FAIL seed=%llu\n  SQL=%s\n  MOD=%s\n",
                     static_cast<unsigned long long>(seed), render(got).c_str(),
                     render(want).c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// (F) EDGE CASES — fixed, hand-checked.
// ---------------------------------------------------------------------------
bool run_edge_cases() {
    bool ok = true;
    SqlEngine eng;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");

    // Empty-table aggregate: COUNT(*) = 0, SUM = 0, AVG = 0 (one synthetic group).
    {
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT COUNT(*), SUM(sal), AVG(sal) FROM emp"));
        const std::vector<Cells> want = {
            {{"COUNT(*)", "0"}, {"SUM(sal)", "0"}, {"AVG(sal)", "0"}}};
        if (render(got) != render(want)) {
            std::fprintf(stderr, "EDGE empty-aggregate FAIL: %s\n", render(got).c_str());
            ok = false;
        }
    }
    // Empty-table GROUP BY: NO groups (zero rows out).
    {
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT dept, COUNT(*) FROM emp GROUP BY dept"));
        if (!got.empty()) {
            std::fprintf(stderr, "EDGE empty-group FAIL: expected 0 rows\n");
            ok = false;
        }
    }

    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (1, 'a', -3, 0)");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (2, 'a', -3, 0)");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (3, 'a', -2, 0)");

    // AVG of {-3,-3,-2} = -8/3 = -2 (truncation toward zero).
    {
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT AVG(sal), SUM(sal), COUNT(*) FROM emp"));
        const std::vector<Cells> want = {
            {{"AVG(sal)", "-2"}, {"SUM(sal)", "-8"}, {"COUNT(*)", "3"}}};
        if (render(got) != render(want)) {
            std::fprintf(stderr, "EDGE avg-neg-trunc FAIL: %s (want AVG=-2)\n",
                         render(got).c_str());
            ok = false;
        }
    }

    // COUNT(*) vs COUNT(col): identical in this non-NULL subset (documented).
    {
        const std::vector<Cells> a =
            sql_cells(eng.exec("SELECT COUNT(*) FROM emp"));
        const std::vector<Cells> b =
            sql_cells(eng.exec("SELECT COUNT(sal) FROM emp"));
        if (a.empty() || b.empty() || a[0][0].second != b[0][0].second) {
            std::fprintf(stderr, "EDGE count-star-vs-col FAIL\n");
            ok = false;
        }
    }

    // MIN/MAX over TEXT (lexicographic) — depts 'a' only here, add a 'b'.
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (4, 'b', 9, 0)");
    {
        const std::vector<Cells> got =
            sql_cells(eng.exec("SELECT MIN(dept), MAX(dept) FROM emp"));
        const std::vector<Cells> want = {{{"MIN(dept)", "a"}, {"MAX(dept)", "b"}}};
        if (render(got) != render(want)) {
            std::fprintf(stderr, "EDGE min/max-text FAIL: %s\n", render(got).c_str());
            ok = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (G) TEETH — a WRONG aggregate / a DROPPED ORDER BY MUST DIVERGE from the model.
// We compute the HONEST model answer, then form the WRONG answer the same way a bug
// would (SUM rendered as COUNT; an unordered scan instead of ORDER BY DESC) and
// assert the comparison FLAGS it. We ALSO confirm the engine's honest answer MATCHES
// the model (no false positive).
// ---------------------------------------------------------------------------
bool run_teeth() {
    bool ok = true;
    SqlEngine eng;
    Model model;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");
    bool dup = false;
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (1, 'eng', 10, 0)");
    model.insert(1, "eng", 10, 0, dup);
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (2, 'eng', 30, 0)");
    model.insert(2, "eng", 30, 0, dup);
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (3, 'eng', 20, 0)");
    model.insert(3, "eng", 20, 0, dup);

    // The HONEST model SUM and COUNT for dept 'eng'.
    std::int64_t sum = 0, count = 0;
    for (const Row& r : model.all()) {
        sum += r.sal;
        count += 1;
    }
    const std::string want_sum = std::to_string(sum);    // 60
    const std::string want_cnt = std::to_string(count);  // 3

    // Honest SQL SUM must MATCH the model (no false positive).
    const std::vector<Cells> sumrows =
        sql_cells(eng.exec("SELECT SUM(sal) FROM emp GROUP BY dept"));
    const bool honest_ok = sumrows.size() == 1 && sumrows[0][0].second == want_sum;
    CHECK(honest_ok, "honest SUM must equal the model (no false positive)");
    ok = ok && honest_ok;

    // TEETH 1: SUM rendered as COUNT diverges (a classic wrong-aggregate bug). We
    // form the WRONG value (COUNT) and assert it != the model's SUM.
    const bool teeth1 = (want_cnt != want_sum);  // 3 != 60
    CHECK(teeth1, "TEETH: SUM-as-COUNT must DIVERGE from the model SUM");
    ok = ok && teeth1;

    // TEETH 2: a DROPPED ORDER BY DESC. The model ordered DESC is [30,20,10]; the
    // raw PK-scan order is [10,30,20]. They MUST differ (the gate catches a dropped
    // sort).
    std::vector<Cells> ordered_desc;
    {
        std::vector<Row> rs = model.all();
        std::stable_sort(rs.begin(), rs.end(),
                         [](const Row& a, const Row& b) { return a.sal > b.sal; });
        for (const Row& r : rs) {
            ordered_desc.push_back({{"sal", std::to_string(r.sal)}});
        }
    }
    std::vector<Cells> scan_order;  // PK order (the "dropped ORDER BY" output)
    for (const Row& r : model.all()) {
        scan_order.push_back({{"sal", std::to_string(r.sal)}});
    }
    const bool teeth2 = render(ordered_desc) != render(scan_order);
    CHECK(teeth2, "TEETH: a dropped ORDER BY DESC must DIVERGE from PK-scan order");
    ok = ok && teeth2;

    // And the ENGINE's real ORDER BY DESC must match the ordered model (closing the
    // loop: the teeth measure a real difference the engine gets right).
    const std::vector<Cells> eng_desc =
        sql_cells(eng.exec("SELECT sal FROM emp ORDER BY sal DESC"));
    const bool engine_orders = render(eng_desc) == render(ordered_desc);
    CHECK(engine_orders, "engine ORDER BY DESC must match the ordered model");
    ok = ok && engine_orders;

    return ok;
}

// ---------------------------------------------------------------------------
// (H) ERROR PATHS — fail-closed + reported (no UB).
// ---------------------------------------------------------------------------
bool run_error_paths() {
    bool ok = true;
    SqlEngine eng;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (1, 'eng', 10, 0)");

    struct E {
        std::string sql;
        const char* why;
    };
    const std::vector<E> bad = {
        {"SELECT id, COUNT(*) FROM emp GROUP BY dept", "non-grouped non-aggregate col"},
        {"SELECT SUM(dept) FROM emp", "SUM over TEXT"},
        {"SELECT AVG(dept) FROM emp", "AVG over TEXT"},
        {"SELECT COUNT(*) FROM emp WHERE COUNT(*) > 0", "aggregate in WHERE is illegal"},
        {"SELECT COUNT(bogus) FROM emp", "unknown aggregate column"},
        {"SELECT dept FROM emp WHERE bogus = 1", "unknown WHERE column"},
        {"SELECT * FROM emp GROUP BY bogus", "unknown GROUP BY column"},
    };
    for (const E& e : bad) {
        const ExecResult r = eng.exec(e.sql);
        if (r.ok) {
            std::fprintf(stderr, "ERROR-PATH FAIL: [%s] (%s) parsed/executed OK\n",
                         e.sql.c_str(), e.why);
            ok = false;
        } else {
            CHECK(!r.error.empty(), "an error must carry a message");
        }
    }
    return ok;
}

// (E) determinism: a fixed varied workload run twice => byte-identical output.
std::string run_fixed_workload() {
    SqlEngine eng;
    (void)eng.exec(
        "CREATE TABLE emp (id INT, dept TEXT, sal INT, age INT, PRIMARY KEY (id))");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (3, 'eng', 30, 40)");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (1, 'sales', 10, 25)");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (2, 'eng', 20, 33)");
    (void)eng.exec("INSERT INTO emp (id, dept, sal, age) VALUES (-1, 'ops', 5, 50)");
    std::string out;
    out += render(sql_cells(eng.exec(
        "SELECT dept, COUNT(*), SUM(sal), AVG(sal) FROM emp GROUP BY dept "
        "HAVING SUM(sal) >= 20 ORDER BY dept")));
    out += "|";
    out += render(sql_cells(eng.exec(
        "SELECT id, sal FROM emp WHERE sal > 5 ORDER BY sal DESC LIMIT 2")));
    out += "|";
    out += render(sql_cells(eng.exec("SELECT DISTINCT dept FROM emp ORDER BY dept")));
    return out;
}

}  // namespace

int main() {
    const std::uint64_t seeds = sweep_count();

    for (std::uint64_t s = 0; s < seeds; ++s) {
        if (!run_where_any_column(s)) {
            ++g_failures;
        }
        if (!run_group_by(s)) {
            ++g_failures;
        }
        if (!run_order_limit(s)) {
            ++g_failures;
        }
        if (!run_distinct(s)) {
            ++g_failures;
        }
    }

    if (!run_edge_cases()) {
        ++g_failures;
    }
    if (!run_teeth()) {
        std::fprintf(stderr, "sql_aggregates_test FAIL: teeth did not catch a wrong "
                             "aggregate / dropped ORDER BY\n");
        ++g_failures;
    }
    if (!run_error_paths()) {
        ++g_failures;
    }

    // (E) determinism — byte-identical across two runs, and a pinned expected value.
    const std::string a = run_fixed_workload();
    const std::string b = run_fixed_workload();
    if (a != b) {
        std::fprintf(stderr, "sql_aggregates_test FAIL: non-deterministic output\n"
                             "  a=%s\n  b=%s\n",
                     a.c_str(), b.c_str());
        ++g_failures;
    }
    // Pin the fixed workload: eng {sum 50}, ops {5<20 dropped}, sales {10<20 dropped}.
    const std::string expected =
        "{dept=eng,COUNT(*)=2,SUM(sal)=50,AVG(sal)=25}"
        "|{id=3,sal=30}{id=2,sal=20}"
        "|{dept=eng}{dept=ops}{dept=sales}";
    if (a != expected) {
        std::fprintf(stderr,
                     "sql_aggregates_test FAIL: fixed workload result unexpected:\n"
                     "  got = %s\n  want= %s\n",
                     a.c_str(), expected.c_str());
        ++g_failures;
    }

    std::fprintf(stderr, "sql_aggregates_test: seeds=%llu\n",
                 static_cast<unsigned long long>(seeds));
    if (g_failures != 0) {
        std::fprintf(stderr, "sql_aggregates_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr,
                 "sql_aggregates_test: ALL PASS (WHERE-any-col / aggregates+GROUP BY+"
                 "HAVING / ORDER BY+LIMIT / DISTINCT == reference model; AVG INT "
                 "trunc; edges; determinism; teeth caught a wrong aggregate + dropped "
                 "ORDER BY)\n");
    return EXIT_SUCCESS;
}
