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

[[nodiscard]] inline const char* type_name(Type t) noexcept {
    switch (t) {
        case Type::Int:
            return "INT";
        case Type::Text:
            return "TEXT";
    }
    return "?";
}

// A column definition: a name + its scalar type.
struct Column {
    std::string name;
    Type type = Type::Int;
};

// A table schema: an ordered column list + the PK column index (single-column PK).
struct Table {
    std::string name;
    std::uint32_t id = 0;  // dense table id => the key-prefix namespace
    std::vector<Column> columns;
    std::size_t pk_index = 0;  // which column is the (single) PRIMARY KEY

    [[nodiscard]] const Column& pk() const { return columns[pk_index]; }

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

    static Datum make_int(std::int64_t v) { return Datum{Type::Int, v, {}}; }
    static Datum make_text(std::string v) { return Datum{Type::Text, 0, std::move(v)}; }

    [[nodiscard]] bool operator==(const Datum& o) const {
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

    // A stable text rendering for SELECT output / determinism dumps.
    [[nodiscard]] std::string render() const {
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

    [[nodiscard]] const Table* find(const std::string& name) const {
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
