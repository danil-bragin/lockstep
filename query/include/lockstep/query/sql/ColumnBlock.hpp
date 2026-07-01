#pragma once

// ColumnBlock.hpp — DURABLE columnar block (SoA chunk) codec for the columnar storage
// engine (PERF_PLAN columnar rollout, option A). A block holds ONE column's values for a
// contiguous run of rows (a ~1024-row chunk) in struct-of-arrays form, so a scan reads a
// column as a dense array (cache-friendly, branchless, SIMD-able — the measured 2.92x vs
// the row/AoS layout) and a projection reads ONLY the needed columns' blocks.
//
// DURABILITY: a block is just a KV VALUE written through the SAME verified commit path +
// WAL the row store uses — the durable core (WAL framing/CRC, fsync, recovery prefix,
// the jepsen acked==durable invariant) is UNCHANGED. This file is ONLY the byte-exact,
// deterministic, platform-independent serialisation of one chunk; nothing here touches
// the engine or storage. Proven in isolation (round-trip + determinism) before any
// write/read path is wired, so a format bug can never reach the durable core.
//
// Layout (big-endian, self-delimiting):
//   [u8 type]                      0 = Int, 1 = Text (raw), 2 = Text (dictionary-encoded)
//   [be32 count]                   rows in this chunk
//   [null bitmap: ceil(count/8)]   bit r set => row r is NULL
//   INT:      [count x be64]       every slot (NULL slot stores 0; bitmap is authoritative)
//   TEXT raw: [be32 arena_len][arena bytes][count x be32 end-offset]
//             row r's bytes = arena[end(r-1) .. end(r)) (end(-1)=0); a NULL row has 0 length.
//   TEXT dict (type 2): stores each DISTINCT value once + a per-row code — a big win for
//     low-cardinality TEXT (e.g. an enum/region column). Chosen at encode time only when it
//     is SMALLER than the raw form (so it never regresses). Deterministic: codes are assigned
//     in first-occurrence ROW order. Decode yields the IDENTICAL ColumnChunk as raw.
//     [be32 ndistinct]
//     [be32 dict_arena_len][dict_arena bytes][ndistinct x be32 end-offset]   -- the distinct values
//     [u8 code_width]                        -- 1, 2, or 4 bytes per code
//     [count x code]                         -- big-endian code into the dictionary (NULL row: 0)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <lockstep/query/sql/Catalog.hpp>  // Datum, Type, put_be32, get_be32

namespace lockstep::query::sql {

// be64 helpers (Catalog has be32 only). Big-endian, deterministic.
inline void put_be64(std::string& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
    }
}
[[nodiscard]] inline std::uint64_t get_be64(const std::string& in, std::size_t off) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b) {
        v = (v << 8) | static_cast<unsigned char>(in[off + b]);
    }
    return v;
}

// A decoded SoA column chunk: dense per-column arrays + a null mask. Scans run over
// `ints` / `texts` directly; `nulls[r]` is authoritative for NULL.
struct ColumnChunk {
    Type type = Type::Int;
    std::uint32_t count = 0;
    std::vector<std::int64_t> ints;   // size==count when type==Int
    std::vector<std::string> texts;   // size==count when type==Text
    std::vector<bool> nulls;          // size==count

    [[nodiscard]] Datum at(std::uint32_t r) const {
        if (nulls[r]) {
            return Datum::make_null(type);
        }
        return type == Type::Int ? Datum::make_int(ints[r]) : Datum::make_text(texts[r]);
    }
};

// Encode one column's cells (a chunk) into a durable, byte-exact SoA block value.
[[nodiscard]] inline std::string encode_column_block(Type type,
                                                     const std::vector<Datum>& cells) {
    const std::uint32_t count = static_cast<std::uint32_t>(cells.size());
    std::vector<unsigned char> bm((cells.size() + 7) / 8, 0);
    for (std::size_t r = 0; r < cells.size(); ++r) {
        if (cells[r].is_null) {
            bm[r / 8] |= static_cast<unsigned char>(1u << (r % 8));
        }
    }
    const auto append_header = [&](std::string& out, char tb) {
        out.push_back(tb);
        put_be32(out, count);
        out.append(reinterpret_cast<const char*>(bm.data()), bm.size());
    };
    std::string out;
    if (type == Type::Int) {
        append_header(out, 0);
        for (const Datum& d : cells) {
            put_be64(out, static_cast<std::uint64_t>(d.is_null ? 0 : d.i));
        }
        return out;
    }
    // TEXT — build the dictionary (codes assigned in first-occurrence ROW order = deterministic)
    // and the raw arena in one pass; pick the SMALLER encoding so dict never regresses.
    std::map<std::string, std::uint32_t> code_of;
    std::vector<const std::string*> dict;  // dict[code] -> value (points into cells)
    std::vector<std::uint32_t> codes(count, 0);
    std::size_t raw_arena = 0;
    std::size_t dict_arena = 0;
    for (std::size_t r = 0; r < cells.size(); ++r) {
        if (cells[r].is_null) {
            continue;  // code 0, ignored (bitmap authoritative)
        }
        raw_arena += cells[r].s.size();
        const auto it = code_of.find(cells[r].s);
        if (it == code_of.end()) {
            const std::uint32_t nc = static_cast<std::uint32_t>(dict.size());
            code_of.emplace(cells[r].s, nc);
            dict.push_back(&cells[r].s);
            dict_arena += cells[r].s.size();
            codes[r] = nc;
        } else {
            codes[r] = it->second;
        }
    }
    const std::uint32_t nd = static_cast<std::uint32_t>(dict.size());
    const std::uint8_t cw = nd <= 256 ? 1 : (nd <= 65536 ? 2 : 4);
    const std::size_t raw_size = 4 + raw_arena + static_cast<std::size_t>(count) * 4;
    const std::size_t dict_size = 4 + 4 + dict_arena + static_cast<std::size_t>(nd) * 4 + 1 +
                                  static_cast<std::size_t>(count) * cw;
    if (dict_size < raw_size) {  // DICTIONARY encoding (type 2)
        append_header(out, 2);
        put_be32(out, nd);
        std::string da;
        std::vector<std::uint32_t> ends;
        ends.reserve(nd);
        for (const std::string* s : dict) {
            da += *s;
            ends.push_back(static_cast<std::uint32_t>(da.size()));
        }
        put_be32(out, static_cast<std::uint32_t>(da.size()));
        out += da;
        for (const std::uint32_t e : ends) {
            put_be32(out, e);
        }
        out.push_back(static_cast<char>(cw));
        for (const std::uint32_t c : codes) {
            for (int sh = (cw - 1) * 8; sh >= 0; sh -= 8) {
                out.push_back(static_cast<char>((c >> sh) & 0xFF));
            }
        }
        return out;
    }
    // RAW TEXT (type 1)
    append_header(out, 1);
    std::string arena;
    std::vector<std::uint32_t> ends;
    ends.reserve(cells.size());
    for (const Datum& d : cells) {
        if (!d.is_null) {
            arena += d.s;
        }
        ends.push_back(static_cast<std::uint32_t>(arena.size()));
    }
    put_be32(out, static_cast<std::uint32_t>(arena.size()));
    out += arena;
    for (const std::uint32_t e : ends) {
        put_be32(out, e);
    }
    return out;
}

// Decode a block value back to an SoA chunk (the inverse of encode_column_block).
[[nodiscard]] inline ColumnChunk decode_column_block(const std::string& v) {
    ColumnChunk c;
    std::size_t off = 0;
    const unsigned char tb = static_cast<unsigned char>(v[off]);
    c.type = tb == 0 ? Type::Int : Type::Text;
    off += 1;
    c.count = get_be32(v, off);
    off += 4;
    const std::size_t bmlen = (c.count + 7) / 8;
    c.nulls.assign(c.count, false);
    for (std::uint32_t r = 0; r < c.count; ++r) {
        c.nulls[r] = ((static_cast<unsigned char>(v[off + r / 8]) >> (r % 8)) & 1u) != 0;
    }
    off += bmlen;
    if (tb == 0) {  // INT
        c.ints.resize(c.count);
        for (std::uint32_t r = 0; r < c.count; ++r) {
            c.ints[r] = static_cast<std::int64_t>(get_be64(v, off));
            off += 8;
        }
    } else if (tb == 2) {  // DICTIONARY TEXT
        const std::uint32_t nd = get_be32(v, off);
        off += 4;
        const std::uint32_t da_len = get_be32(v, off);
        off += 4;
        const std::size_t da_off = off;
        off += da_len;
        std::vector<std::string> dict(nd);
        std::uint32_t prev = 0;
        for (std::uint32_t i = 0; i < nd; ++i) {
            const std::uint32_t end = get_be32(v, off);
            off += 4;
            dict[i] = v.substr(da_off + prev, end - prev);
            prev = end;
        }
        const std::uint8_t cw = static_cast<std::uint8_t>(static_cast<unsigned char>(v[off]));
        off += 1;
        c.texts.resize(c.count);
        for (std::uint32_t r = 0; r < c.count; ++r) {
            std::uint32_t code = 0;
            for (std::uint8_t b = 0; b < cw; ++b) {
                code = (code << 8) | static_cast<unsigned char>(v[off++]);
            }
            c.texts[r] = c.nulls[r] ? std::string() : dict[code];
        }
    } else {  // RAW TEXT (tb == 1)
        const std::uint32_t arena_len = get_be32(v, off);
        off += 4;
        const std::size_t arena_off = off;
        off += arena_len;
        c.texts.resize(c.count);
        std::uint32_t prev = 0;
        for (std::uint32_t r = 0; r < c.count; ++r) {
            const std::uint32_t end = get_be32(v, off);
            off += 4;
            c.texts[r] = v.substr(arena_off + prev, end - prev);
            prev = end;
        }
    }
    return c;
}

}  // namespace lockstep::query::sql
