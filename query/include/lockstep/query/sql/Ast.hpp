#pragma once

// Ast.hpp — SQL SURFACE: the typed AST the recursive-descent Parser produces and
// the Planner lowers onto the verified Database/Query<L>/txn surface.
//
// A statement is a small value (no pointers, ordered vectors) so it is trivially
// deterministic + copyable. The AST is the boundary between the (string-shaped)
// parser and the (value-shaped) planner — the planner NEVER re-touches the raw SQL.
//
// SCOPE (v1): CREATE TABLE / INSERT / UPDATE / DELETE / SELECT over the bounded
// subset documented in Catalog.hpp + Parser.hpp. JOIN / GROUP BY / aggregates /
// subqueries / multi-statement txns are OUT (FLAG): the parser REJECTS them with a
// clear error rather than mis-parsing.

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

struct SelectStmt {
    std::string table;
    bool star = false;                 // SELECT *
    std::vector<std::string> columns;  // projected columns (empty iff star)

    SelectWhereKind where = SelectWhereKind::None;
    std::string where_column;  // the PK column (Eq / Between)
    Datum eq_value;            // Eq
    Datum lo_value;            // Between lower (inclusive)
    Datum hi_value;            // Between upper (inclusive)

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
