#pragma once
// KeyedOp.hpp — the keyed-op ENCODING carried inside a replicated consensus value.
//
// The Raft layer commits OPAQUE byte strings; it neither knows nor cares what they
// mean. To turn a replicated log into an applied KEYSPACE (so replicas can be
// cross-checked by keyspace_hash, plan P3), the application must agree on how a
// committed value encodes a keyed mutation. This is that contract: a client encodes
// a put/del into the value it submits; every replica decodes the SAME bytes and
// applies the SAME mutation to its state-machine engine — so identical committed
// prefixes yield identical keyspaces (and any divergence is corruption, not disagreement).
//
// LAYOUT (little-endian, self-framed):
//   [u8 type: 0 put | 1 del] [u32 klen] [key bytes] [value bytes...]
// A del carries no value (klen key bytes, then end). Opaque key/value bytes (any byte,
// incl. '\0'). Deterministic: the same KeyedOp encodes to the same bytes.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lockstep::storage {

struct KeyedOp {
    bool del = false;
    std::string key;
    std::string value;  // empty for a del
};

// Encode a keyed op to its self-framed byte string (the value a client submits).
[[nodiscard]] inline std::string encode_keyed_op(const KeyedOp& op) {
    std::string out;
    out.reserve(5 + op.key.size() + op.value.size());
    out.push_back(static_cast<char>(op.del ? 1 : 0));
    const std::uint32_t klen = static_cast<std::uint32_t>(op.key.size());
    out.push_back(static_cast<char>(klen & 0xFFu));
    out.push_back(static_cast<char>((klen >> 8) & 0xFFu));
    out.push_back(static_cast<char>((klen >> 16) & 0xFFu));
    out.push_back(static_cast<char>((klen >> 24) & 0xFFu));
    out.append(op.key);
    if (!op.del) out.append(op.value);
    return out;
}

// Decode a keyed op from a committed value. Returns false (leaving `out` unspecified)
// on any malformed/short input: too few bytes for the header, a bad type byte, or a
// klen that runs past the end. A caller that gets false should treat the value as
// "not a keyed op" (e.g. a legacy/opaque payload) rather than fabricate a mutation.
[[nodiscard]] inline bool decode_keyed_op(std::string_view v, KeyedOp& out) {
    if (v.size() < 5) return false;
    const auto type = static_cast<unsigned char>(v[0]);
    if (type > 1) return false;
    const std::uint32_t klen = static_cast<std::uint32_t>(static_cast<unsigned char>(v[1])) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(v[2])) << 8) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(v[3])) << 16) |
                               (static_cast<std::uint32_t>(static_cast<unsigned char>(v[4])) << 24);
    if (static_cast<std::size_t>(5) + klen > v.size()) return false;
    out.del = (type == 1);
    out.key.assign(v.data() + 5, klen);
    out.value.assign(v.data() + 5 + klen, v.size() - 5 - klen);
    if (out.del && !out.value.empty()) return false;  // a del must carry no value.
    return true;
}

}  // namespace lockstep::storage
