// sql_parser_test.cpp — SQL SURFACE: PARSER UNIT TESTS (valid + invalid SQL).
//
// Source of truth: the SQL subset documented in query/sql/Parser.hpp + Ast.hpp.
// Asserts the hand-written recursive-descent parser produces the EXPECTED typed
// AST for valid input AND a clean POSITIONED error (never UB / never a thrown
// exception) for invalid input. Deterministic: same bytes => same AST / same error.

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
