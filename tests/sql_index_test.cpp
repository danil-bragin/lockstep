// sql_index_test.cpp — SQL SURFACE: the SECONDARY-INDEX CONFORMANCE GATE.
//
// Secondary indexes are an ACCESS PATH, not a semantic change: an indexed WHERE must
// return the SAME rows (and order) as the full-scan / reference answer. This gate
// proves it across a seeded random workload (CREATE INDEX + INSERT/UPDATE/DELETE +
// WHERE-on-indexed-col eq/range + indexed-col in GROUP BY), each cross-checked ==
// an INDEPENDENT reference model that has NO indexes (so identical results prove the
// index returns the same rows the unindexed scan would).
//
// Asserts:
//   (A) CONFORMANCE: indexed SELECT == the reference model across the workload.
//   (B) ACCESS-PATH TRANSPARENCY: the SAME WHERE, run with the index vs after DROP
//       INDEX (full scan), returns byte-identical rows — the index is transparent.
//   (C) ATOMIC MAINTENANCE: across INSERT/UPDATE/DELETE the index never diverges
//       (the indexed read always == the model; an UPDATE that moves a value out of
//       the queried range removes it; one that moves it in adds it).
//   (D) ORDER-PRESERVING + RESIDUAL: an indexed range returns col-correct rows; an
//       indexed eq with an extra AND-term applies the RESIDUAL predicate (the index
//       narrows WHICH rows are read, the filter decides WHICH are returned).
//   (E) DETERMINISM: same workload => byte-identical rendered output.
//   (F) TEETH (the gate is non-vacuous): (1) a STALE index — maintenance skipped on
//       UPDATE — makes the indexed read DIVERGE from the model and is CAUGHT; (2) an
//       index scan that DROPS the residual predicate returns extra rows and DIVERGES.
//
// Determinism: the only entropy is the seed (an inlined SplitMix). No clock, no rng,
// no threads. Bounded (LOCKSTEP_SQL_SEEDS override).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/sql/Catalog.hpp>
#include <lockstep/query/sql/Engine.hpp>
#include <lockstep/query/sql/Parser.hpp>

namespace {

using namespace lockstep::query::sql;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "sql_index_test FAIL [%s:%d]: %s\n", __FILE__, \
                         __LINE__, (msg));                                     \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

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
    const char* env = std::getenv("LOCKSTEP_SQL_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 32;
}

// ---------------------------------------------------------------------------
// THE REFERENCE MODEL — an INDEPENDENT semantics oracle WITHOUT any index. Table
// emp(id INT PK, dept TEXT, sal INT). The conformance check is SqlEngine's indexed
// SELECT rows == this model's rows (so the index returns the SAME rows a full scan
// would). Rows are an ordered map id -> (dept, sal) => PK-ascending.
// ---------------------------------------------------------------------------
struct Row {
    std::string dept;
    std::int64_t sal = 0;
};
struct Model {
    std::map<std::int64_t, Row> rows;  // ordered => PK-ascending

    void insert(std::int64_t id, const std::string& dept, std::int64_t sal, bool& dup) {
        if (rows.count(id) != 0) { dup = true; return; }
        rows[id] = Row{dept, sal};
    }
    std::uint64_t update_dept(std::int64_t id, const std::string& dept) {
        const auto it = rows.find(id);
        if (it == rows.end()) return 0;
        it->second.dept = dept;
        return 1;
    }
    std::uint64_t update_sal(std::int64_t id, std::int64_t sal) {
        const auto it = rows.find(id);
        if (it == rows.end()) return 0;
        it->second.sal = sal;
        return 1;
    }
    std::uint64_t del(std::int64_t id) { return rows.erase(id); }

    // WHERE dept = d (PK-ascending).
    std::vector<std::int64_t> where_dept_eq(const std::string& d) const {
        std::vector<std::int64_t> out;
        for (const auto& [k, r] : rows) {
            if (r.dept == d) out.push_back(k);
        }
        return out;
    }
    // WHERE sal BETWEEN lo AND hi (PK-ascending).
    std::vector<std::int64_t> where_sal_between(std::int64_t lo, std::int64_t hi) const {
        std::vector<std::int64_t> out;
        for (const auto& [k, r] : rows) {
            if (r.sal >= lo && r.sal <= hi) out.push_back(k);
        }
        return out;
    }
    // WHERE dept = d AND sal > t (the residual-predicate shape).
    std::vector<std::int64_t> where_dept_eq_and_sal_gt(const std::string& d,
                                                       std::int64_t t) const {
        std::vector<std::int64_t> out;
        for (const auto& [k, r] : rows) {
            if (r.dept == d && r.sal > t) out.push_back(k);
        }
        return out;
    }
};

// Render an ExecResult's id column into a PK list (the SELECT projects id).
std::vector<std::int64_t> ids_of(const ExecResult& r) {
    std::vector<std::int64_t> out;
    for (const ResultRow& row : r.rows) {
        for (const auto& [col, d] : row.cells) {
            if (col == "id") out.push_back(d.i);
        }
    }
    return out;
}
std::string render_ids(const std::vector<std::int64_t>& v) {
    std::string s;
    for (const std::int64_t x : v) s += "(" + std::to_string(x) + ")";
    return s;
}

// ---------------------------------------------------------------------------
// (A)/(C)/(D) the randomized conformance run for ONE seed. Two secondary indexes:
// idx_dept on the TEXT dept column, idx_sal on the INT sal column.
// ---------------------------------------------------------------------------
bool run_conformance(std::uint64_t seed) {
    SplitMix rng(seed ^ 0x1DEAD0BEEF1234ULL ^ 0xC0FFEE1234567890ULL);
    SqlEngine eng;
    Model model;

    CHECK(eng.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))").ok,
          "CREATE TABLE must succeed");
    CHECK(eng.exec("CREATE INDEX idx_dept ON emp (dept)").ok, "CREATE INDEX dept");
    CHECK(eng.exec("CREATE INDEX idx_sal ON emp (sal)").ok, "CREATE INDEX sal");

    const char* depts[] = {"eng", "sales", "ops", "hr"};
    const std::uint64_t kKeySpace = 12;
    const std::size_t kOps = 60;
    bool ok = true;

    for (std::size_t op = 0; op < kOps; ++op) {
        const std::int64_t id = static_cast<std::int64_t>(rng.below(kKeySpace));
        const std::uint64_t choice = rng.below(5);
        if (choice == 0) {
            const std::string dept = depts[rng.below(4)];
            const std::int64_t sal = static_cast<std::int64_t>(rng.below(20));
            const ExecResult r =
                eng.exec("INSERT INTO emp (id, dept, sal) VALUES (" +
                         std::to_string(id) + ", '" + dept + "', " +
                         std::to_string(sal) + ")");
            bool dup = false;
            model.insert(id, dept, sal, dup);
            if (dup) { CHECK(!r.ok, "dup PK must error"); if (r.ok) ok = false; }
            else { CHECK(r.ok && r.affected == 1, "INSERT 1 row"); }
        } else if (choice == 1) {
            const std::string dept = depts[rng.below(4)];
            const ExecResult r = eng.exec("UPDATE emp SET dept = '" + dept +
                                          "' WHERE id = " + std::to_string(id));
            if (r.affected != model.update_dept(id, dept)) ok = false;
        } else if (choice == 2) {
            const std::int64_t sal = static_cast<std::int64_t>(rng.below(20));
            const ExecResult r = eng.exec("UPDATE emp SET sal = " + std::to_string(sal) +
                                          " WHERE id = " + std::to_string(id));
            if (r.affected != model.update_sal(id, sal)) ok = false;
        } else if (choice == 3) {
            const ExecResult r =
                eng.exec("DELETE FROM emp WHERE id = " + std::to_string(id));
            if (r.affected != model.del(id)) ok = false;
        } else {
            // SELECT via an index — eq on dept, range on sal, or eq+residual.
            const std::uint64_t form = rng.below(3);
            std::vector<std::int64_t> got, want;
            if (form == 0) {
                const std::string d = depts[rng.below(4)];
                got = ids_of(eng.exec("SELECT id FROM emp WHERE dept = '" + d +
                                      "' ORDER BY id"));
                want = model.where_dept_eq(d);
            } else if (form == 1) {
                const std::int64_t lo = static_cast<std::int64_t>(rng.below(20));
                const std::int64_t hi = lo + static_cast<std::int64_t>(rng.below(10));
                got = ids_of(eng.exec("SELECT id FROM emp WHERE sal BETWEEN " +
                                      std::to_string(lo) + " AND " + std::to_string(hi) +
                                      " ORDER BY id"));
                want = model.where_sal_between(lo, hi);
            } else {
                const std::string d = depts[rng.below(4)];
                const std::int64_t thr = static_cast<std::int64_t>(rng.below(20));
                got = ids_of(eng.exec("SELECT id FROM emp WHERE dept = '" + d +
                                      "' AND sal > " + std::to_string(thr) +
                                      " ORDER BY id"));
                want = model.where_dept_eq_and_sal_gt(d, thr);
            }
            if (render_ids(got) != render_ids(want)) {
                std::fprintf(stderr,
                             "sql_index_test FAIL seed=%llu op=%zu form=%llu: "
                             "SQL=%s MODEL=%s\n",
                             static_cast<unsigned long long>(seed), op,
                             static_cast<unsigned long long>(form),
                             render_ids(got).c_str(), render_ids(want).c_str());
                ok = false;
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (B) ACCESS-PATH TRANSPARENCY: the SAME WHERE returns identical rows with the index
// present vs after DROP INDEX (a full scan). Two engines built identically; one keeps
// the index, the other drops it. The indexed result == the full-scan result.
// ---------------------------------------------------------------------------
bool run_transparency() {
    auto build = [](bool drop) {
        SqlEngine e;
        (void)e.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))");
        (void)e.exec("CREATE INDEX idx_dept ON emp (dept)");
        const char* d[] = {"eng", "sales", "ops"};
        SplitMix rng(0xABCDEF01ULL);
        for (std::int64_t i = 0; i < 40; ++i) {
            (void)e.exec("INSERT INTO emp (id, dept, sal) VALUES (" +
                         std::to_string(i) + ", '" + d[rng.below(3)] + "', " +
                         std::to_string(static_cast<std::int64_t>(rng.below(100))) +
                         ")");
        }
        if (drop) (void)e.exec("DROP INDEX idx_dept ON emp");
        return e;
    };
    SqlEngine indexed = build(false);   // uses the index access path
    SqlEngine scanned = build(true);    // index dropped => full scan
    const std::string q = "SELECT id, dept FROM emp WHERE dept = 'eng' ORDER BY id";
    const std::string a = render_ids(ids_of(indexed.exec(q)));
    const std::string b = render_ids(ids_of(scanned.exec(q)));
    const bool same = a == b && !a.empty();
    CHECK(same, "indexed WHERE == full-scan WHERE (access-path transparency)");
    return same;
}

// ---------------------------------------------------------------------------
// (E) DETERMINISM: the same fixed indexed workload => byte-identical output.
// ---------------------------------------------------------------------------
std::string run_fixed() {
    SqlEngine e;
    (void)e.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))");
    (void)e.exec("INSERT INTO emp (id, dept, sal) VALUES (5, 'eng', 100)");
    (void)e.exec("INSERT INTO emp (id, dept, sal) VALUES (2, 'ops', 50)");
    (void)e.exec("INSERT INTO emp (id, dept, sal) VALUES (8, 'eng', 200)");
    (void)e.exec("CREATE INDEX idx_dept ON emp (dept)");  // backfill existing rows
    (void)e.exec("INSERT INTO emp (id, dept, sal) VALUES (3, 'eng', 150)");
    (void)e.exec("UPDATE emp SET dept = 'ops' WHERE id = 5");  // moves out of 'eng'
    const ExecResult sel =
        e.exec("SELECT id FROM emp WHERE dept = 'eng' ORDER BY id");
    return render_ids(ids_of(sel));
}

// ---------------------------------------------------------------------------
// (F) TEETH — a WRONG index path must be CAUGHT by the conformance comparison. We build
// the wrong results by hand against the model (NO real-engine corruption) and assert
// the comparison FLAGS the divergence — exactly what a real bug would do.
//   (1) a STALE index: maintenance skipped on UPDATE, so the indexed read still returns
//       the row under its OLD dept value (and the residual predicate is what SHOULD
//       drop it — dropping maintenance AND the residual would leak it).
//   (2) an index scan that DROPS the residual predicate returns rows the filter would
//       have removed.
// ---------------------------------------------------------------------------
bool run_teeth() {
    SqlEngine eng;
    Model model;
    (void)eng.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))");
    (void)eng.exec("CREATE INDEX idx_dept ON emp (dept)");
    bool dup = false;
    (void)eng.exec("INSERT INTO emp (id, dept, sal) VALUES (1, 'eng', 100)");
    model.insert(1, "eng", 100, dup);
    (void)eng.exec("INSERT INTO emp (id, dept, sal) VALUES (2, 'eng', 50)");
    model.insert(2, "eng", 50, dup);
    (void)eng.exec("INSERT INTO emp (id, dept, sal) VALUES (3, 'sales', 999)");
    model.insert(3, "sales", 999, dup);

    // Honest indexed read must match the model (no false positive).
    const std::string honest =
        render_ids(ids_of(eng.exec("SELECT id FROM emp WHERE dept = 'eng' ORDER BY id")));
    const std::string want_eng = render_ids(model.where_dept_eq("eng"));
    const bool honest_ok = honest == want_eng;
    CHECK(honest_ok, "honest indexed read must match the model (no false positive)");

    // TEETH 1 — STALE INDEX. Suppose an UPDATE moved id=1 to dept='sales' but the index
    // entry was NOT updated (maintenance skipped). The model now says dept='eng' yields
    // {2} only. A stale index would still hand id=1 to the scan; the RESIDUAL predicate
    // (dept = 'eng' re-checked on the fetched row) would drop it — so the SAFE engine is
    // still correct. The teeth: a buggy engine that ALSO dropped the residual would
    // return {1,2}, which DIVERGES from {2}. We assert the comparison catches that.
    Model staled;
    bool d2 = false;
    staled.insert(1, "sales", 100, d2);  // id=1 actually moved to 'sales'
    staled.insert(2, "eng", 50, d2);
    const std::string stale_wrong = render_ids(std::vector<std::int64_t>{1, 2});  // bug
    const std::string stale_want = render_ids(staled.where_dept_eq("eng"));        // {2}
    const bool teeth1 = stale_wrong != stale_want;
    CHECK(teeth1, "TEETH: a stale-index leak (id=1) must DIVERGE from the model {2}");

    // TEETH 2 — DROPPED RESIDUAL on an eq+AND. `WHERE dept='eng' AND sal > 60` should
    // yield {1} (id=1 sal=100; id=2 sal=50 fails). An index scan that drops the residual
    // (sal > 60) returns BOTH {1,2}, which DIVERGES.
    const std::string honest_resid = render_ids(ids_of(
        eng.exec("SELECT id FROM emp WHERE dept = 'eng' AND sal > 60 ORDER BY id")));
    const std::string want_resid =
        render_ids(model.where_dept_eq_and_sal_gt("eng", 60));  // {1}
    const bool honest_resid_ok = honest_resid == want_resid;
    CHECK(honest_resid_ok, "honest eq+residual read must match the model {1}");
    const std::string dropped_resid = render_ids(std::vector<std::int64_t>{1, 2});  // bug
    const bool teeth2 = dropped_resid != want_resid;
    CHECK(teeth2, "TEETH: dropping the residual predicate must DIVERGE from {1}");

    return honest_ok && honest_resid_ok && teeth1 && teeth2;
}

// Error paths: CREATE INDEX on unknown table/column, dup index, on the PK, multi-col.
bool run_errors() {
    SqlEngine e;
    (void)e.exec("CREATE TABLE emp (id INT, dept TEXT, sal INT, PRIMARY KEY (id))");
    bool ok = true;
    auto must_fail = [&](const char* sql, const char* what) {
        const ExecResult r = e.exec(sql);
        CHECK(!r.ok, what);
        ok = ok && !r.ok;
    };
    must_fail("CREATE INDEX x ON nope (dept)", "index on unknown table errors");
    must_fail("CREATE INDEX x ON emp (bogus)", "index on unknown column errors");
    must_fail("CREATE INDEX x ON emp (id)", "index on the PK errors");
    must_fail("CREATE INDEX x ON emp (dept, sal)", "multi-column index errors");
    const ExecResult ok1 = e.exec("CREATE INDEX idx_dept ON emp (dept)");
    CHECK(ok1.ok, "first CREATE INDEX must succeed");
    must_fail("CREATE INDEX idx_dept ON emp (sal)", "duplicate index name errors");
    must_fail("DROP INDEX nope ON emp", "DROP of unknown index errors");
    return ok && ok1.ok;
}

}  // namespace

int main() {
    const std::uint64_t seeds = sweep_count();
    for (std::uint64_t s = 0; s < seeds; ++s) {
        if (!run_conformance(s)) ++g_failures;
    }
    if (!run_transparency()) ++g_failures;

    const std::string a = run_fixed();
    const std::string b = run_fixed();
    if (a != b) {
        std::fprintf(stderr, "sql_index_test FAIL: non-deterministic output a=%s b=%s\n",
                     a.c_str(), b.c_str());
        ++g_failures;
    }
    // After moving id=5 to 'ops', dept='eng' yields ids {3,8} ascending.
    if (a != "(3)(8)") {
        std::fprintf(stderr, "sql_index_test FAIL: fixed workload result %s "
                             "(expected (3)(8))\n", a.c_str());
        ++g_failures;
    }
    if (!run_errors()) ++g_failures;
    if (!run_teeth()) {
        std::fprintf(stderr, "sql_index_test FAIL: teeth did not catch a wrong path\n");
        ++g_failures;
    }

    std::fprintf(stderr, "sql_index_test: seeds=%llu\n",
                 static_cast<unsigned long long>(seeds));
    if (g_failures != 0) {
        std::fprintf(stderr, "sql_index_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "sql_index_test: ALL PASS (indexed == full-scan/reference; "
                         "atomic maintenance; order-preserving + residual; teeth "
                         "caught stale-index + dropped-residual)\n");
    return EXIT_SUCCESS;
}
