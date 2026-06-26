#pragma once

// Parser.hpp — SQL SURFACE: a hand-written, DETERMINISTIC recursive-descent parser
// (a tokenizer + a parser) for the bounded SQL subset -> a typed Statement (Ast.hpp).
//
// DESIGN (the spec's parser requirements):
//   * HAND-WRITTEN recursive descent. NO std::regex, NO locale-dependent parsing
//     (we classify bytes by hand: ASCII letters/digits only; keywords are matched
//     case-INSENSITIVELY by an ASCII-lowercase fold). The tokenizer is a pure
//     function of the input string.
//   * NO exceptions as control flow. Every fallible step returns a ParseResult
//     (an `expected`-style value: a Statement on success OR a ParseError with a
//     message + byte position). A malformed query is a clean, reported error —
//     never UB, never a thrown exception leaking nondeterminism.
//   * DETERMINISTIC: same input bytes => same tokens => same AST / same error. No
//     ambient state, no clock, no rng.
//
// THE SUBSET (v1):
//   CREATE TABLE t (c TYPE, ..., PRIMARY KEY (c))
//   INSERT INTO t (c, ...) VALUES (v, ...)
//   UPDATE t SET c = v WHERE pk = v
//   DELETE FROM t WHERE pk = v
//   SELECT [DISTINCT] (* | <item>, ...) FROM t
//          [WHERE <predicate>]                 -- v2: ANY-column boolean tree
//          [GROUP BY c, ...] [HAVING <agg-pred>]
//          [ORDER BY c [ASC|DESC], ...] [LIMIT n [OFFSET m]]
//          [AT STRICT | SNAPSHOT n | BOUNDED n | RYW n]
//   <item>      ::= <column> | <agg>
//   <agg>       ::= COUNT(*) | COUNT(c) | SUM(c) | MIN(c) | MAX(c) | AVG(c)
//   <predicate> ::= or-expr ; or-expr ::= and-expr (OR and-expr)* ;
//                   and-expr ::= not-expr (AND not-expr)* ;
//                   not-expr ::= NOT not-expr | primary ;
//                   primary  ::= '(' or-expr ')' | <column> <cmpop> <literal>
//                              | <pk> BETWEEN a AND b
//   <cmpop>     ::= = | != | <> | < | <= | > | >=
// Literals: a signed integer (e.g. 42, -7) or a single-quoted string ('foo', with
// '' as an escaped quote). Identifiers: [A-Za-z_][A-Za-z0-9_]*. A trailing ';' is
// optional. Anything outside the subset (JOIN, subquery, ...) is a clean
// "unsupported" / "expected X" error.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <lockstep/query/sql/Ast.hpp>
#include <lockstep/query/sql/Catalog.hpp>

namespace lockstep::query::sql {

// ----------------------------------------------------------------------------
// A parse error: a human-readable message + the byte offset it was detected at.
// (NOT an exception — returned by value so the caller branches on it.)
// ----------------------------------------------------------------------------
struct ParseError {
    std::string message;
    std::size_t pos = 0;

    // Explicit member-init constructors (no aggregate brace-init): the analyzer
    // tracks `pos` as definitely-initialized through every construction path,
    // including the std::variant copy in the expect_* helpers.
    ParseError() : pos(0) {}
    ParseError(std::string msg, std::size_t at) : message(std::move(msg)), pos(at) {}

    [[nodiscard]] std::string render() const {
        return "parse error at byte " + std::to_string(pos) + ": " + message;
    }
};

// A Result/expected: either a parsed Statement or a ParseError.
class ParseResult {
public:
    ParseResult(Statement stmt) : value_(std::move(stmt)) {}  // NOLINT(*-explicit*)
    ParseResult(ParseError err) : value_(std::move(err)) {}   // NOLINT(*-explicit*)

    [[nodiscard]] bool ok() const { return std::holds_alternative<Statement>(value_); }
    [[nodiscard]] const Statement& stmt() const { return std::get<Statement>(value_); }
    [[nodiscard]] const ParseError& error() const { return std::get<ParseError>(value_); }

private:
    std::variant<Statement, ParseError> value_;
};

// ----------------------------------------------------------------------------
// THE TOKENIZER. Deterministic byte classification (ASCII only). Token kinds the
// grammar needs. Keywords are NOT a separate kind — an identifier token is matched
// case-insensitively against keyword spellings by the parser.
// ----------------------------------------------------------------------------
enum class Tok : std::uint8_t {
    Ident,    // identifier or keyword text (raw, original case)
    IntLit,   // a signed integer literal
    StrLit,   // a single-quoted string literal (decoded)
    LParen,   // (
    RParen,   // )
    Comma,    // ,
    Eq,       // =
    Ne,       // != or <>
    Lt,       // <
    Le,       // <=
    Gt,       // >
    Ge,       // >=
    Semi,     // ;
    Star,     // *
    Dot,      // .  (v3: qualified column table.col)
    End,      // end of input
    Bad,      // a lexing error (unterminated string / stray byte)
};

struct Token {
    Tok kind = Tok::End;
    std::string text;       // Ident: the raw identifier; StrLit: the decoded string
    std::int64_t int_val = 0;  // IntLit: the parsed value
    std::size_t pos = 0;    // byte offset of the token start
    std::string bad_msg;    // Bad: why
};

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    // Produce the next token. Pure progression over the byte string.
    [[nodiscard]] Token next() {
        skip_space();
        Token t;
        t.pos = i_;
        if (i_ >= src_.size()) {
            t.kind = Tok::End;
            return t;
        }
        const char c = src_[i_];
        if (is_ident_start(c)) {
            return lex_ident();
        }
        if (is_digit(c) ||
            (c == '-' && i_ + 1 < src_.size() && is_digit(src_[i_ + 1]))) {
            return lex_int();
        }
        if (c == '\'') {
            return lex_string();
        }
        switch (c) {
            case '(':
                ++i_;
                t.kind = Tok::LParen;
                return t;
            case ')':
                ++i_;
                t.kind = Tok::RParen;
                return t;
            case ',':
                ++i_;
                t.kind = Tok::Comma;
                return t;
            case '=':
                ++i_;
                t.kind = Tok::Eq;
                return t;
            case '<':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '=') {
                    ++i_;
                    t.kind = Tok::Le;  // <=
                } else if (i_ < src_.size() && src_[i_] == '>') {
                    ++i_;
                    t.kind = Tok::Ne;  // <> (SQL not-equal)
                } else {
                    t.kind = Tok::Lt;  // <
                }
                return t;
            case '>':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '=') {
                    ++i_;
                    t.kind = Tok::Ge;  // >=
                } else {
                    t.kind = Tok::Gt;  // >
                }
                return t;
            case '!':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '=') {
                    ++i_;
                    t.kind = Tok::Ne;  // !=
                    return t;
                }
                t.kind = Tok::Bad;
                t.bad_msg = "expected '=' after '!' (the only '!' use is '!=')";
                return t;
            case ';':
                ++i_;
                t.kind = Tok::Semi;
                return t;
            case '*':
                ++i_;
                t.kind = Tok::Star;
                return t;
            case '.':
                ++i_;
                t.kind = Tok::Dot;
                return t;
            default:
                break;
        }
        t.kind = Tok::Bad;
        t.bad_msg = std::string("unexpected byte '") + c + "'";
        ++i_;
        return t;
    }

private:
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_alpha(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    static bool is_ident_start(char c) { return is_alpha(c) || c == '_'; }
    static bool is_ident_cont(char c) { return is_ident_start(c) || is_digit(c); }
    static bool is_space(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v';
    }

    void skip_space() {
        while (i_ < src_.size() && is_space(src_[i_])) {
            ++i_;
        }
    }

    Token lex_ident() {
        Token t;
        t.pos = i_;
        t.kind = Tok::Ident;
        const std::size_t start = i_;
        while (i_ < src_.size() && is_ident_cont(src_[i_])) {
            ++i_;
        }
        t.text = src_.substr(start, i_ - start);
        return t;
    }

    Token lex_int() {
        Token t;
        t.pos = i_;
        const std::size_t start = i_;
        if (src_[i_] == '-') {
            ++i_;
        }
        while (i_ < src_.size() && is_digit(src_[i_])) {
            ++i_;
        }
        // Reject e.g. "12abc" as an identifier-adjacent number (a stray suffix).
        if (i_ < src_.size() && is_ident_cont(src_[i_])) {
            t.kind = Tok::Bad;
            t.bad_msg = "malformed numeric literal";
            return t;
        }
        const std::string digits = src_.substr(start, i_ - start);
        t.kind = Tok::IntLit;
        t.int_val = parse_i64(digits);
        return t;
    }

    // Deterministic signed-decimal parse (no strtol/locale). Saturates on overflow
    // (a bounded subset; values are small) so the parse is total.
    static std::int64_t parse_i64(const std::string& digits) {
        bool neg = false;
        std::size_t p = 0;
        if (p < digits.size() && digits[p] == '-') {
            neg = true;
            ++p;
        }
        std::int64_t v = 0;
        for (; p < digits.size(); ++p) {
            const int d = digits[p] - '0';
            if (v > (9223372036854775807LL - d) / 10) {
                v = 9223372036854775807LL;  // saturate
                break;
            }
            v = v * 10 + d;
        }
        return neg ? -v : v;
    }

    Token lex_string() {
        Token t;
        t.pos = i_;
        ++i_;  // opening quote
        std::string out;
        while (i_ < src_.size()) {
            const char c = src_[i_];
            if (c == '\'') {
                if (i_ + 1 < src_.size() && src_[i_ + 1] == '\'') {
                    out.push_back('\'');  // '' -> a literal quote
                    i_ += 2;
                    continue;
                }
                ++i_;  // closing quote
                t.kind = Tok::StrLit;
                t.text = std::move(out);
                return t;
            }
            out.push_back(c);
            ++i_;
        }
        t.kind = Tok::Bad;
        t.bad_msg = "unterminated string literal";
        return t;
    }

    std::string src_;
    std::size_t i_ = 0;
};

// ----------------------------------------------------------------------------
// THE PARSER. Recursive descent over a one-token-lookahead stream. Every grammar
// rule returns a ParseResult; on a mismatch it produces a positioned ParseError.
// ----------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::string src) : lex_(std::move(src)) { advance(); }

    // Parse ONE complete statement (the whole input must be one statement + an
    // optional trailing ';'). v1 is one statement per parse (multi-statement is OUT).
    [[nodiscard]] ParseResult parse() {
        if (cur_.kind != Tok::Ident) {
            return err("expected a statement keyword (CREATE/INSERT/UPDATE/DELETE/"
                       "SELECT)");
        }
        const std::string kw = lower(cur_.text);
        ParseResult r = err("unreachable");
        if (kw == "create") {
            r = parse_create();
        } else if (kw == "insert") {
            r = parse_insert();
        } else if (kw == "update") {
            r = parse_update();
        } else if (kw == "delete") {
            r = parse_delete();
        } else if (kw == "select") {
            r = parse_select();
        } else if (kw == "explain") {
            // EXPLAIN [ANALYZE] <select> — transparency. Consume the prefix, parse the inner
            // SELECT, and tag it so the engine returns the plan (+ ANALYZE counters) instead
            // of rows. Only SELECT is explainable in this phase.
            advance();  // consume EXPLAIN
            bool analyze = false;
            if (cur_.kind == Tok::Ident && lower(cur_.text) == "analyze") {
                analyze = true;
                advance();
            }
            if (cur_.kind != Tok::Ident || lower(cur_.text) != "select") {
                return err("EXPLAIN must be followed by a SELECT statement");
            }
            Statement st;
            st.kind = StmtKind::Select;
            if (auto e = parse_select_stmt(st.select)) {
                return ParseResult{*e};
            }
            st.select.explain = true;
            st.select.explain_analyze = analyze;
            r = ParseResult{std::move(st)};
        } else if (kw == "drop") {
            r = parse_drop_index();
        } else {
            return err("unknown / unsupported statement keyword '" + cur_.text +
                       "' (v1 supports CREATE/INSERT/UPDATE/DELETE/SELECT)");
        }
        if (!r.ok()) {
            return r;
        }
        // Allow an optional trailing ';' then require end-of-input.
        if (cur_.kind == Tok::Semi) {
            advance();
        }
        if (cur_.kind != Tok::End) {
            return err("unexpected trailing input after statement (one statement per "
                       "parse; multi-statement is OUT in v1)");
        }
        return r;
    }

private:
    // --- token-stream helpers ---
    void advance() { cur_ = lex_.next(); }

    static std::string lower(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (const char c : s) {
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                                 : c);
        }
        return out;
    }

    [[nodiscard]] bool is_kw(const char* kw) const {
        return cur_.kind == Tok::Ident && lower(cur_.text) == kw;
    }

    ParseError make_err(std::string msg) const { return ParseError{std::move(msg), cur_.pos}; }
    ParseResult err(std::string msg) const { return ParseResult{make_err(std::move(msg))}; }

    // Expect a keyword (case-insensitive). Returns nullopt on success, an error
    // otherwise (so callers write `if (auto e = expect_kw(...)) return e;`).
    [[nodiscard]] std::optional<ParseError> expect_kw(const char* kw) {
        if (cur_.kind == Tok::Bad) {
            return make_err(cur_.bad_msg);
        }
        if (!is_kw(kw)) {
            return make_err(std::string("expected keyword '") + kw + "'");
        }
        advance();
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ParseError> expect(Tok k, const char* what) {
        if (cur_.kind == Tok::Bad) {
            return make_err(cur_.bad_msg);
        }
        if (cur_.kind != k) {
            return make_err(std::string("expected ") + what);
        }
        advance();
        return std::nullopt;
    }

    // Consume an identifier (NOT a keyword check — any identifier token), writing
    // its raw text to `out`. Returns nullopt on success, a positioned error
    // otherwise (the same out-param + optional<ParseError> shape as expect/expect_kw
    // — analyzer-friendly: no variant the path-analysis loses track of).
    [[nodiscard]] std::optional<ParseError> expect_ident(const char* what,
                                                         std::string& out) {
        if (cur_.kind == Tok::Bad) {
            return make_err(cur_.bad_msg);
        }
        if (cur_.kind != Tok::Ident) {
            return make_err(std::string("expected ") + what);
        }
        out = cur_.text;
        advance();
        return std::nullopt;
    }

    // v3: consume an optionally-QUALIFIED column reference: <ident> ['.' <ident>].
    //   `tbl.col`  => qualifier="tbl",  column="col"
    //   `col`      => qualifier="",     column="col"
    // The qualifier is resolved against the joined schema at plan time (NOT here — the
    // parser has no catalog). A trailing '.' with no column name is a clean error.
    [[nodiscard]] std::optional<ParseError> expect_qualified_column(
        const char* what, std::string& qualifier_out, std::string& column_out) {
        std::string first;
        if (auto e = expect_ident(what, first)) {
            return e;
        }
        if (cur_.kind == Tok::Dot) {
            advance();  // '.'
            std::string second;
            if (auto e = expect_ident("a column name after '.'", second)) {
                return e;
            }
            qualifier_out = std::move(first);
            column_out = std::move(second);
        } else {
            qualifier_out.clear();
            column_out = std::move(first);
        }
        return std::nullopt;
    }

    // Consume a literal (int or string) into `out`. Used by INSERT/UPDATE/WHERE.
    [[nodiscard]] std::optional<ParseError> expect_literal(Datum& out) {
        if (cur_.kind == Tok::Bad) {
            return make_err(cur_.bad_msg);
        }
        if (cur_.kind == Tok::IntLit) {
            out = Datum::make_int(cur_.int_val);
            advance();
            return std::nullopt;
        }
        if (cur_.kind == Tok::StrLit) {
            out = Datum::make_text(cur_.text);
            advance();
            return std::nullopt;
        }
        return make_err("expected a literal (integer or 'string')");
    }

    // v4: consume a literal OR the NULL keyword (INSERT VALUES / a comparison RHS).
    // A NULL literal carries `is_null=true` with a PLACEHOLDER type (Int); the Engine
    // re-types it to the target column's declared type when it coerces the value.
    [[nodiscard]] std::optional<ParseError> expect_value_or_null(Datum& out) {
        if (is_kw("null")) {
            advance();
            out = Datum::make_null(Type::Int);  // type fixed up at coerce time
            return std::nullopt;
        }
        return expect_literal(out);
    }

    // --- grammar rules ---

    // CREATE TABLE t (...) | CREATE INDEX name ON t (col)
    ParseResult parse_create() {
        advance();  // CREATE
        if (is_kw("index")) {
            return parse_create_index();
        }
        if (auto e = expect_kw("table")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::Create;
        if (auto e = expect_ident("a table name after CREATE TABLE", st.create.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' to open the column list")) {
            return ParseResult{*e};
        }

        bool seen_pk_clause = false;
        for (;;) {
            if (is_kw("primary")) {
                advance();
                if (auto e = expect_kw("key")) {
                    return ParseResult{*e};
                }
                if (auto e = expect(Tok::LParen, "'(' after PRIMARY KEY")) {
                    return ParseResult{*e};
                }
                if (auto e = expect_ident("the primary-key column name",
                                          st.create.pk_column)) {
                    return ParseResult{*e};
                }
                if (auto e = expect(Tok::RParen, "')' after PRIMARY KEY column")) {
                    return ParseResult{*e};
                }
                // v1: single-column PK only — a comma here would mean a multi-col PK.
                if (cur_.kind == Tok::Comma) {
                    return err("multi-column PRIMARY KEY is OUT in v1 (single-column "
                               "PK only)");
                }
                seen_pk_clause = true;
                break;  // PRIMARY KEY is the last clause
            }
            // A column definition: <name> <TYPE>
            Column col;
            if (auto e = expect_ident("a column name", col.name)) {
                return ParseResult{*e};
            }
            if (is_kw("int")) {
                col.type = Type::Int;
                advance();
            } else if (is_kw("text")) {
                col.type = Type::Text;
                advance();
            } else {
                return err("expected a column type (INT or TEXT) — other types are "
                           "OUT in v1");
            }
            // v4: optional NOT NULL constraint. A column is NULLABLE by default; a
            // NOT NULL column requires a present value at INSERT. (The PK column is
            // forced NOT NULL in the Engine regardless.) A bare `NULL` after the type is
            // accepted as the explicit "is nullable" spelling (default), for symmetry.
            col.nullable = true;
            if (is_kw("not")) {
                advance();
                if (auto e = expect_kw("null")) {
                    return ParseResult{*e};
                }
                col.nullable = false;
            } else if (is_kw("null")) {
                advance();  // explicit NULL == the default (nullable)
            }
            // F6: optional AUTO_INCREMENT (INT only) — an omitted value is assigned the table's
            // next monotonic id. May appear before or after NOT NULL/DEFAULT.
            if (is_kw("auto_increment")) {
                advance();
                if (col.type != Type::Int) {
                    return err("AUTO_INCREMENT requires an INT column ('" + col.name + "')");
                }
                col.auto_increment = true;
            }
            // F4: optional DEFAULT <literal> — used when an INSERT omits the column. The literal
            // must match the column type (checked at INSERT/coerce time).
            if (is_kw("default")) {
                advance();
                Datum dv;
                if (auto e = expect_literal(dv)) {
                    return ParseResult{*e};
                }
                col.has_default = true;
                if (dv.type == Type::Int) {
                    col.default_i = dv.i;
                } else {
                    col.default_s = dv.s;
                }
                // Remember the literal's type so coerce can validate it against the column.
                if (dv.type != col.type) {
                    return err("DEFAULT literal type does not match column '" + col.name + "'");
                }
            }
            st.create.columns.push_back(std::move(col));
            if (cur_.kind == Tok::Comma) {
                advance();
                continue;
            }
            break;
        }
        if (auto e = expect(Tok::RParen, "')' to close the column list")) {
            return ParseResult{*e};
        }
        if (!seen_pk_clause || st.create.pk_column.empty()) {
            return err("a PRIMARY KEY (<col>) clause is required in v1");
        }
        // Validate the PK names a declared column.
        bool found = false;
        for (const Column& c : st.create.columns) {
            if (c.name == st.create.pk_column) {
                found = true;
                break;
            }
        }
        if (!found) {
            return err("PRIMARY KEY column '" + st.create.pk_column +
                       "' is not a declared column");
        }
        return ParseResult{std::move(st)};
    }

    // CREATE INDEX <name> ON <table> (<col>) — single-column secondary index. A
    // multi-column list (a comma after the column) is a clean "OUT in v1" error.
    ParseResult parse_create_index() {
        advance();  // INDEX
        Statement st;
        st.kind = StmtKind::CreateIndex;
        if (auto e = expect_ident("an index name after CREATE INDEX",
                                  st.create_index.index)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("on")) {
            return ParseResult{*e};
        }
        if (auto e = expect_ident("a table name after ON", st.create_index.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' before the indexed column")) {
            return ParseResult{*e};
        }
        if (auto e = expect_ident("the indexed column name", st.create_index.column)) {
            return ParseResult{*e};
        }
        if (cur_.kind == Tok::Comma) {
            return err("multi-column INDEX is OUT in v1 (single-column secondary "
                       "index only)");
        }
        if (auto e = expect(Tok::RParen, "')' after the indexed column")) {
            return ParseResult{*e};
        }
        return ParseResult{std::move(st)};
    }

    // DROP INDEX <name> ON <table>  |  DROP TABLE <table>  (F8)
    ParseResult parse_drop_index() {
        advance();  // DROP
        if (is_kw("table")) {
            advance();  // TABLE
            Statement st;
            st.kind = StmtKind::DropTable;
            if (auto e = expect_ident("a table name after DROP TABLE", st.drop_table.table)) {
                return ParseResult{*e};
            }
            return ParseResult{std::move(st)};
        }
        if (auto e = expect_kw("index")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::DropIndex;
        if (auto e = expect_ident("an index name after DROP INDEX",
                                  st.drop_index.index)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("on")) {
            return ParseResult{*e};
        }
        if (auto e = expect_ident("a table name after ON", st.drop_index.table)) {
            return ParseResult{*e};
        }
        return ParseResult{std::move(st)};
    }

    // INSERT INTO t (c, ...) VALUES (v, ...)
    ParseResult parse_insert() {
        advance();  // INSERT
        if (auto e = expect_kw("into")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::Insert;
        if (auto e = expect_ident("a table name after INSERT INTO", st.insert.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' to open the column list")) {
            return ParseResult{*e};
        }
        for (;;) {
            std::string c;
            if (auto e = expect_ident("a column name", c)) {
                return ParseResult{*e};
            }
            st.insert.columns.push_back(std::move(c));
            if (cur_.kind == Tok::Comma) {
                advance();
                continue;
            }
            break;
        }
        if (auto e = expect(Tok::RParen, "')' to close the column list")) {
            return ParseResult{*e};
        }
        // D5: INSERT INTO t (cols) SELECT ... — the rows come from a query instead of VALUES.
        if (is_kw("select")) {
            auto sub = std::make_shared<SelectStmt>();
            if (auto e = parse_select_stmt(*sub)) {
                return ParseResult{*e};
            }
            st.insert.select_source = std::move(sub);
            if (auto e = parse_on_conflict(st.insert)) {
                return ParseResult{*e};
            }
            return ParseResult{std::move(st)};
        }
        if (auto e = expect_kw("values")) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' to open VALUES")) {
            return ParseResult{*e};
        }
        for (;;) {
            Datum v;
            if (auto e = expect_value_or_null(v)) {  // v4: NULL literal allowed
                return ParseResult{*e};
            }
            st.insert.values.push_back(std::move(v));
            if (cur_.kind == Tok::Comma) {
                advance();
                continue;
            }
            break;
        }
        if (auto e = expect(Tok::RParen, "')' to close VALUES")) {
            return ParseResult{*e};
        }
        if (st.insert.columns.size() != st.insert.values.size()) {
            return err("INSERT column count (" +
                       std::to_string(st.insert.columns.size()) +
                       ") does not match value count (" +
                       std::to_string(st.insert.values.size()) + ")");
        }
        // D6: MULTI-ROW INSERT — `VALUES (..),(..),...`. Each extra tuple after the first is
        // parsed into more_rows; every tuple must match the column count (parser-checked here so
        // the engine sees a clean, uniform batch).
        while (cur_.kind == Tok::Comma) {
            advance();  // ','
            if (auto e = expect(Tok::LParen, "'(' to open another VALUES tuple")) {
                return ParseResult{*e};
            }
            std::vector<Datum> rowv;
            for (;;) {
                Datum v;
                if (auto e = expect_value_or_null(v)) {
                    return ParseResult{*e};
                }
                rowv.push_back(std::move(v));
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
            if (auto e = expect(Tok::RParen, "')' to close the VALUES tuple")) {
                return ParseResult{*e};
            }
            if (rowv.size() != st.insert.columns.size()) {
                return err("INSERT VALUES tuple has " + std::to_string(rowv.size()) +
                           " values but " + std::to_string(st.insert.columns.size()) +
                           " columns were named");
            }
            st.insert.more_rows.push_back(std::move(rowv));
        }
        if (auto e = parse_on_conflict(st.insert)) {
            return ParseResult{*e};
        }
        return ParseResult{std::move(st)};
    }

    // G2: ON CONFLICT DO NOTHING | DO UPDATE SET <col> = <literal> [, ...]. No-op if absent.
    [[nodiscard]] std::optional<ParseError> parse_on_conflict(InsertStmt& ins) {
        if (!is_kw("on")) {
            return std::nullopt;
        }
        advance();
        if (auto e = expect_kw("conflict")) return e;
        if (auto e = expect_kw("do")) return e;
        if (is_kw("nothing")) {
            advance();
            ins.on_conflict = InsertStmt::OnConflict::Nothing;
            return std::nullopt;
        }
        if (is_kw("update")) {
            advance();
            if (auto e = expect_kw("set")) return e;
            ins.on_conflict = InsertStmt::OnConflict::Update;
            for (;;) {
                std::string col;
                if (auto e = expect_ident("a column after SET", col)) return e;
                if (auto e = expect(Tok::Eq, "'=' in ON CONFLICT DO UPDATE SET")) return e;
                Datum v;
                if (auto e = expect_value_or_null(v)) return e;
                ins.conflict_updates.emplace_back(col, v);
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
            return std::nullopt;
        }
        return make_err("expected NOTHING or UPDATE after ON CONFLICT DO");
    }

    // UPDATE t SET c = v WHERE pk = v
    ParseResult parse_update() {
        advance();  // UPDATE
        Statement st;
        st.kind = StmtKind::Update;
        if (auto e = expect_ident("a table name after UPDATE", st.update.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("set")) {
            return ParseResult{*e};
        }
        if (auto e = expect_ident("a column name after SET", st.update.set_column)) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::Eq, "'=' in SET")) {
            return ParseResult{*e};
        }
        if (auto e = expect_value_or_null(st.update.set_value)) {  // v4: SET col = NULL
            return ParseResult{*e};
        }
        // WHERE pk = v
        if (auto e = parse_pk_eq_where(st.update.where_column, st.update.where_value)) {
            return ParseResult{*e};
        }
        return ParseResult{std::move(st)};
    }

    // DELETE FROM t WHERE pk = v
    ParseResult parse_delete() {
        advance();  // DELETE
        if (auto e = expect_kw("from")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::Delete;
        if (auto e = expect_ident("a table name after DELETE FROM", st.del.table)) {
            return ParseResult{*e};
        }
        if (auto e = parse_pk_eq_where(st.del.where_column, st.del.where_value)) {
            return ParseResult{*e};
        }
        return ParseResult{std::move(st)};
    }

    // A shared `WHERE <col> = <literal>` rule for UPDATE/DELETE.
    [[nodiscard]] std::optional<ParseError> parse_pk_eq_where(std::string& col_out,
                                                              Datum& val_out) {
        if (auto e = expect_kw("where")) {
            return e;
        }
        if (auto e = expect_ident("a column name after WHERE", col_out)) {
            return e;
        }
        if (auto e = expect(Tok::Eq, "'=' in WHERE")) {
            return e;
        }
        if (auto e = expect_literal(val_out)) {
            return e;
        }
        return std::nullopt;
    }

    // A keyword that can NEVER start an identifier in a SELECT-list/expression
    // context (so the parser stops projection/predicate parsing at a clause boundary
    // instead of swallowing the keyword as a column name).
    [[nodiscard]] bool at_clause_boundary() const {
        return is_kw("from") || is_kw("where") || is_kw("group") || is_kw("having") ||
               is_kw("order") || is_kw("limit") || is_kw("offset") || is_kw("at") ||
               is_kw("and") || is_kw("or") || is_kw("asc") || is_kw("desc") ||
               is_kw("by") || is_kw("join") || is_kw("inner") || is_kw("left") ||
               is_kw("right") || is_kw("outer") || is_kw("cross") || is_kw("on") ||
               is_kw("as") || is_kw("is") || is_kw("in") || is_kw("exists") ||
               is_kw("null") || is_kw("not") || is_kw("between") ||
               is_kw("full") || is_kw("nulls") || is_kw("like") || is_kw("union") ||
               is_kw("intersect") || is_kw("except");
    }

    // Try to parse an AGGREGATE call at the cursor: NAME '(' ('*' | col) ')'.
    // Returns: filled `agg` + advances on success; nullopt-with-false when the
    // cursor is not an aggregate (caller falls back to a plain column).
    [[nodiscard]] std::optional<ParseError> parse_agg(AggExpr& agg, bool& matched,
                                                      std::string& label) {
        matched = false;
        if (cur_.kind != Tok::Ident) {
            return std::nullopt;
        }
        const std::string fn = lower(cur_.text);
        AggKind kind{};
        if (fn == "count") {
            kind = AggKind::Count;
        } else if (fn == "sum") {
            kind = AggKind::Sum;
        } else if (fn == "min") {
            kind = AggKind::Min;
        } else if (fn == "max") {
            kind = AggKind::Max;
        } else if (fn == "avg") {
            kind = AggKind::Avg;
        } else {
            return std::nullopt;  // not an aggregate name
        }
        // An aggregate name must be immediately followed by '(' to BE an aggregate;
        // otherwise it is a (legal) column named e.g. "sum". Peek: a name not
        // followed by '(' is treated as a column by the caller, so only commit when
        // the next token is '('.
        const Token saved = cur_;
        advance();
        if (cur_.kind != Tok::LParen) {
            // Not an aggregate call — rewind is impossible (single-lookahead), so we
            // report it as a column via the matched=false + an out-param the caller
            // reads. Re-synthesize: the caller expects us NOT to have consumed. We
            // therefore require '(' here and, if absent, surface a positioned error
            // ONLY when this clearly looked like an aggregate. To keep the grammar
            // unambiguous we DISALLOW a bare column named like an aggregate fn.
            return make_err("aggregate function '" + saved.text +
                            "' must be followed by '(' (a column may not be named "
                            "COUNT/SUM/MIN/MAX/AVG in v2)");
        }
        advance();  // '('
        // C1: optional DISTINCT inside the aggregate — `COUNT(DISTINCT col)`, SUM/AVG(DISTINCT ..).
        // Not valid with `*` (COUNT(DISTINCT *) is rejected below). Dedup happens at eval time.
        bool distinct = false;
        if (is_kw("distinct")) {
            advance();
            distinct = true;
        }
        if (kind == AggKind::Count && cur_.kind == Tok::Star) {
            if (distinct) {
                return make_err("COUNT(DISTINCT *) is not valid — DISTINCT needs a column");
            }
            advance();
            agg.kind = AggKind::CountStar;
            label = "COUNT(*)";
        } else {
            std::string qual;
            std::string col;
            if (auto e = expect_qualified_column("a column name inside the aggregate",
                                                 qual, col)) {
                return e;
            }
            if (distinct && kind != AggKind::Count && kind != AggKind::Sum &&
                kind != AggKind::Avg) {
                return make_err("DISTINCT is only supported for COUNT/SUM/AVG");
            }
            agg.kind = kind;
            agg.qualifier = qual;
            agg.column = col;
            agg.distinct = distinct;
            const std::string body = (qual.empty() ? col : qual + "." + col);
            label = upper(fn) + "(" + (distinct ? "DISTINCT " : "") + body + ")";
        }
        if (auto e = expect(Tok::RParen, "')' to close the aggregate")) {
            return e;
        }
        matched = true;
        return std::nullopt;
    }

    static std::string upper(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (const char c : s) {
            out.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A')
                                                 : c);
        }
        return out;
    }

    // SELECT [DISTINCT] (* | item, ...) FROM t [WHERE p] [GROUP BY ..] [HAVING ..]
    //                                          [ORDER BY ..] [LIMIT n [OFFSET m]] [AT ..]
    ParseResult parse_select() {
        Statement st;
        st.kind = StmtKind::Select;
        if (auto e = parse_select_stmt(st.select)) {
            return ParseResult{*e};
        }
        // D1/D2: trailing set operations — `... UNION [ALL] SELECT ...` (also INTERSECT/EXCEPT),
        // right-linked through set_op_rhs. A trailing ORDER BY/LIMIT lands on the LAST arm and the
        // executor applies it to the whole combined result.
        SelectStmt* tail = &st.select;
        for (;;) {
            SetOp op = SetOp::None;
            if (is_kw("union")) {
                op = SetOp::Union;
            } else if (is_kw("intersect")) {
                op = SetOp::Intersect;
            } else if (is_kw("except")) {
                op = SetOp::Except;
            } else {
                break;
            }
            advance();
            bool all = false;
            if (is_kw("all")) {
                all = true;
                advance();
            }
            if (!is_kw("select")) {
                return make_err("expected SELECT after a set operator (UNION/INTERSECT/EXCEPT)");
            }
            auto rhs = std::make_shared<SelectStmt>();
            if (auto e = parse_select_stmt(*rhs)) {
                return ParseResult{*e};
            }
            tail->set_op = op;
            tail->set_op_all = all;
            tail->set_op_rhs = rhs;
            tail = rhs.get();
        }
        return ParseResult{std::move(st)};
    }

    // v4: parse a SELECT body into `sel`. Factored out of parse_select() so a SUBQUERY
    // (IN/EXISTS/scalar) can reuse the SAME SELECT grammar + AST shape (the subquery is
    // a fully-formed SelectStmt the Engine runs through its normal SELECT pipeline). The
    // cursor must be on the SELECT keyword; on return it sits AFTER the SELECT body (the
    // caller consumes any trailing ')' or ';').
    [[nodiscard]] std::optional<ParseError> parse_select_stmt(SelectStmt& sel) {
        advance();  // SELECT

        if (is_kw("distinct")) {
            advance();
            sel.distinct = true;
        }

        if (cur_.kind == Tok::Star) {
            sel.star = true;
            advance();
        } else {
            for (;;) {
                // An aggregate item? (NAME '(' ...)
                AggExpr agg;
                bool is_agg = false;
                std::string label;
                if (auto e = parse_agg(agg, is_agg, label)) {
                    return e;
                }
                if (is_agg) {
                    SelectItem item;
                    item.kind = SelectItemKind::Aggregate;
                    item.agg = agg;
                    item.label = label;
                    sel.items.push_back(std::move(item));
                    sel.has_aggregates = true;
                } else {
                    std::string qual;
                    std::string c;
                    if (auto e = expect_qualified_column(
                            "a column name, aggregate, or '*'", qual, c)) {
                        return e;
                    }
                    SelectItem item;
                    item.kind = SelectItemKind::Column;
                    item.qualifier = qual;
                    item.column = c;
                    // The output label is the qualified spelling for a qualified ref so
                    // self-joins distinguish a.x from b.x; bare for an unqualified one.
                    item.label = qual.empty() ? c : qual + "." + c;
                    sel.items.push_back(std::move(item));
                    sel.columns.push_back(c);  // mirror for the v1 single-table path
                }
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }
        if (auto e = expect_kw("from")) {
            return e;
        }
        // v3: the FROM/JOIN list (left-deep). Fills sel.from; sel.table mirrors the
        // base entry's table for the v1/v2 single-table path.
        if (auto e = parse_from(sel)) {
            return e;
        }

        // Optional WHERE — v2 general predicate. We ALSO recognize a bare PK
        // equality / BETWEEN and record the v1 fast-path fields (the planner uses
        // them when they are an exact PK match).
        if (is_kw("where")) {
            advance();
            if (auto e = parse_predicate(sel.filter, /*allow_agg=*/false)) {
                return e;
            }
            // The PK fast path is a SINGLE-TABLE optimization. With a JOIN (or an
            // alias != table), WHERE runs over the joined row, so never lower it to a
            // base-table point/range read.
            if (!sel.is_join() && sel.from.size() == 1 &&
                sel.from[0].alias == sel.from[0].table) {
                extract_pk_fastpath(sel);
            }
        }

        // Optional GROUP BY <cols>.
        if (is_kw("group")) {
            advance();
            if (auto e = expect_kw("by")) {
                return e;
            }
            for (;;) {
                // v3: a GROUP BY column may be qualified (table.col); we store the
                // qualified SPELLING ("a.x" or "x") and the engine resolves it.
                std::string qual;
                std::string col;
                if (auto e = expect_qualified_column("a column name in GROUP BY", qual,
                                                     col)) {
                    return e;
                }
                sel.group_by.push_back(qual.empty() ? col : qual + "." + col);
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }

        // Optional HAVING <agg-predicate>.
        if (is_kw("having")) {
            advance();
            if (auto e = parse_predicate(sel.having, /*allow_agg=*/true)) {
                return e;
            }
        }

        // Optional ORDER BY <col> [ASC|DESC], ...
        if (is_kw("order")) {
            advance();
            if (auto e = expect_kw("by")) {
                return e;
            }
            for (;;) {
                OrderKey key;
                if (auto e = expect_qualified_column("a column name in ORDER BY",
                                                     key.qualifier, key.column)) {
                    return e;
                }
                if (is_kw("asc")) {
                    advance();
                } else if (is_kw("desc")) {
                    key.descending = true;
                    advance();
                }
                // G3: optional NULLS FIRST | NULLS LAST after the ASC/DESC direction.
                if (is_kw("nulls")) {
                    advance();
                    if (is_kw("first")) {
                        key.nulls = NullsOrder::First;
                        advance();
                    } else if (is_kw("last")) {
                        key.nulls = NullsOrder::Last;
                        advance();
                    } else {
                        return make_err("expected FIRST or LAST after NULLS in ORDER BY");
                    }
                }
                sel.order_by.push_back(std::move(key));
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }

        // Optional LIMIT n [OFFSET m].
        if (is_kw("limit")) {
            advance();
            Datum d;
            if (auto e = expect_literal(d)) {
                return e;
            }
            if (d.type != Type::Int || d.i < 0) {
                return make_err("LIMIT requires a non-negative integer");
            }
            sel.has_limit = true;
            sel.limit = d.i;
            if (is_kw("offset")) {
                advance();
                Datum o;
                if (auto e = expect_literal(o)) {
                    return e;
                }
                if (o.type != Type::Int || o.i < 0) {
                    return make_err("OFFSET requires a non-negative integer");
                }
                sel.offset = o.i;
            }
        }

        // Optional AT <level> — the CALL-SITE-VISIBLE D5 annotation (V-D5-SAFE).
        if (is_kw("at")) {
            advance();
            if (auto e = parse_at_level(sel)) {
                return e;
            }
        }
        return std::nullopt;
    }

    // Map the current comparison token to a CmpOp; returns false if not a cmp op.
    [[nodiscard]] bool cmp_op(CmpOp& out) const {
        switch (cur_.kind) {
            case Tok::Eq: out = CmpOp::Eq; return true;
            case Tok::Ne: out = CmpOp::Ne; return true;
            case Tok::Lt: out = CmpOp::Lt; return true;
            case Tok::Le: out = CmpOp::Le; return true;
            case Tok::Gt: out = CmpOp::Gt; return true;
            case Tok::Ge: out = CmpOp::Ge; return true;
            default: return false;
        }
    }

    // Parse a boolean predicate into `pred` (a node-pool tree). `allow_agg` enables
    // aggregate operands (HAVING); when false an aggregate leaf is a clean error.
    [[nodiscard]] std::optional<ParseError> parse_predicate(Predicate& pred,
                                                            bool allow_agg) {
        std::int32_t root = -1;
        if (auto e = parse_or(pred, allow_agg, root)) {
            return e;
        }
        pred.root = root;
        return std::nullopt;
    }

    // or-expr ::= and-expr (OR and-expr)*
    [[nodiscard]] std::optional<ParseError> parse_or(Predicate& p, bool allow_agg,
                                                     std::int32_t& out) {
        if (auto e = parse_and(p, allow_agg, out)) {
            return e;
        }
        while (is_kw("or")) {
            advance();
            std::int32_t rhs = -1;
            if (auto e = parse_and(p, allow_agg, rhs)) {
                return e;
            }
            PredNode n;
            n.kind = PredNodeKind::Or;
            n.left = out;
            n.right = rhs;
            p.nodes.push_back(std::move(n));
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
        }
        return std::nullopt;
    }

    // and-expr ::= not-expr (AND not-expr)*
    [[nodiscard]] std::optional<ParseError> parse_and(Predicate& p, bool allow_agg,
                                                      std::int32_t& out) {
        if (auto e = parse_not(p, allow_agg, out)) {
            return e;
        }
        while (is_kw("and")) {
            advance();
            std::int32_t rhs = -1;
            if (auto e = parse_not(p, allow_agg, rhs)) {
                return e;
            }
            PredNode n;
            n.kind = PredNodeKind::And;
            n.left = out;
            n.right = rhs;
            p.nodes.push_back(std::move(n));
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
        }
        return std::nullopt;
    }

    // not-expr ::= NOT not-expr | primary
    [[nodiscard]] std::optional<ParseError> parse_not(Predicate& p, bool allow_agg,
                                                      std::int32_t& out) {
        if (is_kw("not")) {
            advance();
            std::int32_t child = -1;
            if (auto e = parse_not(p, allow_agg, child)) {
                return e;
            }
            PredNode n;
            n.kind = PredNodeKind::Not;
            n.left = child;
            p.nodes.push_back(std::move(n));
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
            return std::nullopt;
        }
        return parse_primary(p, allow_agg, out);
    }

    // v4: parse `( SELECT ... )` into a shared SelectStmt (the nested subquery). The
    // cursor must be on '('; on success it sits AFTER the matching ')'. The inner SELECT
    // reuses the full SELECT grammar (so a subquery may itself have WHERE/JOIN/aggregates
    // — UNCORRELATED only: it does not see the outer row; see the Engine's FLAG).
    [[nodiscard]] std::optional<ParseError> parse_subquery(
        std::shared_ptr<SelectStmt>& out) {
        if (auto e = expect(Tok::LParen, "'(' to open a subquery")) {
            return e;
        }
        if (!is_kw("select")) {
            return make_err("expected SELECT to open a subquery");
        }
        auto sel = std::make_shared<SelectStmt>();
        if (auto e = parse_select_stmt(*sel)) {
            return e;
        }
        if (auto e = expect(Tok::RParen, "')' to close the subquery")) {
            return e;
        }
        out = std::move(sel);
        return std::nullopt;
    }

    // primary ::= '(' or-expr ')' | <operand> <cmpop> <literal>
    //           | <column> BETWEEN a AND b   (sugar for col>=a AND col<=b)
    [[nodiscard]] std::optional<ParseError> parse_primary(Predicate& p, bool allow_agg,
                                                          std::int32_t& out) {
        // v4: EXISTS ( SELECT ... ) — a subquery existence test. (NOT EXISTS arrives via
        // the prefix NOT rule in parse_not, wrapping this Exists node.) Subqueries are
        // OUT in HAVING (allow_agg) — a clean error, fail-closed.
        if (is_kw("exists")) {
            if (allow_agg) {
                return make_err("EXISTS subqueries are not supported in HAVING");
            }
            advance();
            std::shared_ptr<SelectStmt> sub;
            if (auto e = parse_subquery(sub)) {
                return e;
            }
            PredNode n;
            n.kind = PredNodeKind::Exists;
            n.is_not = false;
            n.subquery = std::move(sub);
            p.nodes.push_back(std::move(n));
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
            return std::nullopt;
        }

        if (cur_.kind == Tok::LParen) {
            advance();
            if (auto e = parse_or(p, allow_agg, out)) {
                return e;
            }
            return expect(Tok::RParen, "')' to close a parenthesized predicate");
        }

        // The left operand: an aggregate (HAVING) or a column.
        PredNode leaf;
        leaf.kind = PredNodeKind::Cmp;
        AggExpr agg;
        bool is_agg = false;
        std::string label;
        if (allow_agg) {
            if (auto e = parse_agg(agg, is_agg, label)) {
                return e;
            }
        }
        if (is_agg) {
            leaf.operand = OperandKind::Agg;
            leaf.agg = agg;
        } else {
            std::string qual;
            std::string col;
            if (auto e = expect_qualified_column("a column name in the predicate", qual,
                                                 col)) {
                return e;
            }
            leaf.operand = OperandKind::Column;
            leaf.qualifier = qual;
            leaf.column = col;
        }

        // v4: <column> IS [NOT] NULL — a three-valued NULL test (the ONLY predicate that
        // can ever be TRUE for a NULL operand). Not valid for an aggregate operand.
        if (!is_agg && is_kw("is")) {
            advance();
            bool is_not = false;
            if (is_kw("not")) {
                advance();
                is_not = true;
            }
            if (auto e = expect_kw("null")) {
                return e;
            }
            PredNode n;
            n.kind = PredNodeKind::IsNull;
            n.operand = OperandKind::Column;
            n.qualifier = leaf.qualifier;
            n.column = leaf.column;
            n.is_not = is_not;
            p.nodes.push_back(std::move(n));
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
            return std::nullopt;
        }

        // v4: <column> [NOT] IN ( SELECT col FROM ... ) — subquery membership. NULL
        // semantics (a NULL in the subquery makes NOT IN UNKNOWN) are enforced by the
        // Engine. (A literal IN-list `IN (1,2,3)` is OUT — only the subquery form here.)
        {
            bool neg = false;
            if (is_kw("not")) {
                // `NOT IN` / `NOT LIKE` — consume NOT only if IN or LIKE follows; otherwise this
                // NOT belongs to a higher rule (shouldn't happen mid-primary; surface a clean error).
                advance();
                if (!is_kw("in") && !is_kw("like")) {
                    return make_err("expected IN or LIKE after NOT in a predicate");
                }
                neg = true;
            }
            if (is_kw("in")) {
                if (is_agg) {
                    return make_err("IN subqueries are not supported for an aggregate "
                                    "operand");
                }
                advance();
                std::shared_ptr<SelectStmt> sub;
                if (auto e = parse_subquery(sub)) {
                    return e;
                }
                PredNode n;
                n.kind = PredNodeKind::InList;
                n.operand = OperandKind::Column;
                n.qualifier = leaf.qualifier;
                n.column = leaf.column;
                n.is_not = neg;
                n.subquery = std::move(sub);
                p.nodes.push_back(std::move(n));
                out = static_cast<std::int32_t>(p.nodes.size() - 1);
                return std::nullopt;
            }
            // B1: <column> [NOT] LIKE '<pattern>' — `%` matches any run (incl. empty), `_` one char.
            // The pattern must be a (string) literal. NOT LIKE wraps the LIKE leaf in a Not node so it
            // reuses the existing negation eval.
            if (is_kw("like")) {
                if (is_agg) {
                    return make_err("LIKE is not supported for an aggregate operand");
                }
                advance();
                Datum pat;
                if (auto e = expect_literal(pat)) {
                    return e;
                }
                PredNode n = leaf;  // the column operand
                n.kind = PredNodeKind::Cmp;
                n.op = CmpOp::Like;
                n.literal = pat;
                p.nodes.push_back(std::move(n));
                std::int32_t li = static_cast<std::int32_t>(p.nodes.size() - 1);
                if (neg) {
                    PredNode notn;
                    notn.kind = PredNodeKind::Not;
                    notn.left = li;
                    p.nodes.push_back(std::move(notn));
                    li = static_cast<std::int32_t>(p.nodes.size() - 1);
                }
                out = li;
                return std::nullopt;
            }
            if (neg) {
                return make_err("expected IN or LIKE after NOT");  // (unreachable: errored above)
            }
        }

        // BETWEEN sugar (only for a column operand): col BETWEEN a AND b.
        if (!is_agg && is_kw("between")) {
            advance();
            Datum lo;
            if (auto e = expect_literal(lo)) {
                return e;
            }
            if (auto e = expect_kw("and")) {
                return e;
            }
            Datum hi;
            if (auto e = expect_literal(hi)) {
                return e;
            }
            // Lower to (col >= lo) AND (col <= hi).
            PredNode ge = leaf;
            ge.op = CmpOp::Ge;
            ge.literal = lo;
            p.nodes.push_back(ge);
            const std::int32_t gi = static_cast<std::int32_t>(p.nodes.size() - 1);
            PredNode le = leaf;
            le.op = CmpOp::Le;
            le.literal = hi;
            p.nodes.push_back(le);
            const std::int32_t li = static_cast<std::int32_t>(p.nodes.size() - 1);
            PredNode conj;
            conj.kind = PredNodeKind::And;
            conj.left = gi;
            conj.right = li;
            p.nodes.push_back(conj);
            out = static_cast<std::int32_t>(p.nodes.size() - 1);
            return std::nullopt;
        }

        CmpOp op{};
        if (!cmp_op(op)) {
            return make_err("expected a comparison operator (=, !=, <, <=, >, >=) or "
                            "BETWEEN");
        }
        advance();
        leaf.op = op;
        // The RHS is one of:
        //   * a SCALAR SUBQUERY `(SELECT agg/single-col FROM ...)` (v4) — one row/one
        //     col at run time, else an error; 0 rows => NULL => the comparison is UNKNOWN.
        //   * the NULL literal (v4) — the comparison is then always UNKNOWN (false).
        //   * ANOTHER column (col-vs-col theta, e.g. an equi-join key a.x = b.y).
        //   * a plain LITERAL (the v1/v2 case).
        // An aggregate operand (HAVING) only takes a literal RHS.
        if (!is_agg && cur_.kind == Tok::LParen) {
            std::shared_ptr<SelectStmt> sub;
            if (auto e = parse_subquery(sub)) {
                return e;
            }
            leaf.rhs_is_subquery = true;
            leaf.subquery = std::move(sub);
        } else if (!is_agg && cur_.kind == Tok::Ident && !at_clause_boundary()) {
            std::string rq;
            std::string rc;
            if (auto e = expect_qualified_column("a column name on the right of the "
                                                 "comparison",
                                                 rq, rc)) {
                return e;
            }
            leaf.rhs_is_column = true;
            leaf.rhs_qualifier = rq;
            leaf.rhs_column = rc;
        } else {
            if (auto e = expect_value_or_null(leaf.literal)) {  // v4: NULL allowed
                return e;
            }
        }
        p.nodes.push_back(std::move(leaf));
        out = static_cast<std::int32_t>(p.nodes.size() - 1);
        return std::nullopt;
    }

    // Recognize a WHERE that is EXACTLY a PK equality or PK BETWEEN over the order-
    // preserving key, so the planner can keep the v1 point/range fast path. The PK
    // column name is not known to the parser (no catalog), so we record a CANDIDATE
    // here and the Engine validates it is the real PK before using the fast path.
    //   root is a single Cmp(col = v)               => Eq candidate
    //   root is And(Cmp(col>=lo), Cmp(col<=hi))      => Between candidate
    // Anything else leaves where == None (the general scan+filter path runs).
    static void extract_pk_fastpath(SelectStmt& sel) {
        const Predicate& p = sel.filter;
        if (!p.present()) {
            return;
        }
        const PredNode& r = p.nodes[static_cast<std::size_t>(p.root)];
        // The fast path only fires for an UNQUALIFIED column vs a LITERAL (a qualified
        // ref or a col-vs-col theta is never a PK point/range candidate).
        // v4: never lower a NULL-literal / subquery comparison to the PK point/range
        // path (a NULL key is meaningless; `pk = NULL` is UNKNOWN => no rows, handled by
        // the general filter). Require a present literal RHS.
        if (r.kind == PredNodeKind::Cmp && r.operand == OperandKind::Column &&
            r.qualifier.empty() && !r.rhs_is_column && !r.rhs_is_subquery &&
            !r.literal.is_null && r.op == CmpOp::Eq) {
            sel.where = SelectWhereKind::Eq;
            sel.where_column = r.column;
            sel.eq_value = r.literal;
            return;
        }
        if (r.kind == PredNodeKind::And) {
            const PredNode& l = p.nodes[static_cast<std::size_t>(r.left)];
            const PredNode& rr = p.nodes[static_cast<std::size_t>(r.right)];
            if (l.kind == PredNodeKind::Cmp && rr.kind == PredNodeKind::Cmp &&
                l.operand == OperandKind::Column && rr.operand == OperandKind::Column &&
                l.qualifier.empty() && rr.qualifier.empty() && !l.rhs_is_column &&
                !rr.rhs_is_column && !l.rhs_is_subquery && !rr.rhs_is_subquery &&
                !l.literal.is_null && !rr.literal.is_null &&
                l.column == rr.column && l.op == CmpOp::Ge && rr.op == CmpOp::Le) {
                sel.where = SelectWhereKind::Between;
                sel.where_column = l.column;
                sel.lo_value = l.literal;
                sel.hi_value = rr.literal;
            }
        }
    }

    // v3: parse ONE table reference: <table> [AS <alias> | <alias>]. The alias is the
    // binding name in the joined schema (defaults to the table name). A keyword may
    // NOT be used as a bare alias (it would swallow JOIN/WHERE/...); an explicit AS
    // <alias> still requires an identifier.
    [[nodiscard]] std::optional<ParseError> parse_table_ref(JoinEntry& e) {
        if (auto er = expect_ident("a table name", e.table)) {
            return er;
        }
        if (is_kw("as")) {
            advance();
            if (auto er = expect_ident("an alias after AS", e.alias)) {
                return er;
            }
        } else if (cur_.kind == Tok::Ident && !at_clause_boundary()) {
            // A bare alias: an identifier that is NOT a clause keyword.
            if (auto er = expect_ident("an alias", e.alias)) {
                return er;
            }
        } else {
            e.alias = e.table;  // no alias => bind by the table name
        }
        return std::nullopt;
    }

    // v3: FROM <ref> ( ',' <ref> | [INNER] JOIN <ref> ON <pred> | LEFT [OUTER] JOIN
    //                  <ref> ON <pred> | CROSS JOIN <ref> )*
    // Builds the left-deep sel.from list. Each non-base entry carries its JoinKind +
    // (for INNER/LEFT) an ON predicate. A comma or CROSS JOIN is a cartesian (no ON).
    // ON without a predicate, or a JOIN with no ON for INNER/LEFT, is a clean error.
    [[nodiscard]] std::optional<ParseError> parse_from(SelectStmt& sel) {
        JoinEntry base;
        if (auto e = parse_table_ref(base)) {
            return e;
        }
        sel.from.push_back(std::move(base));
        sel.table = sel.from[0].table;  // mirror for the v1/v2 single-table path

        for (;;) {
            if (cur_.kind == Tok::Comma) {
                advance();
                JoinEntry je;
                je.kind = JoinKind::Cross;  // comma == cross join
                if (auto e = parse_table_ref(je)) {
                    return e;
                }
                sel.from.push_back(std::move(je));
                continue;
            }
            // An explicit JOIN keyword sequence.
            JoinKind kind = JoinKind::Inner;
            bool is_join_clause = false;
            if (is_kw("cross")) {
                advance();
                if (auto e = expect_kw("join")) {
                    return e;
                }
                kind = JoinKind::Cross;
                is_join_clause = true;
            } else if (is_kw("inner")) {
                advance();
                if (auto e = expect_kw("join")) {
                    return e;
                }
                kind = JoinKind::Inner;
                is_join_clause = true;
            } else if (is_kw("left")) {
                advance();
                if (is_kw("outer")) {
                    advance();  // LEFT OUTER == LEFT
                }
                if (auto e = expect_kw("join")) {
                    return e;
                }
                kind = JoinKind::Left;
                is_join_clause = true;
            } else if (is_kw("right")) {
                advance();
                if (is_kw("outer")) {
                    advance();  // RIGHT OUTER == RIGHT
                }
                if (auto e = expect_kw("join")) {
                    return e;
                }
                kind = JoinKind::Right;
                is_join_clause = true;
            } else if (is_kw("full")) {
                advance();
                if (is_kw("outer")) {
                    advance();  // FULL OUTER == FULL
                }
                if (auto e = expect_kw("join")) {
                    return e;
                }
                kind = JoinKind::Full;
                is_join_clause = true;
            } else if (is_kw("join")) {
                advance();  // bare JOIN == INNER JOIN
                kind = JoinKind::Inner;
                is_join_clause = true;
            }
            if (!is_join_clause) {
                break;  // no more FROM/JOIN entries
            }
            JoinEntry je;
            je.kind = kind;
            if (auto e = parse_table_ref(je)) {
                return e;
            }
            if (kind == JoinKind::Cross) {
                // A CROSS JOIN takes no ON (a dangling ON is rejected as trailing).
                if (is_kw("on")) {
                    return make_err("CROSS JOIN does not take an ON predicate");
                }
            } else {
                // INNER / LEFT require ON <predicate>.
                if (!is_kw("on")) {
                    return make_err(
                        "expected ON <predicate> after " +
                        std::string(kind == JoinKind::Left ? "LEFT" : "INNER") +
                        " JOIN");
                }
                advance();  // ON
                if (auto e = parse_predicate(je.on, /*allow_agg=*/false)) {
                    return e;
                }
            }
            sel.from.push_back(std::move(je));
        }
        return std::nullopt;
    }

    // AT STRICT | AT SNAPSHOT n | AT BOUNDED n | AT RYW n
    [[nodiscard]] std::optional<ParseError> parse_at_level(SelectStmt& sel) {
        if (is_kw("strict")) {
            advance();
            sel.level = Level::StrictSerializable;
            return std::nullopt;
        }
        if (is_kw("snapshot")) {
            advance();
            Datum d;
            if (auto e = expect_literal(d)) {
                return e;
            }
            if (d.type != Type::Int || d.i < 0) {
                return make_err("AT SNAPSHOT requires a non-negative integer version");
            }
            sel.level = Level::Snapshot;
            sel.snapshot_version = static_cast<Seq>(d.i);
            return std::nullopt;
        }
        if (is_kw("bounded")) {
            advance();
            Datum d;
            if (auto e = expect_literal(d)) {
                return e;
            }
            if (d.type != Type::Int || d.i < 0) {
                return make_err("AT BOUNDED requires a non-negative integer max_lag");
            }
            sel.level = Level::BoundedStaleness;
            sel.max_lag = static_cast<Seq>(d.i);
            return std::nullopt;
        }
        if (is_kw("ryw")) {
            advance();
            Datum d;
            if (auto e = expect_literal(d)) {
                return e;
            }
            if (d.type != Type::Int || d.i < 0) {
                return make_err("AT RYW requires a non-negative integer session id");
            }
            sel.level = Level::ReadYourWrites;
            sel.session = static_cast<SessionId>(d.i);
            return std::nullopt;
        }
        return make_err("expected a consistency level after AT (STRICT / SNAPSHOT n "
                        "/ BOUNDED n / RYW n)");
    }

    Lexer lex_;
    Token cur_;
};

// Convenience: parse one statement from a string.
[[nodiscard]] inline ParseResult parse_sql(std::string src) {
    Parser p(std::move(src));
    return p.parse();
}

}  // namespace lockstep::query::sql
