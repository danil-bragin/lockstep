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
//
// v3 (this stage) EXTENDS the SELECT FROM clause with JOINs (the v1/v2 statements are
// byte-unchanged when no JOIN is present):
//   * INNER JOIN / LEFT [OUTER] JOIN with an ON <predicate> (the general predicate
//     tree, now over QUALIFIED columns table.col / alias.col).
//   * comma-style cross join (FROM a, b WHERE ...) lowering to a CROSS JOIN.
//   * table aliases (FROM t AS x, FROM t x) and multiple left-deep joins
//     (a JOIN b ON .. JOIN c ON ..).
//   * a column reference is now optionally QUALIFIED: <table-or-alias>.<col>.
// v4 (this stage) ADDS explicit NULL + subqueries (the v1-v3 statements are byte-
// unchanged when no NULL/subquery is present):
//   * NULLABLE COLUMNS: a column is NULLABLE unless declared NOT NULL (the PK is always
//     NOT NULL). A NULLABLE column may hold a SQL NULL; INSERT may OMIT it (=> NULL) or
//     write the `NULL` literal. IS NULL / IS NOT NULL predicates test for NULL.
//   * SUBQUERIES (UNCORRELATED only — a correlated subquery referencing an outer column
//     resolves it as unknown => a clean error; FLAG, correlated is next):
//       - scalar:   <col> <op> (SELECT <agg-or-single-col> FROM ...)   (>1 row => error)
//       - IN/NOT IN: <col> [NOT] IN (SELECT col FROM ...)              (membership, 3VL)
//       - EXISTS:   [NOT] EXISTS (SELECT ... )                         (row existence)
//     A subquery carries a full nested SelectStmt (PredNode::subquery), run through the
//     SAME SELECT pipeline — no new query surface. The three-valued NULL rules + the
//     scalar cardinality rule are documented + model-mirrored in Engine.hpp.
// multi-statement txns remain OUT (FLAG): the parser REJECTS them with a clear error.

#include <cstdint>
#include <memory>
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
    std::vector<std::string> checks;  // F5: CHECK predicate source texts (column- or table-level)
};

// CREATE INDEX <name> ON <table> (<col>) — a single-column SECONDARY INDEX. The
// multi-column form is OUT (FLAG): the parser rejects a comma-separated column list.
struct CreateIndexStmt {
    std::string index;   // the index name (unique within the table)
    std::string table;   // the table it indexes
    std::string column;  // the single indexed column
};

// DROP INDEX <name> ON <table> — remove a secondary index (+ its KV entries).
struct DropIndexStmt {
    std::string index;
    std::string table;
};

struct SelectStmt;  // fwd (defined below) — InsertStmt::select_source for INSERT ... SELECT (D5)

// INSERT INTO <t> (<cols>) VALUES (<vals>)
struct InsertStmt {
    std::string table;
    std::vector<std::string> columns;  // the named columns, in stated order
    std::vector<Datum> values;         // row 0: one value per named column (parsed literal)
    // D6 multi-row: VALUES (..),(..),... — rows 1..N-1 (row 0 stays in `values` for back-compat).
    // Empty for a single-row INSERT. Each inner vector has columns.size() values (parser-checked).
    std::vector<std::vector<Datum>> more_rows;
    // D5 INSERT ... SELECT: when set, the rows come from this query instead of VALUES. The SELECT's
    // output arity must match `columns`; each output row becomes one inserted row (atomic, like D6).
    std::shared_ptr<SelectStmt> select_source;
    // G2 UPSERT: ON CONFLICT behavior on a duplicate PK. Error (default) rejects; Nothing skips the
    // row; Update applies `conflict_updates` (col=literal) to the existing row.
    enum class OnConflict : std::uint8_t { Error = 0, Nothing = 1, Update = 2 };
    OnConflict on_conflict = OnConflict::Error;
    std::vector<std::pair<std::string, Datum>> conflict_updates;
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
    Like = 6,  // B1: SQL LIKE pattern match (% any run, _ one char); TEXT only
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
// v3: the target column may be QUALIFIED (table-or-alias.col); `qualifier` is empty
// for an unqualified reference (resolved against the joined schema at plan time).
struct AggExpr {
    AggKind kind = AggKind::CountStar;
    std::string qualifier;  // v3: optional table/alias qualifier ("" == unqualified)
    std::string column;     // empty for COUNT(*)
    bool distinct = false;  // C1: COUNT/SUM/AVG(DISTINCT col) — dedup values per group first
};

// Forward declaration: a subquery node carries a nested SELECT (defined below). It is
// held by shared_ptr so PredNode/Predicate stay copyable (a value tree) without an
// incomplete-type member, and so the inner SELECT reuses the SAME SelectStmt shape /
// execution pipeline (subqueries are lowered by running the inner SELECT, then applying
// the predicate — NO new query surface).
struct SelectStmt;

enum class PredNodeKind : std::uint8_t {
    Cmp = 0,      // a leaf: <operand> <op> <literal-or-column-or-scalar-subquery>
    And = 1,      // left AND right
    Or = 2,       // left OR right
    Not = 3,      // NOT child (uses `left` as the child index)
    // v4: explicit NULL + subqueries.
    IsNull = 4,   // <column> IS [NOT] NULL  (negate via `is_not`)
    InList = 5,   // <column> [NOT] IN ( SELECT ... )   (subquery membership)
    Exists = 6,   // [NOT] EXISTS ( SELECT ... )
};

struct PredNode {
    PredNodeKind kind = PredNodeKind::Cmp;

    // Cmp leaf:
    OperandKind operand = OperandKind::Column;
    std::string qualifier;  // v3: optional table/alias qualifier ("" == unqualified)
    std::string column;     // operand == Column
    AggExpr agg;            // operand == Agg (HAVING)
    CmpOp op = CmpOp::Eq;
    Datum literal;

    // v3: a JOIN ON predicate may compare a column to ANOTHER column (an equi-join
    // key `a.x = b.y` or a general column-vs-column theta), not just a literal. When
    // `rhs_is_column` is true the right operand is a (qualifier,column) reference
    // rather than `literal`.
    bool rhs_is_column = false;
    std::string rhs_qualifier;
    std::string rhs_column;

    // v4: a Cmp leaf whose RIGHT operand is a SCALAR SUBQUERY: `col <op> (SELECT agg)`.
    // The subquery must return EXACTLY one row / one column at run time (>1 row is an
    // error, like real SQL; 0 rows => the scalar is NULL => the comparison is UNKNOWN).
    // Mutually exclusive with rhs_is_column / literal.
    bool rhs_is_subquery = false;

    // v4: IsNull (`is_not` => IS NOT NULL); InList / Exists (`is_not` => NOT IN /
    // NOT EXISTS). For IsNull/InList the tested operand is (qualifier,column); for
    // Exists the operand is unused (existence of the subquery's rows).
    bool is_not = false;

    // v4: the nested SELECT for InList / Exists / a scalar subquery RHS. shared_ptr so
    // the value tree stays copyable; null for non-subquery nodes.
    std::shared_ptr<SelectStmt> subquery;

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

// A1/A2/A3/A4: a SCALAR EXPRESSION tree — arithmetic (+ - * / %), a column reference, a literal,
// a scalar function (UPPER/LOWER/LENGTH/SUBSTR/CONCAT/COALESCE/ABS/CAST), or a CASE expression.
// Evaluated to a Datum over a row. Held by shared_ptr so the tree stays copyable.
enum class ExprKind : std::uint8_t { Col = 0, Lit = 1, Neg = 2, Bin = 3, Func = 4, Case = 5 };
enum class BinOp : std::uint8_t { Add = 0, Sub = 1, Mul = 2, Div = 3, Mod = 4 };

struct Expr {
    ExprKind kind = ExprKind::Lit;
    // Col
    std::string qualifier;
    std::string column;
    // Lit
    Datum lit;
    // Bin / Neg
    BinOp op = BinOp::Add;
    std::shared_ptr<Expr> left;
    std::shared_ptr<Expr> right;  // Bin: both operands; Neg: `left` only
    // Func — A2 string/scalar functions + A4 CAST. `func` is the upper-cased name; CAST stores its
    // target in cast_type.
    std::string func;
    Type cast_type = Type::Int;
    std::vector<std::shared_ptr<Expr>> args;
    // Case — A3: WHEN case_when[i] THEN case_then[i] ... [ELSE case_else]. First true WHEN wins;
    // no match + no ELSE => NULL.
    std::vector<Predicate> case_when;
    std::vector<std::shared_ptr<Expr>> case_then;
    std::shared_ptr<Expr> case_else;  // null => NULL default
    std::string label;                // the rendered output label for a projected expression
};

// One ORDER BY key: a column name + a direction. (ORDER BY references projected /
// table columns by name; tie-break by PK is appended by the planner for a total,
// byte-deterministic order.)
// G3: explicit NULL ordering for an ORDER BY key. Default = "NULL is the smallest value" (so NULLs
// sort FIRST under ASC, LAST under DESC — the engine's pre-G3 behavior); First/Last override it.
enum class NullsOrder : std::uint8_t { Default = 0, First = 1, Last = 2 };

struct OrderKey {
    std::string qualifier;  // v3: optional table/alias qualifier ("" == unqualified)
    std::string column;
    bool descending = false;  // ASC default
    NullsOrder nulls = NullsOrder::Default;  // G3: NULLS FIRST | LAST override
    int position = 0;          // G4: ORDER BY <n> — the 1-based output column (0 == by name)
};

// One SELECT-list item: either a plain column or an aggregate expression. A v1
// `SELECT id, name` is all-Column items; a v2 `SELECT k, COUNT(*)` mixes them.
enum class SelectItemKind : std::uint8_t { Column = 0, Aggregate = 1, Expr = 2 };
struct SelectItem {
    SelectItemKind kind = SelectItemKind::Column;
    std::string qualifier;  // v3: optional table/alias qualifier ("" == unqualified)
    std::string column;     // Column: the column name; the OUTPUT label
    AggExpr agg;            // Aggregate: the aggregate
    std::shared_ptr<Expr> expr;  // A1: a scalar expression (computed column)
    std::string label;      // the rendered output column label (col name or "COUNT(*)")
};

// v3: how a joined input is combined with the accumulated left side.
//   Cross — comma-style cross join (FROM a, b) or an ON-less CROSS JOIN: cartesian.
//   Inner — INNER JOIN ... ON p: keep only matched (left,right) pairs.
//   Left  — LEFT [OUTER] JOIN ... ON p: keep every left row; unmatched ones emit one
//           output row with the right side's columns NULL-filled.
enum class JoinKind : std::uint8_t { Cross = 0, Inner = 1, Left = 2, Right = 3, Full = 4 };

// One FROM/JOIN entry. The FROM clause is a LEFT-DEEP list: entry[0] is the base
// table (kind ignored), entry[k>0] joins onto the accumulated left side. `on` is the
// ON predicate (a general predicate tree over qualified columns); it is absent for a
// CROSS join. `alias` is the table's binding name in the joined schema (defaults to
// `table` when no AS clause is given). Self-joins use distinct aliases.
struct JoinEntry {
    JoinKind kind = JoinKind::Inner;
    std::string table;  // the base table name (catalog lookup)
    std::string alias;  // the binding name (== table when no AS); MUST be unique
    Predicate on;       // ON predicate (root -1 == none, i.e. a CROSS join)
};

// D1/D2: set operation linking two SELECTs. None for a plain query; UNION/INTERSECT/EXCEPT chain
// the right-hand SELECT after this one. `all` keeps duplicates (UNION ALL); otherwise the combined
// result is whole-row deduplicated.
enum class SetOp : std::uint8_t { None = 0, Union = 1, Intersect = 2, Except = 3 };

struct SelectStmt {
    std::string table;                 // v1/v2: the single base table (== from[0].table
                                       // when present); kept for the no-JOIN fast path.
    bool star = false;                 // SELECT *
    bool distinct = false;             // SELECT DISTINCT
    std::vector<std::string> columns;  // v1 projected columns (empty iff star); kept
                                       // for the v1 fast path / back-compat.

    // v2: the rich SELECT list (columns + aggregates). When non-empty it SUPERSEDES
    // `columns`/`star`; the parser fills BOTH (`columns` mirrors plain-column lists)
    // so the v1 lowering keeps working byte-identically when there are no aggregates.
    std::vector<SelectItem> items;
    bool has_aggregates = false;  // any item is an aggregate (=> grouping executor)

    // v3: the FROM/JOIN list (left-deep). When it has exactly ONE entry whose alias
    // equals its table name (no AS, no JOIN), the v1/v2 single-table path runs and
    // `table` mirrors from[0].table. With >1 entry, OR an alias != table, the joined
    // pipeline runs (qualified-column resolution over the joined schema).
    std::vector<JoinEntry> from;

    // v3: true iff this is a genuine JOIN (>1 FROM entry) — the planner takes the
    // joined pipeline (build per-table scans + combine) instead of the single scan.
    [[nodiscard]] bool is_join() const { return from.size() > 1; }

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
    // C2: GROUP BY GROUPING SETS ( (a,b), (a), () ) — each inner vector is one grouping. When
    // non-empty the query runs once per set and UNIONs the rows; a SELECT column not in a given set
    // renders NULL for that set's rows.
    std::vector<std::vector<std::string>> grouping_sets;
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

    // EXPLAIN [ANALYZE] <select> — when set, exec_select returns the chosen PLAN (access
    // path + pipeline stages) as text rows instead of the query result. ANALYZE also RUNS
    // the query and reports DETERMINISTIC per-stage counters (rows / comparisons / decodes
    // — a pure function of the seed, never wall-clock). The transparency surface for finding
    // bottlenecks (PERF_PLAN.md Phase 0).
    bool explain = false;
    bool explain_analyze = false;

    // D1/D2: a trailing set operation. When set_op != None, set_op_rhs is the right-hand SELECT and
    // the two results are combined (UNION/INTERSECT/EXCEPT); set_op_all keeps duplicates (… ALL).
    // ORDER BY / LIMIT on the LAST arm apply to the whole combined result (SQL set-op semantics).
    SetOp set_op = SetOp::None;
    bool set_op_all = false;
    std::shared_ptr<SelectStmt> set_op_rhs;
};

// The tagged statement union (a small value; no heap-owned polymorphism).
// DROP TABLE <t>  (F8)
struct DropTableStmt {
    std::string table;
};

// ALTER TABLE <t> ADD [COLUMN] <col> <type> [constraints]  (F7)
struct AlterStmt {
    std::string table;
    Column add_col;
};

enum class StmtKind : std::uint8_t {
    Create = 0,
    Insert = 1,
    Update = 2,
    Delete = 3,
    Select = 4,
    CreateIndex = 5,
    DropIndex = 6,
    DropTable = 7,
    Alter = 8,
    Begin = 9,     // G1: BEGIN [TRANSACTION]
    Commit = 10,   // G1: COMMIT
    Rollback = 11, // G1: ROLLBACK
};

struct Statement {
    StmtKind kind = StmtKind::Select;
    CreateStmt create;
    InsertStmt insert;
    UpdateStmt update;
    DeleteStmt del;
    SelectStmt select;
    CreateIndexStmt create_index;
    DropIndexStmt drop_index;
    DropTableStmt drop_table;
    AlterStmt alter;
};

}  // namespace lockstep::query::sql
