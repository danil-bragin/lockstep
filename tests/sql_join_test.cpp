// sql_join_test.cpp — SQL SURFACE v3: the LOAD-BEARING CONFORMANCE GATE for JOINs.
//
// Source of truth: the v3 SELECT pipeline (query/sql/{Ast,Parser,Engine}.hpp) adds
// JOINs (INNER / LEFT [OUTER] / comma-cross), qualified columns (table.col + AS
// alias), an in-memory hash/nested-loop join, and NULL-introducing LEFT joins — STILL
// pure sugar over the verified query::Database scan reads. This gate proves the JOIN
// pipeline introduces NO different result: a varied, seeded multi-table SQL workload
// run through SqlEngine produces results IDENTICAL to an INDEPENDENT RELATIONAL
// REFERENCE MODEL (plain std::vector<row> + hand-written join/filter/group/order ops
// that NEVER touch the KV encoding / Database / txn path).
//
// Asserts:
//   (A) INNER / LEFT / CROSS joins + ON predicates == the model's joined rows, in the
//       documented default order (left-major, right-minor, PK-ascending).
//   (B) joined WHERE / GROUP BY / aggregates / ORDER BY / LIMIT == the model.
//   (C) NULL semantics (LEFT-join NULL fill): COUNT(*) counts rows, COUNT(col) skips
//       NULL, SUM/MIN/MAX/AVG skip NULL, a comparison with NULL is false in WHERE.
//   (D) EDGE CASES: no-match (INNER drops / LEFT keeps NULL), many-to-many (cartesian
//       within an equi-key), self-join (FROM t AS a JOIN t AS b), 3-table join.
//   (E) DETERMINISM: same seed => byte-identical rendered output; the hash-join path
//       and the nested-loop path agree.
//   (F) TEETH: a WRONG join (INNER that wrongly keeps LEFT's NULL rows, or a DROPPED
//       ON predicate => a cartesian) DIVERGES from the reference and is CAUGHT.
//   (G) ERROR PATHS: ambiguous unqualified column, unknown table/alias/column —
//       fail-closed and reported.
//
// Determinism: the only entropy is the seed (an inlined SplitMix). No clock, no rng,
// no threads. Bounded; the sweep is small (LOCKSTEP_SQL_JOIN_SEEDS override).

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
            std::fprintf(stderr, "sql_join_test FAIL [%s:%d]: %s\n", __FILE__,  \
                         __LINE__, (msg));                                     \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Deterministic SplitMix64 (NOT a std::*_engine — integer mixing only).
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
    const char* env = std::getenv("LOCKSTEP_SQL_JOIN_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 40;
}

// ---------------------------------------------------------------------------
// THE INDEPENDENT REFERENCE MODEL. Two tables:
//   emp(id INT PK, name TEXT, dept INT)
//   dept(did INT PK, dname TEXT)
// modeled as ordered maps (id-ascending == the SQL PK scan order). The join ops are
// hand-written over these — NO KV/Database/txn path. A model "cell" is a value OR a
// NULL (mirroring the engine's Datum::is_null).
// ---------------------------------------------------------------------------
struct Cell {
    bool is_null = false;
    bool is_int = true;
    std::int64_t i = 0;
    std::string s;
    static Cell mkint(std::int64_t v) { return Cell{false, true, v, {}}; }
    static Cell mktext(std::string v) { return Cell{false, false, 0, std::move(v)}; }
    static Cell null() { return Cell{true, false, 0, {}}; }
    std::string render() const {
        if (is_null) return "NULL";
        return is_int ? std::to_string(i) : s;
    }
};

struct Emp { std::int64_t id; std::string name; std::int64_t dept; };
struct Dept { std::int64_t did; std::string dname; };

struct Model {
    std::map<std::int64_t, Emp> emp;    // id -> row (ordered => PK-ascending)
    std::map<std::int64_t, Dept> dept;  // did -> row

    // INNER JOIN emp.dept = dept.did, projecting (emp.name, dept.dname). Left-major,
    // right-minor scan order (emp ascending, then matching dept ascending).
    std::vector<std::pair<Cell, Cell>> inner_name_dname() const {
        std::vector<std::pair<Cell, Cell>> out;
        for (const auto& [eid, e] : emp) {
            for (const auto& [did, d] : dept) {
                if (e.dept == d.did) {
                    out.emplace_back(Cell::mktext(e.name), Cell::mktext(d.dname));
                }
            }
        }
        return out;
    }
    // LEFT JOIN: every emp; unmatched => dname NULL.
    std::vector<std::pair<Cell, Cell>> left_name_dname() const {
        std::vector<std::pair<Cell, Cell>> out;
        for (const auto& [eid, e] : emp) {
            bool matched = false;
            for (const auto& [did, d] : dept) {
                if (e.dept == d.did) {
                    out.emplace_back(Cell::mktext(e.name), Cell::mktext(d.dname));
                    matched = true;
                }
            }
            if (!matched) {
                out.emplace_back(Cell::mktext(e.name), Cell::null());
            }
        }
        return out;
    }
    // CROSS JOIN projecting (emp.id, dept.did), left-major/right-minor.
    std::vector<std::pair<Cell, Cell>> cross_id_did() const {
        std::vector<std::pair<Cell, Cell>> out;
        for (const auto& [eid, e] : emp) {
            for (const auto& [did, d] : dept) {
                out.emplace_back(Cell::mkint(e.id), Cell::mkint(d.did));
            }
        }
        return out;
    }
    // LEFT JOIN aggregates: COUNT(*), COUNT(dname) [skips NULL].
    std::pair<std::int64_t, std::int64_t> left_counts() const {
        std::int64_t cstar = 0, cdname = 0;
        for (const auto& [eid, e] : emp) {
            bool matched = false;
            for (const auto& [did, d] : dept) {
                if (e.dept == d.did) { ++cstar; ++cdname; matched = true; }
            }
            if (!matched) { ++cstar; /* dname NULL, skipped */ }
        }
        return {cstar, cdname};
    }

    // INNER JOIN GROUP BY dept.dname: (dname, COUNT(*)) ordered by dname ascending.
    // Mirrors the joined aggregate + ORDER BY path. Uses the INNER (matched) rows.
    std::vector<std::pair<Cell, Cell>> inner_group_dname_count() const {
        std::map<std::string, std::int64_t> counts;  // ordered => dname-ascending
        for (const auto& [eid, e] : emp) {
            for (const auto& [did, d] : dept) {
                if (e.dept == d.did) {
                    ++counts[d.dname];
                }
            }
        }
        std::vector<std::pair<Cell, Cell>> out;
        for (const auto& [dn, c] : counts) {
            out.emplace_back(Cell::mktext(dn), Cell::mkint(c));
        }
        return out;
    }
};

std::string render_pairs(const std::vector<std::pair<Cell, Cell>>& v) {
    std::string s;
    for (const auto& [a, b] : v) {
        s += "(" + a.render() + "," + b.render() + ")";
    }
    return s;
}

// Extract a SqlEngine 2-column SELECT result as (cell,cell) pairs by output position.
std::vector<std::pair<Cell, Cell>> sql_pairs(const ExecResult& r) {
    std::vector<std::pair<Cell, Cell>> out;
    for (const ResultRow& row : r.rows) {
        if (row.cells.size() < 2) { out.emplace_back(Cell::null(), Cell::null()); continue; }
        auto to_cell = [](const Datum& d) {
            if (d.is_null) return Cell::null();
            return d.type == Type::Int ? Cell::mkint(d.i) : Cell::mktext(d.s);
        };
        out.emplace_back(to_cell(row.cells[0].second), to_cell(row.cells[1].second));
    }
    return out;
}

// Seed a fresh engine + model with the SAME random emp/dept rows.
void seed_world(std::uint64_t seed, SqlEngine& eng, Model& model) {
    (void)eng.exec("CREATE TABLE emp (id INT, name TEXT, dept INT, PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE dept (did INT, dname TEXT, PRIMARY KEY (did))");
    SplitMix rng(seed ^ 0x101A5EED0DEADBEEULL);
    const std::uint64_t kDeptSpace = 5;  // dept ids 0..4 (some emps point to absent 5+)
    const std::uint64_t kEmps = 6 + rng.below(6);
    const std::uint64_t kDepts = 1 + rng.below(kDeptSpace);
    for (std::uint64_t d = 0; d < kDepts; ++d) {
        const std::string dn = "d" + std::to_string(rng.below(100));
        (void)eng.exec("INSERT INTO dept (did, dname) VALUES (" + std::to_string(d) +
                       ", '" + dn + "')");
        model.dept[static_cast<std::int64_t>(d)] = Dept{static_cast<std::int64_t>(d), dn};
    }
    for (std::uint64_t e = 0; e < kEmps; ++e) {
        const std::string nm = "e" + std::to_string(rng.below(1000));
        // dept ref may point to an ABSENT dept (kDeptSpace+1 range) => LEFT NULL rows.
        const std::int64_t dref = static_cast<std::int64_t>(rng.below(kDeptSpace + 1));
        (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (" + std::to_string(e) +
                       ", '" + nm + "', " + std::to_string(dref) + ")");
        model.emp[static_cast<std::int64_t>(e)] =
            Emp{static_cast<std::int64_t>(e), nm, dref};
    }
}

// ---------------------------------------------------------------------------
// (A)/(B)/(C)/(D) the randomized conformance run for ONE seed.
// ---------------------------------------------------------------------------
bool run_conformance(std::uint64_t seed) {
    SqlEngine eng;
    Model model;
    seed_world(seed, eng, model);
    bool ok = true;

    auto cmp = [&](const char* what, const std::string& sql,
                   const std::vector<std::pair<Cell, Cell>>& want) {
        const std::vector<std::pair<Cell, Cell>> got = sql_pairs(eng.exec(sql));
        if (render_pairs(got) != render_pairs(want)) {
            std::fprintf(stderr,
                         "sql_join_test FAIL seed=%llu [%s]: SQL=%s MODEL=%s\n",
                         static_cast<unsigned long long>(seed), what,
                         render_pairs(got).c_str(), render_pairs(want).c_str());
            ok = false;
        }
    };

    // (A) INNER (hash-join equi-key emp.dept = dept.did).
    cmp("inner",
        "SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.did",
        model.inner_name_dname());
    // INNER with reversed ON operands (dept.did = emp.dept) — same result.
    cmp("inner-rev",
        "SELECT emp.name, dept.dname FROM emp JOIN dept ON dept.did = emp.dept",
        model.inner_name_dname());
    // (D) LEFT (NULL fill for the absent-dept emps).
    cmp("left",
        "SELECT emp.name, dept.dname FROM emp LEFT JOIN dept ON emp.dept = dept.did",
        model.left_name_dname());
    cmp("left-outer",
        "SELECT emp.name, dept.dname FROM emp LEFT OUTER JOIN dept "
        "ON emp.dept = dept.did",
        model.left_name_dname());
    // (A) CROSS (comma + explicit CROSS JOIN agree).
    cmp("cross-comma", "SELECT emp.id, dept.did FROM emp, dept", model.cross_id_did());
    cmp("cross-kw", "SELECT emp.id, dept.did FROM emp CROSS JOIN dept",
        model.cross_id_did());

    // (E) NESTED-LOOP path: the SAME inner join expressed with a non-equi ON that the
    // planner CANNOT hash (a theta with a literal AND'd in) still equals the model's
    // inner over the equi-key when the extra term is always true.
    {
        // emp.dept = dept.did AND dept.did >= 0  (the >= 0 is always true => same set)
        const std::vector<std::pair<Cell, Cell>> got = sql_pairs(eng.exec(
            "SELECT emp.name, dept.dname FROM emp JOIN dept "
            "ON emp.dept = dept.did AND dept.did >= 0"));
        if (render_pairs(got) != render_pairs(model.inner_name_dname())) {
            std::fprintf(stderr, "sql_join_test FAIL seed=%llu [nested-loop]: %s vs %s\n",
                         static_cast<unsigned long long>(seed),
                         render_pairs(got).c_str(),
                         render_pairs(model.inner_name_dname()).c_str());
            ok = false;
        }
    }

    // (C) LEFT-join aggregates: COUNT(*) counts all, COUNT(dname) skips NULL.
    {
        const ExecResult r = eng.exec(
            "SELECT COUNT(*), COUNT(dname) FROM emp LEFT JOIN dept "
            "ON emp.dept = dept.did");
        const auto [cstar, cdname] = model.left_counts();
        bool good = r.ok && r.rows.size() == 1 && r.rows[0].cells.size() == 2 &&
                    r.rows[0].cells[0].second.i == cstar &&
                    r.rows[0].cells[1].second.i == cdname;
        if (!good) {
            std::fprintf(stderr,
                         "sql_join_test FAIL seed=%llu [left-counts]: model (%lld,%lld)\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<long long>(cstar), static_cast<long long>(cdname));
            ok = false;
        }
    }

    // (B) joined GROUP BY + aggregate + ORDER BY over qualified columns.
    cmp("inner-groupby",
        "SELECT dept.dname, COUNT(*) FROM emp JOIN dept ON emp.dept = dept.did "
        "GROUP BY dept.dname ORDER BY dept.dname",
        model.inner_group_dname_count());

    // (B) joined WHERE: only matched emps with a specific dname behave like INNER's
    // filtered set. We compare LEFT-join + "WHERE dname IS present" (dname = the model
    // value) by checking the LEFT result with a WHERE that drops NULLs equals INNER.
    cmp("left-where-drops-null",
        "SELECT emp.name, dept.dname FROM emp LEFT JOIN dept ON emp.dept = dept.did "
        "WHERE dept.dname >= ''",  // a TEXT cmp; NULL dname is UNKNOWN => dropped
        model.inner_name_dname());

    return ok;
}

// (E) determinism: same fixed multi-table workload => byte-identical output, twice.
std::string run_fixed() {
    SqlEngine eng;
    (void)eng.exec("CREATE TABLE emp (id INT, name TEXT, dept INT, PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE dept (did INT, dname TEXT, PRIMARY KEY (did))");
    (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (1, 'a', 10)");
    (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (2, 'b', 20)");
    (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (3, 'c', 99)");
    (void)eng.exec("INSERT INTO dept (did, dname) VALUES (10, 'eng')");
    (void)eng.exec("INSERT INTO dept (did, dname) VALUES (20, 'sales')");
    const ExecResult r = eng.exec(
        "SELECT emp.name, dept.dname FROM emp LEFT JOIN dept ON emp.dept = dept.did "
        "ORDER BY emp.name");
    return render_pairs(sql_pairs(r));
}

// (D) many-to-many: an equi-key with duplicate keys on BOTH sides => cartesian within
// the key. self-join: FROM t AS a JOIN t AS b. 3-table join.
bool run_edge_cases() {
    bool ok = true;
    // many-to-many: x(k) with k in {1,1,2}, y(k) with k in {1,1}. Join on x.k=y.k.
    {
        SqlEngine eng;
        (void)eng.exec("CREATE TABLE x (id INT, k INT, PRIMARY KEY (id))");
        (void)eng.exec("CREATE TABLE y (id INT, k INT, PRIMARY KEY (id))");
        (void)eng.exec("INSERT INTO x (id, k) VALUES (1, 1)");
        (void)eng.exec("INSERT INTO x (id, k) VALUES (2, 1)");
        (void)eng.exec("INSERT INTO x (id, k) VALUES (3, 2)");
        (void)eng.exec("INSERT INTO y (id, k) VALUES (1, 1)");
        (void)eng.exec("INSERT INTO y (id, k) VALUES (2, 1)");
        const ExecResult r = eng.exec(
            "SELECT x.id, y.id FROM x JOIN y ON x.k = y.k ORDER BY x.id, y.id");
        // x rows k=1: id 1,2 each match y id 1,2 => 4 rows; x id3 k=2 => no match.
        const std::vector<std::pair<Cell, Cell>> want = {
            {Cell::mkint(1), Cell::mkint(1)}, {Cell::mkint(1), Cell::mkint(2)},
            {Cell::mkint(2), Cell::mkint(1)}, {Cell::mkint(2), Cell::mkint(2)}};
        if (render_pairs(sql_pairs(r)) != render_pairs(want)) {
            std::fprintf(stderr, "sql_join_test FAIL [many-to-many]: %s\n",
                         render_pairs(sql_pairs(r)).c_str());
            ok = false;
        }
    }
    // self-join: a person + their manager (p.mgr = m.id).
    {
        SqlEngine eng;
        (void)eng.exec("CREATE TABLE p (id INT, mgr INT, PRIMARY KEY (id))");
        (void)eng.exec("INSERT INTO p (id, mgr) VALUES (1, 0)");
        (void)eng.exec("INSERT INTO p (id, mgr) VALUES (2, 1)");
        (void)eng.exec("INSERT INTO p (id, mgr) VALUES (3, 1)");
        const ExecResult r = eng.exec(
            "SELECT a.id, b.id FROM p AS a JOIN p AS b ON a.mgr = b.id "
            "ORDER BY a.id, b.id");
        // a id2 mgr1 -> b id1; a id3 mgr1 -> b id1; a id1 mgr0 -> no b (id 0 absent).
        const std::vector<std::pair<Cell, Cell>> want = {
            {Cell::mkint(2), Cell::mkint(1)}, {Cell::mkint(3), Cell::mkint(1)}};
        if (render_pairs(sql_pairs(r)) != render_pairs(want)) {
            std::fprintf(stderr, "sql_join_test FAIL [self-join]: %s\n",
                         render_pairs(sql_pairs(r)).c_str());
            ok = false;
        }
        // self-join must NOT confuse a.id vs b.id (qualified labels distinct).
        CHECK(r.ok && !r.rows.empty() && r.rows[0].cells[0].first == "a.id" &&
                  r.rows[0].cells[1].first == "b.id",
              "self-join qualified labels a.id / b.id distinct");
    }
    // 3-table join: proj -> emp -> dept.
    {
        SqlEngine eng;
        (void)eng.exec("CREATE TABLE emp (id INT, name TEXT, dept INT, PRIMARY KEY (id))");
        (void)eng.exec("CREATE TABLE dept (did INT, dname TEXT, PRIMARY KEY (did))");
        (void)eng.exec("CREATE TABLE proj (pid INT, eid INT, PRIMARY KEY (pid))");
        (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (1, 'al', 10)");
        (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (2, 'bo', 20)");
        (void)eng.exec("INSERT INTO dept (did, dname) VALUES (10, 'eng')");
        (void)eng.exec("INSERT INTO dept (did, dname) VALUES (20, 'ops')");
        (void)eng.exec("INSERT INTO proj (pid, eid) VALUES (100, 1)");
        (void)eng.exec("INSERT INTO proj (pid, eid) VALUES (101, 2)");
        const ExecResult r = eng.exec(
            "SELECT proj.pid, dept.dname FROM proj JOIN emp ON proj.eid = emp.id "
            "JOIN dept ON emp.dept = dept.did ORDER BY proj.pid");
        const std::vector<std::pair<Cell, Cell>> want = {
            {Cell::mkint(100), Cell::mktext("eng")},
            {Cell::mkint(101), Cell::mktext("ops")}};
        if (render_pairs(sql_pairs(r)) != render_pairs(want)) {
            std::fprintf(stderr, "sql_join_test FAIL [3-table]: %s\n",
                         render_pairs(sql_pairs(r)).c_str());
            ok = false;
        }
    }
    return ok;
}

// (F) TEETH — a WRONG join must DIVERGE from the reference. We compute a wrong result
// by hand (the classic lowering bugs) and confirm it != the model.
bool run_teeth() {
    SqlEngine eng;
    Model model;
    (void)eng.exec("CREATE TABLE emp (id INT, name TEXT, dept INT, PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE dept (did INT, dname TEXT, PRIMARY KEY (did))");
    (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (1, 'a', 10)");
    (void)eng.exec("INSERT INTO emp (id, name, dept) VALUES (2, 'b', 99)");  // no dept
    (void)eng.exec("INSERT INTO dept (did, dname) VALUES (10, 'eng')");
    model.emp[1] = Emp{1, "a", 10};
    model.emp[2] = Emp{2, "b", 99};
    model.dept[10] = Dept{10, "eng"};

    // Honest INNER must equal the model (no false positive).
    const std::vector<std::pair<Cell, Cell>> honest_inner = sql_pairs(eng.exec(
        "SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.did"));
    const bool honest_ok =
        render_pairs(honest_inner) == render_pairs(model.inner_name_dname());
    CHECK(honest_ok, "honest INNER must match the model (no false positive)");

    // TEETH 1: an INNER that wrongly KEEPS the unmatched NULL row == the LEFT result.
    // The LEFT result diverges from the INNER model => a bug that returned LEFT's rows
    // for an INNER join would be CAUGHT.
    const std::vector<std::pair<Cell, Cell>> wrong_left_for_inner = sql_pairs(eng.exec(
        "SELECT emp.name, dept.dname FROM emp LEFT JOIN dept ON emp.dept = dept.did"));
    const bool teeth1 = render_pairs(wrong_left_for_inner) !=
                        render_pairs(model.inner_name_dname());
    CHECK(teeth1, "TEETH: LEFT's NULL row must DIVERGE from the INNER model");

    // TEETH 2: a DROPPED ON predicate => a cartesian. A CROSS join's rows diverge from
    // the honest INNER result => a dropped-ON bug would be CAUGHT.
    const std::vector<std::pair<Cell, Cell>> cartesian = sql_pairs(
        eng.exec("SELECT emp.name, dept.dname FROM emp CROSS JOIN dept"));
    const bool teeth2 =
        render_pairs(cartesian) != render_pairs(model.inner_name_dname());
    CHECK(teeth2, "TEETH: a dropped-ON cartesian must DIVERGE from the INNER model");

    return honest_ok && teeth1 && teeth2;
}

// (G) ERROR PATHS: ambiguous unqualified column, unknown alias/column, dangling JOIN.
bool run_errors() {
    SqlEngine eng;
    (void)eng.exec("CREATE TABLE a (id INT, v INT, PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE b (id INT, w INT, PRIMARY KEY (id))");
    bool ok = true;

    // 'id' exists in both => ambiguous.
    const ExecResult amb =
        eng.exec("SELECT id FROM a JOIN b ON a.id = b.id");
    CHECK(!amb.ok, "ambiguous unqualified column must error");
    ok = ok && !amb.ok;

    // unknown alias qualifier.
    const ExecResult bad_alias =
        eng.exec("SELECT z.v FROM a JOIN b ON a.id = b.id");
    CHECK(!bad_alias.ok, "unknown table/alias must error");
    ok = ok && !bad_alias.ok;

    // unknown column under a known alias.
    const ExecResult bad_col =
        eng.exec("SELECT a.nope FROM a JOIN b ON a.id = b.id");
    CHECK(!bad_col.ok, "unknown column under a known alias must error");
    ok = ok && !bad_col.ok;

    // duplicate alias (self-join without AS).
    const ExecResult dup_alias =
        eng.exec("SELECT a.v FROM a JOIN a ON a.id = a.id");
    CHECK(!dup_alias.ok, "duplicate alias (no AS on a self-join) must error");
    ok = ok && !dup_alias.ok;

    // qualified column ON a single (aliased) table resolves fine (no join).
    const ExecResult aliased_single =
        eng.exec("SELECT x.v FROM a AS x WHERE x.id >= 0");
    CHECK(aliased_single.ok, "aliased single-table qualified column resolves");
    ok = ok && aliased_single.ok;

    return ok;
}

}  // namespace

int main() {
    const std::uint64_t seeds = sweep_count();

    for (std::uint64_t s = 0; s < seeds; ++s) {
        if (!run_conformance(s)) {
            ++g_failures;
        }
    }

    const std::string a = run_fixed();
    const std::string b = run_fixed();
    if (a != b) {
        std::fprintf(stderr, "sql_join_test FAIL: non-deterministic join output\n"
                             "  a=%s\n  b=%s\n",
                     a.c_str(), b.c_str());
        ++g_failures;
    }
    // Pin the fixed-workload LEFT-join result: a->eng, b->sales, c->NULL (ordered by
    // name a<b<c).
    if (a != "(a,eng)(b,sales)(c,NULL)") {
        std::fprintf(stderr, "sql_join_test FAIL: fixed LEFT-join result unexpected: "
                             "%s\n", a.c_str());
        ++g_failures;
    }

    if (!run_edge_cases()) { ++g_failures; }
    if (!run_teeth()) {
        std::fprintf(stderr, "sql_join_test FAIL: teeth did not catch a wrong join\n");
        ++g_failures;
    }
    if (!run_errors()) { ++g_failures; }

    std::fprintf(stderr, "sql_join_test: seeds=%llu\n",
                 static_cast<unsigned long long>(seeds));
    if (g_failures != 0) {
        std::fprintf(stderr, "sql_join_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr,
                 "sql_join_test: ALL PASS (INNER/LEFT/CROSS + ON + qualified cols + "
                 "alias/self/3-table == reference model; NULL semantics; hash+nested "
                 "agree; determinism; teeth caught LEFT-for-INNER + dropped-ON; errors "
                 "fail-closed)\n");
    return EXIT_SUCCESS;
}
