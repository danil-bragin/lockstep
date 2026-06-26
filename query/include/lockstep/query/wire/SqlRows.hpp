#pragma once

// SqlRows — serialize/deserialize SQL SELECT result rows to a self-describing BLOB so they can ride
// the wire (MsgKind::SqlResult). The wire Protocol stays SQL-type-free (it carries the blob as an
// opaque string); BOTH the server (which has the rows) and a SQL-aware client/coordinator (which
// needs them back, e.g. distributed scatter-gather) use this codec. Format:
//   u32 nrows ; per row: u32 ncells ; per cell: str label, u8 type(0=int,1=text), u8 is_null,
//   then (type==int ? i64 value : str value).
// An empty blob decodes to zero rows (a write/DDL result ships no rows).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/query/sql/Engine.hpp>  // sql::ResultRow / sql::ExecResult / sql::Datum / sql::Type

namespace lockstep::query::wire {

inline void sr_put_u32(std::string& o, std::uint32_t v) {
    o.push_back(static_cast<char>(v >> 24));
    o.push_back(static_cast<char>(v >> 16));
    o.push_back(static_cast<char>(v >> 8));
    o.push_back(static_cast<char>(v));
}
inline std::uint32_t sr_get_u32(const std::string& s, std::size_t& p) {
    const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p])) << 24) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 1])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 2])) << 8) |
                            static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 3]));
    p += 4;
    return v;
}
inline void sr_put_u64(std::string& o, std::uint64_t v) {
    for (int b = 56; b >= 0; b -= 8) o.push_back(static_cast<char>(v >> b));
}
inline std::uint64_t sr_get_u64(const std::string& s, std::size_t& p) {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v = (v << 8) | static_cast<unsigned char>(s[p + b]);
    p += 8;
    return v;
}
inline void sr_put_s(std::string& o, const std::string& v) {
    sr_put_u32(o, static_cast<std::uint32_t>(v.size()));
    o += v;
}
inline std::string sr_get_s(const std::string& s, std::size_t& p) {
    const std::uint32_t n = sr_get_u32(s, p);
    std::string v = s.substr(p, n);
    p += n;
    return v;
}

inline std::string serialize_rows(const std::vector<sql::ResultRow>& rows) {
    std::string o;
    sr_put_u32(o, static_cast<std::uint32_t>(rows.size()));
    for (const sql::ResultRow& row : rows) {
        sr_put_u32(o, static_cast<std::uint32_t>(row.cells.size()));
        for (const auto& [label, d] : row.cells) {
            sr_put_s(o, label);
            o.push_back(static_cast<char>(d.type == sql::Type::Int ? 0 : 1));
            o.push_back(d.is_null ? 1 : 0);
            if (d.type == sql::Type::Int) {
                sr_put_u64(o, static_cast<std::uint64_t>(d.i));
            } else {
                sr_put_s(o, d.s);
            }
        }
    }
    return o;
}

inline std::vector<sql::ResultRow> deserialize_rows(const std::string& s) {
    std::vector<sql::ResultRow> rows;
    if (s.empty()) return rows;
    std::size_t p = 0;
    const std::uint32_t nr = sr_get_u32(s, p);
    rows.reserve(nr);
    for (std::uint32_t i = 0; i < nr; ++i) {
        sql::ResultRow row;
        const std::uint32_t nc = sr_get_u32(s, p);
        row.cells.reserve(nc);
        for (std::uint32_t j = 0; j < nc; ++j) {
            std::string label = sr_get_s(s, p);
            const sql::Type ty = (s[p++] == 0) ? sql::Type::Int : sql::Type::Text;
            const bool is_null = s[p++] != 0;
            sql::Datum d = (ty == sql::Type::Int)
                               ? sql::Datum::make_int(static_cast<std::int64_t>(sr_get_u64(s, p)))
                               : sql::Datum::make_text(sr_get_s(s, p));
            if (is_null) d = sql::Datum::make_null(ty);
            row.cells.emplace_back(std::move(label), std::move(d));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace lockstep::query::wire
