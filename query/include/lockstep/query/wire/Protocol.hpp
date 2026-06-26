#pragma once

// Protocol.hpp — Phase 6 Stage B, C6.3. THE CLIENT<->SERVER WIRE PROTOCOL.
//
// Source of truth: briefs/phase6.md (C6.3 = the client network protocol; the
// Postgres-wire shim is explicitly DEFERRED). This is the hand-rolled, length-
// prefixed, CRC'd binary framing the client stub (ClientStub.hpp) encodes and the
// server (Server.hpp) decodes — and vice-versa for responses. It wraps the
// Stage-F surface types (Query.hpp / Database.hpp) onto the wire; it reinvents
// NO txn / query logic.
//
// ============================================================================
// THE REQUEST / RESPONSE SET.
//   Request kinds (client -> server):
//     Ping       — liveness probe (no body); response echoes the request id.
//     Submit     — submit ONE one-shot txn (its declared reads + a chosen op the
//                  server materializes into a deterministic body) and its params.
//                  Carries a SUBMIT KEY (the idempotent request id) so a re-
//                  delivered Submit applies the txn EXACTLY ONCE (V exactly-once).
//     Query      — run a typed read at a call-site-visible D5 level (Strict /
//                  Snapshot / Bounded / RYW), composed of POINT / RANGE steps.
//   Response kinds (server -> client):
//     Pong       — Ping ack.
//     SubmitOk   — the txn committed (or was de-duplicated): commit version +
//                  status + result token + committed writes.
//     QueryOk    — the query result: per-step point/range values + served prefix.
//     Error      — a decode/dispatch error (a textual reason).
// Every response carries the request id it answers, so the client matches a
// reply to its outstanding request and IGNORES a duplicate / late reply.
//
// ============================================================================
// FRAMING + INTEGRITY (reuse the V-NOTORN lesson from harness/kv/Codec.hpp).
//   A frame is: [u8 msg_kind] <kind-specific body> [u32 crc32].
//   Every variable field is length-prefixed (u32 len + bytes); every integer is
//   fixed-width little-endian. The TRAILING per-frame CRC-32 is computed over the
//   whole body (kind + body, NOT the crc itself). On decode we re-derive the CRC
//   and REJECT on mismatch — so a torn / bit-rotted / truncated frame is a CLEAN
//   decode FAILURE (return false), never a mis-decoded / fabricated request or
//   response. A bad frame is dropped on the floor (the sender retries); it is
//   NEVER applied. This is the exact V-NOTORN discipline the KV WAL learned.
//
// ============================================================================
// DETERMINISM (query/ is NOT lint-exempt): NO wall-clock, NO threads, NO
// std::*_distribution, NO unordered iteration affecting output, NO ambient
// randomness. encode/decode are pure byte manipulation; ordered maps only. The
// op a Submit names is a small ENUM the server maps to a fixed deterministic
// body — the body is a pure function of its reads (V-DET-USER preserved: the wire
// never ships executable code, only a named op + params).

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/query/Query.hpp>
#include <lockstep/txn/Transaction.hpp>

namespace lockstep::query::wire {

using Key = txn::Key;
using Value = txn::Value;
using Seq = txn::Seq;
using SessionId = txn::SessionId;
using Level = txn::Level;
using Step = query::Step;            // a query read step (point / range)
using StepKind = query::StepKind;

// The wire message discriminator. Stable, append-only.
enum class MsgKind : std::uint8_t {
    Ping = 0,      // client -> server: liveness
    Submit = 1,    // client -> server: one-shot txn submit (idempotent)
    Query = 2,     // client -> server: typed D5 read
    Pong = 3,      // server -> client: ping ack
    SubmitOk = 4,  // server -> client: commit info
    QueryOk = 5,   // server -> client: query result
    Error = 6,     // server -> client: a decode/dispatch failure
    SqlExec = 7,   // client -> server: a SQL statement string (CREATE/INSERT/SELECT/...)
    SqlResult = 8, // server -> client: SQL exec result (ok, affected, row count)
};

[[nodiscard]] inline bool valid_msg_kind(std::uint8_t k) noexcept {
    return k <= static_cast<std::uint8_t>(MsgKind::SqlResult);
}

// The named, deterministic ops a Submit can carry. The wire NEVER ships a
// std::function (that would not be byte-encodable AND would breach V-DET-USER);
// instead the client names ONE of a fixed catalogue of pure ops and supplies its
// params. The server materializes the matching deterministic body. Append-only.
enum class SubmitOp : std::uint8_t {
    // Unconditional put: write params[0].key = params[0].value (a 1-write txn).
    Put = 0,
    // Transfer `amount` from key `a` to key `b` (reads a,b; writes a-amt, b+amt).
    // The canonical idempotent-money-move op (the deterministic-transferId idea).
    Transfer = 1,
    // Read-modify-write increment: read key, write key = (parsed int + delta).
    Increment = 2,
};

[[nodiscard]] inline bool valid_submit_op(std::uint8_t op) noexcept {
    return op <= static_cast<std::uint8_t>(SubmitOp::Increment);
}

// One named parameter the op body consumes. Opaque byte key + value + an integer
// amount (used by Transfer / Increment). A small flat record keeps the codec
// branch-free.
struct OpParam {
    Key key;
    Value value;
    std::int64_t amount = 0;
};

// ----------------------------------------------------------------------------
// Deterministic integer <-> opaque-value encoding for the arithmetic ops
// (Transfer / Increment). A balance is stored as its DECIMAL string value; an
// absent / unparseable value reads as 0. No locale, no streams, no nondeterminism
// — pure base-10 byte manipulation. Round-trips: parse_balance(encode_balance(x))
// == x for all std::int64_t x.
// ----------------------------------------------------------------------------
[[nodiscard]] inline std::string encode_balance(std::int64_t v) {
    if (v == 0) {
        return "0";
    }
    const bool neg = v < 0;
    // Build magnitude in an unsigned to handle INT64_MIN without UB.
    std::uint64_t mag =
        neg ? (~static_cast<std::uint64_t>(v) + 1ULL) : static_cast<std::uint64_t>(v);
    std::string digits;
    while (mag > 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(mag % 10ULL)));
        mag /= 10ULL;
    }
    std::string out;
    if (neg) {
        out.push_back('-');
    }
    for (std::size_t i = digits.size(); i-- > 0;) {
        out.push_back(digits[i]);
    }
    return out;
}

[[nodiscard]] inline std::int64_t parse_balance(const std::optional<Value>& v) {
    if (!v.has_value() || v->empty()) {
        return 0;
    }
    const std::string& s = *v;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
    }
    std::uint64_t mag = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return 0;  // unparseable -> 0 (deterministic fallback)
        }
        mag = mag * 10ULL + static_cast<std::uint64_t>(c - '0');
    }
    return neg ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
}

// ----------------------------------------------------------------------------
// THE DECODED REQUEST. One flat struct (kind-tagged); not every field is
// meaningful for every kind. `req_id` is the per-client monotonic request id used
// to MATCH a reply. `submit_key` is the idempotent SUBMIT key: a re-delivered
// Submit with the same submit_key is applied EXACTLY ONCE by the server.
// ----------------------------------------------------------------------------
struct Request {
    MsgKind kind = MsgKind::Ping;
    std::uint64_t req_id = 0;  // client-chosen, monotonic; matches the reply

    // --- Submit ---
    std::uint64_t submit_key = 0;     // idempotent submit id (dedup / exactly-once)
    SubmitOp op = SubmitOp::Put;
    std::vector<OpParam> params;      // op inputs

    // --- Query ---
    Level level = Level::StrictSerializable;
    Seq snapshot_version = txn::kNoSeq;  // Snapshot level param
    Seq max_lag = 0;                     // Bounded level param
    SessionId session = 0;               // RYW level param
    std::vector<Step> steps;             // composed point/range read steps

    // --- SqlExec ---
    std::string sql;  // a SQL statement string (server runs it through sql::Engine)
};

// ----------------------------------------------------------------------------
// THE DECODED RESPONSE. Flat, kind-tagged. `req_id` echoes the request it
// answers. A POINT result is (key, present, value); a RANGE result is a key-
// ascending list of (key,value) rows.
// ----------------------------------------------------------------------------
struct PointWire {
    Key key;
    bool present = false;
    Value value;
};
struct RangeWire {
    Key lo;
    Key hi;
    bool hi_unbounded = false;
    std::vector<std::pair<Key, Value>> rows;  // key-ascending live rows
};

struct Response {
    MsgKind kind = MsgKind::Pong;
    std::uint64_t req_id = 0;  // echoes the request id

    // --- SubmitOk ---
    std::uint8_t status = 0;          // txn::Status (Committed/Aborted/Pending)
    Seq commit_version = txn::kNoSeq;
    std::string result;               // the body's observable result token
    std::map<Key, Value> writes;      // committed writes (ordered)

    // --- QueryOk ---
    Level level = Level::StrictSerializable;
    Seq served_version = txn::kNoSeq;  // the committed prefix the query read as-of
    std::vector<PointWire> points;
    std::vector<RangeWire> ranges;

    // --- Error ---
    std::string error;

    // --- SqlResult ---
    bool sql_ok = false;             // statement executed without error
    std::string sql_error;           // error text iff !sql_ok
    std::uint64_t sql_affected = 0;  // rows inserted/updated/deleted/returned
    std::uint64_t sql_rows = 0;      // SELECT result row count (rows not shipped — bench)
};

// ===========================================================================
// PRIMITIVE ENCODERS — fixed-width little-endian, length-prefixed strings. Same
// discipline as harness/kv/Codec.hpp (byte-deterministic, no endianness ambiguity).
// ===========================================================================

inline void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}
inline void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
inline void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
inline void put_i64(std::vector<std::byte>& out, std::int64_t v) {
    put_u64(out, static_cast<std::uint64_t>(v));  // two's-complement bit pattern
}
inline void put_str(std::vector<std::byte>& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

// ===========================================================================
// PRIMITIVE DECODERS — bounds-checked; return false on short input (never read
// out of bounds). A clean failure on a torn frame, never UB.
// ===========================================================================

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool u8(std::uint8_t& v) noexcept {
        if (pos_ + 1 > data_.size()) {
            return false;
        }
        v = static_cast<std::uint8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& v) noexcept {
        if (pos_ + 4 > data_.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                     data_[pos_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos_ += 4;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& v) noexcept {
        if (pos_ + 8 > data_.size()) {
            return false;
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(
                     data_[pos_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
        }
        pos_ += 8;
        return true;
    }
    [[nodiscard]] bool i64(std::int64_t& v) noexcept {
        std::uint64_t raw = 0;
        if (!u64(raw)) {
            return false;
        }
        v = static_cast<std::int64_t>(raw);
        return true;
    }
    [[nodiscard]] bool str(std::string& s) {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        if (pos_ + len > data_.size()) {
            return false;
        }
        s.assign(len, '\0');
        for (std::uint32_t i = 0; i < len; ++i) {
            s[i] = static_cast<char>(static_cast<std::uint8_t>(data_[pos_ + i]));
        }
        pos_ += len;
        return true;
    }
    [[nodiscard]] std::size_t consumed() const noexcept { return pos_; }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// ===========================================================================
// PER-FRAME INTEGRITY — hand-rolled CRC-32 (reflected, poly 0xEDB88320, init/
// xorout 0xFFFFFFFF). Identical to zlib/PNG CRC-32, no external dependency, fully
// byte-deterministic. (Same routine as harness/kv/Codec.hpp; the V-NOTORN lesson.)
// ===========================================================================
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b));
        for (int k = 0; k < 8; ++k) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Append the trailing CRC over everything written so far (the frame body).
inline void seal_crc(std::vector<std::byte>& out) {
    const std::uint32_t crc = crc32(std::span<const std::byte>(out.data(), out.size()));
    put_u32(out, crc);
}

// Verify + strip the trailing CRC. On success, `body` views the frame WITHOUT the
// 4-byte CRC trailer. Returns false on a frame too short to hold a CRC OR on a CRC
// mismatch (torn / bit-rotted body that still parses within bounds).
[[nodiscard]] inline bool open_crc(std::span<const std::byte> frame,
                                   std::span<const std::byte>& body) noexcept {
    if (frame.size() < 4) {
        return false;
    }
    const std::size_t body_len = frame.size() - 4;
    std::uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) {
        stored |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                      frame[body_len + static_cast<std::size_t>(i)]))
                  << (8 * i);
    }
    const std::uint32_t actual =
        crc32(std::span<const std::byte>(frame.data(), body_len));
    if (actual != stored) {
        return false;
    }
    body = std::span<const std::byte>(frame.data(), body_len);
    return true;
}

// ===========================================================================
// REQUEST CODEC.
// ===========================================================================

[[nodiscard]] inline std::vector<std::byte> encode_request(const Request& r) {
    std::vector<std::byte> out;
    put_u8(out, static_cast<std::uint8_t>(r.kind));
    put_u64(out, r.req_id);
    switch (r.kind) {
        case MsgKind::Ping:
            break;
        case MsgKind::Submit: {
            put_u64(out, r.submit_key);
            put_u8(out, static_cast<std::uint8_t>(r.op));
            put_u32(out, static_cast<std::uint32_t>(r.params.size()));
            for (const OpParam& p : r.params) {
                put_str(out, p.key);
                put_str(out, p.value);
                put_i64(out, p.amount);
            }
            break;
        }
        case MsgKind::Query: {
            put_u8(out, static_cast<std::uint8_t>(r.level));
            put_u64(out, r.snapshot_version);
            put_u64(out, r.max_lag);
            put_u64(out, r.session);
            put_u32(out, static_cast<std::uint32_t>(r.steps.size()));
            for (const Step& s : r.steps) {
                put_u8(out, static_cast<std::uint8_t>(s.kind));
                put_str(out, s.key);
                put_str(out, s.hi);
                put_u8(out, s.hi_unbounded ? 1u : 0u);
            }
            break;
        }
        case MsgKind::SqlExec:
            put_u64(out, r.submit_key);  // dedup key: a retried SqlExec applies EXACTLY ONCE
            put_str(out, r.sql);
            break;
        default:
            // A response kind never goes out as a request; still seal so a stray
            // never decodes (kind-validity is rechecked on decode anyway).
            break;
    }
    seal_crc(out);
    return out;
}

[[nodiscard]] inline bool decode_request(std::span<const std::byte> frame, Request& r) {
    std::span<const std::byte> body;
    if (!open_crc(frame, body)) {
        return false;  // torn / corrupt frame: reject, never fabricate
    }
    ByteReader rd(body);
    std::uint8_t kind = 0;
    if (!rd.u8(kind) || !valid_msg_kind(kind)) {
        return false;
    }
    if (!rd.u64(r.req_id)) {
        return false;
    }
    r.kind = static_cast<MsgKind>(kind);
    switch (r.kind) {
        case MsgKind::Ping:
            return rd.consumed() == body.size();
        case MsgKind::Submit: {
            std::uint8_t op = 0;
            std::uint32_t n = 0;
            if (!rd.u64(r.submit_key) || !rd.u8(op) || !valid_submit_op(op) ||
                !rd.u32(n)) {
                return false;
            }
            r.op = static_cast<SubmitOp>(op);
            r.params.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                OpParam p;
                if (!rd.str(p.key) || !rd.str(p.value) || !rd.i64(p.amount)) {
                    return false;
                }
                r.params.push_back(std::move(p));
            }
            return rd.consumed() == body.size();
        }
        case MsgKind::Query: {
            std::uint8_t lvl = 0;
            std::uint32_t n = 0;
            if (!rd.u8(lvl) || lvl > static_cast<std::uint8_t>(Level::ReadYourWrites)) {
                return false;
            }
            if (!rd.u64(r.snapshot_version) || !rd.u64(r.max_lag) ||
                !rd.u64(r.session) || !rd.u32(n)) {
                return false;
            }
            r.level = static_cast<Level>(lvl);
            r.steps.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint8_t sk = 0;
                std::uint8_t hu = 0;
                Step s;
                if (!rd.u8(sk) || sk > static_cast<std::uint8_t>(StepKind::Range)) {
                    return false;
                }
                if (!rd.str(s.key) || !rd.str(s.hi) || !rd.u8(hu)) {
                    return false;
                }
                s.kind = static_cast<StepKind>(sk);
                s.hi_unbounded = hu != 0;
                r.steps.push_back(std::move(s));
            }
            return rd.consumed() == body.size();
        }
        case MsgKind::SqlExec: {
            if (!rd.u64(r.submit_key) || !rd.str(r.sql)) {
                return false;
            }
            return rd.consumed() == body.size();
        }
        default:
            return false;  // a response kind is not a valid request
    }
}

// ===========================================================================
// RESPONSE CODEC.
// ===========================================================================

[[nodiscard]] inline std::vector<std::byte> encode_response(const Response& r) {
    std::vector<std::byte> out;
    put_u8(out, static_cast<std::uint8_t>(r.kind));
    put_u64(out, r.req_id);
    switch (r.kind) {
        case MsgKind::Pong:
            break;
        case MsgKind::SubmitOk: {
            put_u8(out, r.status);
            put_u64(out, r.commit_version);
            put_str(out, r.result);
            put_u32(out, static_cast<std::uint32_t>(r.writes.size()));
            for (const auto& [k, v] : r.writes) {  // std::map -> key-ascending
                put_str(out, k);
                put_str(out, v);
            }
            break;
        }
        case MsgKind::QueryOk: {
            put_u8(out, static_cast<std::uint8_t>(r.level));
            put_u64(out, r.served_version);
            put_u32(out, static_cast<std::uint32_t>(r.points.size()));
            for (const PointWire& p : r.points) {
                put_str(out, p.key);
                put_u8(out, p.present ? 1u : 0u);
                put_str(out, p.value);
            }
            put_u32(out, static_cast<std::uint32_t>(r.ranges.size()));
            for (const RangeWire& rg : r.ranges) {
                put_str(out, rg.lo);
                put_str(out, rg.hi);
                put_u8(out, rg.hi_unbounded ? 1u : 0u);
                put_u32(out, static_cast<std::uint32_t>(rg.rows.size()));
                for (const auto& [k, v] : rg.rows) {
                    put_str(out, k);
                    put_str(out, v);
                }
            }
            break;
        }
        case MsgKind::Error:
            put_str(out, r.error);
            break;
        case MsgKind::SqlResult:
            put_u8(out, r.sql_ok ? 1u : 0u);
            put_str(out, r.sql_error);
            put_u64(out, r.sql_affected);
            put_u64(out, r.sql_rows);
            break;
        default:
            break;
    }
    seal_crc(out);
    return out;
}

[[nodiscard]] inline bool decode_response(std::span<const std::byte> frame, Response& r) {
    std::span<const std::byte> body;
    if (!open_crc(frame, body)) {
        return false;  // torn / corrupt frame: reject, never fabricate
    }
    ByteReader rd(body);
    std::uint8_t kind = 0;
    if (!rd.u8(kind) || !valid_msg_kind(kind)) {
        return false;
    }
    if (!rd.u64(r.req_id)) {
        return false;
    }
    r.kind = static_cast<MsgKind>(kind);
    switch (r.kind) {
        case MsgKind::Pong:
            return rd.consumed() == body.size();
        case MsgKind::SubmitOk: {
            std::uint32_t n = 0;
            if (!rd.u8(r.status) || !rd.u64(r.commit_version) || !rd.str(r.result) ||
                !rd.u32(n)) {
                return false;
            }
            r.writes.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                Key k;
                Value v;
                if (!rd.str(k) || !rd.str(v)) {
                    return false;
                }
                r.writes.emplace(std::move(k), std::move(v));
            }
            return rd.consumed() == body.size();
        }
        case MsgKind::QueryOk: {
            std::uint8_t lvl = 0;
            std::uint32_t np = 0;
            if (!rd.u8(lvl) || lvl > static_cast<std::uint8_t>(Level::ReadYourWrites)) {
                return false;
            }
            if (!rd.u64(r.served_version) || !rd.u32(np)) {
                return false;
            }
            r.level = static_cast<Level>(lvl);
            r.points.clear();
            for (std::uint32_t i = 0; i < np; ++i) {
                PointWire p;
                std::uint8_t present = 0;
                if (!rd.str(p.key) || !rd.u8(present) || !rd.str(p.value)) {
                    return false;
                }
                p.present = present != 0;
                r.points.push_back(std::move(p));
            }
            std::uint32_t nr = 0;
            if (!rd.u32(nr)) {
                return false;
            }
            r.ranges.clear();
            for (std::uint32_t i = 0; i < nr; ++i) {
                RangeWire rg;
                std::uint8_t hu = 0;
                std::uint32_t nrows = 0;
                if (!rd.str(rg.lo) || !rd.str(rg.hi) || !rd.u8(hu) || !rd.u32(nrows)) {
                    return false;
                }
                rg.hi_unbounded = hu != 0;
                for (std::uint32_t j = 0; j < nrows; ++j) {
                    Key k;
                    Value v;
                    if (!rd.str(k) || !rd.str(v)) {
                        return false;
                    }
                    rg.rows.emplace_back(std::move(k), std::move(v));
                }
                r.ranges.push_back(std::move(rg));
            }
            return rd.consumed() == body.size();
        }
        case MsgKind::Error:
            if (!rd.str(r.error)) {
                return false;
            }
            return rd.consumed() == body.size();
        case MsgKind::SqlResult: {
            std::uint8_t ok = 0;
            if (!rd.u8(ok) || !rd.str(r.sql_error) || !rd.u64(r.sql_affected) ||
                !rd.u64(r.sql_rows)) {
                return false;
            }
            r.sql_ok = ok != 0;
            return rd.consumed() == body.size();
        }
        default:
            return false;
    }
}

}  // namespace lockstep::query::wire
