// Json.hpp — F13: a minimal, DETERMINISTIC JSON value model for the JSON type (logical 11).
// The canonical form (sorted object keys, compact, normalized numbers) is what gets STORED, so two
// equal documents serialize byte-identically across nodes — the byte-determinism contract. No float
// is parsed: a number is canonicalized as a string (strip sign-plus / leading zeros / trailing
// fraction zeros), so there is no platform-dependent rounding. Pure; no locale, no wall-clock.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lockstep::query::sql::json {

struct JVal {
    enum Kind : std::uint8_t { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    std::string text;  // Num: canonical digits; Str: the raw (unescaped) string
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
};

inline void skip_ws(const std::string& s, std::size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
}

[[nodiscard]] inline bool parse_string(const std::string& s, std::size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    while (p < s.size()) {
        const char c = s[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= s.size()) return false;
            const char e = s[p++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {  // \uXXXX -> raw bytes (BMP only; deterministic UTF-8 encode)
                    if (p + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[p++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

// Canonicalize a JSON number: optional '-', strip a leading '+', strip leading zeros (keep one before
// '.'), strip trailing zeros in the fraction (and a bare trailing '.'). Exponent notation is rejected
// (out of scope) so there is one canonical spelling per value.
[[nodiscard]] inline bool parse_number(const std::string& s, std::size_t& p, std::string& out) {
    std::size_t start = p;
    bool neg = false;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) { neg = s[p] == '-'; ++p; }
    std::string ip, fp;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ip.push_back(s[p++]);
    if (p < s.size() && s[p] == '.') {
        ++p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') fp.push_back(s[p++]);
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) return false;  // exponent unsupported
    if (ip.empty() && fp.empty()) { p = start; return false; }
    std::size_t i = 0;
    while (i + 1 < ip.size() && ip[i] == '0') ++i;  // strip leading zeros
    ip = ip.substr(i);
    if (ip.empty()) ip = "0";
    while (!fp.empty() && fp.back() == '0') fp.pop_back();  // strip trailing fraction zeros
    out.clear();
    const bool zero = ip == "0" && fp.empty();
    if (neg && !zero) out.push_back('-');
    out += ip;
    if (!fp.empty()) { out.push_back('.'); out += fp; }
    return true;
}

[[nodiscard]] inline bool parse_value(const std::string& s, std::size_t& p, JVal& v) {
    skip_ws(s, p);
    if (p >= s.size()) return false;
    const char c = s[p];
    if (c == '"') { v.kind = JVal::Str; return parse_string(s, p, v.text); }
    if (c == '{') {
        ++p;
        v.kind = JVal::Obj;
        skip_ws(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        for (;;) {
            skip_ws(s, p);
            std::string key;
            if (!parse_string(s, p, key)) return false;
            skip_ws(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            ++p;
            JVal child;
            if (!parse_value(s, p, child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return false;
        }
    }
    if (c == '[') {
        ++p;
        v.kind = JVal::Arr;
        skip_ws(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        for (;;) {
            JVal child;
            if (!parse_value(s, p, child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return false;
        }
    }
    if (s.compare(p, 4, "true") == 0) { v.kind = JVal::Bool; v.b = true; p += 4; return true; }
    if (s.compare(p, 5, "false") == 0) { v.kind = JVal::Bool; v.b = false; p += 5; return true; }
    if (s.compare(p, 4, "null") == 0) { v.kind = JVal::Null; p += 4; return true; }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) { v.kind = JVal::Num; return parse_number(s, p, v.text); }
    return false;
}

inline void escape_into(const std::string& in, std::string& out) {
    out.push_back('"');
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
}

// Serialize CANONICALLY: object keys sorted ascending (byte order), compact (no spaces).
inline void serialize(const JVal& v, std::string& out) {
    switch (v.kind) {
        case JVal::Null: out += "null"; return;
        case JVal::Bool: out += v.b ? "true" : "false"; return;
        case JVal::Num: out += v.text; return;
        case JVal::Str: escape_into(v.text, out); return;
        case JVal::Arr: {
            out.push_back('[');
            for (std::size_t k = 0; k < v.arr.size(); ++k) {
                if (k) out.push_back(',');
                serialize(v.arr[k], out);
            }
            out.push_back(']');
            return;
        }
        case JVal::Obj: {
            std::vector<const std::pair<std::string, JVal>*> ents;
            ents.reserve(v.obj.size());
            for (const auto& e : v.obj) ents.push_back(&e);
            std::sort(ents.begin(), ents.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            out.push_back('{');
            for (std::size_t k = 0; k < ents.size(); ++k) {
                if (k) out.push_back(',');
                escape_into(ents[k]->first, out);
                out.push_back(':');
                serialize(ents[k]->second, out);
            }
            out.push_back('}');
            return;
        }
    }
}

// Parse `in` and write its canonical form to `out`. Returns false on malformed JSON or trailing junk.
[[nodiscard]] inline bool canonical(const std::string& in, std::string& out) {
    std::size_t p = 0;
    JVal v;
    if (!parse_value(in, p, v)) return false;
    skip_ws(in, p);
    if (p != in.size()) return false;  // trailing junk
    out.clear();
    serialize(v, out);
    return true;
}

}  // namespace lockstep::query::sql::json
