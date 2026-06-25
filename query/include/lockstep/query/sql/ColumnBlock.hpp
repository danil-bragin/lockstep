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
//   [u8 type]                      0 = Int, 1 = Text
//   [be32 count]                   rows in this chunk
//   [null bitmap: ceil(count/8)]   bit r set => row r is NULL
//   INT:  [count x be64]           every slot (NULL slot stores 0; bitmap is authoritative)
//   TEXT: [be32 arena_len][arena bytes][count x be32 end-offset]
//         row r's bytes = arena[end(r-1) .. end(r)) (end(-1)=0); a NULL row has 0 length.

#include <cstdint>
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
    std::string out;
    out.push_back(static_cast<char>(type == Type::Int ? 0 : 1));
    put_be32(out, static_cast<std::uint32_t>(cells.size()));
    std::vector<unsigned char> bm((cells.size() + 7) / 8, 0);
    for (std::size_t r = 0; r < cells.size(); ++r) {
        if (cells[r].is_null) {
            bm[r / 8] |= static_cast<unsigned char>(1u << (r % 8));
        }
    }
    out.append(reinterpret_cast<const char*>(bm.data()), bm.size());
    if (type == Type::Int) {
        for (const Datum& d : cells) {
            put_be64(out, static_cast<std::uint64_t>(d.is_null ? 0 : d.i));
        }
    } else {
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
        for (std::uint32_t e : ends) {
            put_be32(out, e);
        }
    }
    return out;
}

// Decode a block value back to an SoA chunk (the inverse of encode_column_block).
[[nodiscard]] inline ColumnChunk decode_column_block(const std::string& v) {
    ColumnChunk c;
    std::size_t off = 0;
    c.type = static_cast<unsigned char>(v[off]) == 0 ? Type::Int : Type::Text;
    off += 1;
    c.count = get_be32(v, off);
    off += 4;
    const std::size_t bmlen = (c.count + 7) / 8;
    c.nulls.assign(c.count, false);
    for (std::uint32_t r = 0; r < c.count; ++r) {
        c.nulls[r] = ((static_cast<unsigned char>(v[off + r / 8]) >> (r % 8)) & 1u) != 0;
    }
    off += bmlen;
    if (c.type == Type::Int) {
        c.ints.resize(c.count);
        for (std::uint32_t r = 0; r < c.count; ++r) {
            c.ints[r] = static_cast<std::int64_t>(get_be64(v, off));
            off += 8;
        }
    } else {
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
