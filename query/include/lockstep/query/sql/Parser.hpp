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
//   SELECT * | c, ... FROM t [WHERE pk = v | WHERE pk BETWEEN a AND b]
//                            [AT STRICT | SNAPSHOT n | BOUNDED n | RYW n]
// Literals: a signed integer (e.g. 42, -7) or a single-quoted string ('foo', with
// '' as an escaped quote). Identifiers: [A-Za-z_][A-Za-z0-9_]*. A trailing ';' is
// optional. Anything outside the subset (JOIN, GROUP BY, subquery, ...) is a clean
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
    Semi,     // ;
    Star,     // *
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
            case ';':
                ++i_;
                t.kind = Tok::Semi;
                return t;
            case '*':
                ++i_;
                t.kind = Tok::Star;
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
    // otherwise (so callers write `if (auto e = expect_kw(...)) return *e;`).
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

    // --- grammar rules ---

    // CREATE TABLE t (c TYPE, ..., PRIMARY KEY (c))
    ParseResult parse_create() {
        advance();  // CREATE
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
        if (auto e = expect_kw("values")) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' to open VALUES")) {
            return ParseResult{*e};
        }
        for (;;) {
            Datum v;
            if (auto e = expect_literal(v)) {
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
        return ParseResult{std::move(st)};
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
        if (auto e = expect_literal(st.update.set_value)) {
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

    // SELECT * | c, ... FROM t [WHERE ...] [AT ...]
    ParseResult parse_select() {
        advance();  // SELECT
        Statement st;
        st.kind = StmtKind::Select;
        if (cur_.kind == Tok::Star) {
            st.select.star = true;
            advance();
        } else {
            for (;;) {
                std::string c;
                if (auto e = expect_ident("a column name or '*'", c)) {
                    return ParseResult{*e};
                }
                st.select.columns.push_back(std::move(c));
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }
        if (auto e = expect_kw("from")) {
            return ParseResult{*e};
        }
        if (auto e = expect_ident("a table name after FROM", st.select.table)) {
            return ParseResult{*e};
        }

        // Optional WHERE.
        if (is_kw("where")) {
            advance();
            if (auto e = expect_ident("a column name after WHERE",
                                      st.select.where_column)) {
                return ParseResult{*e};
            }
            if (is_kw("between")) {
                advance();
                if (auto e = expect_literal(st.select.lo_value)) {
                    return ParseResult{*e};
                }
                if (auto e = expect_kw("and")) {
                    return ParseResult{*e};
                }
                if (auto e = expect_literal(st.select.hi_value)) {
                    return ParseResult{*e};
                }
                st.select.where = SelectWhereKind::Between;
            } else {
                if (auto e = expect(Tok::Eq, "'=' or BETWEEN in WHERE")) {
                    return ParseResult{*e};
                }
                if (auto e = expect_literal(st.select.eq_value)) {
                    return ParseResult{*e};
                }
                st.select.where = SelectWhereKind::Eq;
            }
        }

        // Optional AT <level> — the CALL-SITE-VISIBLE D5 annotation (V-D5-SAFE).
        if (is_kw("at")) {
            advance();
            if (auto e = parse_at_level(st.select)) {
                return ParseResult{*e};
            }
        }
        return ParseResult{std::move(st)};
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
