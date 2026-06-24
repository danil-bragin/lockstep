#pragma once

// Ast.hpp — SQL SURFACE: the typed AST the recursive-descent Parser produces and
// the Planner lowers onto the verified Database/Query<L>/txn surface.
//
// A statement is a small value (no pointers, ordered vectors) so it is trivially
// deterministic + copyable. The AST is the boundary between the (string-shaped)
// parser and the (value-shaped) planner — the planner NEVER re-touches the raw SQL.
//
// SCOPE (v1 + v2): CREATE TABLE / INSERT / UPDATE / DELETE / SELECT over the bounded
// subset documented in Catalog.hpp + Parser.hpp.
//
// v2 (this stage) EXTENDS the SELECT surface (the v1 statements are byte-unchanged):
//   * WHERE on ANY column: a general boolean predicate tree (comparisons =,<,<=,
//     >,>=,!= on a column vs a literal, combined with AND / OR / NOT, parenthesized).
//   * Aggregates: COUNT(*) / COUNT(col) / SUM / MIN / MAX / AVG, with optional
//     GROUP BY <cols> and HAVING <pred-on-aggregates>.
//   * ORDER BY <cols> [ASC|DESC] + LIMIT n [OFFSET m].
//   * DISTINCT.
// JOIN / secondary indexes / subqueries / multi-statement txns remain OUT (FLAG):
// the parser REJECTS them with a clear error rather than mis-parsing. They are the
// NEXT stages.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/Query.hpp>       // Level (D5) for the AT annotation
#include <lockstep/query/sql/Catalog.hpp>  // Type, Datum

namespace lockstep::query::sql {

// CREATE TABLE <name> (<col> <type>, ..., PRIMARY KEY (<col>))
struct CreateStmt {
    std::string table;
    std::vector<Column> columns;
    std::string pk_column;  // the single PK column name
};

// INSERT INTO <t> (<cols>) VALUES (<vals>)
struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;  // the named columns, in stated order
    std::vector<Datum> values;         // one value per named column (parsed literal)
};

// UPDATE <t> SET <col> = <v> WHERE <pk> = <v>
struct UpdateStmt {
    std::string table;
    std::string set_column;
    Datum set_value;
    std::string where_column;  // must be the PK column (v1)
    Datum where_value;
};

// DELETE FROM <t> WHERE <pk> = <v>
struct DeleteStmt {
    std::string table;
    std::string where_column;  // must be the PK column (v1)
    Datum where_value;
};

// The WHERE shape a SELECT supports (v1, all over the PK column):
//   None     — SELECT ... FROM t                  (full scan)
//   Eq       — WHERE pk = v                        (point get)
//   Between  — WHERE pk BETWEEN a AND b            (inclusive range)
enum class SelectWhereKind : std::uint8_t { None = 0, Eq = 1, Between = 2 };

// ----------------------------------------------------------------------------
// v2: a GENERAL boolean predicate tree over a row (WHERE on ANY column) + a
// predicate over aggregate results (HAVING). It is a small value tree (an ordered
// vector pool, NOT heap-owned polymorphism) so the AST stays trivially copyable +
// deterministic. A node references its children by INDEX into `Predicate::nodes`.
// ----------------------------------------------------------------------------
enum class CmpOp : std::uint8_t {
    Eq = 0,  // =
    Ne = 1,  // != / <>
    Lt = 2,  // <
    Le = 3,  // <=
    Gt = 4,  // >
    Ge = 5,  // >=
};

// What the left-hand side of a comparison REFERENCES.
enum class OperandKind : std::uint8_t {
    Column = 0,  // a column name (resolved at plan time vs the schema)
    Agg = 1,     // an aggregate expression (HAVING only): e.g. COUNT(*) > 2
};

enum class AggKind : std::uint8_t {
    CountStar = 0,  // COUNT(*)
    Count = 1,      // COUNT(col)
    Sum = 2,        // SUM(col)
    Min = 3,        // MIN(col)
    Max = 4,        // MAX(col)
    Avg = 5,        // AVG(col) — INT truncation toward zero (documented; see Engine)
};

// One aggregate expression: a kind + (for non-CountStar) the target column name.
struct AggExpr {
    AggKind kind = AggKind::CountStar;
    std::string column;  // empty for COUNT(*)
};

enum class PredNodeKind : std::uint8_t {
    Cmp = 0,  // a leaf: <operand> <op> <literal>
    And = 1,  // left AND right
    Or = 2,   // left OR right
    Not = 3,  // NOT child (uses `left` as the child index)
};

struct PredNode {
    PredNodeKind kind = PredNodeKind::Cmp;

    // Cmp leaf:
    OperandKind operand = OperandKind::Column;
    std::string column;  // operand == Column
    AggExpr agg;         // operand == Agg (HAVING)
    CmpOp op = CmpOp::Eq;
    Datum literal;

    // And/Or/Not: child indices into Predicate::nodes (Not uses `left` only).
    std::int32_t left = -1;
    std::int32_t right = -1;
};

// A predicate tree: a node pool + the root index (-1 == no predicate / always true).
struct Predicate {
    std::vector<PredNode> nodes;
    std::int32_t root = -1;  // -1 == absent (no filter)

    [[nodiscard]] bool present() const { return root >= 0; }
};

// One ORDER BY key: a column name + a direction. (ORDER BY references projected /
// table columns by name; tie-break by PK is appended by the planner for a total,
// byte-deterministic order.)
struct OrderKey {
    std::string column;
    bool descending = false;  // ASC default
};

// One SELECT-list item: either a plain column or an aggregate expression. A v1
// `SELECT id, name` is all-Column items; a v2 `SELECT k, COUNT(*)` mixes them.
enum class SelectItemKind : std::uint8_t { Column = 0, Aggregate = 1 };
struct SelectItem {
    SelectItemKind kind = SelectItemKind::Column;
    std::string column;  // Column: the column name; the OUTPUT label
    AggExpr agg;         // Aggregate: the aggregate
    std::string label;   // the rendered output column label (col name or "COUNT(*)")
};

struct SelectStmt {
    std::string table;
    bool star = false;                 // SELECT *
    bool distinct = false;             // SELECT DISTINCT
    std::vector<std::string> columns;  // v1 projected columns (empty iff star); kept
                                       // for the v1 fast path / back-compat.

    // v2: the rich SELECT list (columns + aggregates). When non-empty it SUPERSEDES
    // `columns`/`star`; the parser fills BOTH (`columns` mirrors plain-column lists)
    // so the v1 lowering keeps working byte-identically when there are no aggregates.
    std::vector<SelectItem> items;
    bool has_aggregates = false;  // any item is an aggregate (=> grouping executor)

    // v1 PK WHERE fast-path (still recognized for point/range lowering).
    SelectWhereKind where = SelectWhereKind::None;
    std::string where_column;  // the PK column (Eq / Between)
    Datum eq_value;            // Eq
    Datum lo_value;            // Between lower (inclusive)
    Datum hi_value;            // Between upper (inclusive)

    // v2: the GENERAL WHERE predicate (over ANY column). When present the planner
    // does a full scan + a row filter, UNLESS it is exactly a PK equality/range that
    // the planner recognizes and lowers to the v1 point/range fast path.
    Predicate filter;

    // v2: GROUP BY <cols>, HAVING <agg-pred>, ORDER BY <keys>, LIMIT/OFFSET.
    std::vector<std::string> group_by;
    Predicate having;            // a predicate over aggregates (root -1 == none)
    std::vector<OrderKey> order_by;
    bool has_limit = false;
    std::int64_t limit = 0;
    std::int64_t offset = 0;

    // The CALL-SITE-VISIBLE D5 level (V-D5-SAFE): an optional `AT <level>` clause.
    // Defaults to StrictSerializable (the strong default, never a silent stale
    // read). Snapshot/Bounded carry a parameter.
    Level level = Level::StrictSerializable;
    Seq snapshot_version = kNoSeq;  // AT SNAPSHOT <version>
    Seq max_lag = 0;                // AT BOUNDED <max_lag>
    SessionId session = 0;          // AT RYW <session>
};

// The tagged statement union (a small value; no heap-owned polymorphism).
enum class StmtKind : std::uint8_t {
    Create = 0,
    Insert = 1,
    Update = 2,
    Delete = 3,
    Select = 4,
};

struct Statement {
    StmtKind kind = StmtKind::Select;
    CreateStmt create;
    InsertStmt insert;
    UpdateStmt update;
    DeleteStmt del;
    SelectStmt select;
};

}  // namespace lockstep::query::sql
