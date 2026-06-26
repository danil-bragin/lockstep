#pragma once

// Catalog.hpp — SQL SURFACE (a NEW layer over the verified query/txn surface).
//
// THE CATALOG + the deterministic ROW <-> KV ENCODING. This is sugar: a SQL row is
// just an opaque storage KV pair, and a SQL table is a key PREFIX range inside the
// SAME storage::Engine MVCC the typed Query<L> reads. core/sim/consensus/txn/
// storage are UNCHANGED — this header only DEFINES an encoding ON TOP of the opaque
// byte Key/Value the existing surface already uses.
//
// ============================================================================
// SCOPE (v1, a bounded-but-real subset; the rest is FLAGGED future work):
//   * scalar types: INT (signed 64-bit), TEXT (opaque bytes).
//   * a SINGLE-column PRIMARY KEY (multi-column PK is OUT — FLAG).
//   * no secondary indexes, no NULL columns in v1 (every column is required at
//     INSERT; an UPDATE sets one column). Absent value == row not present.
//
// ============================================================================
// THE KEY ENCODING (ORDER-PRESERVING for the PK — load-bearing for range SELECT).
// A row's storage key is:   table_prefix(table_id) ++ pk_encode(pk_value)
//   table_prefix = "t" ++ be32(table_id) ++ ":"   (a fixed-width table namespace;
//                  be32 is big-endian so the namespace bytes sort by table_id and
//                  one table's keys form a CONTIGUOUS byte range — exactly what a
//                  storage scan([lo,hi)) needs to enumerate one table).
//   pk_encode(INT v)  = a single byte (0x00 for v<0, 0x01 for v>=0) ++ be64 of the
//                       bit-flipped-sign value, so the BYTE order of encoded keys
//                       equals the NUMERIC order of the ints (negatives first, then
//                       non-negatives, each ascending). This is the standard
//                       order-preserving signed-int encoding.
//   pk_encode(TEXT s) = the raw bytes of s. TEXT bytes are already byte-order ==
//                       lexicographic order; the PK is the key SUFFIX so it needs no
//                       terminator (one table == one PK type, fixed-width for INT,
//                       and the table prefix delimits the namespace).
// => for any table, scan([prefix ++ pk_encode(a), prefix ++ pk_encode(b))) returns
//    exactly the rows with a <= pk < b, key-ascending == PK-ascending. The range
//    SELECT lowers DIRECTLY onto storage scan order (the conformance gate proves
//    decode(scan) == the typed-query rows).
//
// THE VALUE ENCODING (a column tuple; deterministic, self-describing length-prefix).
//   value = for each NON-PK column in schema order: type_tag(1 byte) ++ be32(len)
//           ++ raw field bytes. The PK column is NOT stored in the value (it is the
//           key); decode reconstructs the PK from the key. Fixed schema order =>
//           the byte image is a pure function of the row (V-DET): same row => same
//           bytes, so determinism + the byte-identical conformance check hold.
//
// FORBIDDEN (query/ is NOT lint-exempt): no wall-clock, no rng, no threads, no
// std::*_distribution, no locale-dependent parsing, ordered containers only. Every
// function here is a pure, deterministic function of its arguments.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <lockstep/query/Query.hpp>  // Key, Value (== txn opaque bytes)

namespace lockstep::query::sql {

// The two scalar column types v1 supports. (FLOAT/BLOB/NULL/multi-col PK = OUT.)
enum class Type : std::uint8_t { Int = 0, Text = 1 };

// v4: a reserved value-encoding tag byte that marks a NULL field in the stored value
// tuple. It is DISTINCT from any real Type tag (0/1) so decode is unambiguous, and a
// NULL field carries NO length/bytes (the column's declared type comes from the
// schema). Non-NULL fields keep their byte-identical type_tag++be32(len)++bytes shape,
// so a row with no NULLs encodes EXACTLY as it did pre-v4 (existing conformance holds).
inline constexpr std::uint8_t kNullTag = 0xFE;

[[nodiscard]] inline const char* type_name(Type t) noexcept {
    switch (t) {
        case Type::Int:
            return "INT";
        case Type::Text:
            return "TEXT";
    }
    return "?";
}

// A column definition: a name + its scalar type + a NULLABILITY flag (v4: explicit
// NULL support). A column is NULLABLE unless it is declared NOT NULL or it is the
// PRIMARY KEY (a PK column is ALWAYS NOT NULL — see CREATE TABLE in the Engine). A
// NULLABLE column may hold a SQL NULL: an INSERT may omit it (=> NULL) or write the
// NULL literal; a non-nullable column REQUIRES a present value at INSERT (fail-closed).
struct Column {
    std::string name;
    Type type = Type::Int;
    bool nullable = false;  // v4: true => may store NULL (NOT NULL / PK => false)
};

// A SECONDARY INDEX over ONE column of a table (single-column index — multi-column
// is OUT, FLAG). Each index has a dense per-table id => its own key-prefix namespace
// (disjoint from the table-row namespace, which uses the 't' prefix; indexes use 'i').
// An index entry is a KV pair: key = index_prefix(table_id, index_id) ++
// encode_index_col(col_value) ++ encode_pk(pk), value = empty. The order-preserving,
// self-delimiting col encoding makes a Database range scan over the index prefix yield
// (col_value, pk) in col-ASCENDING order — exactly what an index range access needs.
struct Index {
    std::string name;           // the index name (unique within the table)
    std::uint32_t id = 0;       // dense per-table index id => the index key namespace
    std::size_t column = 0;     // the indexed column (its schema index)
};

// A table schema: an ordered column list + the PK column index (single-column PK).
struct Table {
    std::string name;
    std::uint32_t id = 0;  // dense table id => the key-prefix namespace
    std::vector<Column> columns;
    std::size_t pk_index = 0;  // which column is the (single) PRIMARY KEY
    std::vector<Index> indexes;       // secondary indexes (in CREATE order)
    std::uint32_t next_index_id = 1;  // dense index-id assignment (0 reserved)

    // COLUMNAR layout (PERF_PLAN columnar rollout): when true, the table is an LSM of a
    // row 'd' delta over flushed column blocks ('B'). Opt-in at CREATE; the durable KV
    // core is unchanged (writes commit in one atomic batch). `delta_dirty` tracks whether
    // the delta has live entries since the last flush, so the vectorized read fast path
    // can skip a per-query delta scan (a pure-block read needs delta empty).
    bool columnar = false;
    bool delta_dirty = false;
    std::uint64_t flush_gen = 0;    // bumped each flush; the decoded-block cache validity tag
    std::uint64_t delta_count = 0;  // live delta writes since the last flush (auto-flush trigger)

    // STATISTICS (PERF_PLAN Phase 2) — maintained incrementally on write so the cost-based
    // planner can estimate cardinalities without a scan. row_count is exact (INSERT +1 on a
    // new PK, DELETE -1 on a present row, UPDATE no change). Per-column min/max (INT) track the
    // OBSERVED value range (grow-only — a DELETE of an extreme leaves a slightly-wide range,
    // fine for an estimate). Deterministic (a pure function of the committed write sequence).
    // Future: n_distinct (HLL) + equi-depth histograms for eq selectivity.
    std::size_t row_count = 0;
    struct ColStat {
        bool seen = false;       // any INT value observed
        std::int64_t lo = 0;     // observed min
        std::int64_t hi = 0;     // observed max
    };
    std::vector<ColStat> col_stats;  // one per column (sized at CREATE; INT columns only)

    [[nodiscard]] const Column& pk() const { return columns[pk_index]; }

    // Find a secondary index BY the column it covers; returns it or nullptr. (v1:
    // single-column indexes, so at most one index per column is used by the planner —
    // the FIRST index found on the column wins, deterministic CREATE order.)
    [[nodiscard]] const Index* index_for_column(std::size_t col) const {
        for (const Index& ix : indexes) {
            if (ix.column == col) {
                return &ix;
            }
        }
        return nullptr;
    }

    // Find a secondary index by name (DROP INDEX / dup-name detection).
    [[nodiscard]] const Index* index_by_name(const std::string& nm) const {
        for (const Index& ix : indexes) {
            if (ix.name == nm) {
                return &ix;
            }
        }
        return nullptr;
    }

    // Find a column by name; returns its index or nullopt (unknown column).
    [[nodiscard]] std::optional<std::size_t> column_index(const std::string& col) const {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == col) {
                return i;
            }
        }
        return std::nullopt;
    }
};

// ----------------------------------------------------------------------------
// A scalar VALUE the SQL layer manipulates: an INT (signed 64-bit) or a TEXT
// (opaque bytes). Kept as a small typed variant (no std::variant overhead beyond a
// tag) so encode/decode + WHERE comparisons are total + deterministic.
// ----------------------------------------------------------------------------
struct Datum {
    Type type = Type::Int;
    std::int64_t i = 0;     // valid when type == Int
    std::string s;          // valid when type == Text
    // v3 (JOIN): a SQL NULL. The base v1/v2 row encoding has NO nulls (every column is
    // present), so `is_null` is ALWAYS false there. NULLs are introduced ONLY by a
    // LEFT JOIN's unmatched right side (the NULL-filled columns). `type` still records
    // the column's declared type so comparisons stay type-checked; the value bytes are
    // ignored when is_null. NULL semantics (three-valued logic, COUNT(col) skips NULL,
    // a comparison with NULL is UNKNOWN==false) are documented in Engine.hpp.
    bool is_null = false;

    static Datum make_int(std::int64_t v) { return Datum{Type::Int, v, {}, false}; }
    static Datum make_text(std::string v) {
        return Datum{Type::Text, 0, std::move(v), false};
    }
    // A typed NULL (carries the column type for type-checking, but no value).
    static Datum make_null(Type t) { return Datum{t, 0, {}, true}; }

    [[nodiscard]] bool operator==(const Datum& o) const {
        // NULL is not equal to anything (incl. another NULL) under SQL three-valued
        // logic; but for the structural identity DISTINCT/render uses we treat two
        // NULLs of the same type as the SAME output cell (a single NULL group/row).
        if (is_null || o.is_null) {
            return is_null && o.is_null && type == o.type;
        }
        if (type != o.type) {
            return false;
        }
        return type == Type::Int ? (i == o.i) : (s == o.s);
    }

    // Total deterministic order WITHIN one type (the PK ordering the range relies
    // on). Cross-type compare is never invoked (a column has ONE type).
    [[nodiscard]] bool less_than(const Datum& o) const {
        return type == Type::Int ? (i < o.i) : (s < o.s);
    }

    // A stable text rendering for SELECT output / determinism dumps. NULL renders as
    // the literal "NULL" (distinct from any INT/TEXT value the subset produces).
    [[nodiscard]] std::string render() const {
        if (is_null) {
            return "NULL";
        }
        return type == Type::Int ? std::to_string(i) : s;
    }
};

// ----------------------------------------------------------------------------
// ORDER-PRESERVING ENCODING PRIMITIVES (pure, deterministic).
// ----------------------------------------------------------------------------

// Big-endian 32-bit (the table-id namespace + the value field-length prefix).
inline void put_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

[[nodiscard]] inline std::uint32_t get_be32(const std::string& in, std::size_t off) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(in[off])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(in[off + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(in[off + 2])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(in[off + 3])));
}

// Order-preserving signed-int key: a leading sign byte (0x00 for negatives so they
// sort BEFORE 0x01 non-negatives) then a big-endian image with the sign bit
// flipped, so byte-order == numeric-order across the whole signed range.
inline void put_pk_int(std::string& out, std::int64_t v) {
    out.push_back(v < 0 ? static_cast<char>(0x00) : static_cast<char>(0x01));
    const std::uint64_t bits = static_cast<std::uint64_t>(v) ^ 0x8000000000000000ULL;
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((bits >> shift) & 0xFF));
    }
}

// The table key-prefix namespace: "t" ++ be32(table_id) ++ ":". One table's rows
// occupy a contiguous byte range; the upper bound is the prefix with the trailing
// ':' (0x3A) bumped to ';' (0x3B) so [prefix, prefix_end) covers EVERY pk under it.
[[nodiscard]] inline Key table_prefix(std::uint32_t table_id) {
    Key k = "t";
    put_be32(k, table_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key table_prefix_end(std::uint32_t table_id) {
    Key k = "t";
    put_be32(k, table_id);
    k.push_back(';');  // ':' + 1 — the exclusive upper bound of the namespace
    return k;
}

// Encode a row's PK datum to the key SUFFIX (order-preserving).
[[nodiscard]] inline Key encode_pk(const Datum& pk) {
    Key out;
    if (pk.type == Type::Int) {
        put_pk_int(out, pk.i);
    } else {
        out += pk.s;  // raw TEXT bytes (already order-preserving as the suffix)
    }
    return out;
}

// The full storage key for a row identified by its PK datum.
[[nodiscard]] inline Key encode_key(const Table& t, const Datum& pk) {
    return table_prefix(t.id) + encode_pk(pk);
}

// ----------------------------------------------------------------------------
// COLUMNAR KEY-SCHEMA (column-family layout) — opt-in per table.
//
// A columnar table stores each (row, column) pair as its OWN key-value entry, in a
// namespace DISJOINT from row-mode tables ('c' vs 't') and indexes ('i'):
//   col_key = "c" ++ be32(table_id) ++ ":" ++ be32(col_id) ++ ":" ++ encode_pk(pk)
// col_id is a PREFIX (before the pk), so every value of ONE column is a CONTIGUOUS
// byte range — a projection scans ONLY the needed columns' families (the pushdown
// win). pk is the SUFFIX, so within a column the values stay pk-ascending (row
// assembly merges the per-column streams by pk; a pk range stays a sub-range).
// Per-column VALUE = one datum: a single kNullTag (0xFE) for SQL NULL, else a 0x00
// present tag + payload (INT 8-byte be64 / TEXT raw bytes); type comes from schema.
// The durable KV core (WalEngine/WAL/MVCC/recovery) is UNCHANGED: a columnar row's N
// entries commit in ONE write batch (the existing atomic unit), so row atomicity +
// acked==durable hold by the SAME mechanism row-mode already uses.
// ----------------------------------------------------------------------------

// Whole columnar-table namespace [lo, hi): "c" ++ be32(table_id) ++ ":" .. ";".
[[nodiscard]] inline Key col_table_prefix(std::uint32_t table_id) {
    Key k = "c";
    put_be32(k, table_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key col_table_prefix_end(std::uint32_t table_id) {
    Key k = "c";
    put_be32(k, table_id);
    k.push_back(';');  // ':' + 1 — exclusive upper bound of the table namespace
    return k;
}
// One column family's range [lo, hi): every value of column `col_id` is contiguous.
[[nodiscard]] inline Key col_prefix(std::uint32_t table_id, std::uint32_t col_id) {
    Key k = "c";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key col_prefix_end(std::uint32_t table_id, std::uint32_t col_id) {
    Key k = "c";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(';');  // ':' + 1 — exclusive upper bound of this column family
    return k;
}
// The full columnar key for (col_id, pk).
[[nodiscard]] inline Key col_key(std::uint32_t table_id, std::uint32_t col_id, const Datum& pk) {
    return col_prefix(table_id, col_id) + encode_pk(pk);
}

// One column's datum -> storage value. NULL == a single kNullTag byte; else a 0x00
// present tag (distinct from kNullTag) + payload. The value's own size delimits it
// (one datum per KV — no length prefix needed); the type is recovered from schema.
[[nodiscard]] inline Value encode_col_value(const Datum& d) {
    Value v;
    if (d.is_null) {
        v.push_back(static_cast<char>(kNullTag));
        return v;
    }
    v.push_back(static_cast<char>(0x00));  // present tag (!= kNullTag)
    if (d.type == Type::Int) {
        const std::uint64_t bits = static_cast<std::uint64_t>(d.i);
        for (int shift = 56; shift >= 0; shift -= 8) {
            v.push_back(static_cast<char>((bits >> shift) & 0xFF));
        }
    } else {
        v += d.s;  // raw TEXT bytes
    }
    return v;
}
[[nodiscard]] inline Datum decode_col_value(Type ty, const Value& v) {
    if (!v.empty() && static_cast<unsigned char>(v[0]) == kNullTag) {
        return Datum::make_null(ty);
    }
    // v[0] is the present tag (0x00); the payload begins at offset 1.
    if (ty == Type::Int) {
        std::uint64_t bits = 0;
        for (std::size_t b = 1; b <= 8; ++b) {
            bits = (bits << 8) | static_cast<unsigned char>(v[b]);
        }
        return Datum::make_int(static_cast<std::int64_t>(bits));
    }
    return Datum::make_text(v.substr(1));
}

// COLUMNAR DELTA storage (LSM write buffer; namespace 'd', disjoint from row 't' / index
// 'i' / block 'B'). A columnar table's INSERT/UPDATE/DELETE writes a ROW-mode KV here
//   row_delta_key = "d" ++ be32(table_id) ++ ":" ++ encode_pk(pk)
// value = encode_value(row) for a live row, or the single-byte kRowDelMarker for a
// DELETE (a LIVE sentinel — a scan returns it, so a delta delete SHADOWS a flushed block
// row; a plain tombstone would be hidden by the scan and the block row would wrongly
// survive). The read path merges blocks (base) with the delta overlay (delta wins per
// pk; a marker drops the pk); FLUSH packs the merged rows into column blocks + clears the
// delta. Delta values reuse the ROW codec (encode_value/decode_row), so a pre-flush
// columnar table reads exactly like a row-mode table (delta IS the whole table).
inline const std::string& row_del_marker() {
    static const std::string m(1, '\x01');  // 1 byte; no encode_value output is 1 byte
    return m;                                // (a non-pk column needs >=1 tag byte +data)
}
[[nodiscard]] inline bool is_row_del_marker(const Value& v) { return v == row_del_marker(); }

[[nodiscard]] inline Key row_delta_prefix(std::uint32_t table_id) {
    Key k = "d";
    put_be32(k, table_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key row_delta_prefix_end(std::uint32_t table_id) {
    Key k = "d";
    put_be32(k, table_id);
    k.push_back(';');  // ':' + 1 — exclusive upper bound of the delta namespace
    return k;
}
[[nodiscard]] inline Key row_delta_key(const Table& t, const Datum& pk) {
    return row_delta_prefix(t.id) + encode_pk(pk);
}
// Decode the PK from a delta key (strip "d" + be32 + ":" = 6 bytes, like decode_pk).
[[nodiscard]] inline Datum decode_pk_from_delta_key(const Table& t, const Key& key) {
    constexpr std::size_t kPrefixLen = 6;  // "d" + be32 + ":"
    const std::string suffix = key.substr(kPrefixLen);
    if (t.pk().type == Type::Int) {
        std::uint64_t bits = 0;
        for (std::size_t b = 1; b <= 8; ++b) {
            bits = (bits << 8) | static_cast<unsigned char>(suffix[b]);
        }
        return Datum::make_int(static_cast<std::int64_t>(bits ^ 0x8000000000000000ULL));
    }
    return Datum::make_text(suffix);
}

// COLUMNAR BLOCK storage keys (LSM-flushed SoA chunks; namespace 'B', disjoint from row
// 't' / index 'i' / columnar-delta 'd'). A flushed column chunk lives at
//   block_key = "B" ++ be32(table_id) ++ ":" ++ be32(col_id) ++ ":" ++ be64(block_no)
// block_no orders chunks within a column (recency / pk-range). The value is an
// encode_column_block SoA chunk. Written through the verified commit path (durable core
// unchanged). [lo,hi) over col_id enumerates one column's blocks key-ascending.
[[nodiscard]] inline Key block_col_prefix(std::uint32_t table_id, std::uint32_t col_id) {
    Key k = "B";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key block_col_prefix_end(std::uint32_t table_id, std::uint32_t col_id) {
    Key k = "B";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(';');  // ':' + 1 — exclusive upper bound of this column's blocks
    return k;
}
// MULTI-RUN LSM OVERLAY keys. Base = run 0 = the 'B' blocks (unchanged). A non-append flush
// writes an OVERLAY RUN instead of rewriting the base: live rows as chunks under 'R', deleted
// pks as a tombstone list under 'T', and the active-run list in a manifest under 'M'. Reads
// merge base + overlay runs (newest run wins per pk; a tombstone drops it) + the live delta;
// compaction folds everything back into run 0 and clears the overlays. Namespaces R/T/M are
// disjoint from t/i/d/B/c/Z.
[[nodiscard]] inline Key overlay_key(std::uint32_t table_id, std::uint32_t run,
                                     std::uint32_t col_id, std::uint64_t chunk) {
    Key k = "R";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, run);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(':');
    for (int shift = 56; shift >= 0; shift -= 8) {
        k.push_back(static_cast<char>((chunk >> shift) & 0xFF));
    }
    return k;
}
[[nodiscard]] inline Key overlay_run_col_prefix(std::uint32_t table_id, std::uint32_t run,
                                                std::uint32_t col_id) {
    Key k = "R";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, run);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(':');
    return k;
}
[[nodiscard]] inline Key overlay_run_col_prefix_end(std::uint32_t table_id, std::uint32_t run,
                                                    std::uint32_t col_id) {
    Key k = "R";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, run);
    k.push_back(':');
    put_be32(k, col_id);
    k.push_back(';');
    return k;
}
[[nodiscard]] inline Key overlay_tomb_key(std::uint32_t table_id, std::uint32_t run) {
    Key k = "T";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, run);
    return k;
}
[[nodiscard]] inline Key overlay_manifest_key(std::uint32_t table_id) {
    Key k = "M";
    put_be32(k, table_id);
    return k;
}

// ZONE-MAP key (Phase 4 data skipping; namespace 'Z', disjoint from t/i/d/B/c). One KV per
// columnar table holds every chunk's per-INT-column [min,max] so a WHERE col CMP literal can
// SKIP chunks that can't match WITHOUT decoding their column blocks. Maintained on flush.
[[nodiscard]] inline Key zone_key(std::uint32_t table_id) {
    Key k = "Z";
    put_be32(k, table_id);
    return k;
}

[[nodiscard]] inline Key block_key(std::uint32_t table_id, std::uint32_t col_id,
                                   std::uint64_t block_no) {
    Key k = block_col_prefix(table_id, col_id);
    for (int shift = 56; shift >= 0; shift -= 8) {
        k.push_back(static_cast<char>((block_no >> shift) & 0xFF));  // be64, order-preserving
    }
    return k;
}

// Decode the PK datum from a COLUMNAR key (strip the fixed 11-byte col prefix, decode
// the suffix per the PK type — the same suffix transform decode_pk uses for row keys).
[[nodiscard]] inline Datum decode_pk_from_col_key(const Table& t, const Key& key) {
    constexpr std::size_t kColPrefixLen = 11;  // "c" + be32 + ":" + be32 + ":"
    const std::string suffix = key.substr(kColPrefixLen);
    if (t.pk().type == Type::Int) {
        std::uint64_t bits = 0;
        for (std::size_t b = 1; b <= 8; ++b) {
            bits = (bits << 8) | static_cast<unsigned char>(suffix[b]);
        }
        return Datum::make_int(static_cast<std::int64_t>(bits ^ 0x8000000000000000ULL));
    }
    return Datum::make_text(suffix);
}

// ----------------------------------------------------------------------------
// SECONDARY-INDEX KEY ENCODING (order-preserving + self-delimiting).
//
// An index lives in a DISJOINT key namespace from the table rows: the row namespace
// is "t" ++ be32(table_id) ++ ":"; the index namespace is
//   index_prefix = "i" ++ be32(table_id) ++ ":" ++ be32(index_id) ++ ":"
// so a Database range scan over one index's prefix enumerates ONLY that index's
// entries, key-ascending. ('i' != 't' => the table-row scan never sees index keys,
// and vice-versa; the row scan bound table_prefix_end stays exact.)
//
// An index entry KEY = index_prefix ++ encode_index_col(col_value) ++ encode_pk(pk).
// The value is EMPTY. The col value is encoded ORDER-PRESERVING and SELF-DELIMITING so
// the variable-length PK suffix can never be confused with the col value:
//   INT  -> put_pk_int (fixed 9 bytes, order-preserving — already self-delimiting).
//   TEXT -> each 0x00 byte escaped as 0x00 0x01, then a 0x00 0x00 terminator. The
//           terminator (0x00 0x00) sorts BEFORE any escaped data byte (0x00 0x01..),
//           so byte order == lexicographic value order AND the token is unambiguously
//           delimited from the PK suffix. (Standard order-preserving escaped encoding.)
// => entries for one col value v share the EXACT key prefix index_prefix++col_enc(v),
//    differing only in the PK suffix => an equality lookup is a half-open prefix scan
//    [index_prefix++col_enc(v), successor(index_prefix++col_enc(v))); a BETWEEN [a,b]
//    is [index_prefix++col_enc(a), successor(index_prefix++col_enc(b))).
// ----------------------------------------------------------------------------

[[nodiscard]] inline Key index_prefix(std::uint32_t table_id, std::uint32_t index_id) {
    Key k = "i";
    put_be32(k, table_id);
    k.push_back(':');
    put_be32(k, index_id);
    k.push_back(':');
    return k;
}

// Order-preserving, self-delimiting encode of an indexed COLUMN value (NOT the PK).
inline void put_index_col(std::string& out, const Datum& d) {
    if (d.type == Type::Int) {
        put_pk_int(out, d.i);  // fixed-width 9 bytes, order-preserving
        return;
    }
    for (const char c : d.s) {
        out.push_back(c);
        if (c == '\0') {
            out.push_back('\x01');  // escape: 0x00 -> 0x00 0x01
        }
    }
    out.push_back('\0');
    out.push_back('\0');  // terminator 0x00 0x00 (sorts before any escaped byte)
}

[[nodiscard]] inline Key encode_index_col(const Datum& d) {
    Key out;
    put_index_col(out, d);
    return out;
}

// The full index ENTRY key for (col_value, pk). value is empty (the PK lives in the key).
[[nodiscard]] inline Key encode_index_entry(std::uint32_t table_id, const Index& ix,
                                            const Datum& col_value, const Datum& pk) {
    Key k = index_prefix(table_id, ix.id);
    put_index_col(k, col_value);
    k += encode_pk(pk);
    return k;
}

// The byte-string SUCCESSOR of `k`: the smallest key strictly greater than EVERY key
// that has `k` as a prefix. Increment the last byte that is < 0xFF, dropping trailing
// 0xFF bytes (carry). If `k` is all 0xFF, returns empty (== "no upper bound"); callers
// pair this with the table/index namespace so an empty successor never under-scans in
// practice. Used to turn a prefix into a half-open [k, successor(k)) range.
[[nodiscard]] inline Key key_successor(const Key& k) {
    Key out = k;
    while (!out.empty()) {
        const auto last = static_cast<unsigned char>(out.back());
        if (last != 0xFF) {
            out.back() = static_cast<char>(last + 1);
            return out;
        }
        out.pop_back();  // 0xFF carries: drop it and bump the next byte
    }
    return out;  // all 0xFF => empty (unbounded above)
}

// Decode the PK datum from a storage key (strip the table prefix, decode the
// suffix per the PK column's type). The prefix is fixed width (1 + 4 + 1 = 6).
[[nodiscard]] inline Datum decode_pk(const Table& t, const Key& key) {
    constexpr std::size_t kPrefixLen = 6;  // "t" + be32 + ":"
    const std::string suffix = key.substr(kPrefixLen);
    if (t.pk().type == Type::Int) {
        // sign byte + be64(bit-flipped); reverse the put_pk_int transform.
        std::uint64_t bits = 0;
        for (std::size_t b = 1; b <= 8; ++b) {
            bits = (bits << 8) | static_cast<unsigned char>(suffix[b]);
        }
        const std::int64_t v = static_cast<std::int64_t>(bits ^ 0x8000000000000000ULL);
        return Datum::make_int(v);
    }
    return Datum::make_text(suffix);
}

// Encode the NON-PK column tuple to the storage value (self-describing length
// prefix per field, in schema order). Pure function of the row datums.
[[nodiscard]] inline Value encode_value(const Table& t,
                                        const std::vector<Datum>& row) {
    Value out;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (c == t.pk_index) {
            continue;  // the PK lives in the key, not the value
        }
        const Datum& d = row[c];
        // v4: a NULL field is a single tag byte (no length, no payload). Decode knows
        // the column's type from the schema, so the NULL is reconstructed typed.
        if (d.is_null) {
            out.push_back(static_cast<char>(kNullTag));
            continue;
        }
        out.push_back(static_cast<char>(d.type));
        if (d.type == Type::Int) {
            put_be32(out, 8);
            const std::uint64_t bits = static_cast<std::uint64_t>(d.i);
            for (int shift = 56; shift >= 0; shift -= 8) {
                out.push_back(static_cast<char>((bits >> shift) & 0xFF));
            }
        } else {
            put_be32(out, static_cast<std::uint32_t>(d.s.size()));
            out += d.s;
        }
    }
    return out;
}

// Decode a full row (every column, PK reconstructed from the key) from a KV pair.
// Returns the columns in schema order. A pure inverse of encode_key/encode_value.
[[nodiscard]] inline std::vector<Datum> decode_row(const Table& t, const Key& key,
                                                   const Value& value) {
    std::vector<Datum> row(t.columns.size());
    row[t.pk_index] = decode_pk(t, key);
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (c == t.pk_index) {
            continue;
        }
        // v4: a NULL field is a single kNullTag byte — reconstruct a typed NULL.
        if (static_cast<unsigned char>(value[off]) == kNullTag) {
            off += 1;
            row[c] = Datum::make_null(t.columns[c].type);
            continue;
        }
        const Type ty = static_cast<Type>(static_cast<unsigned char>(value[off]));
        off += 1;
        const std::uint32_t len = get_be32(value, off);
        off += 4;
        if (ty == Type::Int) {
            std::uint64_t bits = 0;
            for (std::uint32_t b = 0; b < len; ++b) {
                bits = (bits << 8) | static_cast<unsigned char>(value[off + b]);
            }
            row[c] = Datum::make_int(static_cast<std::int64_t>(bits));
        } else {
            row[c] = Datum::make_text(value.substr(off, len));
        }
        off += len;
    }
    return row;
}

// PROJECTED / LAZY decode (the scan-decode optimization). Decode ONLY the columns in
// `need` (true == decode this column index); the rest are SKIPPED — for an INT field
// we step over its fixed 8 bytes, for a TEXT field we step over its length WITHOUT
// copying the bytes (the dominant cost on a wide TEXT row). The PK is always cheap
// (reconstructed from the key) so it is decoded iff needed. Skipped columns are left
// default-constructed (Datum{} == INT 0) — the caller MUST only read columns it asked
// for (the v2 pipeline computes `need` from projection + WHERE + GROUP BY + aggregates).
// PURE inverse over the needed subset; byte-identical to decode_row for those columns,
// so the conformance gate (== full-decode reference) still proves equality.
// Decode a projected row INTO a caller-provided buffer (reused across rows). Lets a scan
// decode-then-discard a filtered-out row WITHOUT a per-row vector allocation (the buffer's
// capacity is reused) — only surviving rows are copied into the result. Same bytes as the
// returning variant. (PERF_PLAN Phase 3: the per-row vector<Datum> alloc was the measured
// scan bottleneck, not the predicate eval.)
inline void decode_row_projected_into(const Table& t, const Key& key, const Value& value,
                                      const std::vector<bool>& need, std::vector<Datum>& row) {
    row.assign(t.columns.size(), Datum{});  // reuses capacity (no realloc after the first row)
    if (need[t.pk_index]) {
        row[t.pk_index] = decode_pk(t, key);
    }
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (c == t.pk_index) {
            continue;
        }
        if (static_cast<unsigned char>(value[off]) == kNullTag) {
            off += 1;
            if (need[c]) {
                row[c] = Datum::make_null(t.columns[c].type);
            }
            continue;
        }
        const Type ty = static_cast<Type>(static_cast<unsigned char>(value[off]));
        off += 1;
        const std::uint32_t len = get_be32(value, off);
        off += 4;
        if (need[c]) {
            if (ty == Type::Int) {
                std::uint64_t bits = 0;
                for (std::uint32_t b = 0; b < len; ++b) {
                    bits = (bits << 8) | static_cast<unsigned char>(value[off + b]);
                }
                row[c] = Datum::make_int(static_cast<std::int64_t>(bits));
            } else {
                row[c] = Datum::make_text(value.substr(off, len));
            }
        }
        off += len;
    }
}

[[nodiscard]] inline std::vector<Datum> decode_row_projected(
    const Table& t, const Key& key, const Value& value,
    const std::vector<bool>& need) {
    std::vector<Datum> row(t.columns.size());
    if (need[t.pk_index]) {
        row[t.pk_index] = decode_pk(t, key);
    }
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (c == t.pk_index) {
            continue;
        }
        // v4: a NULL field is a single kNullTag byte (no length/payload).
        if (static_cast<unsigned char>(value[off]) == kNullTag) {
            off += 1;
            if (need[c]) {
                row[c] = Datum::make_null(t.columns[c].type);
            }
            continue;
        }
        const Type ty = static_cast<Type>(static_cast<unsigned char>(value[off]));
        off += 1;
        const std::uint32_t len = get_be32(value, off);
        off += 4;
        if (need[c]) {
            if (ty == Type::Int) {
                std::uint64_t bits = 0;
                for (std::uint32_t b = 0; b < len; ++b) {
                    bits = (bits << 8) | static_cast<unsigned char>(value[off + b]);
                }
                row[c] = Datum::make_int(static_cast<std::int64_t>(bits));
            } else {
                row[c] = Datum::make_text(value.substr(off, len));
            }
        }
        // else: SKIP the field bytes (no copy, no parse) — just advance the offset.
        off += len;
    }
    return row;
}

// ----------------------------------------------------------------------------
// THE CATALOG — table name -> schema. Deterministic dense table-id assignment (in
// CREATE order). An ordered map => deterministic iteration. No ambient state.
// ----------------------------------------------------------------------------
class Catalog {
public:
    // Register a new table. Returns false if the name already exists (dup table).
    [[nodiscard]] bool create(Table t) {
        if (tables_.count(t.name) != 0) {
            return false;
        }
        t.id = next_id_++;
        tables_.emplace(t.name, std::move(t));
        return true;
    }

    // RECOVERY: re-register a table with its PERSISTED id (the data keys are namespaced by that
    // id, so it must be preserved verbatim). Keeps the id allocator monotonic past every recovered
    // id so a post-recovery CREATE never collides. Returns false on a duplicate name.
    [[nodiscard]] bool insert_recovered(Table t) {
        if (tables_.count(t.name) != 0) {
            return false;
        }
        if (t.id >= next_id_) {
            next_id_ = t.id + 1;
        }
        tables_.emplace(t.name, std::move(t));
        return true;
    }

    [[nodiscard]] const Table* find(const std::string& name) const {
        const auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    // Mutable table lookup (for index DDL — CREATE/DROP INDEX edits the schema's
    // `indexes` list). Deterministic: the catalog is an ordered map.
    [[nodiscard]] Table* find_mut(const std::string& name) {
        const auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool has(const std::string& name) const {
        return tables_.count(name) != 0;
    }

private:
    std::map<std::string, Table> tables_;  // ordered => deterministic
    std::uint32_t next_id_ = 1;            // 0 reserved (no table)
};

}  // namespace lockstep::query::sql
