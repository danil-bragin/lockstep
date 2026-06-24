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
    expect_err("CREATE TABLE t (a INT, b INT, PRIMARY KEY (a, b))",
               "multi-column PK is OUT");
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
    expect_err("SELECT * FROM t JOIN x", "JOIN is OUT (trailing input)");
    expect_err("SELECT * FROM t AT FUNKY", "unknown AT level");
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
    test_where_predicate();
    test_aggregates();
    test_order_limit_distinct();
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
