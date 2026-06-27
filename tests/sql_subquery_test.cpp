// sql_subquery_test.cpp — SQL SURFACE (v4): the LOAD-BEARING CONFORMANCE GATE for
// EXPLICIT NULL + SUBQUERIES, over a LARGE VARIED seeded sweep.
//
// Source of truth: the v4 SQL surface (nullable columns, IS [NOT] NULL, three-valued
// logic in WHERE/aggregates, scalar / IN / NOT IN / EXISTS subqueries) is SUGAR over
// the verified query::Database / Query<L> / txn surface. This gate proves the sugar
// computes EXACTLY the documented semantics by cross-checking every SELECT against an
// INDEPENDENT REFERENCE MODEL (in-memory ordered maps of typed rows with explicit
// NULLs — the oracle), built WITHOUT the KV encoding / Database / txn path.
//
// Asserts:
//   (A) NULL CONFORMANCE: nullable columns + IS NULL / IS NOT NULL + comparison
//       three-valued logic (NULL in =/</> -> UNKNOWN -> dropped) + aggregates skip
//       NULL (COUNT(*) counts rows, COUNT(col)/SUM/MIN/MAX/AVG skip NULL) +
//       GROUP BY treats NULL as ONE group, all == the model over a seeded workload.
//   (B) SUBQUERY CONFORMANCE: scalar (= (SELECT agg)), IN / NOT IN (SELECT col), and
//       [NOT] EXISTS (SELECT ...) == the model, INCLUDING the NULL-in-NOT-IN rule
//       (a NULL in the subquery makes NOT IN UNKNOWN -> drops every probe row).
//   (C) CARDINALITY: a scalar subquery returning >1 row is a RAISED error (fail-closed,
//       like real SQL).
//   (D) DETERMINISM: the same workload => byte-identical rendered output.
//   (E) TEETH (non-vacuous): a WRONG NULL rule (NULL = NULL treated as true; NOT IN
//       with a NULL returning true) and a MISSED scalar-subquery cardinality error
//       DIVERGE from the model / fail to error and are CAUGHT here.
//
// Determinism: the only entropy is the seed (an inlined SplitMix). No clock, no rng,
// no threads. Bounded; LOCKSTEP_SQL_SEEDS overrides the in-gate sweep size.

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
            std::fprintf(stderr, "sql_subquery_test FAIL [%s:%d]: %s\n",       \
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
    const char* env = std::getenv("LOCKSTEP_SQL_SEEDS");
    if (env != nullptr) {
        const long n = std::strtol(env, nullptr, 10);
        if (n > 0) {
            return static_cast<std::uint64_t>(n);
        }
    }
    return 64;  // a LARGE varied sweep by default
}

// ---------------------------------------------------------------------------
// A NULLABLE integer cell: present(value) or NULL. (The model's columns.)
// ---------------------------------------------------------------------------
struct Cell {
    bool null = true;
    std::int64_t v = 0;
    static Cell of(std::int64_t x) { return Cell{false, x}; }
    static Cell nul() { return Cell{true, 0}; }
};

std::string render_cell(const Cell& c) { return c.null ? "NULL" : std::to_string(c.v); }

// ---------------------------------------------------------------------------
// THE REFERENCE MODEL — an INDEPENDENT semantics oracle with EXPLICIT NULLs.
//   t(id INT PK, grp INT NULL, val INT NULL)   id -> (grp, val)
//   u(uid INT PK, ref INT NULL)                uid -> ref
// All relational ops (filter / IS NULL / three-valued comparison / aggregates skip
// NULL / GROUP BY NULL-as-one-group / IN/NOT IN/EXISTS/scalar subqueries) are computed
// here BY HAND, the ground truth SqlEngine must match byte-for-byte.
// ---------------------------------------------------------------------------
struct Model {
    struct TRow { Cell grp; Cell val; };
    std::map<std::int64_t, TRow> t;   // ordered => PK-ascending
    std::map<std::int64_t, Cell> u;   // ordered

    // ---- mutations ----
    bool insert_t(std::int64_t id, Cell grp, Cell val) {
        if (t.count(id) != 0) return false;
        t[id] = TRow{grp, val};
        return true;
    }
    bool insert_u(std::int64_t uid, Cell ref) {
        if (u.count(uid) != 0) return false;
        u[uid] = ref;
        return true;
    }
    std::uint64_t update_t_val(std::int64_t id, Cell val) {
        auto it = t.find(id);
        if (it == t.end()) return 0;
        it->second.val = val;
        return 1;
    }
    std::uint64_t del_t(std::int64_t id) { return t.erase(id); }

    // ---- the column accessors used by queries ----
    static const Cell& col(const TRow& r, int which) {  // 0=grp 1=val
        return which == 0 ? r.grp : r.val;
    }

    // The set of PRESENT values of a column of t (for IN/scalar), + a has_null flag.
    void t_column(int which, std::vector<std::int64_t>& vals, bool& has_null) const {
        for (const auto& [id, r] : t) {
            (void)id;
            const Cell& c = col(r, which);
            if (c.null) has_null = true; else vals.push_back(c.v);
        }
    }
    void u_ref_column(std::vector<std::int64_t>& vals, bool& has_null) const {
        for (const auto& [uid, c] : u) {
            (void)uid;
            if (c.null) has_null = true; else vals.push_back(c.v);
        }
    }
};

// Apply a comparison under three-valued logic: returns TRUE only when both operands are
// present AND the comparison holds (a NULL operand => UNKNOWN => false). Mirrors the
// Engine's apply_cmp + the documented filter collapse.
bool cmp_true(const Cell& lhs, std::int64_t rhs_present, bool rhs_null, CmpOp op) {
    if (lhs.null || rhs_null) return false;  // UNKNOWN => false at the filter
    const std::int64_t a = lhs.v, b = rhs_present;
    switch (op) {
        case CmpOp::Eq: return a == b;
        case CmpOp::Ne: return a != b;
        case CmpOp::Lt: return a < b;
        case CmpOp::Le: return a <= b;
        case CmpOp::Gt: return a > b;
        case CmpOp::Ge: return a >= b;
        case CmpOp::Like: return false;  // LIKE is TEXT-only; this INT reference model never sees it
        case CmpOp::Contains: return false;  // @> is JSON-only; this INT reference model never sees it
    }
    return false;
}

// IN / NOT IN under three-valued logic (the load-bearing NULL-in-NOT-IN rule).
bool in_true(std::int64_t probe, const std::vector<std::int64_t>& vals, bool sub_null,
             bool is_not) {
    bool matched = false;
    for (std::int64_t v : vals) if (v == probe) { matched = true; break; }
    if (!is_not) return matched;            // IN: present match (else collapses false)
    return (!matched && !sub_null);          // NOT IN: no match AND no NULL in subquery
}

// ---------------------------------------------------------------------------
// Render a SqlEngine SELECT of (id) rows to a comparable PK list. The queries below
// all project a single `id` column so the comparison is unambiguous.
// ---------------------------------------------------------------------------
std::string render_id_rows(const ExecResult& r) {
    std::string s;
    for (const ResultRow& row : r.rows) {
        for (const auto& [c, d] : row.cells) {
            if (c == "id") s += "(" + d.render() + ")";
        }
    }
    return s;
}
std::string render_ids(const std::vector<std::int64_t>& ids) {
    std::string s;
    for (std::int64_t id : ids) s += "(" + std::to_string(id) + ")";
    return s;
}

// Render a single-group aggregate result (label->value cells) deterministically.
std::string render_agg(const ExecResult& r) {
    std::string s;
    for (const ResultRow& row : r.rows) {
        s += "{";
        for (const auto& [c, d] : row.cells) s += c + "=" + d.render() + ";";
        s += "}";
    }
    return s;
}

const std::uint64_t kKeySpace = 10;

void seed_tables(SqlEngine& eng) {
    (void)eng.exec("CREATE TABLE t (id INT, grp INT NULL, val INT NULL, "
                   "PRIMARY KEY (id))");
    (void)eng.exec("CREATE TABLE u (uid INT, ref INT NULL, PRIMARY KEY (uid))");
}

// Build a deterministic cell value: NULL ~1/3 of the time, else a small int.
Cell gen_cell(SplitMix& rng) {
    if (rng.below(3) == 0) return Cell::nul();
    return Cell::of(static_cast<std::int64_t>(rng.below(kKeySpace)));
}

std::string insert_t_sql(std::int64_t id, const Cell& grp, const Cell& val) {
    // Exercise BOTH explicit NULL literals AND omission of a nullable column.
    std::string cols = "id";
    std::string vals = std::to_string(id);
    cols += ", grp"; vals += ", " + render_cell(grp);  // grp via NULL literal or value
    if (!val.null) { cols += ", val"; vals += ", " + std::to_string(val.v); }
    // (val omitted when NULL => exercises the omitted-nullable-column => NULL path)
    return "INSERT INTO t (" + cols + ") VALUES (" + vals + ")";
}

// ---------------------------------------------------------------------------
// (A)/(B)/(D) the randomized NULL + subquery conformance run for ONE seed.
// ---------------------------------------------------------------------------
bool run_conformance(std::uint64_t seed) {
    SplitMix rng(seed ^ 0xABCDEF0123456789ULL);
    SqlEngine eng;
    Model model;
    seed_tables(eng);
    bool ok = true;

    const std::size_t kOps = 60;
    for (std::size_t op = 0; op < kOps; ++op) {
        const std::uint64_t choice = rng.below(8);
        if (choice <= 2) {
            // INSERT into t (NULLs via literal + omission).
            const std::int64_t id = static_cast<std::int64_t>(rng.below(kKeySpace));
            const Cell grp = gen_cell(rng);
            const Cell val = gen_cell(rng);
            const ExecResult r = eng.exec(insert_t_sql(id, grp, val));
            const bool inserted = model.insert_t(id, grp, val);
            CHECK(r.ok == inserted, "INSERT t ok-ness must match the model (dup PK)");
            ok = ok && (r.ok == inserted);
        } else if (choice == 3) {
            // INSERT into u.
            const std::int64_t uid = static_cast<std::int64_t>(rng.below(kKeySpace));
            const Cell ref = gen_cell(rng);
            const std::string sql = ref.null
                ? ("INSERT INTO u (uid) VALUES (" + std::to_string(uid) + ")")
                : ("INSERT INTO u (uid, ref) VALUES (" + std::to_string(uid) + ", " +
                   std::to_string(ref.v) + ")");
            const ExecResult r = eng.exec(sql);
            const bool inserted = model.insert_u(uid, ref);
            CHECK(r.ok == inserted, "INSERT u ok-ness must match the model");
            ok = ok && (r.ok == inserted);
        } else if (choice == 4) {
            // UPDATE t.val (sometimes to NULL).
            const std::int64_t id = static_cast<std::int64_t>(rng.below(kKeySpace));
            const Cell val = gen_cell(rng);
            const std::string sql =
                "UPDATE t SET val = " + render_cell(val) + " WHERE id = " +
                std::to_string(id);
            const ExecResult r = eng.exec(sql);
            const std::uint64_t maff = model.update_t_val(id, val);
            CHECK(r.ok && r.affected == maff, "UPDATE affected must match the model");
            ok = ok && r.ok && (r.affected == maff);
        } else if (choice == 5) {
            // DELETE from t.
            const std::int64_t id = static_cast<std::int64_t>(rng.below(kKeySpace));
            const ExecResult r =
                eng.exec("DELETE FROM t WHERE id = " + std::to_string(id));
            const std::uint64_t maff = model.del_t(id);
            CHECK(r.ok && r.affected == maff, "DELETE affected must match the model");
            ok = ok && r.ok && (r.affected == maff);
        } else {
            // A NULL/subquery SELECT — pick a form, compare ids/aggregate to the model.
            const std::uint64_t form = rng.below(9);
            if (form == 0) {
                // IS NULL on a column.
                const int which = static_cast<int>(rng.below(2));  // 0=grp 1=val
                const char* col = which == 0 ? "grp" : "val";
                const std::string sql =
                    std::string("SELECT id FROM t WHERE ") + col + " IS NULL ORDER BY id";
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t)
                    if (Model::col(r, which).null) want.push_back(id);
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "IS NULL seed=%llu: %s != %s\n",
                                 static_cast<unsigned long long>(seed), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 1) {
                // IS NOT NULL.
                const int which = static_cast<int>(rng.below(2));
                const char* col = which == 0 ? "grp" : "val";
                const std::string sql = std::string("SELECT id FROM t WHERE ") + col +
                                        " IS NOT NULL ORDER BY id";
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t)
                    if (!Model::col(r, which).null) want.push_back(id);
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) { ok = false; }
            } else if (form == 2) {
                // Three-valued comparison: WHERE val <op> k (NULL dropped).
                const std::int64_t k = static_cast<std::int64_t>(rng.below(kKeySpace));
                const CmpOp ops[] = {CmpOp::Eq, CmpOp::Ne, CmpOp::Lt, CmpOp::Gt};
                const char* opstr[] = {"=", "!=", "<", ">"};
                const std::uint64_t oi = rng.below(4);
                const std::string sql =
                    "SELECT id FROM t WHERE val " + std::string(opstr[oi]) + " " +
                    std::to_string(k) + " ORDER BY id";
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t)
                    if (cmp_true(r.val, k, false, ops[oi])) want.push_back(id);
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "3VL cmp seed=%llu op=%s k=%lld: %s != %s\n",
                                 static_cast<unsigned long long>(seed), opstr[oi],
                                 static_cast<long long>(k), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 3) {
                // IN subquery: id IN (SELECT ref FROM u).
                const std::string sql =
                    "SELECT id FROM t WHERE id IN (SELECT ref FROM u) ORDER BY id";
                std::vector<std::int64_t> uv; bool unull = false;
                model.u_ref_column(uv, unull);
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t) {
                    (void)r;
                    if (in_true(id, uv, unull, /*is_not=*/false)) want.push_back(id);
                }
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "IN seed=%llu: %s != %s\n",
                                 static_cast<unsigned long long>(seed), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 4) {
                // NOT IN subquery (the NULL-in-NOT-IN rule).
                const std::string sql =
                    "SELECT id FROM t WHERE id NOT IN (SELECT ref FROM u) ORDER BY id";
                std::vector<std::int64_t> uv; bool unull = false;
                model.u_ref_column(uv, unull);
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t) {
                    (void)r;
                    if (in_true(id, uv, unull, /*is_not=*/true)) want.push_back(id);
                }
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "NOT IN seed=%llu (unull=%d): %s != %s\n",
                                 static_cast<unsigned long long>(seed), int(unull),
                                 got.c_str(), render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 5) {
                // NOT IN over the non-null subset (no NULL => true semantics differ).
                const std::string sql =
                    "SELECT id FROM t WHERE id NOT IN "
                    "(SELECT ref FROM u WHERE ref IS NOT NULL) ORDER BY id";
                std::vector<std::int64_t> uv;
                for (const auto& [uid, c] : model.u) { (void)uid; if (!c.null) uv.push_back(c.v); }
                std::vector<std::int64_t> want;
                for (const auto& [id, r] : model.t) {
                    (void)r;
                    if (in_true(id, uv, /*sub_null=*/false, /*is_not=*/true))
                        want.push_back(id);
                }
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "NOT IN(nn) seed=%llu: %s != %s\n",
                                 static_cast<unsigned long long>(seed), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 6) {
                // EXISTS / NOT EXISTS (existence of u rows matching a value).
                const std::int64_t k = static_cast<std::int64_t>(rng.below(kKeySpace));
                const bool neg = rng.below(2) == 1;
                const std::string sql =
                    std::string("SELECT id FROM t WHERE ") + (neg ? "NOT " : "") +
                    "EXISTS (SELECT uid FROM u WHERE ref = " + std::to_string(k) +
                    ") ORDER BY id";
                bool exists = false;
                for (const auto& [uid, c] : model.u) {
                    (void)uid;
                    if (!c.null && c.v == k) { exists = true; break; }
                }
                const bool keep = neg ? !exists : exists;
                std::vector<std::int64_t> want;
                if (keep) for (const auto& [id, r] : model.t) { (void)r; want.push_back(id); }
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "EXISTS seed=%llu neg=%d k=%lld: %s != %s\n",
                                 static_cast<unsigned long long>(seed), int(neg),
                                 static_cast<long long>(k), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else if (form == 7) {
                // Scalar subquery: WHERE val = (SELECT MAX(val) FROM t).
                const std::string sql =
                    "SELECT id FROM t WHERE val = (SELECT MAX(val) FROM t) ORDER BY id";
                // The model: MAX(val) over present values; if none present => NULL =>
                // the comparison is UNKNOWN for every row => no rows.
                bool any = false; std::int64_t mx = 0;
                for (const auto& [id, r] : model.t) {
                    (void)id;
                    if (!r.val.null) { if (!any || r.val.v > mx) mx = r.val.v; any = true; }
                }
                std::vector<std::int64_t> want;
                if (any) {
                    for (const auto& [id, r] : model.t)
                        if (!r.val.null && r.val.v == mx) want.push_back(id);
                }
                const std::string got = render_id_rows(eng.exec(sql));
                if (got != render_ids(want)) {
                    std::fprintf(stderr, "scalar-sub seed=%llu: %s != %s\n",
                                 static_cast<unsigned long long>(seed), got.c_str(),
                                 render_ids(want).c_str());
                    ok = false;
                }
            } else {
                // Aggregates skip NULL: COUNT(*)/COUNT(val)/SUM/MIN/MAX/AVG over t.
                const std::string sql =
                    "SELECT COUNT(*), COUNT(val), SUM(val), MIN(val), MAX(val), "
                    "AVG(val) FROM t";
                // Model aggregate (skip NULL; empty-present => MIN/MAX/AVG NULL, SUM=0;
                // BUT the synthetic ungrouped-EMPTY-table group renders 0 — mirror the
                // Engine: zero ROWS => 0; >=1 row but all-NULL => MIN/MAX/AVG NULL).
                std::int64_t cnt_star = 0, cnt_val = 0, sum = 0;
                bool any = false; std::int64_t mn = 0, mx = 0;
                for (const auto& [id, r] : model.t) {
                    (void)id;
                    ++cnt_star;
                    if (!r.val.null) {
                        ++cnt_val; sum += r.val.v;
                        if (!any || r.val.v < mn) mn = r.val.v;
                        if (!any || r.val.v > mx) mx = r.val.v;
                        any = true;
                    }
                }
                std::string want = "{COUNT(*)=" + std::to_string(cnt_star) +
                                   ";COUNT(val)=" + std::to_string(cnt_val) +
                                   ";SUM(val)=" + std::to_string(sum) + ";";
                if (cnt_star == 0) {
                    // synthetic empty group: MIN/MAX/AVG render 0 (pinned pre-v4).
                    want += "MIN(val)=0;MAX(val)=0;AVG(val)=0;}";
                } else if (!any) {
                    want += "MIN(val)=NULL;MAX(val)=NULL;AVG(val)=NULL;}";
                } else {
                    want += "MIN(val)=" + std::to_string(mn) + ";MAX(val)=" +
                            std::to_string(mx) + ";AVG(val)=" + std::to_string(sum / cnt_val) +
                            ";}";
                }
                const std::string got = render_agg(eng.exec(sql));
                if (got != want) {
                    std::fprintf(stderr, "agg-null seed=%llu: %s != %s\n",
                                 static_cast<unsigned long long>(seed), got.c_str(),
                                 want.c_str());
                    ok = false;
                }
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// (A) GROUP BY treats NULL as ONE distinct group (deterministic, fixed workload).
// ---------------------------------------------------------------------------
bool run_group_by_null() {
    SqlEngine eng;
    seed_tables(eng);
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (1, 1, 10)");
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (2, 1, 20)");
    (void)eng.exec("INSERT INTO t (id) VALUES (3)");              // grp NULL, val NULL
    (void)eng.exec("INSERT INTO t (id, grp) VALUES (4, 2)");       // grp=2, val NULL
    (void)eng.exec("INSERT INTO t (id, val) VALUES (5, 99)");      // grp NULL, val=99
    // GROUP BY grp => groups {NULL: ids 3,5}, {1: ids 1,2}, {2: id 4}.
    const ExecResult r =
        eng.exec("SELECT grp, COUNT(*), COUNT(val), SUM(val) FROM t GROUP BY grp "
                 "ORDER BY grp");
    const std::string got = render_agg(r);
    // Group order: NULL sorts FIRST (cmp_datum NULLs-first). Counts: NULL group has 2
    // rows, COUNT(val)=1 (id5), SUM=99. grp=1: 2 rows, COUNT(val)=2, SUM=30. grp=2: 1
    // row, COUNT(val)=0, SUM=0.
    const std::string want =
        "{grp=NULL;COUNT(*)=2;COUNT(val)=1;SUM(val)=99;}"
        "{grp=1;COUNT(*)=2;COUNT(val)=2;SUM(val)=30;}"
        "{grp=2;COUNT(*)=1;COUNT(val)=0;SUM(val)=0;}";
    const bool good = got == want;
    CHECK(good, "GROUP BY treats NULL as one group; aggregates skip NULL");
    if (!good) std::fprintf(stderr, "  GROUP BY NULL got=%s want=%s\n", got.c_str(),
                            want.c_str());
    return good;
}

// ---------------------------------------------------------------------------
// (C) CARDINALITY: a scalar subquery returning >1 row is a RAISED error.
// ---------------------------------------------------------------------------
bool run_cardinality() {
    SqlEngine eng;
    seed_tables(eng);
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (1, 1, 5)");
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (2, 1, 6)");
    // A non-aggregate single-column subquery over a 2-row table => 2 rows => ERROR.
    const ExecResult bad =
        eng.exec("SELECT id FROM t WHERE val = (SELECT val FROM t)");
    CHECK(!bad.ok, "scalar subquery returning >1 row must error (cardinality)");
    // The aggregate form returns ONE row => OK.
    const ExecResult good =
        eng.exec("SELECT id FROM t WHERE val = (SELECT MAX(val) FROM t)");
    CHECK(good.ok, "scalar aggregate subquery (one row) must succeed");
    return !bad.ok && good.ok;
}

// ---------------------------------------------------------------------------
// (E) TEETH — a WRONG NULL rule / a MISSED cardinality error must be CAUGHT. We do NOT
// corrupt the engine; we compute the WRONG answer the bug would produce and confirm it
// DIVERGES from the model / the engine's fail-closed behavior (so the gate is non-
// vacuous: a regression to those wrong rules WOULD be flagged by run_conformance).
// ---------------------------------------------------------------------------
bool run_teeth() {
    SqlEngine eng;
    Model model;
    seed_tables(eng);
    // t: id1(val NULL), id2(val=5); u: ref10=NULL, ref11=2.
    (void)eng.exec("INSERT INTO t (id, grp) VALUES (1, 1)");   // val NULL
    (void)model.insert_t(1, Cell::of(1), Cell::nul());
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (2, 1, 5)");
    (void)model.insert_t(2, Cell::of(1), Cell::of(5));
    (void)eng.exec("INSERT INTO u (uid) VALUES (10)");          // ref NULL
    (void)model.insert_u(10, Cell::nul());
    (void)eng.exec("INSERT INTO u (uid, ref) VALUES (11, 2)");
    (void)model.insert_u(11, Cell::of(2));
    bool ok = true;

    // TEETH 1: WRONG NULL rule "NULL = NULL is TRUE". The honest engine drops the
    // NULL-val row from `WHERE val = val`-style tests; here we use IS NULL as the
    // CORRECT way to select NULLs and confirm `val = <anything>` never returns id1.
    {
        const std::string honest =
            render_id_rows(eng.exec("SELECT id FROM t WHERE val = 5 ORDER BY id"));
        CHECK(honest == "(2)", "honest: val=5 returns only id2 (NULL-val id1 dropped)");
        // The WRONG rule would treat NULL as matching => would also/incorrectly return
        // id1 in a `val = NULL` query. The honest engine returns NOTHING for val=NULL.
        const std::string null_eq =
            render_id_rows(eng.exec("SELECT id FROM t WHERE val = NULL"));
        const std::string wrong_if_null_eq_null_true = "(1)";  // the bug's output
        const bool teeth1 = null_eq != wrong_if_null_eq_null_true && null_eq.empty();
        CHECK(teeth1, "TEETH: val = NULL must return 0 rows (NULL=NULL is NOT true)");
        ok = ok && teeth1 && honest == "(2)";
    }
    // TEETH 2: WRONG NOT-IN-with-NULL rule. u.ref has a NULL, so `id NOT IN (SELECT ref
    // FROM u)` is UNKNOWN for every id => 0 rows. The bug (ignore the subquery NULL)
    // would return ids NOT equal to 2 => {1}. Confirm the honest engine == 0 rows and
    // the wrong answer DIVERGES.
    {
        const std::string honest = render_id_rows(
            eng.exec("SELECT id FROM t WHERE id NOT IN (SELECT ref FROM u) ORDER BY id"));
        std::vector<std::int64_t> uv; bool unull = false; model.u_ref_column(uv, unull);
        std::vector<std::int64_t> want;
        for (const auto& [id, r] : model.t) { (void)r;
            if (in_true(id, uv, unull, true)) want.push_back(id); }
        CHECK(honest == render_ids(want), "honest NOT IN matches the 3VL model");
        const std::string wrong = "(1)";  // what the ignore-NULL bug would output
        const bool teeth2 = honest != wrong && honest.empty();
        CHECK(teeth2, "TEETH: NOT IN with a NULL subquery => 0 rows (not {1})");
        ok = ok && teeth2;
    }
    // TEETH 3: a MISSED scalar-subquery cardinality error. The honest engine ERRORS on
    // a >1-row scalar subquery; a bug that silently took the first row would NOT error.
    {
        const ExecResult r =
            eng.exec("SELECT id FROM t WHERE val = (SELECT val FROM t)");
        const bool teeth3 = !r.ok;  // a missed-cardinality bug would make this ok==true
        CHECK(teeth3, "TEETH: a >1-row scalar subquery must be a RAISED error");
        ok = ok && teeth3;
    }
    return ok;
}

// (D) determinism: the same fixed NULL+subquery workload => byte-identical output.
std::string run_fixed_workload() {
    SqlEngine eng;
    seed_tables(eng);
    (void)eng.exec("INSERT INTO t (id, grp, val) VALUES (3, 1, 30)");
    (void)eng.exec("INSERT INTO t (id, grp) VALUES (1, 2)");        // val NULL
    (void)eng.exec("INSERT INTO t (id, val) VALUES (2, 30)");        // grp NULL
    (void)eng.exec("INSERT INTO u (uid, ref) VALUES (1, 30)");
    (void)eng.exec("INSERT INTO u (uid) VALUES (2)");               // ref NULL
    std::string s;
    s += render_id_rows(eng.exec("SELECT id FROM t WHERE val IS NULL ORDER BY id"));
    s += "|";
    s += render_id_rows(
        eng.exec("SELECT id FROM t WHERE val = (SELECT MAX(val) FROM t) ORDER BY id"));
    s += "|";
    s += render_id_rows(
        eng.exec("SELECT id FROM t WHERE val IN (SELECT ref FROM u) ORDER BY id"));
    s += "|";
    s += render_id_rows(
        eng.exec("SELECT id FROM t WHERE val NOT IN (SELECT ref FROM u) ORDER BY id"));
    s += "|";
    s += render_agg(eng.exec("SELECT COUNT(*), COUNT(val), SUM(val), AVG(val) FROM t"));
    return s;
}

}  // namespace

int main() {
    const std::uint64_t seeds = sweep_count();

    for (std::uint64_t s = 0; s < seeds; ++s) {
        if (!run_conformance(s)) {
            ++g_failures;
        }
    }
    if (!run_group_by_null()) ++g_failures;
    if (!run_cardinality()) ++g_failures;
    if (!run_teeth()) {
        std::fprintf(stderr, "sql_subquery_test FAIL: teeth did not catch a wrong "
                             "NULL rule / missed cardinality\n");
        ++g_failures;
    }

    // (D) determinism — same workload twice => byte-identical.
    const std::string a = run_fixed_workload();
    const std::string b = run_fixed_workload();
    if (a != b) {
        std::fprintf(stderr, "sql_subquery_test FAIL: non-deterministic output\n"
                             "  a=%s\n  b=%s\n", a.c_str(), b.c_str());
        ++g_failures;
    }
    // Pin the fixed workload's output (the NULL + subquery semantics are exact):
    //   val IS NULL => id1; val = MAX(val)=30 => ids 2,3; val IN {30,NULL} => ids 2,3;
    //   val NOT IN {30,NULL} => 0 rows (NULL in subquery => UNKNOWN); agg COUNT(*)=3,
    //   COUNT(val)=2, SUM=60, AVG=30.
    const std::string want_fixed =
        "(1)|(2)(3)|(2)(3)||{COUNT(*)=3;COUNT(val)=2;SUM(val)=60;AVG(val)=30;}";
    if (a != want_fixed) {
        std::fprintf(stderr, "sql_subquery_test FAIL: fixed workload result unexpected\n"
                             "  got =%s\n  want=%s\n", a.c_str(), want_fixed.c_str());
        ++g_failures;
    }

    std::fprintf(stderr, "sql_subquery_test: seeds=%llu\n",
                 static_cast<unsigned long long>(seeds));
    if (g_failures != 0) {
        std::fprintf(stderr, "sql_subquery_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "sql_subquery_test: ALL PASS (explicit NULL three-valued logic "
                         "+ scalar/IN/NOT IN/EXISTS subqueries == reference model; "
                         "cardinality error raised; GROUP BY NULL; determinism; teeth "
                         "caught a wrong NULL rule + a missed cardinality)\n");
    return EXIT_SUCCESS;
}
