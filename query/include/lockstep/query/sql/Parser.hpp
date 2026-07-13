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
#include <charconv>  // F14: locale-free double parse for float literals
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
    [[nodiscard]] Statement& stmt_mut() { return std::get<Statement>(value_); }  // D4: attach WITH ctes
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
    FloatLit, // F14: a floating-point literal (has a '.' fraction or an exponent) -> REAL
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
    Plus,     // +  (A1 arithmetic)
    Minus,    // -
    Slash,    // /
    Percent,  // %
    Param,    // $N (K4.13: an extended-protocol bind placeholder; int_val = N)
    DblColon, // ::  (ORM-compat: PG postfix cast — absorbed, the value keeps its type)
    LBracket, // [  (F12 array literal / subscript)
    RBracket, // ]
    Arrow,    // ->   (F13 JSON get)
    ArrowText,// ->>  (F13 JSON get as text)
    Contains, // @>   (JSON containment)
    PathArrow,    // #>   (JSON get by path -> JSON)
    PathArrowText,// #>>  (JSON get by path -> text)
    DistL2,   // <->  (K1.2: L2 distance, pgvector)
    DistIp,   // <#>  (K1.2: NEGATIVE inner product, pgvector)
    DistCos,  // <=>  (K1.2: cosine distance, pgvector)
    End,      // end of input
    Bad,      // a lexing error (unterminated string / stray byte)
};

struct Token {
    Tok kind = Tok::End;
    std::string text;       // Ident: the raw identifier; StrLit: the decoded string; IntLit: raw digits
    std::int64_t int_val = 0;  // IntLit: the parsed value (saturated at int64 if int_overflow)
    bool int_overflow = false;  // F11: the IntLit's true value exceeds int64 (carry it as a string)
    std::size_t pos = 0;    // byte offset of the token start
    std::string bad_msg;    // Bad: why
};

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}
    [[nodiscard]] const std::string& src() const { return src_; }  // F5: raw text capture

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
        if (c == '$' && i_ + 1 < src_.size() && is_digit(src_[i_ + 1])) {
            ++i_;  // K4.13: $N bind placeholder
            std::int64_t n = 0;
            while (i_ < src_.size() && is_digit(src_[i_])) n = n * 10 + (src_[i_++] - '0');
            t.kind = Tok::Param;
            t.int_val = n;
            return t;
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
            case '[':
                ++i_;
                t.kind = Tok::LBracket;
                return t;
            case ']':
                ++i_;
                t.kind = Tok::RBracket;
                return t;
            case ',':
                ++i_;
                t.kind = Tok::Comma;
                return t;
            case ':':
                if (i_ + 1 < src_.size() && src_[i_ + 1] == ':') {
                    i_ += 2;  // ORM-compat: '::' postfix cast (PG syntax)
                    t.kind = Tok::DblColon;
                    return t;
                }
                break;  // a lone ':' stays unrecognized (falls to the Bad path)
            case '=':
                ++i_;
                t.kind = Tok::Eq;
                return t;
            case '<':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '=') {
                    ++i_;
                    // K1.2: '<=>' (pgvector cosine distance) — only when '>' follows the '='.
                    if (i_ < src_.size() && src_[i_] == '>') {
                        ++i_;
                        t.kind = Tok::DistCos;
                    } else {
                        t.kind = Tok::Le;  // <=
                    }
                } else if (i_ < src_.size() && src_[i_] == '>') {
                    ++i_;
                    t.kind = Tok::Ne;  // <> (SQL not-equal)
                } else if (i_ + 1 < src_.size() && src_[i_] == '-' && src_[i_ + 1] == '>') {
                    i_ += 2;
                    t.kind = Tok::DistL2;  // <-> (K1.2; `a < -5` is untouched — no trailing '>')
                } else if (i_ + 1 < src_.size() && src_[i_] == '#' && src_[i_ + 1] == '>') {
                    i_ += 2;
                    t.kind = Tok::DistIp;  // <#> (K1.2)
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
            case '+':
                ++i_;
                t.kind = Tok::Plus;
                return t;
            case '-':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '>') {  // F13: -> or ->>
                    ++i_;
                    if (i_ < src_.size() && src_[i_] == '>') { ++i_; t.kind = Tok::ArrowText; }
                    else t.kind = Tok::Arrow;
                    return t;
                }
                t.kind = Tok::Minus;
                return t;
            case '/':
                ++i_;
                t.kind = Tok::Slash;
                return t;
            case '%':
                ++i_;
                t.kind = Tok::Percent;
                return t;
            case '@':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '>') { ++i_; t.kind = Tok::Contains; return t; }  // @>
                t.kind = Tok::Bad;
                t.bad_msg = "expected '>' after '@' (the only '@' use is '@>')";
                return t;
            case '#':
                ++i_;
                if (i_ < src_.size() && src_[i_] == '>') {  // #> or #>>
                    ++i_;
                    if (i_ < src_.size() && src_[i_] == '>') { ++i_; t.kind = Tok::PathArrowText; }
                    else t.kind = Tok::PathArrow;
                    return t;
                }
                t.kind = Tok::Bad;
                t.bad_msg = "expected '>' after '#' (the only '#' uses are '#>' and '#>>')";
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
        // F14: a fractional part ('.' followed by a digit) and/or an exponent (e[+-]?digits)
        // make this a FLOAT literal (a REAL). A bare '.' with no following digit is left alone
        // (it stays a Dot token, e.g. a qualified name never has digit.digit).
        bool is_float = false;
        if (i_ + 1 < src_.size() && src_[i_] == '.' && is_digit(src_[i_ + 1])) {
            is_float = true;
            ++i_;  // consume '.'
            while (i_ < src_.size() && is_digit(src_[i_])) ++i_;
        }
        if (i_ < src_.size() && (src_[i_] == 'e' || src_[i_] == 'E')) {
            std::size_t j = i_ + 1;
            if (j < src_.size() && (src_[j] == '+' || src_[j] == '-')) ++j;
            if (j < src_.size() && is_digit(src_[j])) {  // a real exponent (else leave 'e' to the tokenizer)
                is_float = true;
                i_ = j;
                while (i_ < src_.size() && is_digit(src_[i_])) ++i_;
            }
        }
        // Reject e.g. "12abc" / "1.5x" as an identifier-adjacent number (a stray suffix).
        if (i_ < src_.size() && is_ident_cont(src_[i_])) {
            t.kind = Tok::Bad;
            t.bad_msg = "malformed numeric literal";
            return t;
        }
        std::string digits = src_.substr(start, i_ - start);
        if (is_float) {
            t.kind = Tok::FloatLit;
            t.text = std::move(digits);  // the raw literal; parsed to a double in expect_literal
            return t;
        }
        t.kind = Tok::IntLit;
        t.int_val = parse_i64(digits, t.int_overflow);  // F11: flag values past int64
        t.text = std::move(digits);                     // raw digits (used when int_overflow)
        return t;
    }

    // Deterministic signed-decimal parse (no strtol/locale). Saturates on overflow
    // (a bounded subset; values are small) so the parse is total; `overflow` is set when the
    // true value exceeds int64 (the caller then carries the raw digits as a numeric string).
    static std::int64_t parse_i64(const std::string& digits, bool& overflow) {
        overflow = false;
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
                overflow = true;
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
        } else if (kw == "refresh") {  // REFRESH MATERIALIZED VIEW name
            advance();  // REFRESH
            if (auto e = expect_kw("materialized")) return ParseResult{*e};
            if (auto e = expect_kw("view")) return ParseResult{*e};
            Statement st;
            st.kind = StmtKind::RefreshMatView;
            if (auto e = expect_table_name("a name after REFRESH MATERIALIZED VIEW", st.truncate.table))
                return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "changes") {  // K4: CHANGES t SINCE n [LIMIT k]
            advance();
            Statement st;
            st.kind = StmtKind::Changes;
            if (auto e = expect_table_name("a table name after CHANGES", st.changes.table))
                return ParseResult{*e};
            if (is_kw("shard")) {  // K4.2: one shard's feed — the Kafka-partition shape
                advance();
                if (cur_.kind != Tok::IntLit || cur_.int_val < 0)
                    return err("SHARD expects a non-negative shard index");
                st.changes.shard = cur_.int_val;
                advance();
            }
            if (auto e = expect_kw("since")) return ParseResult{*e};
            if (cur_.kind != Tok::IntLit || cur_.int_val < 0)
                return err("SINCE expects a non-negative Seq");
            st.changes.since = cur_.int_val;
            advance();
            if (is_kw("limit")) {
                advance();
                if (cur_.kind != Tok::IntLit || cur_.int_val < 1)
                    return err("LIMIT expects a positive integer");
                st.changes.limit = cur_.int_val;
                advance();
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "publish") {  // K4.10: PUBLISH t, <expr>
            advance();
            Statement st;
            st.kind = StmtKind::Publish;
            if (auto e = expect_ident("a topic name after PUBLISH", st.queue.queue))
                return ParseResult{*e};
            if (auto e = expect(Tok::Comma, "',' after the topic name")) return ParseResult{*e};
            for (;;) {  // batch form: PUBLISH t, p1, p2, ... (one commit for all)
                std::shared_ptr<Expr> pe;
                if (auto e = parse_scalar_expr(pe)) return ParseResult{*e};
                st.queue.payloads.push_back(std::move(pe));
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            st.queue.payload = st.queue.payloads.front();
            r = ParseResult{std::move(st)};
        } else if (kw == "consume") {  // K4.10: CONSUME t SINCE off [LIMIT n]
            advance();
            Statement st;
            st.kind = StmtKind::Consume;
            if (auto e = expect_ident("a topic name after CONSUME", st.changes.feed))
                return ParseResult{*e};
            if (auto e = expect_kw("since")) return ParseResult{*e};
            if (cur_.kind != Tok::IntLit || cur_.int_val < 0)
                return err("SINCE expects a non-negative offset");
            st.changes.since = cur_.int_val;
            advance();
            if (is_kw("limit")) {
                advance();
                if (cur_.kind != Tok::IntLit || cur_.int_val < 1)
                    return err("LIMIT expects a positive integer");
                st.changes.limit = cur_.int_val;
                advance();
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "fetch") {  // K4.4: FETCH cf [LIMIT n]
            advance();
            Statement st;
            st.kind = StmtKind::Fetch;
            if (auto e = expect_ident("a changefeed name after FETCH", st.changes.feed))
                return ParseResult{*e};
            if (is_kw("limit")) {
                advance();
                if (cur_.kind != Tok::IntLit || cur_.int_val < 1)
                    return err("LIMIT expects a positive integer");
                st.changes.limit = cur_.int_val;
                advance();
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "send") {  // K3: SEND q, <payload>
            advance();
            Statement st;
            st.kind = StmtKind::Send;
            if (auto e = expect_ident("a queue name after SEND", st.queue.queue))
                return ParseResult{*e};
            if (auto e = expect(Tok::Comma, "',' after the queue name")) return ParseResult{*e};
            if (auto e = parse_scalar_expr(st.queue.payload)) return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "receive") {  // K3: RECEIVE q [BATCH n] [VISIBILITY v] [DLQ]
            advance();
            Statement st;
            st.kind = StmtKind::Receive;
            if (auto e = expect_ident("a queue name after RECEIVE", st.queue.queue))
                return ParseResult{*e};
            for (;;) {
                if (is_kw("batch")) {
                    advance();
                    if (cur_.kind != Tok::IntLit || cur_.int_val < 1)
                        return err("BATCH expects a positive integer");
                    st.queue.batch = cur_.int_val;
                    advance();
                } else if (is_kw("visibility")) {
                    advance();
                    if (cur_.kind != Tok::IntLit || cur_.int_val < 1)
                        return err("VISIBILITY expects a positive integer (Seq units)");
                    st.queue.visibility = cur_.int_val;
                    advance();
                } else if (is_kw("dlq")) {
                    advance();
                    st.queue.dlq = true;
                } else {
                    break;
                }
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "ack") {  // K3: ACK q, <mid> | K4.4: ACK CHANGEFEED cf AT s
            advance();
            if (is_kw("changefeed")) {
                advance();
                Statement st;
                st.kind = StmtKind::AckFeed;
                if (auto e = expect_ident("a changefeed name", st.changes.feed))
                    return ParseResult{*e};
                if (auto e = expect_kw("at")) return ParseResult{*e};
                if (cur_.kind != Tok::IntLit || cur_.int_val < 0)
                    return err("ACK CHANGEFEED expects AT <non-negative seq>");
                st.changes.at = cur_.int_val;
                advance();
                r = ParseResult{std::move(st)};
            } else {
            Statement st;
            st.kind = StmtKind::Ack;
            if (auto e = expect_ident("a queue name after ACK", st.queue.queue))
                return ParseResult{*e};
            if (auto e = expect(Tok::Comma, "',' after the queue name")) return ParseResult{*e};
            for (;;) {
                if (cur_.kind != Tok::IntLit) return err("ACK expects a message id");
                st.queue.mids.push_back(cur_.int_val);
                advance();
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            st.queue.mid = st.queue.mids.front();
            r = ParseResult{std::move(st)};
            }
        } else if (kw == "insert") {
            r = parse_insert();
        } else if (kw == "update") {
            r = parse_update();
        } else if (kw == "delete") {
            r = parse_delete();
        } else if (kw == "select") {
            r = parse_select();
        } else if (kw == "with") {
            r = parse_with();
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
        } else if (kw == "begin") {
            advance();
            if (is_kw("transaction") || is_kw("work")) advance();  // optional
            Statement st;
            st.kind = StmtKind::Begin;
            r = ParseResult{std::move(st)};
        } else if (kw == "commit") {
            advance();
            Statement st;
            st.kind = StmtKind::Commit;
            r = ParseResult{std::move(st)};
        } else if (kw == "rollback") {
            advance();
            if (is_kw("transaction") || is_kw("work")) advance();  // optional
            Statement st;
            if (is_kw("to")) {  // ROLLBACK TO [SAVEPOINT] name
                advance();
                if (is_kw("savepoint")) advance();  // optional keyword
                st.kind = StmtKind::RollbackToSavepoint;
                if (auto e = expect_ident("a savepoint name after ROLLBACK TO", st.savepoint_name))
                    return ParseResult{*e};
            } else {
                st.kind = StmtKind::Rollback;
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "savepoint") {
            advance();
            Statement st;
            st.kind = StmtKind::Savepoint;
            if (auto e = expect_ident("a savepoint name after SAVEPOINT", st.savepoint_name))
                return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "release") {
            advance();
            if (is_kw("savepoint")) advance();  // optional keyword
            Statement st;
            st.kind = StmtKind::ReleaseSavepoint;
            if (auto e = expect_ident("a savepoint name after RELEASE", st.savepoint_name))
                return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "alter") {
            r = parse_alter();
        } else if (kw == "drop") {
            r = parse_drop_index();
        } else if (kw == "truncate") {
            advance();
            if (is_kw("table")) advance();  // optional TABLE
            Statement st;
            st.kind = StmtKind::Truncate;
            if (auto e = expect_table_name("a table name after TRUNCATE", st.truncate.table)) return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "show") {  // E5: SHOW TABLES | SHOW COLUMNS FROM t
            advance();
            Statement st;
            if (is_kw("tables")) {
                advance();
                st.kind = StmtKind::ShowTables;
            } else if (is_kw("columns")) {
                advance();
                if (auto e = expect_kw("from")) return *e;
                st.kind = StmtKind::Describe;
                if (auto e = expect_table_name("a table name", st.truncate.table)) return ParseResult{*e};
            } else if (cur_.kind == Tok::Ident) {
                // ORM-compat: SHOW <guc name...> (PG session parameters; drivers probe
                // these on connect). The name may be several words / dotted.
                st.kind = StmtKind::SetParam;      // reuse the param fields
                st.set_param_value = "\x01show";  // marker: this is SHOW, not SET
                std::string nm;
                while (cur_.kind == Tok::Ident || cur_.kind == Tok::Dot) {
                    if (cur_.kind == Tok::Dot) nm += ".";
                    else nm += (nm.empty() || nm.back() == '.' ? "" : " ") + lower(cur_.text);
                    advance();
                }
                st.set_param_name = nm;
            } else {
                return err("expected TABLES or COLUMNS after SHOW");
            }
            r = ParseResult{std::move(st)};
        } else if (kw == "analyze") {  // I6: ANALYZE [TABLE] t
            advance();
            if (is_kw("table")) advance();
            Statement st;
            st.kind = StmtKind::Analyze;
            if (auto e = expect_table_name("a table name after ANALYZE", st.truncate.table)) return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "describe" || kw == "desc") {  // E5: DESCRIBE t
            advance();
            Statement st;
            st.kind = StmtKind::Describe;
            if (auto e = expect_table_name("a table name after DESCRIBE", st.truncate.table)) return ParseResult{*e};
            r = ParseResult{std::move(st)};
        } else if (kw == "set") {  // E4: SET search_path TO s | DEFAULT ; W3.1: SET <name> = <value>
            advance();
            if (is_kw("search_path")) {
                advance();
                if (is_kw("to") || cur_.kind == Tok::Eq) advance();
                Statement st;
                st.kind = StmtKind::SetSearchPath;
                if (is_kw("default")) { advance(); st.schema_arg.clear(); }
                else if (auto e = expect_ident("a schema name or DEFAULT", st.schema_arg)) return ParseResult{*e};
                r = ParseResult{std::move(st)};
            } else {
                // W3.1: generic session parameter — SET <dotted.name> [=|TO] <value>.
                // The name may be dotted (e.g. lockstep.max_query_memory); lower-cased.
                Statement st;
                st.kind = StmtKind::SetParam;
                std::string part;
                if (auto e = expect_ident("a parameter name after SET", part)) return ParseResult{*e};
                st.set_param_name = lower(part);
                while (cur_.kind == Tok::Dot) {
                    advance();
                    if (auto e = expect_ident("a parameter name segment after '.'", part)) return ParseResult{*e};
                    st.set_param_name += "." + lower(part);
                }
                if (is_kw("to") || cur_.kind == Tok::Eq) advance();
                else return err("expected '=' or TO after the parameter name in SET");
                // Accept an int / string / identifier value token.
                if (cur_.kind == Tok::IntLit || cur_.kind == Tok::StrLit || cur_.kind == Tok::Ident) {
                    st.set_param_value = cur_.text;
                    advance();
                } else {
                    return err("expected a value after '=' in SET");
                }
                r = ParseResult{std::move(st)};
            }
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

    // E4: a possibly schema-qualified TABLE name: <ident> ['.' <ident>] -> "schema.table" (or bare).
    [[nodiscard]] std::optional<ParseError> expect_table_name(const char* what, std::string& out) {
        if (auto e = expect_ident(what, out)) return e;
        if (cur_.kind == Tok::Dot) {
            advance();
            std::string tbl;
            if (auto e = expect_ident("a table name after the schema", tbl)) return e;
            out += "." + tbl;
        }
        return std::nullopt;
    }

    // v3: consume an optionally-QUALIFIED column reference: <ident> ['.' <ident>].
    //   `tbl.col`  => qualifier="tbl",  column="col"
    //   `col`      => qualifier="",     column="col"
    // The qualifier is resolved against the joined schema at plan time (NOT here — the
    // parser has no catalog). A trailing '.' with no column name is a clean error.
    // ORM-compat: absorb a PG postfix cast `::typename` (e.g. %s::VARCHAR in driver
    // introspection SQL). v1 semantics: the value keeps its parsed type — PG's text
    // casts on already-typed parameters are no-ops for the shapes drivers emit.
    void absorb_pg_cast() {
        while (cur_.kind == Tok::DblColon) {
            advance();
            if (cur_.kind == Tok::Ident) advance();
            if (cur_.kind == Tok::LParen) {  // e.g. ::VARCHAR(64)
                advance();
                if (cur_.kind == Tok::IntLit) advance();
                if (cur_.kind == Tok::RParen) advance();
            }
        }
    }

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
            if (cur_.kind == Tok::Dot) {  // ORM-compat: schema.table.col — fold the
                advance();                // schema into the table qualifier
                std::string third;
                if (auto e = expect_ident("a column name after '.'", third)) return e;
                qualifier_out = first + "." + second;
                column_out = std::move(third);
                absorb_pg_cast();
                return std::nullopt;
            }
            qualifier_out = std::move(first);
            column_out = std::move(second);
        } else {
            qualifier_out.clear();
            column_out = std::move(first);
        }
        return std::nullopt;
    }

    // F9: parse a column type. INT/BIGINT/INTEGER + BOOL/BOOLEAN are INT-backed; TEXT/VARCHAR/CHAR
    // are TEXT (an optional VARCHAR length is parsed + ignored). FLOAT/DOUBLE/DECIMAL/NUMERIC are
    // rejected — they break the engine's byte-deterministic INT model (cross-check/conformance).
    // F12: parse a scalar type, then an optional `[]` suffix promoting it to a one-dimensional ARRAY
    // of that element type (element kept in elem_type/elem_logical/elem_scale; the array itself is
    // TEXT-backed, logical 7). Nested arrays (T[][]) are rejected.
    [[nodiscard]] std::optional<ParseError> parse_column_type(Column& col) {
        if (auto e = parse_scalar_column_type(col)) return e;
        if (cur_.kind == Tok::LBracket) {
            advance();
            if (auto e = expect(Tok::RBracket, "']' to close an array type")) return e;
            if (col.logical == 15) return make_err("VECTOR[] arrays are not supported");
            col.elem_type = col.type;
            col.elem_logical = col.logical;
            col.elem_scale = col.scale;
            col.type = Type::Text;
            col.logical = 7;
            col.scale = 0;
            if (cur_.kind == Tok::LBracket) return make_err("nested arrays (T[][]) are not supported");
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> parse_scalar_column_type(Column& col) {
        Type& out = col.type;
        if (is_kw("int") || is_kw("bigint") || is_kw("integer") || is_kw("bool") ||
            is_kw("boolean")) {
            out = Type::Int;
            advance();
            return std::nullopt;
        }
        // ORM-compat: SERIAL family = INT + AUTO_INCREMENT (PG's sequence-backed ints;
        // ours is the same monotonic per-table counter AUTO_INCREMENT already provides).
        if (is_kw("serial") || is_kw("bigserial") || is_kw("smallserial")) {
            out = Type::Int;
            col.auto_increment = true;
            advance();
            return std::nullopt;
        }
        // F11: BIT(n) — an n-bit UNSIGNED bitmask over INT (range 0..2^n-1). Default n=1. Stored in
        // signed int64, so n is capped at 63. A value is an ordinary integer literal.
        if (is_kw("bit")) {
            out = Type::Int;
            col.is_unsigned = true;
            col.int_bits = 1;
            advance();
            if (cur_.kind == Tok::LParen) {
                advance();
                Datum n;
                if (auto e = expect_literal(n)) return e;
                if (n.type != Type::Int || n.i < 1 || n.i > 63)
                    return make_err("BIT(n) width must be 1..63");
                col.int_bits = static_cast<std::uint8_t>(n.i);
                if (auto e = expect(Tok::RParen, "')' after BIT width")) return e;
            }
            return std::nullopt;
        }
        // F10: narrow integer aliases — INT-backed, with a RANGE check (int_bits) at coerce.
        if (is_kw("tinyint") || is_kw("smallint") || is_kw("int32") || is_kw("int4") ||
            is_kw("mediumint")) {
            out = Type::Int;
            col.int_bits = (is_kw("tinyint")) ? 8 : (is_kw("smallint") ? 16 : 32);
            advance();
            return std::nullopt;
        }
        // F9b: DATE / TIMESTAMP / DECIMAL — INT-backed logical types (byte-deterministic).
        if (is_kw("date")) {
            out = Type::Int;
            col.logical = 2;
            advance();
            return std::nullopt;
        }
        // F13: TIME (seconds since midnight) / INTERVAL (seconds) — INT-backed.
        if (is_kw("time")) { out = Type::Int; col.logical = 8; advance(); return std::nullopt; }
        if (is_kw("interval")) { out = Type::Int; col.logical = 10; advance(); return std::nullopt; }
        // F13: ENUM('a','b',...) — INT-backed ordinal; the label set is stored on the column.
        if (is_kw("enum")) {
            out = Type::Int;
            col.logical = 9;
            advance();
            if (auto e = expect(Tok::LParen, "'(' after ENUM")) return e;
            for (;;) {
                if (cur_.kind != Tok::StrLit) return make_err("ENUM labels must be 'string' literals");
                col.enum_labels.push_back(cur_.text);
                advance();
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            if (auto e = expect(Tok::RParen, "')' to close ENUM(...)")) return e;
            if (col.enum_labels.empty()) return make_err("ENUM must have at least one label");
            return std::nullopt;
        }
        if (is_kw("timestamp") || is_kw("datetime")) {
            out = Type::Int;
            col.logical = 3;
            advance();
            return std::nullopt;
        }
        // F9e: HUGEINT / INT128 — a 128-bit integer over TEXT (16-byte order-preserving payload).
        if (is_kw("int128") || is_kw("hugeint")) {
            out = Type::Text;
            col.logical = 5;
            advance();
            return std::nullopt;
        }
        // UINT256 — a 256-bit UNSIGNED integer (crypto-scale) over TEXT (32-byte order-preserving
        // big-endian payload). Inherently unsigned (range 0 .. 2^256-1).
        if (is_kw("uint256") || is_kw("u256")) {
            out = Type::Text;
            col.logical = 13;
            col.is_unsigned = true;
            advance();
            return std::nullopt;
        }
        if (is_kw("decimal") || is_kw("numeric") || is_kw("decimal128") || is_kw("numeric128")) {
            // DECIMAL128 is always 128-bit; DECIMAL/NUMERIC promotes to 128-bit when precision > 18.
            const bool force128 = is_kw("decimal128") || is_kw("numeric128");
            out = Type::Int;
            col.logical = 1;
            col.scale = 0;
            advance();
            std::int64_t precision = 0;
            if (cur_.kind == Tok::LParen) {  // DECIMAL(precision[, scale])
                advance();
                Datum prec;
                if (auto e = expect_literal(prec)) return e;
                if (prec.type == Type::Int) precision = prec.i;
                if (cur_.kind == Tok::Comma) {
                    advance();
                    Datum sc;
                    if (auto e = expect_literal(sc)) return e;
                    const bool wide = force128 || precision > 18;
                    if (sc.type != Type::Int || sc.i < 0 || sc.i > (wide ? 38 : 18))
                        return make_err(wide ? "DECIMAL128 scale must be 0..38" : "DECIMAL scale must be 0..18");
                    col.scale = static_cast<std::uint8_t>(sc.i);
                }
                if (auto e = expect(Tok::RParen, "')' after DECIMAL precision/scale")) return e;
            }
            if (precision > 0 && precision <= 38) col.precision = static_cast<std::uint8_t>(precision);  // F10
            if (force128 || precision > 18) {  // promote to 128-bit fixed-point over TEXT
                out = Type::Text;
                col.logical = 6;
            }
            return std::nullopt;
        }
        if (is_kw("uuid")) {  // F9c: UUID over TEXT (validated, canonicalised string)
            out = Type::Text;
            col.logical = 4;
            advance();
            return std::nullopt;
        }
        if (is_kw("json") || is_kw("jsonb")) {  // F13: JSON over TEXT (canonical form stored)
            out = Type::Text;
            col.logical = 11;
            advance();
            return std::nullopt;
        }
        if (is_kw("text")) {
            out = Type::Text;
            advance();
            return std::nullopt;
        }
        // F10: VARCHAR(n) / CHAR(n) / BLOB(n) / [VAR]BINARY(n) — TEXT-backed with a LENGTH limit.
        // CHAR right-pads to n; the rest just bound the length. BLOB/BINARY are byte aliases of TEXT.
        if (is_kw("varchar") || is_kw("char") || is_kw("blob") || is_kw("bytes") ||
            is_kw("binary") || is_kw("varbinary")) {
            const bool is_char = is_kw("char") || is_kw("binary");  // fixed-length forms
            advance();
            if (cur_.kind == Tok::LParen) {  // VARCHAR(n) — enforce n
                advance();
                Datum d;
                if (auto e = expect_literal(d)) return e;
                if (d.type != Type::Int || d.i < 0 || d.i > 0xFFFFFFLL)
                    return make_err("length must be a non-negative integer");
                col.max_len = static_cast<std::uint32_t>(d.i);
                col.fixed_char = is_char && col.max_len > 0;
                if (auto e = expect(Tok::RParen, "')' after the length")) return e;
            }
            out = Type::Text;
            return std::nullopt;
        }
        // K1: VECTOR(n) — a fixed-dimension embedding (pgvector-style). Logical 15 over physical
        // TEXT; the stored payload is the generic ARRAY codec with REAL elements, the dimension
        // lives in max_len (0 == unconstrained) and is enforced at coerce.
        if (is_kw("vector")) {
            advance();
            out = Type::Text;
            col.logical = 15;
            col.elem_type = Type::Text;
            col.elem_logical = 14;
            col.elem_scale = 0;
            if (cur_.kind == Tok::LParen) {
                advance();
                Datum n;
                if (auto e = expect_literal(n)) return e;
                if (n.type != Type::Int || n.i < 1 || n.i > 16000)
                    return make_err("VECTOR(n) dimension must be 1..16000");
                col.max_len = static_cast<std::uint32_t>(n.i);
                if (auto e = expect(Tok::RParen, "')' after the VECTOR dimension")) return e;
            }
            return std::nullopt;
        }
        if (is_kw("float") || is_kw("double") || is_kw("real")) {
            // F14: REAL / DOUBLE PRECISION / FLOAT — an IEEE-754 double stored as an 8-byte payload
            // (logical=14 over TEXT physical). Determinism holds: the bit pattern is fixed and every
            // arithmetic op is IEEE-deterministic given the same inputs and evaluation order.
            advance();
            if (is_kw("precision")) advance();  // "DOUBLE PRECISION"
            col.logical = 14;
            out = Type::Text;
            return std::nullopt;
        }
        return make_err("expected a column type (INT/BIGINT/BOOL/DECIMAL/REAL/DATE/TIMESTAMP or TEXT)");
    }

    // Consume a literal (int or string) into `out`. Used by INSERT/UPDATE/WHERE.
    [[nodiscard]] std::optional<ParseError> expect_literal(Datum& out) {
        if (auto e = expect_literal_raw(out)) return e;
        absorb_pg_cast();  // ORM-compat: any literal may carry a ::TYPE postfix
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> expect_literal_raw(Datum& out) {
        if (cur_.kind == Tok::Bad) {
            return make_err(cur_.bad_msg);
        }
        if (cur_.kind == Tok::Param) {  // K5.2: $N in a literal position — a MARKER
            // Datum (logical 120, i = N) resolved against the bound parameters at the
            // top of exec(Statement) on a per-execution copy; the cached AST keeps the
            // marker, so one prepared shape serves every parameter value.
            out = Datum::make_int(cur_.int_val);
            out.logical = 120;
            advance();
            return std::nullopt;
        }
        // F9: BOOL literals TRUE/FALSE -> INT 1/0.
        if (is_kw("true") || is_kw("false")) {
            out = Datum::make_int(is_kw("true") ? 1 : 0);
            advance();
            return std::nullopt;
        }
        if (cur_.kind == Tok::IntLit) {
            // F11: a bare integer literal that exceeds int64 is carried as a NUMERIC STRING (so an
            // INT128/DECIMAL128 column parses it losslessly, and an INT64 column gets a clean type
            // error instead of a silent saturation).
            out = cur_.int_overflow ? Datum::make_text(cur_.text) : Datum::make_int(cur_.int_val);
            advance();
            return std::nullopt;
        }
        if (cur_.kind == Tok::FloatLit) {
            // F14: a floating-point literal -> a REAL Datum (double). Locale-free parse.
            double d = 0.0;
            if (!parse_double_strict(cur_.text.data(), cur_.text.data() + cur_.text.size(), d)) {
                return make_err("malformed floating-point literal '" + cur_.text + "'");
            }
            out = Datum::make_real(d);
            advance();
            return std::nullopt;
        }
        if (cur_.kind == Tok::StrLit) {
            out = Datum::make_text(cur_.text);
            advance();
            return std::nullopt;
        }
        // F13: INTERVAL '1 day 02:30:00' -> a logical-INTERVAL Datum (seconds).
        if (is_kw("interval")) {
            advance();
            if (cur_.kind != Tok::StrLit) return make_err("INTERVAL requires a 'string' literal");
            std::int64_t secs = 0;
            if (!parse_interval(cur_.text, secs)) return make_err("invalid INTERVAL literal '" + cur_.text + "'");
            out = Datum::make_int(secs);
            out.logical = 10;
            advance();
            return std::nullopt;
        }
        // F12: an ARRAY[...] constant in a VALUES / WHERE position — its elements are literals, so
        // build the array Datum here (re-encoded to the column's element type at coerce).
        if (is_kw("array")) {
            advance();
            if (auto e = expect(Tok::LBracket, "'[' after ARRAY")) return e;
            std::vector<Datum> elems;
            if (cur_.kind != Tok::RBracket) {
                for (;;) {
                    Datum el;
                    if (auto e = expect_value_or_null(el)) return e;
                    elems.push_back(el);
                    if (cur_.kind == Tok::Comma) { advance(); continue; }
                    break;
                }
            }
            if (auto e = expect(Tok::RBracket, "']' to close an ARRAY literal")) return e;
            const std::uint8_t el = elems.empty() ? 0 : elems.front().logical;
            const std::uint8_t es = elems.empty() ? 0 : elems.front().scale;
            out = Datum::make_text(Datum::encode_array(el, es, elems));
            out.logical = 7;
            return std::nullopt;
        }
        return make_err("expected a literal (integer, 'string', or ARRAY[...])");
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

    // F5: CHECK ( <predicate> ) — capture the predicate's RAW SOURCE TEXT (so it persists in the
    // catalog and is re-parsed + evaluated on every write). Validates the predicate parses here.
    [[nodiscard]] std::optional<ParseError> parse_check(CreateStmt& cr, const std::string& name) {
        advance();  // CHECK
        if (auto e = expect(Tok::LParen, "'(' after CHECK")) return e;
        const std::size_t start = cur_.pos;
        Predicate tmp;
        if (auto e = parse_predicate(tmp, /*allow_agg=*/false)) return e;
        const std::size_t end = cur_.pos;  // position of the closing ')'
        std::string text = lex_.src().substr(start, end > start ? end - start : 0);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n')) {
            text.pop_back();
        }
        if (auto e = expect(Tok::RParen, "')' to close CHECK")) return e;
        cr.checks.push_back(std::move(text));
        cr.check_names.push_back(name);  // "" => the engine auto-names it
        return std::nullopt;
    }

    // F7: ALTER TABLE <t> ADD [COLUMN] <col> <type> [NOT NULL] [DEFAULT <lit>]
    ParseResult parse_alter() {
        advance();  // ALTER
        if (auto e = expect_kw("table")) return ParseResult{*e};
        Statement st;
        st.kind = StmtKind::Alter;
        if (auto e = expect_table_name("a table name after ALTER TABLE", st.alter.table)) {
            return ParseResult{*e};
        }
        // RENAME TO <newtable>  |  RENAME [COLUMN] <a> TO <b>
        if (is_kw("rename")) {
            advance();
            if (is_kw("to")) {
                advance();
                st.alter.op = AlterOp::RenameTable;
                if (auto e = expect_ident("a new table name", st.alter.new_name)) return ParseResult{*e};
                return ParseResult{std::move(st)};
            }
            if (is_kw("column")) advance();
            st.alter.op = AlterOp::RenameColumn;
            if (auto e = expect_ident("a column name", st.alter.col_name)) return ParseResult{*e};
            if (auto e = expect_kw("to")) return ParseResult{*e};
            if (auto e = expect_ident("a new column name", st.alter.new_name)) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        // DROP [COLUMN] <c>  |  DROP CONSTRAINT <name>
        if (is_kw("drop")) {
            advance();
            if (is_kw("constraint")) {
                advance();
                st.alter.op = AlterOp::DropConstraint;
                if (auto e = expect_ident("a constraint name to drop", st.alter.constraint_name)) {
                    return ParseResult{*e};
                }
                return ParseResult{std::move(st)};
            }
            if (is_kw("column")) advance();
            st.alter.op = AlterOp::DropColumn;
            if (auto e = expect_ident("a column name to drop", st.alter.col_name)) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        // ALTER [COLUMN] <c> TYPE <t> | SET/DROP DEFAULT | SET/DROP NOT NULL | DROP UNIQUE
        if (is_kw("alter")) {
            advance();
            if (is_kw("column")) advance();
            if (auto e = expect_ident("a column name", st.alter.col_name)) return ParseResult{*e};
            if (is_kw("type") || is_kw("set")) {
                const bool is_type = is_kw("type");
                advance();
                if (is_type) {
                    st.alter.op = AlterOp::AlterType;
                    if (auto e = parse_column_type(st.alter.add_col)) return ParseResult{*e};
                    if (is_kw("unsigned")) { advance(); st.alter.add_col.is_unsigned = true; }
                    return ParseResult{std::move(st)};
                }
                if (is_kw("default")) {
                    advance();
                    st.alter.op = AlterOp::SetDefault;
                    if (auto e = expect_literal(st.alter.default_val)) return ParseResult{*e};
                    return ParseResult{std::move(st)};
                }
                if (is_kw("not")) {
                    advance();
                    if (auto e = expect_kw("null")) return ParseResult{*e};
                    st.alter.op = AlterOp::SetNotNull;
                    return ParseResult{std::move(st)};
                }
                return err("expected DEFAULT or NOT NULL after SET");
            }
            if (is_kw("drop")) {
                advance();
                if (is_kw("default")) { advance(); st.alter.op = AlterOp::DropDefault; return ParseResult{std::move(st)}; }
                if (is_kw("unique")) { advance(); st.alter.op = AlterOp::DropUnique; return ParseResult{std::move(st)}; }
                if (is_kw("not")) {
                    advance();
                    if (auto e = expect_kw("null")) return ParseResult{*e};
                    st.alter.op = AlterOp::DropNotNull;
                    return ParseResult{std::move(st)};
                }
                return err("expected DEFAULT, NOT NULL, or UNIQUE after DROP");
            }
            return err("expected TYPE / SET / DROP after ALTER COLUMN");
        }
        // ADD [COLUMN] <coldef>  |  ADD [CONSTRAINT name] CHECK (expr) | UNIQUE (col)
        if (auto e = expect_kw("add")) return ParseResult{*e};
        if (is_kw("constraint")) {
            advance();
            if (auto e = expect_ident("a constraint name", st.alter.constraint_name)) {
                return ParseResult{*e};
            }
        }
        if (is_kw("check")) {
            advance();
            st.alter.op = AlterOp::AddCheck;
            if (auto e = expect(Tok::LParen, "'(' after CHECK")) return ParseResult{*e};
            const std::size_t start = cur_.pos;
            Predicate tmp;
            if (auto e = parse_predicate(tmp, /*allow_agg=*/false)) return ParseResult{*e};
            std::string text = lex_.src().substr(start, cur_.pos > start ? cur_.pos - start : 0);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
            if (auto e = expect(Tok::RParen, "')' to close CHECK")) return ParseResult{*e};
            st.alter.check_src = std::move(text);
            return ParseResult{std::move(st)};
        }
        if (is_kw("unique")) {
            advance();
            st.alter.op = AlterOp::AddUnique;
            if (auto e = expect(Tok::LParen, "'(' after UNIQUE")) return ParseResult{*e};
            if (auto e = expect_ident("a column name", st.alter.unique_col)) return ParseResult{*e};
            if (auto e = expect(Tok::RParen, "')' after UNIQUE column")) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("column")) advance();  // optional COLUMN keyword
        Column col;
        st.alter.op = AlterOp::AddColumn;
        if (auto e = expect_ident("a column name", col.name)) return ParseResult{*e};
        if (auto e = parse_column_type(col)) return ParseResult{*e};
        if (is_kw("unsigned")) { advance(); col.is_unsigned = true; }  // F10
        col.nullable = true;
        if (is_kw("not")) {
            advance();
            if (auto e = expect_kw("null")) return ParseResult{*e};
            col.nullable = false;
        }
        if (is_kw("default")) {
            advance();
            Datum dv;
            if (auto e = expect_literal(dv)) return ParseResult{*e};
            col.has_default = true;
            if (dv.type == Type::Int) col.default_i = dv.i;
            else col.default_s = dv.s;
        }
        st.alter.add_col = std::move(col);
        return ParseResult{std::move(st)};
    }

    // CREATE TABLE t (...) | CREATE INDEX name ON t (col)
    ParseResult parse_create() {
        advance();  // CREATE
        bool or_replace = false;
        if (is_kw("or")) {  // H1: CREATE OR REPLACE VIEW
            advance();
            if (auto e = expect_kw("replace")) return ParseResult{*e};
            if (!is_kw("view")) return err("expected VIEW after CREATE OR REPLACE");
            or_replace = true;
        }
        if (is_kw("view")) {  // H1: CREATE [OR REPLACE] VIEW name [(cols)] AS SELECT ...
            return parse_create_view(or_replace);  // consumes VIEW itself
        }
        if (is_kw("queue")) {  // K3: CREATE QUEUE q
            advance();
            Statement st;
            st.kind = StmtKind::CreateQueue;
            if (auto e = expect_ident("a queue name after CREATE QUEUE", st.queue.queue))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("topic")) {  // K4.10: CREATE TOPIC t
            advance();
            Statement st;
            st.kind = StmtKind::CreateTopic;
            if (auto e = expect_ident("a topic name after CREATE TOPIC", st.queue.queue))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("changefeed")) {  // K4.4: CREATE CHANGEFEED cf FOR t
            advance();
            Statement st;
            st.kind = StmtKind::CreateChangefeed;
            if (auto e = expect_ident("a changefeed name", st.changes.feed))
                return ParseResult{*e};
            if (auto e = expect_kw("for")) return ParseResult{*e};
            if (auto e = expect_table_name("a table name after FOR", st.changes.table))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("materialized")) {  // CREATE MATERIALIZED VIEW name AS SELECT ...
            advance();  // MATERIALIZED
            if (auto e = expect_kw("view")) return ParseResult{*e};
            return parse_create_matview(/*incremental=*/false);
        }
        if (is_kw("incremental")) {  // K5: CREATE INCREMENTAL MATERIALIZED VIEW (pg_ivm shape)
            advance();
            if (auto e = expect_kw("materialized")) return ParseResult{*e};
            if (auto e = expect_kw("view")) return ParseResult{*e};
            return parse_create_matview(/*incremental=*/true);
        }
        if (is_kw("unique")) {  // E5: CREATE UNIQUE INDEX
            advance();  // UNIQUE
            if (!is_kw("index")) return err("expected INDEX after CREATE UNIQUE");
            return parse_create_index(/*unique=*/true);  // consumes INDEX itself
        }
        if (is_kw("index")) {
            return parse_create_index(/*unique=*/false);
        }
        if (is_kw("schema")) {  // E4: CREATE SCHEMA [IF NOT EXISTS] s
            advance();
            Statement st;
            st.kind = StmtKind::CreateSchema;
            if (is_kw("if")) {
                advance();
                if (auto e = expect_kw("not")) return ParseResult{*e};
                if (auto e = expect_kw("exists")) return ParseResult{*e};
                st.schema_if_not_exists = true;
            }
            if (auto e = expect_ident("a schema name", st.schema_arg)) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (auto e = expect_kw("table")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::Create;
        if (is_kw("if")) {  // E2: CREATE TABLE IF NOT EXISTS
            advance();
            if (auto e = expect_kw("not")) return ParseResult{*e};
            if (auto e = expect_kw("exists")) return ParseResult{*e};
            st.create.if_not_exists = true;
        }
        if (auto e = expect_table_name("a table name after CREATE TABLE", st.create.table)) {
            return ParseResult{*e};
        }
        // E2: CREATE TABLE t LIKE other — copy other's schema (no column list, no data).
        if (is_kw("like")) {
            advance();
            if (auto e = expect_table_name("a source table name after LIKE", st.create.like_table)) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        // E3: CREATE TABLE t AS SELECT ... — populate from a query.
        if (is_kw("as")) {
            advance();
            if (!is_kw("select")) return err("expected SELECT after CREATE TABLE ... AS");
            auto sel = std::make_shared<SelectStmt>();  // parse_select_stmt consumes SELECT
            if (auto e = parse_select_stmt(*sel)) return ParseResult{*e};
            st.create.as_select = std::move(sel);
            return ParseResult{std::move(st)};
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
                // F1: a single OR composite PK — one or more column names.
                for (;;) {
                    std::string pkc;
                    if (auto e = expect_ident("a primary-key column name", pkc)) {
                        return ParseResult{*e};
                    }
                    st.create.pk_columns.push_back(pkc);
                    if (cur_.kind == Tok::Comma) {
                        advance();
                        continue;
                    }
                    break;
                }
                st.create.pk_column = st.create.pk_columns.front();  // back-compat (first PK col)
                if (auto e = expect(Tok::RParen, "')' after the PRIMARY KEY column(s)")) {
                    return ParseResult{*e};
                }
                seen_pk_clause = true;
                break;  // PRIMARY KEY is the last clause
            }
            // F5: a TABLE-level CHECK constraint (must precede the PRIMARY KEY clause, which breaks),
            // optionally named: `CONSTRAINT <name> CHECK (...)`.
            if (is_kw("constraint") || is_kw("check")) {
                std::string cname;
                if (is_kw("constraint")) {
                    advance();
                    if (auto e = expect_ident("a constraint name after CONSTRAINT", cname)) {
                        return ParseResult{*e};
                    }
                }
                if (!is_kw("check")) {
                    return err("only named CHECK constraints are supported at the table level "
                               "(name a UNIQUE / FOREIGN KEY at the column, or use ALTER)");
                }
                if (auto e = parse_check(st.create, cname)) return ParseResult{*e};
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
            // A column definition: <name> <TYPE>
            Column col;
            if (auto e = expect_ident("a column name", col.name)) {
                return ParseResult{*e};
            }
            if (auto e = parse_column_type(col)) {
                return ParseResult{*e};
            }
            // F10: optional UNSIGNED (a non-negativity constraint; e.g. `BIGINT UNSIGNED`).
            if (is_kw("unsigned")) {
                advance();
                col.is_unsigned = true;
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
            // F2: optional UNIQUE constraint.
            if (is_kw("unique")) {
                advance();
                col.unique = true;
            }
            // F3: optional REFERENCES <table> [( <col> )] — a foreign key to the parent's PK.
            if (is_kw("references")) {
                advance();
                if (auto e = expect_table_name("a referenced table after REFERENCES", col.fk_table)) {
                    return ParseResult{*e};
                }
                if (cur_.kind == Tok::LParen) {
                    advance();
                    if (auto e = expect_ident("a referenced column", col.fk_column)) {
                        return ParseResult{*e};
                    }
                    if (auto e = expect(Tok::RParen, "')' after the referenced column")) {
                        return ParseResult{*e};
                    }
                }
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
                // F9c: DEFAULT gen_uuid() / uuid() — a deterministic generated default (UUID column).
                if (is_kw("gen_uuid") || is_kw("gen_random_uuid")) {
                    if (col.logical != 4) {
                        return err("gen_uuid() DEFAULT requires a UUID column ('" + col.name + "')");
                    }
                    advance();
                    if (auto e = expect(Tok::LParen, "'(' after gen_uuid")) return ParseResult{*e};
                    if (auto e = expect(Tok::RParen, "')' after gen_uuid(")) return ParseResult{*e};
                    col.uuid_default = true;
                } else {
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
            }
            // F5: a COLUMN-level CHECK (e.g. `age INT CHECK (age >= 0)`) — same store as table-level.
            if (is_kw("check")) {
                if (auto e = parse_check(st.create, /*name=*/"")) return ParseResult{*e};
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

    // H1: CREATE [OR REPLACE] VIEW <name> [(<col>, ...)] AS <SELECT ...>. The view body is captured
    // as its RAW SELECT source text (from the SELECT keyword to end-of-statement) — it is re-parsed
    // on every reference, so no AST is stored (durable + version-independent). `is_kw("view")` is
    // still current on entry.
    ParseResult parse_create_view(bool or_replace) {
        advance();  // VIEW
        Statement st;
        st.kind = StmtKind::CreateView;
        st.create_view.or_replace = or_replace;
        if (is_kw("if")) {  // CREATE VIEW IF NOT EXISTS
            advance();
            if (auto e = expect_kw("not")) return ParseResult{*e};
            if (auto e = expect_kw("exists")) return ParseResult{*e};
            st.create_view.if_not_exists = true;
        }
        if (auto e = expect_table_name("a view name after CREATE VIEW", st.create_view.name)) {
            return ParseResult{*e};
        }
        if (cur_.kind == Tok::LParen) {  // optional explicit output-column list
            advance();
            for (;;) {
                std::string col;
                if (auto e = expect_ident("a column name in the view column list", col)) return ParseResult{*e};
                st.create_view.columns.push_back(std::move(col));
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            if (auto e = expect(Tok::RParen, "')' to close the view column list")) return ParseResult{*e};
        }
        if (auto e = expect_kw("as")) return ParseResult{*e};
        if (!is_kw("select")) {
            return err("expected SELECT after CREATE VIEW ... AS");
        }
        // Capture the raw SELECT source text (re-parsed on reference). Validate it parses NOW so a
        // malformed view is rejected at definition time, not at first use.
        const std::size_t start = cur_.pos;
        SelectStmt probe;
        if (auto e = parse_select_stmt(probe)) return ParseResult{*e};
        const std::size_t end = cur_.pos;  // at the trailing ';' or end-of-input
        std::string text = lex_.src().substr(start, end > start ? end - start : 0);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                 text.back() == '\n' || text.back() == '\r' || text.back() == ';')) {
            text.pop_back();
        }
        st.create_view.select_src = std::move(text);
        return ParseResult{std::move(st)};
    }

    // CREATE MATERIALIZED VIEW name AS SELECT ... — materialize the query into a table AND
    // store the raw SELECT text so REFRESH MATERIALIZED VIEW can recompute it.
    ParseResult parse_create_matview(bool incremental) {
        Statement st;
        st.kind = StmtKind::Create;
        st.create.materialized = true;
        st.create.incremental = incremental;
        if (is_kw("if")) {  // IF NOT EXISTS
            advance();
            if (auto e = expect_kw("not")) return ParseResult{*e};
            if (auto e = expect_kw("exists")) return ParseResult{*e};
            st.create.if_not_exists = true;
        }
        if (auto e = expect_table_name("a name after CREATE MATERIALIZED VIEW", st.create.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("as")) return ParseResult{*e};
        if (!is_kw("select")) return err("expected SELECT after CREATE MATERIALIZED VIEW ... AS");
        const std::size_t start = cur_.pos;
        auto sel = std::make_shared<SelectStmt>();
        if (auto e = parse_select_stmt(*sel)) return ParseResult{*e};
        const std::size_t end = cur_.pos;
        std::string text = lex_.src().substr(start, end > start ? end - start : 0);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                 text.back() == '\n' || text.back() == '\r' || text.back() == ';')) {
            text.pop_back();
        }
        st.create.source_sql = std::move(text);
        st.create.as_select = std::move(sel);
        return ParseResult{std::move(st)};
    }

    // CREATE INDEX <name> ON <table> (<col>) — single-column secondary index. A
    // multi-column list (a comma after the column) is a clean "OUT in v1" error.
    ParseResult parse_create_index(bool unique) {
        advance();  // INDEX
        Statement st;
        st.kind = StmtKind::CreateIndex;
        st.create_index.unique = unique;  // E5
        if (auto e = expect_ident("an index name after CREATE INDEX",
                                  st.create_index.index)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("on")) {
            return ParseResult{*e};
        }
        if (auto e = expect_table_name("a table name after ON", st.create_index.table)) {
            return ParseResult{*e};
        }
        if (auto e = expect(Tok::LParen, "'(' before the indexed column")) {
            return ParseResult{*e};
        }
        // J2: EXPRESSION index — `CREATE INDEX ... ON t ((expr))`. The double paren (an inner '('
        // right after the outer one) disambiguates an expression from a column list. We capture the
        // BARE expression source (without the inner parens) so it matches the per-row LHS a query
        // writes (`WHERE doc->>'k' = 'red'` => leaf expr source "doc->>'k'").
        if (cur_.kind == Tok::LParen) {
            advance();  // consume the inner '('
            const std::size_t start = cur_.pos;
            std::shared_ptr<Expr> ex;
            if (auto e = parse_scalar_expr(ex)) return ParseResult{*e};
            std::string text = lex_.src().substr(start, cur_.pos > start ? cur_.pos - start : 0);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
            st.create_index.expr_src = std::move(text);
            if (auto e = expect(Tok::RParen, "')' to close the indexed expression")) {
                return ParseResult{*e};
            }
            if (auto e = expect(Tok::RParen, "')' after the indexed expression")) {
                return ParseResult{*e};
            }
        } else {
            // E5: one OR MORE indexed columns (composite index).
            for (;;) {
                std::string col;
                if (auto e = expect_ident("an indexed column name", col)) return ParseResult{*e};
                st.create_index.columns.push_back(col);
                // K1.3c: an optional pgvector OPERATOR CLASS after the column name
                // (`(emb vector_cosine_ops)`). Only the three vector opclasses are known.
                if (cur_.kind == Tok::Ident) {
                    const std::string oc = lower(cur_.text);
                    if (oc == "vector_l2_ops") st.create_index.vec_op = 0;
                    else if (oc == "vector_cosine_ops") st.create_index.vec_op = 1;
                    else if (oc == "vector_ip_ops") st.create_index.vec_op = 2;
                    else return err("unknown operator class '" + cur_.text + "'");
                    advance();
                }
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            st.create_index.column = st.create_index.columns.front();  // leading column
            if (auto e = expect(Tok::RParen, "')' after the indexed columns")) {
                return ParseResult{*e};
            }
        }
        if (is_kw("using")) {  // I7: HASH | BTREE ; J3: GIN ; K1.3: IVFFLAT ; K1.4: HNSW
            advance();
            if (is_kw("hash")) { advance(); st.create_index.hash = true; }
            else if (is_kw("btree")) { advance(); }
            else if (is_kw("gin")) { advance(); st.create_index.gin = true; }
            else if (is_kw("ivfflat")) { advance(); st.create_index.ivfflat = true; }
            else if (is_kw("hnsw")) { advance(); st.create_index.hnsw = true; }
            else if (is_kw("bm25")) { advance(); st.create_index.bm25 = true; }  // K2
            else return err("USING expects HASH, BTREE, GIN, IVFFLAT, HNSW, or BM25");
        }
        // K1.3/K1.4: WITH (knob = N, ...) — the vector-index build/search knobs (pgvector shape):
        // IVFFLAT takes lists/probes; HNSW takes m/ef_construction.
        if ((st.create_index.ivfflat || st.create_index.hnsw) && is_kw("with")) {
            advance();
            if (auto e = expect(Tok::LParen, "'(' after WITH")) return ParseResult{*e};
            for (;;) {
                std::string knob;
                if (auto e = expect_ident("a vector-index option", knob)) return ParseResult{*e};
                if (auto e = expect(Tok::Eq, "'=' after the option name")) return ParseResult{*e};
                if (cur_.kind != Tok::IntLit || cur_.int_val < 1 || cur_.int_val > 32768)
                    return err("the vector-index option value must be an integer in 1..32768");
                const auto v = static_cast<std::uint32_t>(cur_.int_val);
                advance();
                const std::string lk = lower(knob);
                if (st.create_index.ivfflat && lk == "lists") st.create_index.lists = v;
                else if (st.create_index.ivfflat && lk == "probes") st.create_index.probes = v;
                else if (st.create_index.hnsw && lk == "m") st.create_index.hnsw_m = v;
                else if (st.create_index.hnsw && lk == "ef_construction") st.create_index.hnsw_efc = v;
                else return err("unknown option '" + knob + "' (IVFFLAT: lists, probes; HNSW: m, ef_construction)");
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
            if (auto e = expect(Tok::RParen, "')' to close WITH (...)")) return ParseResult{*e};
        }
        if (is_kw("where")) {  // I5: PARTIAL index — capture the predicate's source text (re-parsed).
            advance();
            const std::size_t start = cur_.pos;
            Predicate tmp;
            if (auto e = parse_predicate(tmp, /*allow_agg=*/false)) return ParseResult{*e};
            std::string text = lex_.src().substr(start, cur_.pos > start ? cur_.pos - start : 0);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
            st.create_index.partial_src = std::move(text);
        }
        return ParseResult{std::move(st)};
    }

    // DROP INDEX <name> ON <table>  |  DROP TABLE <table>  (F8)
    ParseResult parse_drop_index() {
        advance();  // DROP
        auto parse_if_exists = [&]() -> bool {
            if (is_kw("if")) { advance(); (void)expect_kw("exists"); return true; }
            return false;
        };
        if (is_kw("queue")) {  // K3: DROP QUEUE q
            advance();
            Statement st;
            st.kind = StmtKind::DropQueue;
            if (auto e = expect_ident("a queue name after DROP QUEUE", st.queue.queue))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("changefeed")) {  // K4.4: DROP CHANGEFEED cf
            advance();
            Statement st;
            st.kind = StmtKind::DropChangefeed;
            if (auto e = expect_ident("a changefeed name", st.changes.feed))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("schema")) {  // E4: DROP SCHEMA [IF EXISTS] s
            advance();
            Statement st;
            st.kind = StmtKind::DropSchema;
            st.schema_if_exists = parse_if_exists();
            if (auto e = expect_ident("a schema name", st.schema_arg)) return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("table")) {
            advance();  // TABLE
            Statement st;
            st.kind = StmtKind::DropTable;
            st.drop_table.if_exists = parse_if_exists();  // E2
            if (auto e = expect_table_name("a table name after DROP TABLE", st.drop_table.table)) {
                return ParseResult{*e};
            }
            return ParseResult{std::move(st)};
        }
        if (is_kw("materialized")) {  // DROP MATERIALIZED VIEW [IF EXISTS] name — drops the table
            advance();  // MATERIALIZED
            if (auto e = expect_kw("view")) return ParseResult{*e};
            Statement st;
            st.kind = StmtKind::DropTable;
            st.drop_table.if_exists = parse_if_exists();
            if (auto e = expect_table_name("a name after DROP MATERIALIZED VIEW", st.drop_table.table))
                return ParseResult{*e};
            return ParseResult{std::move(st)};
        }
        if (is_kw("view")) {  // H1: DROP VIEW [IF EXISTS] name
            advance();  // VIEW
            Statement st;
            st.kind = StmtKind::DropView;
            st.drop_view.if_exists = parse_if_exists();
            if (auto e = expect_table_name("a view name after DROP VIEW", st.drop_view.name)) {
                return ParseResult{*e};
            }
            return ParseResult{std::move(st)};
        }
        if (auto e = expect_kw("index")) {
            return ParseResult{*e};
        }
        Statement st;
        st.kind = StmtKind::DropIndex;
        st.drop_index.if_exists = parse_if_exists();  // E2
        if (auto e = expect_ident("an index name after DROP INDEX",
                                  st.drop_index.index)) {
            return ParseResult{*e};
        }
        if (auto e = expect_kw("on")) {
            return ParseResult{*e};
        }
        if (auto e = expect_table_name("a table name after ON", st.drop_index.table)) {
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
        if (auto e = expect_table_name("a table name after INSERT INTO", st.insert.table)) {
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
            if (is_kw("returning")) {
                advance();
                for (;;) {
                    if (cur_.kind == Tok::Star) { st.insert.returning.push_back("*"); advance(); }
                    else {
                        std::string rq, rc;
                        if (auto e = expect_qualified_column("a column after RETURNING", rq, rc))
                            return ParseResult{*e};
                        if (is_kw("as")) { advance(); if (cur_.kind == Tok::Ident) advance(); }
                        st.insert.returning.push_back(rc);
                    }
                    if (cur_.kind == Tok::Comma) { advance(); continue; }
                    break;
                }
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
        if (is_kw("returning")) {
            advance();
            for (;;) {
                if (cur_.kind == Tok::Star) { st.insert.returning.push_back("*"); advance(); }
                else {
                    std::string rq, rc;
                    if (auto e = expect_qualified_column("a column after RETURNING", rq, rc))
                        return ParseResult{*e};
                    if (is_kw("as")) { advance(); if (cur_.kind == Tok::Ident) advance(); }
                    st.insert.returning.push_back(rc);
                }
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
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
        if (auto e = expect_table_name("a table name after UPDATE", st.update.table)) {
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
        if (auto e = expect_table_name("a table name after DELETE FROM", st.del.table)) {
            return ParseResult{*e};
        }
        if (auto e = parse_pk_eq_where(st.del.where_column, st.del.where_value)) {
            return ParseResult{*e};
        }
        if (is_kw("returning")) {  // ORM-compat: INSERT ... RETURNING col[, ...]
            advance();
            for (;;) {
                if (cur_.kind == Tok::Star) {
                    st.insert.returning.push_back("*");
                    advance();
                } else {
                    std::string rq, rc;
                    if (auto e = expect_qualified_column("a column after RETURNING", rq, rc))
                        return ParseResult{*e};
                    if (is_kw("as")) {  // aliases: keep the source column
                        advance();
                        if (cur_.kind == Tok::Ident) advance();
                    }
                    st.insert.returning.push_back(rc);
                }
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
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
        } else if (fn == "array_agg") {
            kind = AggKind::ArrayAgg;  // F12
        } else if (fn == "json_agg") {
            kind = AggKind::JsonAgg;
        } else if (fn == "string_agg" || fn == "group_concat") {
            kind = AggKind::StringAgg;
        } else if (fn == "bool_and") {
            kind = AggKind::BoolAnd;
        } else if (fn == "bool_or") {
            kind = AggKind::BoolOr;
        } else if (fn == "bit_and") {
            kind = AggKind::BitAnd;
        } else if (fn == "bit_or") {
            kind = AggKind::BitOr;
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
            // STRING_AGG(col, 'delim') — an optional string-literal separator (default ",").
            if (kind == AggKind::StringAgg && cur_.kind == Tok::Comma) {
                advance();
                Datum d;
                if (auto e = expect_literal(d)) return e;
                if (d.type != Type::Text) return make_err("STRING_AGG separator must be a string");
                agg.delim = d.s;
            }
            const std::string body = (qual.empty() ? col : qual + "." + col);
            label = upper(fn) + "(" + (distinct ? "DISTINCT " : "") + body + ")";
        }
        if (auto e = expect(Tok::RParen, "')' to close the aggregate")) {
            return e;
        }
        // FILTER (WHERE <predicate>) — only rows passing the predicate contribute (conditional agg).
        if (is_kw("filter")) {
            advance();
            if (auto e = expect(Tok::LParen, "'(' after FILTER")) return e;
            if (auto e = expect_kw("where")) return e;
            auto pred = std::make_shared<Predicate>();
            if (auto e = parse_predicate(*pred, /*allow_agg=*/false)) return e;
            if (auto e = expect(Tok::RParen, "')' to close the FILTER clause")) return e;
            agg.filter = std::move(pred);
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

    // C3: parse an OVER ( [PARTITION BY ...] [ORDER BY ...] ) clause into a WindowFunc.
    [[nodiscard]] std::optional<ParseError> parse_over(WindowFunc& w) {
        if (auto e = expect_kw("over")) return e;
        if (auto e = expect(Tok::LParen, "'(' after OVER")) return e;
        if (is_kw("partition")) {
            advance();
            if (auto e = expect_kw("by")) return e;
            for (;;) {
                std::string q, c;
                if (auto e = expect_qualified_column("a PARTITION BY column", q, c)) return e;
                w.partition_by.push_back(q.empty() ? c : q + "." + c);
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
        }
        if (is_kw("order")) {
            advance();
            if (auto e = expect_kw("by")) return e;
            for (;;) {
                OrderKey k;
                if (auto e = expect_qualified_column("an ORDER BY column", k.qualifier, k.column))
                    return e;
                if (is_kw("asc")) advance();
                else if (is_kw("desc")) { k.descending = true; advance(); }
                w.order_by.push_back(std::move(k));
                if (cur_.kind == Tok::Comma) { advance(); continue; }
                break;
            }
        }
        // Optional ROWS frame: ROWS BETWEEN <start> AND <end> | ROWS <start> (= .. AND CURRENT ROW).
        if (is_kw("rows")) {
            advance();
            w.has_frame = true;
            if (is_kw("between")) {
                advance();
                if (auto e = parse_frame_bound(w.frame_start)) return e;
                if (auto e = expect_kw("and")) return e;
                if (auto e = parse_frame_bound(w.frame_end)) return e;
            } else {
                if (auto e = parse_frame_bound(w.frame_start)) return e;
                w.frame_end.kind = FrameBoundKind::CurrentRow;  // single bound -> up to current row
            }
        }
        return expect(Tok::RParen, "')' to close OVER");
    }

    // Parse one ROWS-frame bound: UNBOUNDED PRECEDING | CURRENT ROW | n PRECEDING | n FOLLOWING |
    // UNBOUNDED FOLLOWING.
    [[nodiscard]] std::optional<ParseError> parse_frame_bound(FrameBound& b) {
        if (is_kw("unbounded")) {
            advance();
            if (is_kw("preceding")) { advance(); b.kind = FrameBoundKind::UnboundedPreceding; return std::nullopt; }
            if (is_kw("following")) { advance(); b.kind = FrameBoundKind::UnboundedFollowing; return std::nullopt; }
            return make_err("expected PRECEDING/FOLLOWING after UNBOUNDED");
        }
        if (is_kw("current")) {
            advance();
            if (auto e = expect_kw("row")) return e;
            b.kind = FrameBoundKind::CurrentRow;
            return std::nullopt;
        }
        Datum d;
        if (auto e = expect_literal(d)) return e;
        if (d.type != Type::Int || d.i < 0) return make_err("a frame offset must be a non-negative integer");
        b.offset = d.i;
        if (is_kw("preceding")) { advance(); b.kind = FrameBoundKind::Preceding; return std::nullopt; }
        if (is_kw("following")) { advance(); b.kind = FrameBoundKind::Following; return std::nullopt; }
        return make_err("expected PRECEDING/FOLLOWING after the frame offset");
    }

    // C3: try to parse a WINDOW function at the cursor — ROW_NUMBER()/RANK() OVER (...) or an
    // aggregate with a trailing OVER. Sets matched + fills `item` (kind Window). For an aggregate
    // call WITHOUT a trailing OVER, leaves matched=false AND fills `agg`/`agg_label` so the caller
    // can emit a plain aggregate item (single-token lookahead means we must commit to parsing the
    // call here). `consumed_agg` says the call was an aggregate already parsed into agg/agg_label.
    [[nodiscard]] std::optional<ParseError> parse_window_or_agg(SelectItem& item, bool& is_window,
                                                               AggExpr& agg, bool& is_agg,
                                                               std::string& agg_label) {
        is_window = false;
        is_agg = false;
        // ROW_NUMBER() / RANK() / DENSE_RANK() — window-only, no arguments.
        if (is_kw("row_number") || is_kw("rank") || is_kw("dense_rank")) {
            const std::string fn = lower(cur_.text);
            advance();
            if (auto e = expect(Tok::LParen, "'(' after the window function")) return e;
            if (auto e = expect(Tok::RParen, "')' (ROW_NUMBER/RANK/DENSE_RANK take no arguments)")) return e;
            auto w = std::make_shared<WindowFunc>();
            w->kind = fn == "rank" ? WinKind::Rank
                    : fn == "dense_rank" ? WinKind::DenseRank
                                         : WinKind::RowNumber;
            if (auto e = parse_over(*w)) return e;
            item.kind = SelectItemKind::Window;
            item.win = std::move(w);
            item.label = upper(fn) + "()";
            is_window = true;
            return std::nullopt;
        }
        // Column-argument window functions: LAG / LEAD / FIRST_VALUE / LAST_VALUE (col) OVER.
        if (is_kw("lag") || is_kw("lead") || is_kw("first_value") || is_kw("last_value")) {
            const std::string fn = lower(cur_.text);
            advance();
            if (auto e = expect(Tok::LParen, "'(' after the window function")) return e;
            std::string qual, col;
            if (auto e = expect_qualified_column("a column inside the window function", qual, col)) return e;
            if (auto e = expect(Tok::RParen, "')' to close the window function")) return e;
            auto w = std::make_shared<WindowFunc>();
            w->kind = fn == "lag" ? WinKind::Lag
                    : fn == "lead" ? WinKind::Lead
                    : fn == "first_value" ? WinKind::FirstValue
                                          : WinKind::LastValue;
            w->arg_column = col;
            if (auto e = parse_over(*w)) return e;
            item.kind = SelectItemKind::Window;
            item.win = std::move(w);
            item.label = upper(fn) + "(" + col + ")";
            is_window = true;
            return std::nullopt;
        }
        // NTILE(n) OVER — bucket the ordered partition into n groups (n is an integer literal).
        if (is_kw("ntile")) {
            advance();
            if (auto e = expect(Tok::LParen, "'(' after NTILE")) return e;
            Datum d;
            if (auto e = expect_literal(d)) return e;
            if (d.type != Type::Int || d.i < 1) return make_err("NTILE(n) needs a positive integer");
            if (auto e = expect(Tok::RParen, "')' to close NTILE")) return e;
            auto w = std::make_shared<WindowFunc>();
            w->kind = WinKind::Ntile;
            w->ntile_n = d.i;
            if (auto e = parse_over(*w)) return e;
            item.kind = SelectItemKind::Window;
            item.win = std::move(w);
            item.label = "NTILE(" + std::to_string(d.i) + ")";
            is_window = true;
            return std::nullopt;
        }
        // An aggregate call — may be windowed (trailing OVER) or a plain aggregate.
        if (auto e = parse_agg(agg, is_agg, agg_label)) return e;
        if (is_agg && is_kw("over")) {
            auto w = std::make_shared<WindowFunc>();
            switch (agg.kind) {
                case AggKind::CountStar: w->kind = WinKind::CountStar; break;
                case AggKind::Count: w->kind = WinKind::Count; break;
                case AggKind::Sum: w->kind = WinKind::Sum; break;
                case AggKind::Min: w->kind = WinKind::Min; break;
                case AggKind::Max: w->kind = WinKind::Max; break;
                case AggKind::Avg: w->kind = WinKind::Avg; break;
                default: return make_err("that aggregate is not supported as a window function");
            }
            w->arg_column = agg.column;
            if (auto e = parse_over(*w)) return e;
            item.kind = SelectItemKind::Window;
            item.win = std::move(w);
            item.label = agg_label + " OVER";
            is_window = true;
            is_agg = false;  // it became a window, not a plain aggregate
        }
        return std::nullopt;
    }

    // A readable, deterministic output label for an UNALIASED projected expression.
    static std::string expr_label(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Param:
                return "$" + std::to_string(e.param_idx);
            case ExprKind::Col:
                return e.qualifier.empty() ? e.column : e.qualifier + "." + e.column;
            case ExprKind::Lit:
                return e.lit.is_null ? "NULL"
                                     : (e.lit.type == Type::Int ? std::to_string(e.lit.i) : e.lit.s);
            case ExprKind::Func:
                return e.func == "CAST" ? "CAST" : e.func;
            case ExprKind::Case:
                return "CASE";
            case ExprKind::Neg:
                return "-expr";
            case ExprKind::Bin:
                return "expr";
            case ExprKind::Array:
                return "array";
            case ExprKind::Subscript:
                return "element";
        }
        return "expr";
    }

    // ---- A1-A4 scalar-expression grammar (precedence: add < mul < unary < primary) -----------
    static std::shared_ptr<Expr> mk_expr(ExprKind k) {
        auto e = std::make_shared<Expr>();
        e->kind = k;
        return e;
    }
    [[nodiscard]] std::optional<ParseError> parse_scalar_expr(std::shared_ptr<Expr>& out) {
        if (auto e = parse_expr_add(out)) return e;
        // K1.2: pgvector distance operators `a <-> b` / `a <#> b` / `a <=> b`, lowest scalar
        // precedence (they bind looser than +/-, like generic PostgreSQL operators). Each lowers
        // to a Func node the engine's distance kernel evaluates ("<#>" is the NEGATIVE inner
        // product, pgvector's convention).
        for (;;) {
            if (cur_.kind != Tok::DistL2 && cur_.kind != Tok::DistIp && cur_.kind != Tok::DistCos)
                break;
            const Tok op = cur_.kind;
            advance();
            std::shared_ptr<Expr> r;
            if (auto e = parse_expr_add(r)) return e;
            auto n = mk_expr(ExprKind::Func);
            n->func = op == Tok::DistL2 ? "<->" : (op == Tok::DistIp ? "<#>" : "<=>");
            n->args.push_back(out);
            n->args.push_back(r);
            out = n;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> parse_expr_add(std::shared_ptr<Expr>& out) {
        if (auto e = parse_expr_mul(out)) return e;
        for (;;) {
            BinOp op;
            if (cur_.kind == Tok::Plus) {
                op = BinOp::Add;
                advance();
            } else if (cur_.kind == Tok::Minus) {
                op = BinOp::Sub;
                advance();
            } else if (cur_.kind == Tok::IntLit && cur_.int_val < 0) {
                // A glued negative literal (`x-5` lexes x then IntLit(-5)) is a subtraction.
                auto r = mk_expr(ExprKind::Lit);
                r->lit = Datum::make_int(-cur_.int_val);
                advance();
                auto n = mk_expr(ExprKind::Bin);
                n->op = BinOp::Sub;
                n->left = out;
                n->right = r;
                out = n;
                continue;
            } else {
                break;
            }
            std::shared_ptr<Expr> r;
            if (auto e = parse_expr_mul(r)) return e;
            auto n = mk_expr(ExprKind::Bin);
            n->op = op;
            n->left = out;
            n->right = r;
            out = n;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> parse_expr_mul(std::shared_ptr<Expr>& out) {
        if (auto e = parse_expr_unary(out)) return e;
        for (;;) {
            BinOp op;
            if (cur_.kind == Tok::Star) {
                op = BinOp::Mul;
            } else if (cur_.kind == Tok::Slash) {
                op = BinOp::Div;
            } else if (cur_.kind == Tok::Percent) {
                op = BinOp::Mod;
            } else {
                break;
            }
            advance();
            std::shared_ptr<Expr> r;
            if (auto e = parse_expr_unary(r)) return e;
            auto n = mk_expr(ExprKind::Bin);
            n->op = op;
            n->left = out;
            n->right = r;
            out = n;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> parse_expr_unary(std::shared_ptr<Expr>& out) {
        if (cur_.kind == Tok::Minus) {
            advance();
            std::shared_ptr<Expr> c;
            if (auto e = parse_expr_unary(c)) return e;
            auto n = mk_expr(ExprKind::Neg);
            n->left = c;
            out = n;
            return std::nullopt;
        }
        return parse_expr_primary(out);
    }
    // F12: a primary expression followed by zero or more `[index]` array subscripts (1-based).
    [[nodiscard]] std::optional<ParseError> parse_expr_primary(std::shared_ptr<Expr>& out) {
        if (auto e = parse_expr_atom(out)) return e;
        for (;;) {
            if (cur_.kind == Tok::LBracket) {
                advance();
                std::shared_ptr<Expr> idx;
                if (auto e = parse_scalar_expr(idx)) return e;
                if (auto e = expect(Tok::RBracket, "']' to close an array subscript")) return e;
                auto sub = mk_expr(ExprKind::Subscript);
                sub->left = out;
                sub->right = idx;
                out = sub;
            } else if (cur_.kind == Tok::DblColon) {
                absorb_pg_cast();  // ORM-compat: `expr::type` keeps expr as-is
            } else if (cur_.kind == Tok::Arrow || cur_.kind == Tok::ArrowText) {
                // F13: JSON access `json -> key`/`json ->> key` -> a Func over (json, key).
                const bool as_text = cur_.kind == Tok::ArrowText;
                advance();
                std::shared_ptr<Expr> key;
                if (auto e = parse_expr_atom(key)) return e;
                auto g = mk_expr(ExprKind::Func);
                g->func = as_text ? "->>" : "->";
                g->args.push_back(out);
                g->args.push_back(key);
                out = g;
            } else if (cur_.kind == Tok::PathArrow || cur_.kind == Tok::PathArrowText) {
                // JSON path access `json #> path` / `json #>> path` -> a Func over (json, path). The
                // path is a TEXT array of keys/indices ('{a,1,b}' or ARRAY['a','1','b']).
                const bool as_text = cur_.kind == Tok::PathArrowText;
                advance();
                std::shared_ptr<Expr> path;
                if (auto e = parse_expr_atom(path)) return e;
                auto g = mk_expr(ExprKind::Func);
                g->func = as_text ? "#>>" : "#>";
                g->args.push_back(out);
                g->args.push_back(path);
                out = g;
            } else {
                break;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<ParseError> parse_expr_atom(std::shared_ptr<Expr>& out) {
        if (cur_.kind == Tok::LParen) {
            advance();
            if (auto e = parse_scalar_expr(out)) return e;
            return expect(Tok::RParen, "')' to close a parenthesized expression");
        }
        if (cur_.kind == Tok::IntLit) {
            auto n = mk_expr(ExprKind::Lit);
            n->lit = cur_.int_overflow ? Datum::make_text(cur_.text)  // F11
                                       : Datum::make_int(cur_.int_val);
            advance();
            out = n;
            return std::nullopt;
        }
        if (cur_.kind == Tok::FloatLit) {  // F14: a REAL literal in an expression (e.g. x * 1.5)
            double d = 0.0;
            if (!parse_double_strict(cur_.text.data(), cur_.text.data() + cur_.text.size(), d)) {
                return make_err("malformed floating-point literal '" + cur_.text + "'");
            }
            auto n = mk_expr(ExprKind::Lit);
            n->lit = Datum::make_real(d);
            advance();
            out = n;
            return std::nullopt;
        }
        if (cur_.kind == Tok::Param) {  // K4.13: $N placeholder
            auto n = mk_expr(ExprKind::Param);
            n->param_idx = static_cast<std::int32_t>(cur_.int_val);
            advance();
            out = n;
            return std::nullopt;
        }
        // F13: INTERVAL '...' literal in an expression (e.g. ts + INTERVAL '1 day').
        if (is_kw("interval")) {
            advance();
            if (cur_.kind != Tok::StrLit) return make_err("INTERVAL requires a 'string' literal");
            std::int64_t months = 0, secs = 0;
            if (!parse_interval_my(cur_.text, months, secs))
                return make_err("invalid INTERVAL literal '" + cur_.text + "'");
            if (months != 0 && secs != 0)
                return make_err("an INTERVAL literal cannot mix months/years with days/time ('" +
                                cur_.text + "'); use separate INTERVALs");
            auto n = mk_expr(ExprKind::Lit);
            // F13b: a year/month interval is logical 12 (the int holds months); else logical 10 (secs).
            n->lit = Datum::make_int(months != 0 ? months : secs);
            n->lit.logical = months != 0 ? 12 : 10;
            advance();
            out = n;
            return std::nullopt;
        }
        // F12: ARRAY[e0, e1, ...] literal.
        if (is_kw("array")) {
            advance();
            if (auto e = expect(Tok::LBracket, "'[' after ARRAY")) return e;
            auto n = mk_expr(ExprKind::Array);
            if (cur_.kind != Tok::RBracket) {
                for (;;) {
                    std::shared_ptr<Expr> el;
                    if (auto e = parse_scalar_expr(el)) return e;
                    n->args.push_back(el);
                    if (cur_.kind == Tok::Comma) { advance(); continue; }
                    break;
                }
            }
            if (auto e = expect(Tok::RBracket, "']' to close an ARRAY literal")) return e;
            out = n;
            return std::nullopt;
        }
        if (cur_.kind == Tok::StrLit) {
            auto n = mk_expr(ExprKind::Lit);
            n->lit = Datum::make_text(cur_.text);
            advance();
            out = n;
            return std::nullopt;
        }
        if (is_kw("null")) {
            advance();
            auto n = mk_expr(ExprKind::Lit);
            n->lit = Datum::make_null(Type::Int);
            out = n;
            return std::nullopt;
        }
        if (is_kw("case")) {
            return parse_case_expr(out);
        }
        if (is_kw("cast")) {
            advance();
            if (auto e = expect(Tok::LParen, "'(' after CAST")) return e;
            auto n = mk_expr(ExprKind::Func);
            n->func = "CAST";
            std::shared_ptr<Expr> arg;
            if (auto e = parse_scalar_expr(arg)) return e;
            n->args.push_back(arg);
            if (auto e = expect_kw("as")) return e;
            if (is_kw("int")) {
                n->cast_type = Type::Int;
                advance();
            } else if (is_kw("text")) {
                n->cast_type = Type::Text;
                advance();
            } else {
                return make_err("CAST target must be INT or TEXT");
            }
            if (auto e = expect(Tok::RParen, "')' to close CAST")) return e;
            out = n;
            return std::nullopt;
        }
        if (cur_.kind == Tok::Ident) {
            const std::string name = cur_.text;
            advance();
            return parse_scalar_primary_named(name, out);
        }
        return make_err("expected a scalar expression");
    }

    // The Ident-headed tail of the scalar primary (function call or column ref),
    // with `name` ALREADY consumed — shared by the plain path and the ORM-compat
    // pg_catalog-qualified path (pg_catalog.version() strips its schema qualifier
    // and re-enters here).
    [[nodiscard]] std::optional<ParseError> parse_scalar_primary_named(
        const std::string& name, std::shared_ptr<Expr>& out) {
        {
            // A function call NAME '(' args ')'.
            if (cur_.kind == Tok::LParen) {
                advance();
                auto n = mk_expr(ExprKind::Func);
                n->func = upper(name);
                // W9: EXTRACT(field FROM expr) — SQL-standard syntax, rewritten to the
                // DATE_PART('field', expr) function that evaluates it.
                if (n->func == "EXTRACT") {
                    if (cur_.kind != Tok::Ident) {
                        return make_err("expected a field name after EXTRACT(");
                    }
                    const std::string field = lower(cur_.text);
                    advance();
                    if (!is_kw("from")) return make_err("expected FROM in EXTRACT(field FROM ...)");
                    advance();
                    std::shared_ptr<Expr> val;
                    if (auto e = parse_scalar_expr(val)) return e;
                    if (auto e = expect(Tok::RParen, "')' to close EXTRACT")) return e;
                    auto litf = mk_expr(ExprKind::Lit);
                    litf->lit = Datum::make_text(field);
                    n->func = "DATE_PART";
                    n->args.push_back(litf);
                    n->args.push_back(val);
                    out = n;
                    return std::nullopt;
                }
                if (cur_.kind != Tok::RParen) {
                    for (;;) {
                        std::shared_ptr<Expr> a;
                        if (auto e = parse_scalar_expr(a)) return e;
                        n->args.push_back(a);
                        if (cur_.kind == Tok::Comma) {
                            advance();
                            continue;
                        }
                        break;
                    }
                }
                if (auto e = expect(Tok::RParen, "')' to close a function call")) return e;
                out = n;
                return std::nullopt;
            }
            // A column reference (optionally qualified `table.col`) — or, ORM-compat,
            // a pg_catalog-qualified FUNCTION call (pg_catalog.version() etc.): the
            // schema qualifier is stripped and the call re-enters the primary parser.
            auto n = mk_expr(ExprKind::Col);
            n->column = name;
            if (cur_.kind == Tok::Dot) {
                advance();
                if (cur_.kind != Tok::Ident) return make_err("expected a column name after '.'");
                if (lower(name) == "pg_catalog") {
                    const std::string fname = cur_.text;
                    advance();
                    if (cur_.kind == Tok::Dot) {  // pg_catalog.table.col (3-part column)
                        advance();
                        if (cur_.kind != Tok::Ident) return make_err("expected a column after '.'");
                        n->qualifier = "pg_catalog." + fname;
                        n->column = cur_.text;
                        advance();
                        out = n;
                        return std::nullopt;
                    }
                    return parse_scalar_primary_named(fname, out);
                }
                n->qualifier = name;
                n->column = cur_.text;
                advance();
                if (cur_.kind == Tok::Dot) {  // schema.table.col (general 3-part)
                    advance();
                    if (cur_.kind != Tok::Ident) return make_err("expected a column after '.'");
                    n->qualifier = name + "." + n->column;
                    n->column = cur_.text;
                    advance();
                }
            }
            out = n;
            return std::nullopt;
        }
        return make_err("expected a scalar expression");
    }
    // CASE WHEN <pred> THEN <expr> [WHEN ...] [ELSE <expr>] END
    [[nodiscard]] std::optional<ParseError> parse_case_expr(std::shared_ptr<Expr>& out) {
        advance();  // CASE
        auto n = mk_expr(ExprKind::Case);
        if (!is_kw("when")) {
            return make_err("expected WHEN after CASE");
        }
        while (is_kw("when")) {
            advance();
            Predicate cond;
            if (auto e = parse_predicate(cond, /*allow_agg=*/false)) return e;
            if (auto e = expect_kw("then")) return e;
            std::shared_ptr<Expr> th;
            if (auto e = parse_scalar_expr(th)) return e;
            n->case_when.push_back(std::move(cond));
            n->case_then.push_back(std::move(th));
        }
        if (is_kw("else")) {
            advance();
            std::shared_ptr<Expr> el;
            if (auto e = parse_scalar_expr(el)) return e;
            n->case_else = el;
        }
        if (auto e = expect_kw("end")) return e;
        out = n;
        return std::nullopt;
    }

    // SELECT [DISTINCT] (* | item, ...) FROM t [WHERE p] [GROUP BY ..] [HAVING ..]
    //                                          [ORDER BY ..] [LIMIT n [OFFSET m]] [AT ..]
    // D4: WITH name AS ( <select> )[, name2 AS ( <select> )] <main select>. Parse the CTE list,
    // then the main query (reusing parse_select for set-op chaining), and attach the CTEs so the
    // engine materializes them before running the main query. Non-recursive (no RECURSIVE keyword).
    ParseResult parse_with() {
        advance();  // WITH
        bool recursive = false;
        if (is_kw("recursive")) { advance(); recursive = true; }
        std::vector<std::pair<std::string, std::shared_ptr<SelectStmt>>> ctes;
        for (;;) {
            if (cur_.kind != Tok::Ident) return make_err("expected a CTE name after WITH");
            std::string name = cur_.text;
            advance();
            std::vector<std::string> cte_cols;  // optional explicit output column names
            if (cur_.kind == Tok::LParen) {
                advance();
                for (;;) {
                    std::string c;
                    if (auto e = expect_ident("a column name in the CTE column list", c)) return ParseResult{*e};
                    cte_cols.push_back(std::move(c));
                    if (cur_.kind == Tok::Comma) { advance(); continue; }
                    break;
                }
                if (auto e = expect(Tok::RParen, "')' to close the CTE column list")) return ParseResult{*e};
            }
            if (!is_kw("as")) return make_err("expected AS after the CTE name");
            advance();  // AS
            if (cur_.kind != Tok::LParen) return make_err("expected ( before the CTE body");
            advance();  // (
            if (!is_kw("select")) return make_err("a CTE body must be a SELECT");
            auto sub = std::make_shared<SelectStmt>();
            if (auto e = parse_select_stmt(*sub)) return ParseResult{*e};
            // A CTE body may carry a trailing set-op chain (e.g. a recursive `base UNION recursive`).
            if (auto e = parse_set_op_tail(*sub)) return ParseResult{*e};
            sub->recursive = recursive;
            sub->cte_columns = std::move(cte_cols);
            if (cur_.kind != Tok::RParen) return make_err("expected ) to close the CTE body");
            advance();  // )
            ctes.emplace_back(std::move(name), std::move(sub));
            if (cur_.kind == Tok::Comma) { advance(); continue; }
            break;
        }
        if (!is_kw("select")) return make_err("expected SELECT after the WITH clause");
        ParseResult r = parse_select();
        if (!r.ok()) return r;
        r.stmt_mut().select.ctes = std::move(ctes);
        return r;
    }

    ParseResult parse_select() {
        Statement st;
        st.kind = StmtKind::Select;
        if (auto e = parse_select_stmt(st.select)) {
            return ParseResult{*e};
        }
        // D1/D2: trailing set operations — `... UNION [ALL] SELECT ...` (also INTERSECT/EXCEPT),
        // right-linked through set_op_rhs. A trailing ORDER BY/LIMIT lands on the LAST arm and the
        // executor applies it to the whole combined result.
        if (auto e = parse_set_op_tail(st.select)) return ParseResult{*e};
        return ParseResult{std::move(st)};
    }

    // D1/D2: parse a trailing set-operation chain (UNION/INTERSECT/EXCEPT [ALL] SELECT ...)
    // onto `first`, right-linked through set_op_rhs. Reused by the CTE body (so a recursive
    // CTE's `base UNION recursive` parses inside the parentheses).
    [[nodiscard]] std::optional<ParseError> parse_set_op_tail(SelectStmt& first) {
        SelectStmt* tail = &first;
        for (;;) {
            SetOp op = SetOp::None;
            if (is_kw("union")) op = SetOp::Union;
            else if (is_kw("intersect")) op = SetOp::Intersect;
            else if (is_kw("except")) op = SetOp::Except;
            else break;
            advance();
            bool all = false;
            if (is_kw("all")) { all = true; advance(); }
            if (!is_kw("select")) {
                return make_err("expected SELECT after a set operator (UNION/INTERSECT/EXCEPT)");
            }
            auto rhs = std::make_shared<SelectStmt>();
            if (auto e = parse_select_stmt(*rhs)) return e;
            tail->set_op = op;
            tail->set_op_all = all;
            tail->set_op_rhs = rhs;
            tail = rhs.get();
        }
        return std::nullopt;
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
                // A WINDOW function (ROW_NUMBER/RANK/agg OVER) or a plain AGGREGATE? (NAME '(' ...)
                AggExpr agg;
                bool is_agg = false;
                bool is_window = false;
                std::string label;
                SelectItem witem;
                if (auto e = parse_window_or_agg(witem, is_window, agg, is_agg, label)) {
                    return e;
                }
                if (is_window) {
                    if (is_kw("as")) {  // optional AS <alias>
                        advance();
                        std::string alias;
                        if (auto e = expect_ident("an alias after AS", alias)) return e;
                        witem.label = alias;
                    }
                    sel.items.push_back(std::move(witem));
                } else if (is_agg) {
                    SelectItem item;
                    item.kind = SelectItemKind::Aggregate;
                    item.agg = agg;
                    item.label = label;
                    if (is_kw("as")) {  // E3: optional AS <alias> on an aggregate (e.g. SUM(v) AS total)
                        advance();
                        std::string alias;
                        if (auto e = expect_ident("an alias after AS", alias)) return e;
                        item.label = alias;
                    }
                    sel.items.push_back(std::move(item));
                    sel.has_aggregates = true;
                } else {
                    // A1-A4: parse a scalar expression (covers a bare column, arithmetic, functions,
                    // CASE, CAST). An optional `AS <alias>` names the output column.
                    std::shared_ptr<Expr> ex;
                    if (auto e = parse_scalar_expr(ex)) return e;
                    std::string alias;
                    bool has_alias = false;
                    if (is_kw("as")) {
                        advance();
                        if (auto e = expect_ident("an alias after AS", alias)) return e;
                        has_alias = true;
                    }
                    if (ex->kind == ExprKind::Col && !has_alias) {
                        // A bare column stays a Column item (back-compat: GROUP BY validation, the v1
                        // single-table fast path, qualified-label spelling).
                        SelectItem item;
                        item.kind = SelectItemKind::Column;
                        item.qualifier = ex->qualifier;
                        item.column = ex->column;
                        item.label =
                            ex->qualifier.empty() ? ex->column : ex->qualifier + "." + ex->column;
                        sel.items.push_back(std::move(item));
                        sel.columns.push_back(ex->column);
                    } else {
                        SelectItem item;
                        item.kind = SelectItemKind::Expr;
                        item.expr = ex;
                        item.label = has_alias ? alias : expr_label(*ex);
                        sel.items.push_back(std::move(item));
                    }
                }
                if (cur_.kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }
        // K11: FROM-less SELECT (SELECT CURRENT_VERSION(), SELECT 1+1, SELECT version())
        // — a pure expression projection evaluated against no table, one output row.
        // Only Expr items qualify (a bare column with no table is meaningless).
        if (!is_kw("from")) {
            bool exprs_only = !sel.items.empty() && sel.columns.empty() && !sel.star;
            for (const SelectItem& it : sel.items) {
                if (it.kind != SelectItemKind::Expr) exprs_only = false;
            }
            if (exprs_only && cur_.kind == Tok::End) {
                sel.table.clear();
                sel.fromless = true;
                return std::nullopt;
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
            // C2: GROUP BY GROUPING SETS ( ( cols ), ( cols ), () )
            if (is_kw("grouping")) {
                advance();
                if (auto e = expect_kw("sets")) return e;
                if (auto e = expect(Tok::LParen, "'(' after GROUPING SETS")) return e;
                for (;;) {
                    if (auto e = expect(Tok::LParen, "'(' to open a grouping set")) return e;
                    std::vector<std::string> set;
                    if (cur_.kind != Tok::RParen) {
                        for (;;) {
                            std::string q, c;
                            if (auto e = expect_qualified_column("a column in a grouping set", q, c))
                                return e;
                            set.push_back(q.empty() ? c : q + "." + c);
                            if (cur_.kind == Tok::Comma) {
                                advance();
                                continue;
                            }
                            break;
                        }
                    }
                    if (auto e = expect(Tok::RParen, "')' to close a grouping set")) return e;
                    sel.grouping_sets.push_back(std::move(set));
                    if (cur_.kind == Tok::Comma) {
                        advance();
                        continue;
                    }
                    break;
                }
                if (auto e = expect(Tok::RParen, "')' to close GROUPING SETS")) return e;
            } else if (is_kw("rollup") || is_kw("cube")) {
                // GROUP BY ROLLUP(a,b,..) / CUBE(a,b,..) — expand into equivalent grouping sets.
                const bool is_cube = is_kw("cube");
                advance();
                if (auto e = expect(Tok::LParen, "'(' after ROLLUP/CUBE")) return e;
                std::vector<std::string> cols;
                for (;;) {
                    std::string q, c;
                    if (auto e = expect_qualified_column("a column in ROLLUP/CUBE", q, c)) return e;
                    cols.push_back(q.empty() ? c : q + "." + c);
                    if (cur_.kind == Tok::Comma) { advance(); continue; }
                    break;
                }
                if (auto e = expect(Tok::RParen, "')' to close ROLLUP/CUBE")) return e;
                const std::size_t n = cols.size();
                if (is_cube) {
                    // Power set, from the full set down to the empty set (deterministic order).
                    for (std::size_t mask = (std::size_t{1} << n); mask-- > 0;) {
                        std::vector<std::string> set;
                        for (std::size_t k = 0; k < n; ++k)
                            if (mask & (std::size_t{1} << k)) set.push_back(cols[k]);
                        sel.grouping_sets.push_back(std::move(set));
                    }
                } else {
                    // Prefixes: (a,b,c), (a,b), (a), ().
                    for (std::size_t k = n + 1; k-- > 0;) {
                        sel.grouping_sets.push_back(
                            std::vector<std::string>(cols.begin(), cols.begin() + k));
                    }
                }
            } else
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
                // G4: ORDER BY <n> — order by the n-th output column (1-based).
                if (cur_.kind == Tok::IntLit) {
                    if (cur_.int_val < 1) {
                        return make_err("ORDER BY position must be >= 1");
                    }
                    key.position = static_cast<int>(cur_.int_val);
                    advance();
                } else {
                    // K1.2: a full scalar expression (`ORDER BY emb <-> '[1,0,0]'`). A bare
                    // (optionally qualified) column keeps the classic column-key path.
                    std::shared_ptr<Expr> ex;
                    if (auto e = parse_scalar_expr(ex)) return e;
                    if (ex->kind == ExprKind::Col) {
                        key.qualifier = ex->qualifier;
                        key.column = ex->column;
                    } else {
                        key.expr = ex;
                    }
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

        // K10 TIME TRAVEL: AS OF SEQ n — read the MVCC state as of version n. A friendly
        // PostgreSQL/Cockroach-style alias for AT SNAPSHOT n (the SEQ/SNAPSHOT/VERSION
        // keyword after AS OF is optional). At this tail position an `AS` can only be
        // `AS OF` (projection/table aliases were already consumed upstream).
        if (is_kw("as")) {
            advance();
            if (auto e = expect_kw("of")) return e;
            if (is_kw("seq") || is_kw("snapshot") || is_kw("version")) advance();
            Datum d;
            if (auto e = expect_literal(d)) return e;
            if (d.type != Type::Int || d.i < 0) {
                return make_err("AS OF SEQ requires a non-negative integer version");
            }
            sel.level = Level::Snapshot;
            sel.snapshot_version = static_cast<Seq>(d.i);
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
            case Tok::Contains: out = CmpOp::Contains; return true;  // @> JSON containment
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
        } else if (allow_agg) {
            // HAVING: a non-aggregate operand is a GROUP BY column (no expression LHS here).
            std::string qual;
            std::string col;
            if (auto e = expect_qualified_column("a column name in the predicate", qual,
                                                 col)) {
                return e;
            }
            leaf.operand = OperandKind::Column;
            leaf.qualifier = qual;
            leaf.column = col;
        } else {
            // J1: parse the LHS as a scalar expression. A bare column stays
            // OperandKind::Column (back-compat: the index / conjunct / PK-fastpath paths all
            // key off a column operand); any richer expression (a+b, doc->>'k', UPPER(x))
            // becomes an Expr operand evaluated per row.
            std::shared_ptr<Expr> lhs;
            if (auto e = parse_scalar_expr(lhs)) {
                return e;
            }
            if (lhs && lhs->kind == ExprKind::Col) {
                leaf.operand = OperandKind::Column;
                leaf.qualifier = lhs->qualifier;
                leaf.column = lhs->column;
            } else {
                leaf.operand = OperandKind::Expr;
                leaf.expr = lhs;
            }
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
            n.operand = leaf.operand;  // J1: an expression LHS keeps its Expr operand
            n.expr = leaf.expr;
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
                n.operand = leaf.operand;  // J1: an expression LHS keeps its Expr operand
                n.expr = leaf.expr;
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
            if (leaf.operand == OperandKind::Expr && leaf.expr &&
                leaf.expr->kind == ExprKind::Func) {
                // ORM-compat: a BARE boolean function call as a predicate
                // (pg_table_is_visible(...) AND ...) — PG truthiness: != 0.
                leaf.op = CmpOp::Ne;
                leaf.literal = Datum::make_int(0);
                p.nodes.push_back(std::move(leaf));
                out = static_cast<std::int32_t>(p.nodes.size() - 1);
                return std::nullopt;
            }
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
        if (!is_agg && (is_kw("any") || is_kw("all"))) {  // F12: lhs <op> ANY|ALL ( <array> )
            leaf.any_quant = is_kw("any");
            leaf.all_quant = is_kw("all");
            advance();
            if (auto e = expect(Tok::LParen, "'(' after ANY/ALL")) return e;
            if (cur_.kind == Tok::Ident && !is_kw("array") && !at_clause_boundary()) {
                std::string rq, rc;
                if (auto e = expect_qualified_column("an array column inside ANY/ALL", rq, rc)) return e;
                leaf.rhs_is_column = true;
                leaf.rhs_qualifier = rq;
                leaf.rhs_column = rc;
            } else if (auto e = expect_value_or_null(leaf.literal)) {
                return e;
            }
            if (auto e = expect(Tok::RParen, "')' to close ANY/ALL")) return e;
        } else if (!is_agg && cur_.kind == Tok::LParen) {
            std::shared_ptr<SelectStmt> sub;
            if (auto e = parse_subquery(sub)) {
                return e;
            }
            leaf.rhs_is_subquery = true;
            leaf.subquery = std::move(sub);
        } else if (!is_agg && cur_.kind == Tok::Ident && !at_clause_boundary() &&
                   !is_kw("true") && !is_kw("false") && !is_kw("array") &&
                   !is_kw("interval")) {  // F9/F12/F13: literals, not columns
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
        // D3: a DERIVED TABLE — `( SELECT ... ) [AS] alias`. Parse the subquery and require an
        // alias (the binding name; the engine materializes it into an ephemeral table so named).
        if (cur_.kind == Tok::LParen) {
            advance();  // (
            if (!is_kw("select")) return make_err("a FROM subquery must be a SELECT");
            auto sub = std::make_shared<SelectStmt>();
            if (auto er = parse_select_stmt(*sub)) return er;
            if (cur_.kind != Tok::RParen) return make_err("expected ) to close the FROM subquery");
            advance();  // )
            if (is_kw("as")) advance();
            if (auto er = expect_ident("an alias for the FROM subquery", e.alias)) return er;
            e.subquery = std::move(sub);
            e.table = e.alias;  // the materialized table is named by the alias
            return std::nullopt;
        }
        if (auto er = expect_table_name("a table name", e.table)) {  // E4: schema.table
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
            // E4: no alias => bind by the UNQUALIFIED table name (the part after a 'schema.').
            const std::size_t dot = e.table.rfind('.');
            e.alias = dot == std::string::npos ? e.table : e.table.substr(dot + 1);
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
