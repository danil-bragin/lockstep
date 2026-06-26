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
#include <cstdio>
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
    // F4 DEFAULT: a column-level default used when an INSERT omits the column. Stored as primitives
    // (Datum is defined below this struct): default_i for INT, default_s for TEXT, per `type`.
    bool has_default = false;
    std::int64_t default_i = 0;
    std::string default_s;
    bool auto_increment = false;  // F6: INT column auto-assigned from Table::next_auto_id when omitted
    bool unique = false;          // F2: UNIQUE constraint (no two non-NULL rows share a value)
    // F3 FOREIGN KEY: when fk_table is non-empty, a non-NULL value must exist as the PK of fk_table.
    std::string fk_table;
    std::string fk_column;  // the referenced column (the parent's PK)
    // F9b: a LOGICAL type over physical storage — 0=plain, 1=DECIMAL(scale), 2=DATE, 3=TIMESTAMP
    // (these over INT), 4=UUID (over TEXT: a validated, canonicalised 36-char string).
    std::uint8_t logical = 0;
    std::uint8_t scale = 0;
    // F9c: DEFAULT gen_uuid() — an omitted UUID column gets a DETERMINISTIC v4-shaped id derived from
    // the table's next_uuid counter (NOT random — random would diverge across the two Raft impls and
    // break the cross-check). Independent of has_default (which holds literal defaults).
    bool uuid_default = false;
    // F10: DOMAIN constraints (checked at coerce; all deterministic). 0/false == unconstrained.
    std::uint32_t max_len = 0;     // VARCHAR(n)/CHAR(n)/BLOB(n): max byte length (TEXT columns)
    bool fixed_char = false;       // CHAR(n): right-pad with spaces to max_len
    bool is_unsigned = false;      // UNSIGNED: reject a negative value (INT/BIGINT/INT128/DECIMAL*)
    std::uint8_t precision = 0;    // DECIMAL(p,s): max total significant digits (0 == unconstrained)
    std::uint8_t int_bits = 0;     // TINYINT/SMALLINT/INT32: width for the range check (0 == 64-bit)
    // F12: ARRAY (logical == 7). The ELEMENT type: its physical type + its own logical/scale tag
    // (so INT[], TEXT[], DECIMAL[], INT128[], etc. all work). The array payload lives in the value `s`.
    Type elem_type = Type::Int;
    std::uint8_t elem_logical = 0;
    std::uint8_t elem_scale = 0;
    // F13: ENUM (logical == 9). The ordered label set; the stored value is the 0-based ordinal (so
    // comparison follows DECLARATION order, like Postgres/MySQL). Render shows the label.
    std::vector<std::string> enum_labels;
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
    std::size_t pk_index = 0;  // the FIRST PK column (== pk_columns[0]; the single-col fast paths)
    // F1: the PRIMARY KEY column list. Size 1 for a single-column PK (the common case, byte-identical
    // to before); >1 for a COMPOSITE PK (all-INT, row-mode — the key is the columns' order-preserving
    // encodings concatenated). Empty is treated as {pk_index} for back-compat with recovered records.
    std::vector<std::size_t> pk_columns;
    [[nodiscard]] bool composite_pk() const { return pk_columns.size() > 1; }
    std::vector<Index> indexes;       // secondary indexes (in CREATE order)
    std::uint32_t next_index_id = 1;  // dense index-id assignment (0 reserved)
    std::int64_t next_auto_id = 1;    // F6: next AUTO_INCREMENT value (monotonic; persisted)
    std::uint64_t next_uuid = 0;      // F9c: next DEFAULT gen_uuid() counter (monotonic; persisted)
    std::vector<std::string> checks;  // F5: CHECK predicate source texts (re-parsed + eval'd on write)

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
    // F9b: a LOGICAL type layered over the physical INT storage so the engine stays byte-deterministic
    // (all arithmetic/comparison/keys use `i`). 0=raw INT/TEXT, 1=DECIMAL(scale), 2=DATE (days since
    // 1970-01-01), 3=TIMESTAMP (seconds since the epoch). Only render()/literal-parse use it.
    std::uint8_t logical = 0;
    std::uint8_t scale = 0;  // DECIMAL fractional digits

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
        if (type == Type::Int && logical != 0) {  // F9b: DECIMAL / DATE / TIMESTAMP; F13: TIME/ENUM/INTERVAL
            if (logical == 1) return render_decimal(i, scale);
            if (logical == 2) return render_date(i);
            if (logical == 3) return render_timestamp(i);
            if (logical == 8) return render_time(i);
            if (logical == 9) return s;  // ENUM: label populated at decode (ordinal kept in i)
            if (logical == 10) return render_interval(i);
        }
        if (type == Type::Text && logical == 7) return render_array(s);  // F12: ARRAY
        if (type == Type::Text && logical >= 5) {  // F9e: INT128 / DECIMAL128 (16-byte payload in s)
            if (logical == 5) return render_i128(decode_i128(s));
            if (logical == 6) return render_decimal128(decode_i128(s), scale);
        }
        return type == Type::Int ? std::to_string(i) : s;
    }

    // --- F12: ARRAY (logical=7) over physical TEXT. Self-describing payload in `s`:
    //   [elem_logical:u8][elem_scale:u8][count:be32]  then per element:
    //   [0]                          == SQL NULL
    //   [1][8-byte be64]             == an INT-backed element (int64 value)
    //   [2][len:be32][bytes]         == a TEXT-backed element (text, or a 16-byte INT128 payload)
    // Deterministic; element ordering preserved. The element's logical/scale is the header's (every
    // element shares the column's element type), re-tagged on decode so render() prints each. ---
    static void put_be32_(std::string& o, std::uint32_t v) {
        o.push_back(static_cast<char>((v >> 24) & 0xFF));
        o.push_back(static_cast<char>((v >> 16) & 0xFF));
        o.push_back(static_cast<char>((v >> 8) & 0xFF));
        o.push_back(static_cast<char>(v & 0xFF));
    }
    static std::uint32_t get_be32_(const std::string& s, std::size_t off) {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3]));
    }
    static std::string encode_array(std::uint8_t elem_logical, std::uint8_t elem_scale,
                                    const std::vector<Datum>& elems) {
        std::string s;
        s.push_back(static_cast<char>(elem_logical));
        s.push_back(static_cast<char>(elem_scale));
        put_be32_(s, static_cast<std::uint32_t>(elems.size()));
        for (const Datum& e : elems) {
            if (e.is_null) { s.push_back(0); continue; }
            if (e.type == Type::Int) {
                s.push_back(1);
                for (int k = 7; k >= 0; --k)
                    s.push_back(static_cast<char>(static_cast<unsigned char>(
                        static_cast<std::uint64_t>(e.i) >> (8 * k))));
            } else {
                s.push_back(2);
                put_be32_(s, static_cast<std::uint32_t>(e.s.size()));
                s += e.s;
            }
        }
        return s;
    }
    static std::vector<Datum> decode_array(const std::string& s) {
        std::vector<Datum> out;
        if (s.size() < 6) return out;
        const std::uint8_t el = static_cast<unsigned char>(s[0]);
        const std::uint8_t es = static_cast<unsigned char>(s[1]);
        const std::uint32_t n = get_be32_(s, 2);
        std::size_t off = 6;
        for (std::uint32_t k = 0; k < n && off < s.size(); ++k) {
            const std::uint8_t tag = static_cast<unsigned char>(s[off++]);
            Datum d;
            if (tag == 0) {
                d = make_null(el >= 5 ? Type::Text : Type::Int);
            } else if (tag == 1) {
                std::uint64_t bits = 0;
                for (int b = 0; b < 8; ++b) bits = (bits << 8) | static_cast<unsigned char>(s[off++]);
                d = make_int(static_cast<std::int64_t>(bits));
            } else {
                const std::uint32_t len = get_be32_(s, off);
                off += 4;
                d = make_text(s.substr(off, len));
                off += len;
            }
            d.logical = el;
            d.scale = es;
            out.push_back(std::move(d));
        }
        return out;
    }
    static std::string render_array(const std::string& s) {
        const std::vector<Datum> elems = decode_array(s);
        std::string out = "{";
        for (std::size_t k = 0; k < elems.size(); ++k) {
            if (k != 0) out.push_back(',');
            out += elems[k].render();
        }
        out.push_back('}');
        return out;
    }
    // F13: TIME (logical=8) — seconds since midnight (0..86399) -> "HH:MM:SS".
    static std::string render_time(std::int64_t secs) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", static_cast<long long>(secs / 3600),
                      static_cast<long long>((secs % 3600) / 60), static_cast<long long>(secs % 60));
        return std::string(buf);
    }
    // F13: INTERVAL (logical=10) — a signed second count -> "[-]Dd HH:MM:SS" (days + clock part).
    static std::string render_interval(std::int64_t secs) {
        const bool neg = secs < 0;
        std::int64_t a = neg ? -secs : secs;
        const std::int64_t days = a / 86400;
        a %= 86400;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s%lldd %02lld:%02lld:%02lld", neg ? "-" : "",
                      static_cast<long long>(days), static_cast<long long>(a / 3600),
                      static_cast<long long>((a % 3600) / 60), static_cast<long long>(a % 60));
        return std::string(buf);
    }

    // --- F9e: 128-bit integer / fixed-point (logical=5/6) over physical TEXT. The value is a 16-byte
    // ORDER-PRESERVING big-endian encoding (top sign bit flipped) held in `s`, so a plain TEXT byte
    // compare == numeric order and the TEXT key/value codecs need no change. Deterministic. ---
    static std::string encode_i128(__int128 v) {
        unsigned __int128 u = static_cast<unsigned __int128>(v) ^ (static_cast<unsigned __int128>(1) << 127);
        std::string s(16, '\0');
        for (int k = 0; k < 16; ++k) s[15 - k] = static_cast<char>(static_cast<unsigned char>(u >> (8 * k)));
        return s;
    }
    static __int128 decode_i128(const std::string& s) {
        unsigned __int128 u = 0;
        for (int k = 0; k < 16; ++k) u = (u << 8) | static_cast<unsigned char>(s[static_cast<std::size_t>(k)]);
        u ^= (static_cast<unsigned __int128>(1) << 127);
        return static_cast<__int128>(u);
    }
    static std::string render_u128(unsigned __int128 u) {
        if (u == 0) return "0";
        char buf[40];
        int i = 40;
        while (u != 0) { buf[--i] = static_cast<char>('0' + static_cast<int>(u % 10)); u /= 10; }
        return std::string(buf + i, static_cast<std::size_t>(40 - i));
    }
    static unsigned __int128 abs_u128(__int128 v) {
        return v < 0 ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
    }
    static std::string render_i128(__int128 v) {
        return (v < 0 ? "-" : "") + render_u128(abs_u128(v));
    }
    static std::string render_decimal128(__int128 v, std::uint8_t scale) {
        if (scale == 0) return render_i128(v);
        unsigned __int128 mag = abs_u128(v);
        unsigned __int128 pow = 1;
        for (std::uint8_t k = 0; k < scale; ++k) pow *= 10;
        std::string frac = render_u128(mag % pow);
        while (frac.size() < scale) frac.insert(frac.begin(), '0');
        return (v < 0 ? "-" : "") + render_u128(mag / pow) + "." + frac;
    }

    // --- F9b logical-type formatting (pure, deterministic; no locale, no wall-clock) ---
    static std::string render_decimal(std::int64_t v, std::uint8_t scale) {
        if (scale == 0) return std::to_string(v);
        const bool neg = v < 0;
        std::uint64_t mag = neg ? static_cast<std::uint64_t>(-(v + 1)) + 1 : static_cast<std::uint64_t>(v);
        std::uint64_t pow = 1;
        for (std::uint8_t k = 0; k < scale; ++k) pow *= 10;
        std::string frac = std::to_string(mag % pow);
        while (frac.size() < scale) frac.insert(frac.begin(), '0');
        return (neg ? "-" : "") + std::to_string(mag / pow) + "." + frac;
    }
    // Days since 1970-01-01 -> "YYYY-MM-DD" (proleptic Gregorian, integer Howard Hinnant algorithm).
    static std::string render_date(std::int64_t days) {
        std::int64_t z = days + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const std::uint64_t doe = static_cast<std::uint64_t>(z - era * 146097);
        const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
        const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const std::uint64_t mp = (5 * doy + 2) / 153;
        const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
        const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
        const std::int64_t yy = y + (m <= 2 ? 1 : 0);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04lld-%02llu-%02llu", static_cast<long long>(yy),
                      (unsigned long long)m, (unsigned long long)d);
        return std::string(buf);
    }
    static std::string render_timestamp(std::int64_t secs) {
        const std::int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
        std::int64_t rem = secs - days * 86400;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld", render_date(days).c_str(),
                      (long long)(rem / 3600), (long long)((rem % 3600) / 60), (long long)(rem % 60));
        return std::string(buf);
    }
};

// ----------------------------------------------------------------------------
// F9b: parse a DECIMAL / DATE / TIMESTAMP literal STRING into its INT representation (pure,
// deterministic — no locale, no wall-clock). Return false on a malformed literal.
// ----------------------------------------------------------------------------
[[nodiscard]] inline bool parse_decimal(const std::string& in, std::uint8_t scale, std::int64_t& out) {
    std::size_t p = 0;
    bool neg = false;
    if (p < in.size() && (in[p] == '+' || in[p] == '-')) { neg = in[p] == '-'; ++p; }
    std::int64_t intpart = 0;
    bool any = false;
    for (; p < in.size() && in[p] >= '0' && in[p] <= '9'; ++p) { intpart = intpart * 10 + (in[p] - '0'); any = true; }
    std::int64_t frac = 0;
    std::uint8_t fdig = 0;
    if (p < in.size() && in[p] == '.') {
        ++p;
        for (; p < in.size() && in[p] >= '0' && in[p] <= '9'; ++p) {
            if (fdig < scale) { frac = frac * 10 + (in[p] - '0'); ++fdig; }
            any = true;  // a digit past `scale` is truncated (deterministic)
        }
    }
    if (!any || p != in.size()) return false;
    std::int64_t pow = 1;
    for (std::uint8_t k = 0; k < scale; ++k) pow *= 10;
    for (std::uint8_t k = fdig; k < scale; ++k) frac *= 10;  // pad to `scale` digits
    std::int64_t v = intpart * pow + frac;
    out = neg ? -v : v;
    return true;
}
inline std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
[[nodiscard]] inline bool parse_date(const std::string& in, std::int64_t& days) {
    int y = 0, m = 0, d = 0, n = 0;
    if (std::sscanf(in.c_str(), "%d-%d-%d%n", &y, &m, &d, &n) != 3 ||
        static_cast<std::size_t>(n) != in.size() || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    days = days_from_civil(y, m, d);
    return true;
}
[[nodiscard]] inline bool parse_timestamp(const std::string& in, std::int64_t& secs) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, n = 0;
    int got = std::sscanf(in.c_str(), "%d-%d-%d %d:%d:%d%n", &y, &mo, &d, &h, &mi, &s, &n);
    if (got == 3) {  // date-only timestamp -> midnight
        std::int64_t days = 0;
        if (!parse_date(in, days)) return false;
        secs = days * 86400;
        return true;
    }
    if (got != 6 || static_cast<std::size_t>(n) != in.size() || mo < 1 || mo > 12 || d < 1 ||
        d > 31 || h > 23 || mi > 59 || s > 59) {
        return false;
    }
    secs = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

// F13: TIME 'HH:MM[:SS]' -> seconds since midnight (0..86399). Pure.
[[nodiscard]] inline bool parse_time(const std::string& in, std::int64_t& secs) {
    int h = 0, m = 0, s = 0, n = 0;
    int got = std::sscanf(in.c_str(), "%d:%d:%d%n", &h, &m, &s, &n);
    if (got == 2) { n = 0; got = std::sscanf(in.c_str(), "%d:%d%n", &h, &m, &n); s = 0; }
    if (got < 2 || static_cast<std::size_t>(n) != in.size() || h < 0 || h > 23 || m < 0 || m > 59 ||
        s < 0 || s > 59) {
        return false;
    }
    secs = h * 3600 + m * 60 + s;
    return true;
}
// F13: INTERVAL — a count + unit phrases ('1 day', '2 hours 30 minutes', '90 seconds'), and/or an
// embedded clock 'HH:MM:SS'. Units: week/day/hour/minute/second (+ common abbrevs). Months/years are
// NOT supported (calendar-variable). Sums to a signed SECOND count. Pure.
[[nodiscard]] inline bool parse_interval(const std::string& in, std::int64_t& secs) {
    std::int64_t total = 0;
    bool any = false;
    std::size_t p = 0;
    bool neg = false;
    auto skip_ws = [&]() { while (p < in.size() && (in[p] == ' ' || in[p] == '\t')) ++p; };
    skip_ws();
    if (p < in.size() && in[p] == '-') { neg = true; ++p; }
    while (p < in.size()) {
        skip_ws();
        if (p >= in.size()) break;
        // an embedded clock part HH:MM:SS (no unit word).
        std::size_t q = p;
        std::int64_t num = 0;
        bool has_num = false;
        while (q < in.size() && in[q] >= '0' && in[q] <= '9') { num = num * 10 + (in[q] - '0'); ++q; has_num = true; }
        if (!has_num) return false;
        if (q < in.size() && in[q] == ':') {  // a clock 'H:M[:S]' chunk
            int h = 0, m = 0, s = 0, cn = 0;
            int got = std::sscanf(in.c_str() + p, "%d:%d:%d%n", &h, &m, &s, &cn);
            if (got == 2) { cn = 0; got = std::sscanf(in.c_str() + p, "%d:%d%n", &h, &m, &cn); s = 0; }
            if (got < 2) return false;
            total += h * 3600 + m * 60 + s;
            p += static_cast<std::size_t>(cn);
            any = true;
            continue;
        }
        p = q;
        skip_ws();
        std::string unit;
        while (p < in.size() && in[p] >= 'a' && in[p] <= 'z') unit.push_back(in[p++]);
        while (p < in.size() && in[p] >= 'A' && in[p] <= 'Z') unit.push_back(static_cast<char>(in[p++] - 'A' + 'a'));
        if (!unit.empty() && unit.back() == 's' && unit.size() > 1) unit.pop_back();  // plural -> singular
        std::int64_t mult = 0;
        if (unit == "second" || unit == "sec" || unit == "s") mult = 1;
        else if (unit == "minute" || unit == "min") mult = 60;
        else if (unit == "hour" || unit == "hr" || unit == "h") mult = 3600;
        else if (unit == "day" || unit == "d") mult = 86400;
        else if (unit == "week" || unit == "w") mult = 604800;
        else return false;  // unknown / month / year
        total += num * mult;
        any = true;
    }
    if (!any) return false;
    secs = neg ? -total : total;
    return true;
}

// F9c: validate a UUID string (8-4-4-4-12 hex, optionally braced/uppercased) and write its
// CANONICAL form (lowercase, dashed) to `out`. Returns false on a malformed value. Pure.
[[nodiscard]] inline bool parse_uuid(const std::string& in, std::string& out) {
    std::string h;
    h.reserve(32);
    for (char ch : in) {
        if (ch == '-' || ch == '{' || ch == '}' || ch == ' ') continue;
        char lo = ch;
        if (lo >= 'A' && lo <= 'F') lo = static_cast<char>(lo - 'A' + 'a');
        const bool hex = (lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f');
        if (!hex) return false;
        h.push_back(lo);
    }
    if (h.size() != 32) return false;
    out.clear();
    out.reserve(36);
    for (std::size_t i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
        out.push_back(h[i]);
    }
    return true;
}
// F9c: a DETERMINISTIC v4-shaped UUID from (table_id, counter) — splitmix64 mixed into 128 bits
// with the version (4) and variant (10xx) nibbles set. Same inputs → same id on every replica, so
// the cross-check stays sound. NOT a random UUID (random is forbidden — it would diverge).
inline std::string format_uuid(std::uint32_t table_id, std::uint64_t counter) {
    auto mix = [](std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    };
    const std::uint64_t seed = (static_cast<std::uint64_t>(table_id) << 40) ^ (counter + 1);
    std::uint64_t hi = mix(seed);
    std::uint64_t lo = mix(seed ^ 0xD1B54A32D192ED03ULL);
    std::uint8_t b[16];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) b[8 + i] = static_cast<std::uint8_t>(lo >> (56 - 8 * i));
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10xx
    static const char* hexd = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hexd[b[i] >> 4]);
        s.push_back(hexd[b[i] & 0x0F]);
    }
    return s;
}

// F9e: parse a decimal STRING (optional sign) into a signed 128-bit integer. Returns false on a
// malformed digit or on overflow past the int128 range (NOT saturating — a clean error). Pure.
[[nodiscard]] inline bool parse_i128(const std::string& in, __int128& out) {
    std::size_t p = 0;
    bool neg = false;
    if (p < in.size() && (in[p] == '+' || in[p] == '-')) { neg = in[p] == '-'; ++p; }
    if (p >= in.size()) return false;
    unsigned __int128 v = 0;
    const unsigned __int128 lim = static_cast<unsigned __int128>(1) << 127;  // |INT128_MIN|
    const unsigned __int128 maxmag = neg ? lim : lim - 1;
    for (; p < in.size(); ++p) {
        if (in[p] < '0' || in[p] > '9') return false;
        const unsigned d = static_cast<unsigned>(in[p] - '0');
        if (v > (maxmag - d) / 10) return false;  // would overflow
        v = v * 10 + d;
    }
    out = neg ? -static_cast<__int128>(v) : static_cast<__int128>(v);
    return true;
}
// F9e: parse 'int[.frac]' (optional sign) into a scaled 128-bit fixed-point value (× 10^scale).
// Digits past `scale` are truncated. Returns false on malformed input or overflow. Pure.
[[nodiscard]] inline bool parse_decimal128(const std::string& in, std::uint8_t scale, __int128& out) {
    std::size_t p = 0;
    bool neg = false;
    if (p < in.size() && (in[p] == '+' || in[p] == '-')) { neg = in[p] == '-'; ++p; }
    unsigned __int128 v = 0;
    bool any = false;
    const unsigned __int128 lim = static_cast<unsigned __int128>(1) << 127;
    const unsigned __int128 maxmag = neg ? lim : lim - 1;
    auto push = [&](unsigned d) -> bool {
        if (v > (maxmag - d) / 10) return false;
        v = v * 10 + d;
        return true;
    };
    for (; p < in.size() && in[p] >= '0' && in[p] <= '9'; ++p) {
        if (!push(static_cast<unsigned>(in[p] - '0'))) return false;
        any = true;
    }
    std::uint8_t fdig = 0;
    if (p < in.size() && in[p] == '.') {
        ++p;
        for (; p < in.size() && in[p] >= '0' && in[p] <= '9'; ++p) {
            if (fdig < scale) { if (!push(static_cast<unsigned>(in[p] - '0'))) return false; ++fdig; }
            any = true;
        }
    }
    if (!any || p != in.size()) return false;
    for (std::uint8_t k = fdig; k < scale; ++k) { if (!push(0)) return false; }  // pad to scale
    out = neg ? -static_cast<__int128>(v) : static_cast<__int128>(v);
    return true;
}

// F9e: parse a numeric string into a 128-bit value, INFERRING the scale from its fractional digits
// ('9000000000' -> (9e9, scale 0); '0.0000000001' -> (1, scale 10)). Used when a bare string literal
// is an operand in INT128/DECIMAL128 arithmetic (no column to fix the scale). Pure.
[[nodiscard]] inline bool parse_decimal128_infer(const std::string& in, __int128& val,
                                                 std::uint8_t& scale) {
    const std::size_t dot = in.find('.');
    std::uint8_t sc = 0;
    if (dot != std::string::npos) {
        const std::size_t fd = in.size() - dot - 1;
        sc = static_cast<std::uint8_t>(fd > 38 ? 38 : fd);
    }
    if (!parse_decimal128(in, sc, val)) return false;
    scale = sc;
    return true;
}

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

// F1: the storage key for a ROW (single OR composite PK). For a single-column PK this is exactly
// encode_key(t, row[pk_index]) (byte-identical). For a composite PK (all-INT) it is the prefix
// followed by each PK column's fixed-width order-preserving INT encoding, concatenated — so the
// composite key sorts by the PK tuple lexicographically and is self-delimiting (9 bytes/column).
[[nodiscard]] inline Key encode_key_row(const Table& t, const std::vector<Datum>& row) {
    if (!t.composite_pk()) {
        return encode_key(t, row[t.pk_index]);
    }
    Key out = table_prefix(t.id);
    for (const std::size_t c : t.pk_columns) {
        out += encode_pk(row[c]);
    }
    return out;
}

// F1: is column `c` part of the PRIMARY KEY (single or composite)? PK columns live in the storage
// KEY, not the value, so the (de)serializers skip them.
[[nodiscard]] inline bool is_pk_col(const Table& t, std::size_t c) {
    if (!t.composite_pk()) return c == t.pk_index;
    return std::find(t.pk_columns.begin(), t.pk_columns.end(), c) != t.pk_columns.end();
}

// F1: reconstruct a composite PK's columns into `row` from the storage key (all-INT 9-byte chunks).
inline void decode_composite_pk(const Table& t, const Key& key, std::vector<Datum>& row) {
    constexpr std::size_t kPrefixLen = 6;  // "t" + be32 + ":"
    std::size_t off = kPrefixLen;
    for (const std::size_t c : t.pk_columns) {
        std::uint64_t bits = 0;
        for (std::size_t b = 1; b <= 8; ++b) {
            bits = (bits << 8) | static_cast<unsigned char>(key[off + b]);
        }
        row[c] = Datum::make_int(static_cast<std::int64_t>(bits ^ 0x8000000000000000ULL));
        off += 9;
    }
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
        if (is_pk_col(t, c)) {
            continue;  // the PK column(s) live in the key, not the value
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
// F9b: stamp each row Datum with its column's LOGICAL tag (DECIMAL/DATE/TIMESTAMP) so render()
// prints the logical form. Storage is raw INT; the tag is read-side presentation only.
inline void tag_logical_cols(const Table& t, std::vector<Datum>& row) {
    for (std::size_t c = 0; c < t.columns.size() && c < row.size(); ++c) {
        if (t.columns[c].logical != 0) {
            row[c].logical = t.columns[c].logical;
            row[c].scale = t.columns[c].scale;
            // F13 ENUM: populate the label string from the stored ordinal so render shows the label.
            if (t.columns[c].logical == 9 && !row[c].is_null && row[c].i >= 0 &&
                row[c].i < static_cast<std::int64_t>(t.columns[c].enum_labels.size())) {
                row[c].s = t.columns[c].enum_labels[static_cast<std::size_t>(row[c].i)];
            }
        }
    }
}

[[nodiscard]] inline std::vector<Datum> decode_row(const Table& t, const Key& key,
                                                   const Value& value) {
    std::vector<Datum> row(t.columns.size());
    if (t.composite_pk()) decode_composite_pk(t, key, row);  // F1
    else row[t.pk_index] = decode_pk(t, key);
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (is_pk_col(t, c)) {
            continue;  // PK column(s) come from the key, not the value
        }
        // F7: a column ADDed (ALTER TABLE) after this row was written has no bytes in the stored
        // value — pad it with its DEFAULT (or NULL). The encoder writes the PK from the key, so the
        // PK is never in this suffix.
        if (off >= value.size()) {
            row[c] = t.columns[c].has_default
                         ? (t.columns[c].type == Type::Int ? Datum::make_int(t.columns[c].default_i)
                                                           : Datum::make_text(t.columns[c].default_s))
                         : Datum::make_null(t.columns[c].type);
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
    tag_logical_cols(t, row);
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
    if (t.composite_pk()) decode_composite_pk(t, key, row);  // F1 (all PK cols; cheap INT)
    else if (need[t.pk_index]) row[t.pk_index] = decode_pk(t, key);
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (is_pk_col(t, c)) {
            continue;
        }
        if (off >= value.size()) {  // F7: ALTER-added suffix column — pad with DEFAULT/NULL
            if (need[c]) {
                row[c] = t.columns[c].has_default
                             ? (t.columns[c].type == Type::Int
                                    ? Datum::make_int(t.columns[c].default_i)
                                    : Datum::make_text(t.columns[c].default_s))
                             : Datum::make_null(t.columns[c].type);
            }
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
    tag_logical_cols(t, row);
}

[[nodiscard]] inline std::vector<Datum> decode_row_projected(
    const Table& t, const Key& key, const Value& value,
    const std::vector<bool>& need) {
    std::vector<Datum> row(t.columns.size());
    if (t.composite_pk()) decode_composite_pk(t, key, row);  // F1
    else if (need[t.pk_index]) row[t.pk_index] = decode_pk(t, key);
    std::size_t off = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        if (is_pk_col(t, c)) {
            continue;
        }
        if (off >= value.size()) {  // F7: ALTER-added suffix column — pad with DEFAULT/NULL
            if (need[c]) {
                row[c] = t.columns[c].has_default
                             ? (t.columns[c].type == Type::Int
                                    ? Datum::make_int(t.columns[c].default_i)
                                    : Datum::make_text(t.columns[c].default_s))
                             : Datum::make_null(t.columns[c].type);
            }
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
    tag_logical_cols(t, row);
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

    // DROP TABLE (F8): forget the table so a later reference is "unknown table" and a re-CREATE
    // of the same name starts empty. The on-disk data under the old (monotonic) table id is left
    // orphaned-but-invisible — matches the no-GC model (DROP INDEX likewise leaves tombstones).
    // Returns false if the table did not exist.
    bool remove(const std::string& name) {
        return tables_.erase(name) != 0;
    }

    [[nodiscard]] bool has(const std::string& name) const {
        return tables_.count(name) != 0;
    }

    // F3: iterate every table (ordered => deterministic) — used to find FK references on DELETE.
    [[nodiscard]] const std::map<std::string, Table>& all() const { return tables_; }

private:
    std::map<std::string, Table> tables_;  // ordered => deterministic
    std::uint32_t next_id_ = 1;            // 0 reserved (no table)
};

}  // namespace lockstep::query::sql
