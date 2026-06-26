// sql_parser_test.cpp — SQL SURFACE: PARSER UNIT TESTS (valid + invalid SQL).
//
// Source of truth: the SQL subset documented in query/sql/Parser.hpp + Ast.hpp.
// Asserts the hand-written recursive-descent parser produces the EXPECTED typed
// AST for valid input AND a clean POSITIONED error (never UB / never a thrown
// exception) for invalid input. Deterministic: same bytes => same AST / same error.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <lockstep/query/sql/Ast.hpp>
#include <lockstep/query/sql/Parser.hpp>

namespace {

using namespace lockstep::query::sql;
namespace q = lockstep::query;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "sql_parser_test FAIL [%s:%d]: %s\n",         \
                         __FILE__, __LINE__, (msg));                           \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Parse and expect SUCCESS; return the statement (or a default + a flagged fail).
Statement ok_parse(const std::string& sql) {
    ParseResult r = parse_sql(sql);
    if (!r.ok()) {
        std::fprintf(stderr, "sql_parser_test FAIL: expected OK for [%s] but got: %s\n",
                     sql.c_str(), r.error().render().c_str());
        ++g_failures;
        return Statement{};
    }
    return r.stmt();
}

// Parse and expect a clean ERROR (no exception, no UB).
void expect_err(const std::string& sql, const char* why) {
    ParseResult r = parse_sql(sql);
    if (r.ok()) {
        std::fprintf(stderr, "sql_parser_test FAIL: expected ERROR (%s) for [%s] but "
                             "parsed OK\n",
                     why, sql.c_str());
        ++g_failures;
        return;
    }
    // A reported error carries a non-empty message (the error model is real).
    CHECK(!r.error().message.empty(), "error must carry a message");
}

void test_create() {
    const Statement st = ok_parse(
        "CREATE TABLE users (id INT, name TEXT, PRIMARY KEY (id))");
    CHECK(st.kind == StmtKind::Create, "CREATE kind");
    CHECK(st.create.table == "users", "table name");
    CHECK(st.create.columns.size() == 2, "two columns");
    CHECK(st.create.columns[0].name == "id" && st.create.columns[0].type == Type::Int,
          "col0 id INT");
    CHECK(st.create.columns[1].name == "name" &&
              st.create.columns[1].type == Type::Text,
          "col1 name TEXT");
    CHECK(st.create.pk_column == "id", "pk id");

    // case-insensitive keywords + trailing semicolon
    const Statement st2 = ok_parse(
        "create table t (k int, v text, primary key (k));");
    CHECK(st2.kind == StmtKind::Create && st2.create.pk_column == "k",
          "lowercase keywords parse");

    expect_err("CREATE TABLE t (id INT, name TEXT)", "missing PRIMARY KEY");
    expect_err("CREATE TABLE t (id FLOAT, PRIMARY KEY (id))", "unsupported type FLOAT");
    expect_err("CREATE TABLE t (id INT, PRIMARY KEY (missing))",
               "PK names an undeclared column");
    (void)ok_parse("CREATE TABLE t (a INT, b INT, PRIMARY KEY (a, b))");  // F1: composite PK now parses
}

void test_insert() {
    const Statement st = ok_parse(
        "INSERT INTO users (id, name) VALUES (7, 'alice')");
    CHECK(st.kind == StmtKind::Insert, "INSERT kind");
    CHECK(st.insert.table == "users", "table");
    CHECK(st.insert.columns.size() == 2 && st.insert.values.size() == 2,
          "2 cols / 2 vals");
    CHECK(st.insert.values[0].type == Type::Int && st.insert.values[0].i == 7,
          "int literal 7");
    CHECK(st.insert.values[1].type == Type::Text && st.insert.values[1].s == "alice",
          "text literal alice");

    // negative int + escaped quote in a string
    const Statement st2 = ok_parse("INSERT INTO t (k, v) VALUES (-3, 'a''b')");
    CHECK(st2.insert.values[0].i == -3, "negative int");
    CHECK(st2.insert.values[1].s == "a'b", "escaped quote '' -> '");

    expect_err("INSERT INTO t (a, b) VALUES (1)", "col/val count mismatch");
    expect_err("INSERT INTO t (a) VALUES ('unterminated", "unterminated string");
}

void test_update_delete() {
    const Statement up = ok_parse("UPDATE users SET name = 'bob' WHERE id = 7");
    CHECK(up.kind == StmtKind::Update, "UPDATE kind");
    CHECK(up.update.set_column == "name" && up.update.set_value.s == "bob",
          "SET name=bob");
    CHECK(up.update.where_column == "id" && up.update.where_value.i == 7,
          "WHERE id=7");

    const Statement del = ok_parse("DELETE FROM users WHERE id = 7");
    CHECK(del.kind == StmtKind::Delete, "DELETE kind");
    CHECK(del.del.where_column == "id" && del.del.where_value.i == 7, "WHERE id=7");

    expect_err("UPDATE t SET a = 1", "missing WHERE");
    expect_err("DELETE FROM t", "missing WHERE");
}

void test_select() {
    const Statement star = ok_parse("SELECT * FROM users");
    CHECK(star.kind == StmtKind::Select && star.select.star, "SELECT *");
    CHECK(star.select.where == SelectWhereKind::None, "no WHERE => full scan");
    CHECK(star.select.level == q::Level::StrictSerializable,
          "default level is Strict (V-D5-SAFE strong default)");

    const Statement proj = ok_parse("SELECT id, name FROM users WHERE id = 5");
    CHECK(!proj.select.star && proj.select.columns.size() == 2, "projection cols");
    CHECK(proj.select.where == SelectWhereKind::Eq && proj.select.eq_value.i == 5,
          "WHERE id = 5 => point");

    const Statement rng = ok_parse(
        "SELECT * FROM users WHERE id BETWEEN 3 AND 9");
    CHECK(rng.select.where == SelectWhereKind::Between, "BETWEEN => range");
    CHECK(rng.select.lo_value.i == 3 && rng.select.hi_value.i == 9, "range bounds");

    // AT level annotations (V-D5-SAFE call-site visible)
    const Statement snap = ok_parse("SELECT * FROM t AT SNAPSHOT 4");
    CHECK(snap.select.level == q::Level::Snapshot && snap.select.snapshot_version == 4,
          "AT SNAPSHOT 4");
    const Statement bnd = ok_parse("SELECT * FROM t AT BOUNDED 2");
    CHECK(bnd.select.level == q::Level::BoundedStaleness && bnd.select.max_lag == 2,
          "AT BOUNDED 2");
    const Statement ryw = ok_parse("SELECT * FROM t WHERE id = 1 AT RYW 9");
    CHECK(ryw.select.level == q::Level::ReadYourWrites && ryw.select.session == 9,
          "AT RYW 9");

    expect_err("SELECT FROM t", "empty projection");
    expect_err("SELECT * users", "missing FROM");
    expect_err("SELECT * FROM t JOIN x", "dangling JOIN (missing ON)");
    expect_err("SELECT * FROM t AT FUNKY", "unknown AT level");
}

// v3: JOIN / ON / alias / qualified-column grammar (valid ASTs + clean errors).
void test_join() {
    // INNER JOIN with an equi ON over qualified columns.
    const Statement s1 = ok_parse(
        "SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.did");
    CHECK(s1.kind == StmtKind::Select, "join select kind");
    CHECK(s1.select.from.size() == 2, "two FROM entries");
    CHECK(s1.select.is_join(), "is_join true");
    CHECK(s1.select.from[0].table == "emp" && s1.select.from[0].alias == "emp",
          "base table emp, alias defaults to name");
    CHECK(s1.select.from[1].kind == JoinKind::Inner &&
              s1.select.from[1].table == "dept",
          "join entry is INNER on dept");
    CHECK(s1.select.from[1].on.present(), "ON predicate present");
    {
        const PredNode& on = s1.select.from[1].on.nodes[static_cast<std::size_t>(
            s1.select.from[1].on.root)];
        CHECK(on.kind == PredNodeKind::Cmp && on.op == CmpOp::Eq, "ON is an equality");
        CHECK(on.qualifier == "emp" && on.column == "dept", "ON lhs emp.dept");
        CHECK(on.rhs_is_column && on.rhs_qualifier == "dept" && on.rhs_column == "did",
              "ON rhs is column dept.did (equi-join key)");
    }
    // Qualified SELECT-list labels keep the qualifier (self-join disambiguation).
    CHECK(s1.select.items.size() == 2 && s1.select.items[0].qualifier == "emp" &&
              s1.select.items[0].column == "name" &&
              s1.select.items[0].label == "emp.name",
          "qualified select item emp.name");

    // LEFT [OUTER] JOIN.
    const Statement s2 = ok_parse(
        "SELECT a.x FROM a LEFT JOIN b ON a.k = b.k");
    CHECK(s2.select.from[1].kind == JoinKind::Left, "LEFT JOIN kind");
    const Statement s2o = ok_parse(
        "SELECT a.x FROM a LEFT OUTER JOIN b ON a.k = b.k");
    CHECK(s2o.select.from[1].kind == JoinKind::Left, "LEFT OUTER == LEFT");

    // AS alias + bare alias.
    const Statement s3 = ok_parse(
        "SELECT x.id, y.id FROM t AS x JOIN t AS y ON x.id = y.id");
    CHECK(s3.select.from[0].alias == "x" && s3.select.from[1].alias == "y",
          "AS aliases x / y on a self-join");
    const Statement s3b = ok_parse("SELECT p.v FROM tbl p WHERE p.v > 0");
    CHECK(s3b.select.from.size() == 1 && s3b.select.from[0].table == "tbl" &&
              s3b.select.from[0].alias == "p",
          "bare alias 'p' on a single table");

    // Comma cross join + explicit CROSS JOIN.
    const Statement s4 = ok_parse("SELECT a.id, b.id FROM a, b");
    CHECK(s4.select.from.size() == 2 && s4.select.from[1].kind == JoinKind::Cross,
          "comma => CROSS join");
    const Statement s4c = ok_parse("SELECT a.id FROM a CROSS JOIN b");
    CHECK(s4c.select.from[1].kind == JoinKind::Cross, "CROSS JOIN keyword");

    // Multiple (left-deep) joins.
    const Statement s5 = ok_parse(
        "SELECT a.id FROM a JOIN b ON a.k = b.k JOIN c ON b.m = c.m");
    CHECK(s5.select.from.size() == 3, "three-table left-deep join");
    CHECK(s5.select.from[1].kind == JoinKind::Inner &&
              s5.select.from[2].kind == JoinKind::Inner,
          "both joins INNER");

    // A bare JOIN == INNER JOIN.
    const Statement s6 = ok_parse("SELECT a.id FROM a JOIN b ON a.k = b.k");
    CHECK(s6.select.from[1].kind == JoinKind::Inner, "bare JOIN == INNER");

    // A JOIN suppresses the PK fast-path (WHERE runs over the joined row).
    const Statement s7 = ok_parse(
        "SELECT a.id FROM a JOIN b ON a.k = b.k WHERE a.id = 5");
    CHECK(s7.select.where == SelectWhereKind::None,
          "JOIN suppresses the PK point fast-path");

    // ORDER BY / GROUP BY accept qualified columns.
    const Statement s8 = ok_parse(
        "SELECT a.dept, COUNT(*) FROM a JOIN b ON a.k = b.k GROUP BY a.dept "
        "ORDER BY a.dept DESC");
    CHECK(s8.select.group_by.size() == 1 && s8.select.group_by[0] == "a.dept",
          "qualified GROUP BY a.dept");
    CHECK(s8.select.order_by.size() == 1 && s8.select.order_by[0].qualifier == "a" &&
              s8.select.order_by[0].column == "dept" &&
              s8.select.order_by[0].descending,
          "qualified ORDER BY a.dept DESC");

    // CLEAN POSITIONED ERRORS.
    expect_err("SELECT * FROM a JOIN b", "INNER JOIN missing ON");
    expect_err("SELECT * FROM a LEFT JOIN b", "LEFT JOIN missing ON");
    expect_err("SELECT * FROM a JOIN b ON", "ON missing predicate");
    expect_err("SELECT * FROM a CROSS JOIN b ON a.k = b.k", "CROSS takes no ON");
    (void)ok_parse("SELECT * FROM a RIGHT JOIN b ON a.k = b.k");  // E1: RIGHT JOIN now supported
    (void)ok_parse("SELECT * FROM a FULL OUTER JOIN b ON a.k = b.k");  // E1: FULL JOIN now supported
    expect_err("SELECT * FROM a AS", "AS without an alias");
    expect_err("SELECT a. FROM a", "dangling '.' (no column after qualifier)");
    expect_err("SELECT * FROM a JOIN ON a.k = b.k", "JOIN without a table");
}

// v2: WHERE on ANY column — a general boolean predicate tree (=,!=,<,<=,>,>=,
// AND/OR/NOT, parens, BETWEEN sugar).
void test_where_predicate() {
    // A non-PK column comparison parses into the general filter (NOT the PK fast
    // path, so where == None unless it is exactly a PK eq/range).
    const Statement s1 = ok_parse("SELECT * FROM t WHERE sal > 100");
    CHECK(s1.select.filter.present(), "general filter present");
    CHECK(s1.select.where == SelectWhereKind::None,
          "a non-PK '>' is not a PK fast-path candidate");
    CHECK(s1.select.filter.nodes.size() == 1, "single Cmp node");
    {
        const PredNode& n = s1.select.filter.nodes[0];
        CHECK(n.kind == PredNodeKind::Cmp && n.column == "sal" && n.op == CmpOp::Gt &&
                  n.literal.i == 100,
              "sal > 100 leaf");
    }

    // AND / OR / NOT + parens build the tree.
    const Statement s2 =
        ok_parse("SELECT * FROM t WHERE a = 1 AND (b < 2 OR NOT c >= 3)");
    CHECK(s2.select.filter.present(), "compound filter present");
    {
        const Predicate& p = s2.select.filter;
        const PredNode& root = p.nodes[static_cast<std::size_t>(p.root)];
        CHECK(root.kind == PredNodeKind::And, "root is AND");
    }

    // != and <> both parse to Ne.
    const Statement s3 = ok_parse("SELECT * FROM t WHERE a != 1");
    CHECK(s3.select.filter.nodes[0].op == CmpOp::Ne, "!= => Ne");
    const Statement s4 = ok_parse("SELECT * FROM t WHERE a <> 1");
    CHECK(s4.select.filter.nodes[0].op == CmpOp::Ne, "<> => Ne");

    // A bare PK equality still records the v1 fast-path candidate.
    const Statement s5 = ok_parse("SELECT * FROM t WHERE id = 5");
    CHECK(s5.select.where == SelectWhereKind::Eq && s5.select.eq_value.i == 5,
          "PK eq fast-path candidate recorded");

    // A column BETWEEN lowers to >= AND <= (sugar) and records a range candidate.
    const Statement s6 = ok_parse("SELECT * FROM t WHERE id BETWEEN 3 AND 9");
    CHECK(s6.select.where == SelectWhereKind::Between && s6.select.lo_value.i == 3 &&
              s6.select.hi_value.i == 9,
          "BETWEEN range candidate");
    CHECK(s6.select.filter.nodes.size() == 3, "BETWEEN sugar => Cmp,Cmp,And");

    expect_err("SELECT * FROM t WHERE a >", "missing rhs literal");
    expect_err("SELECT * FROM t WHERE (a = 1", "unclosed paren");
    expect_err("SELECT * FROM t WHERE a ! 1", "stray '!' is not '!='");
    expect_err("SELECT * FROM t WHERE AND a = 1", "leading AND");
}

// v2: aggregates in the SELECT list + GROUP BY + HAVING.
void test_aggregates() {
    const Statement s1 = ok_parse("SELECT COUNT(*) FROM t");
    CHECK(s1.select.has_aggregates, "has aggregates");
    CHECK(s1.select.items.size() == 1 &&
              s1.select.items[0].kind == SelectItemKind::Aggregate &&
              s1.select.items[0].agg.kind == AggKind::CountStar,
          "COUNT(*) item");
    CHECK(s1.select.items[0].label == "COUNT(*)", "COUNT(*) label");

    const Statement s2 =
        ok_parse("SELECT dept, SUM(sal), MIN(sal), MAX(sal), AVG(sal), COUNT(sal) "
                 "FROM t GROUP BY dept");
    CHECK(s2.select.items.size() == 6, "6 select items");
    CHECK(s2.select.items[0].kind == SelectItemKind::Column &&
              s2.select.items[0].column == "dept",
          "grouped column item");
    CHECK(s2.select.items[1].agg.kind == AggKind::Sum &&
              s2.select.items[1].agg.column == "sal",
          "SUM(sal)");
    CHECK(s2.select.items[5].agg.kind == AggKind::Count, "COUNT(sal)");
    CHECK(s2.select.group_by.size() == 1 && s2.select.group_by[0] == "dept",
          "GROUP BY dept");

    const Statement s3 = ok_parse(
        "SELECT dept, COUNT(*) FROM t GROUP BY dept HAVING COUNT(*) > 2 AND "
        "SUM(sal) >= 100");
    CHECK(s3.select.having.present(), "HAVING present");
    {
        const Predicate& h = s3.select.having;
        const PredNode& root = h.nodes[static_cast<std::size_t>(h.root)];
        CHECK(root.kind == PredNodeKind::And, "HAVING root AND");
    }

    expect_err("SELECT COUNT() FROM t", "COUNT needs * or a column");
    expect_err("SELECT SUM FROM t", "aggregate name without '('");
    expect_err("SELECT COUNT(* FROM t", "unclosed aggregate");
    expect_err("SELECT * FROM t GROUP BY", "GROUP BY needs a column");
    expect_err("SELECT * FROM t GROUP dept", "GROUP without BY");
    expect_err("SELECT * FROM t HAVING > 1", "HAVING needs a predicate");
}

// v2: ORDER BY [ASC|DESC] + LIMIT/OFFSET + DISTINCT.
void test_order_limit_distinct() {
    const Statement s1 =
        ok_parse("SELECT id FROM t ORDER BY sal DESC, id ASC LIMIT 10 OFFSET 5");
    CHECK(s1.select.order_by.size() == 2, "two order keys");
    CHECK(s1.select.order_by[0].column == "sal" && s1.select.order_by[0].descending,
          "ORDER BY sal DESC");
    CHECK(s1.select.order_by[1].column == "id" && !s1.select.order_by[1].descending,
          "ORDER BY id ASC");
    CHECK(s1.select.has_limit && s1.select.limit == 10 && s1.select.offset == 5,
          "LIMIT 10 OFFSET 5");

    const Statement s2 = ok_parse("SELECT DISTINCT dept FROM t");
    CHECK(s2.select.distinct, "DISTINCT");

    const Statement s3 = ok_parse("SELECT * FROM t ORDER BY id LIMIT 3");
    CHECK(!s3.select.order_by[0].descending, "default ASC");
    CHECK(s3.select.has_limit && s3.select.limit == 3 && s3.select.offset == 0,
          "LIMIT without OFFSET => offset 0");

    // A full pipeline parses end to end.
    const Statement s4 = ok_parse(
        "SELECT DISTINCT dept, COUNT(*) FROM t WHERE sal > 0 GROUP BY dept "
        "HAVING COUNT(*) >= 2 ORDER BY dept DESC LIMIT 5 OFFSET 1 AT SNAPSHOT 3");
    CHECK(s4.select.distinct && s4.select.has_aggregates &&
              s4.select.filter.present() && s4.select.group_by.size() == 1 &&
              s4.select.having.present() && s4.select.order_by.size() == 1 &&
              s4.select.has_limit && s4.select.level == q::Level::Snapshot &&
              s4.select.snapshot_version == 3,
          "full v2 pipeline parses with all clauses + AT level");

    expect_err("SELECT * FROM t ORDER BY", "ORDER BY needs a column");
    expect_err("SELECT * FROM t ORDER id", "ORDER without BY");
    expect_err("SELECT * FROM t LIMIT -1", "negative LIMIT");
    expect_err("SELECT * FROM t LIMIT 5 OFFSET -2", "negative OFFSET");
    expect_err("SELECT * FROM t LIMIT 'x'", "non-integer LIMIT");
}

// v4: explicit NULL grammar (NOT NULL constraint, NULL literal, IS [NOT] NULL) +
// subquery grammar (scalar (SELECT ...), [NOT] IN (SELECT ...), [NOT] EXISTS (...)).
void test_null_and_subqueries() {
    // NOT NULL constraint + nullable default + a column nullability flag.
    {
        const Statement st = ok_parse(
            "CREATE TABLE t (id INT, a INT NOT NULL, b INT, c TEXT NULL, "
            "PRIMARY KEY (id))");
        CHECK(st.create.columns.size() == 4, "four columns");
        CHECK(!st.create.columns[1].nullable, "a is NOT NULL");
        CHECK(st.create.columns[2].nullable, "b is nullable by default");
        CHECK(st.create.columns[3].nullable, "c NULL spelling is nullable");
    }
    // NULL literal in INSERT VALUES + an INSERT that OMITS a column.
    {
        const Statement st =
            ok_parse("INSERT INTO t (id, a, b) VALUES (1, 2, NULL)");
        CHECK(st.insert.values.size() == 3, "three values");
        CHECK(st.insert.values[2].is_null, "third value is a NULL literal");
        const Statement st2 = ok_parse("INSERT INTO t (id, a) VALUES (1, 2)");
        CHECK(st2.insert.columns.size() == 2, "omitted columns allowed by parser");
    }
    // IS NULL / IS NOT NULL.
    {
        const Statement st =
            ok_parse("SELECT id FROM t WHERE b IS NULL");
        const Predicate& p = st.select.filter;
        CHECK(p.present(), "filter present");
        CHECK(p.nodes[static_cast<std::size_t>(p.root)].kind == PredNodeKind::IsNull,
              "IS NULL node");
        CHECK(!p.nodes[static_cast<std::size_t>(p.root)].is_not, "IS NULL not negated");
        const Statement st2 =
            ok_parse("SELECT id FROM t WHERE b IS NOT NULL");
        const Predicate& p2 = st2.select.filter;
        CHECK(p2.nodes[static_cast<std::size_t>(p2.root)].kind == PredNodeKind::IsNull &&
                  p2.nodes[static_cast<std::size_t>(p2.root)].is_not,
              "IS NOT NULL node negated");
    }
    // A NULL literal on a comparison RHS.
    {
        const Statement st = ok_parse("SELECT id FROM t WHERE b = NULL");
        const Predicate& p = st.select.filter;
        CHECK(p.nodes[static_cast<std::size_t>(p.root)].literal.is_null,
              "= NULL is a NULL-literal comparison (UNKNOWN at run time)");
    }
    // Scalar subquery RHS.
    {
        const Statement st = ok_parse(
            "SELECT id FROM t WHERE a = (SELECT MAX(a) FROM t)");
        const Predicate& p = st.select.filter;
        const PredNode& n = p.nodes[static_cast<std::size_t>(p.root)];
        CHECK(n.kind == PredNodeKind::Cmp && n.rhs_is_subquery && n.subquery != nullptr,
              "scalar subquery RHS");
        CHECK(n.subquery->has_aggregates, "inner SELECT has an aggregate");
    }
    // IN / NOT IN subquery.
    {
        const Statement st = ok_parse(
            "SELECT id FROM t WHERE a IN (SELECT b FROM t)");
        const Predicate& p = st.select.filter;
        const PredNode& n = p.nodes[static_cast<std::size_t>(p.root)];
        CHECK(n.kind == PredNodeKind::InList && !n.is_not && n.subquery != nullptr,
              "IN subquery node");
        const Statement st2 = ok_parse(
            "SELECT id FROM t WHERE a NOT IN (SELECT b FROM t)");
        const Predicate& p2 = st2.select.filter;
        CHECK(p2.nodes[static_cast<std::size_t>(p2.root)].kind == PredNodeKind::InList &&
                  p2.nodes[static_cast<std::size_t>(p2.root)].is_not,
              "NOT IN subquery node");
    }
    // EXISTS / NOT EXISTS.
    {
        const Statement st = ok_parse(
            "SELECT id FROM t WHERE EXISTS (SELECT b FROM t WHERE b = 1)");
        const Predicate& p = st.select.filter;
        CHECK(p.nodes[static_cast<std::size_t>(p.root)].kind == PredNodeKind::Exists,
              "EXISTS node");
        // NOT EXISTS arrives as a prefix-NOT wrapping the Exists node.
        const Statement st2 = ok_parse(
            "SELECT id FROM t WHERE NOT EXISTS (SELECT b FROM t)");
        const Predicate& p2 = st2.select.filter;
        const PredNode& root = p2.nodes[static_cast<std::size_t>(p2.root)];
        CHECK(root.kind == PredNodeKind::Not, "NOT EXISTS => prefix NOT");
        CHECK(p2.nodes[static_cast<std::size_t>(root.left)].kind == PredNodeKind::Exists,
              "NOT wraps an Exists node");
    }
    // A subquery may itself carry WHERE / JOIN / GROUP BY (full SELECT grammar reused).
    {
        const Statement st = ok_parse(
            "SELECT id FROM t WHERE a IN (SELECT b FROM t WHERE id > 3 "
            "GROUP BY b HAVING COUNT(*) >= 1)");
        const Predicate& p = st.select.filter;
        CHECK(p.nodes[static_cast<std::size_t>(p.root)].subquery->group_by.size() == 1,
              "subquery carries its own GROUP BY");
    }

    // ---- clean ERRORS for malformed NULL / subquery grammar ----
    expect_err("SELECT id FROM t WHERE a IS", "IS with no NULL");
    expect_err("SELECT id FROM t WHERE a IS NOT", "IS NOT with no NULL");
    expect_err("SELECT id FROM t WHERE a IN (1, 2, 3)",
               "literal IN-list is OUT (subquery only)");
    expect_err("SELECT id FROM t WHERE a IN ()", "empty IN parens");
    expect_err("SELECT id FROM t WHERE a IN (SELECT b FROM t",
               "unclosed subquery paren");
    expect_err("SELECT id FROM t WHERE EXISTS (DELETE FROM t WHERE id = 1)",
               "EXISTS body must be a SELECT");
    expect_err("SELECT id FROM t WHERE a NOT b", "NOT not followed by IN");
}

void test_misc_errors() {
    expect_err("", "empty input");
    expect_err("FROBNICATE x", "unknown statement keyword");
    expect_err("SELECT * FROM t; SELECT * FROM u",
               "multi-statement is OUT (trailing input)");
    expect_err("INSERT INTO t (a) VALUES (1) garbage", "trailing garbage");
}

void test_determinism() {
    const std::string sql =
        "SELECT id, name FROM users WHERE id BETWEEN -2 AND 100 AT BOUNDED 3";
    ParseResult a = parse_sql(sql);
    ParseResult b = parse_sql(sql);
    CHECK(a.ok() && b.ok(), "both parse");
    // Same bytes => identical AST shape (compare the load-bearing fields).
    CHECK(a.stmt().select.table == b.stmt().select.table &&
              a.stmt().select.lo_value.i == b.stmt().select.lo_value.i &&
              a.stmt().select.hi_value.i == b.stmt().select.hi_value.i &&
              a.stmt().select.level == b.stmt().select.level &&
              a.stmt().select.max_lag == b.stmt().select.max_lag,
          "deterministic parse: same bytes => same AST");
}

}  // namespace

int main() {
    test_create();
    test_insert();
    test_update_delete();
    test_select();
    test_join();
    test_where_predicate();
    test_aggregates();
    test_order_limit_distinct();
    test_null_and_subqueries();
    test_misc_errors();
    test_determinism();

    if (g_failures != 0) {
        std::fprintf(stderr, "sql_parser_test: %d FAILURE(S)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "sql_parser_test: ALL PASS (valid AST + clean positioned "
                         "errors; deterministic)\n");
    return EXIT_SUCCESS;
}
