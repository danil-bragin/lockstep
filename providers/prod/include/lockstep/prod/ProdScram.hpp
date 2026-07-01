#pragma once
// ProdScram.hpp — server-side SCRAM-SHA-256 (RFC 5802 / RFC 7677), the modern PostgreSQL
// SASL authentication mechanism, so the password is NEVER sent over the wire (the client
// proves knowledge of it via a challenge/response). Crypto is OpenSSL (HMAC-SHA-256,
// SHA-256, PBKDF2), which providers/prod already links for TLS — so this is gated on
// LOCKSTEP_TLS. All crypto stays in providers/prod (the lint-exempt boundary).
//
// The server must KNOW the user's password (or a stored SCRAM verifier) to derive the keys;
// unlike cleartext auth, it never receives the password. Here the daemon holds the cleartext
// password (from its flags) and derives SaltedPassword per handshake with a fresh salt.
#ifdef __linux__
#ifdef LOCKSTEP_TLS

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace lockstep::prod {

namespace scram_detail {

inline std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len) {
    std::array<std::uint8_t, 32> out{};
    ::SHA256(data, len, out.data());
    return out;
}

inline std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t klen,
                                                const std::uint8_t* msg, std::size_t mlen) {
    std::array<std::uint8_t, 32> out{};
    unsigned int olen = 0;
    ::HMAC(::EVP_sha256(), key, static_cast<int>(klen), msg, mlen, out.data(), &olen);
    return out;
}

inline std::array<std::uint8_t, 32> pbkdf2(const std::string& pw, const std::vector<std::uint8_t>& salt,
                                           int iters) {
    std::array<std::uint8_t, 32> out{};
    ::PKCS5_PBKDF2_HMAC(pw.data(), static_cast<int>(pw.size()), salt.data(),
                        static_cast<int>(salt.size()), iters, ::EVP_sha256(), 32, out.data());
    return out;
}

inline std::string b64_encode(const std::uint8_t* data, std::size_t len) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += T[(n >> 6) & 63];
        out += T[n & 63];
    }
    if (len - i == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += "==";
    } else if (len - i == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += T[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

inline std::vector<std::uint8_t> b64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (const char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Extract the value of "key=" (SCRAM attribute) from a comma-separated message.
inline std::string attr(const std::string& msg, char key) {
    const std::string pfx = std::string(1, key) + "=";
    std::size_t pos = 0;
    while (pos < msg.size()) {
        std::size_t end = msg.find(',', pos);
        if (end == std::string::npos) end = msg.size();
        if (msg.compare(pos, pfx.size(), pfx) == 0) {
            return msg.substr(pos + pfx.size(), end - pos - pfx.size());
        }
        pos = end + 1;
    }
    return "";
}

}  // namespace scram_detail

// One SCRAM-SHA-256 server exchange. begin() consumes the client-first-bare + the (server-
// known) password and yields the server-first-message; finish() verifies the client proof
// and yields the server-final-message (the server signature the client checks).
class ScramServer {
public:
    // client_first_bare = "n=<user>,r=<client-nonce>". Returns the server-first-message.
    [[nodiscard]] bool begin(const std::string& client_first_bare, const std::string& password) {
        const std::string cnonce = scram_detail::attr(client_first_bare, 'r');
        if (cnonce.empty()) {
            return false;
        }
        // Fresh server nonce + salt (real randomness — auth material, not sim state).
        std::array<std::uint8_t, 18> nb{};
        std::array<std::uint8_t, 16> sb{};
        if (::RAND_bytes(nb.data(), static_cast<int>(nb.size())) != 1 ||
            ::RAND_bytes(sb.data(), static_cast<int>(sb.size())) != 1) {
            return false;
        }
        const std::string snonce = scram_detail::b64_encode(nb.data(), nb.size());
        combined_nonce_ = cnonce + snonce;
        const std::vector<std::uint8_t> salt(sb.begin(), sb.end());
        salted_ = scram_detail::pbkdf2(password, salt, kIters);
        client_first_bare_ = client_first_bare;
        server_first_ = "r=" + combined_nonce_ + ",s=" +
                        scram_detail::b64_encode(salt.data(), salt.size()) + ",i=" +
                        std::to_string(kIters);
        return true;
    }

    [[nodiscard]] const std::string& server_first() const noexcept { return server_first_; }
    [[nodiscard]] const std::string& server_final() const noexcept { return server_final_; }

    // client_final = "c=biws,r=<combined>,p=<proof-b64>". Verifies the proof; on success sets
    // server_final() to "v=<server-signature-b64>". Returns true iff the proof is valid.
    [[nodiscard]] bool finish(const std::string& client_final) {
        const std::size_t ppos = client_final.rfind(",p=");
        if (ppos == std::string::npos) {
            return false;
        }
        const std::string without_proof = client_final.substr(0, ppos);
        const std::string proof_b64 = client_final.substr(ppos + 3);
        // Nonce must echo the combined nonce (anti-replay).
        if (scram_detail::attr(client_final, 'r') != combined_nonce_) {
            return false;
        }
        const std::vector<std::uint8_t> proof = scram_detail::b64_decode(proof_b64);
        if (proof.size() != 32) {
            return false;
        }
        const std::string auth_message =
            client_first_bare_ + "," + server_first_ + "," + without_proof;
        const auto* am = reinterpret_cast<const std::uint8_t*>(auth_message.data());

        static constexpr char kClientKey[] = "Client Key";
        static constexpr char kServerKey[] = "Server Key";
        const auto client_key = scram_detail::hmac_sha256(
            salted_.data(), salted_.size(), reinterpret_cast<const std::uint8_t*>(kClientKey), 10);
        const auto stored_key = scram_detail::sha256(client_key.data(), client_key.size());
        const auto client_sig =
            scram_detail::hmac_sha256(stored_key.data(), stored_key.size(), am, auth_message.size());
        // RecoveredClientKey = ClientProof XOR ClientSignature; SHA256 must match StoredKey.
        std::array<std::uint8_t, 32> recovered{};
        for (std::size_t i = 0; i < 32; ++i) {
            recovered[i] = static_cast<std::uint8_t>(proof[i] ^ client_sig[i]);
        }
        const auto check = scram_detail::sha256(recovered.data(), recovered.size());
        if (std::memcmp(check.data(), stored_key.data(), 32) != 0) {
            return false;
        }
        const auto server_key = scram_detail::hmac_sha256(
            salted_.data(), salted_.size(), reinterpret_cast<const std::uint8_t*>(kServerKey), 10);
        const auto server_sig =
            scram_detail::hmac_sha256(server_key.data(), server_key.size(), am, auth_message.size());
        server_final_ = "v=" + scram_detail::b64_encode(server_sig.data(), server_sig.size());
        return true;
    }

private:
    static constexpr int kIters = 4096;
    std::string client_first_bare_;
    std::string server_first_;
    std::string server_final_;
    std::string combined_nonce_;
    std::array<std::uint8_t, 32> salted_{};
};

}  // namespace lockstep::prod

#endif  // LOCKSTEP_TLS
#endif  // __linux__
