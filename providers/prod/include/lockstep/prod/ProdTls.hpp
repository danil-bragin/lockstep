#pragma once

// ProdTls.hpp — TLS TRANSPORT ENCRYPTION for the prod TCP transport. Wraps each
// ProdConnection's byte stream in a real TLS session (OpenSSL), so the verified async
// core moves bytes over an ENCRYPTED wire with ZERO core changes — TLS is entirely a
// ProdNetwork-layer concern, below the core::INetwork byte interface.
//
// ----------------------------------------------------------------------------
// DESIGN (memory-BIO TLS over the SAME non-blocking reactor IO)
// ----------------------------------------------------------------------------
// The plaintext ProdNetwork already pumps the SOCKET via the reactor: pump_read drains
// the socket into a buffer, pump_write flushes a buffer to the socket, EPOLLIN/EPOLLOUT
// re-arm partial IO. TLS slots in WITHOUT touching that pump by using OpenSSL MEMORY BIOs
// (no BIO_set_fd, so OpenSSL itself NEVER calls a socket syscall):
//
//   * rbio_ (read BIO)  — CIPHERTEXT INBOUND. The reactor reads ciphertext off the socket
//     and feeds it here (feed_ciphertext -> BIO_write(rbio_)). SSL_read then decrypts from
//     rbio_ into APP plaintext.
//   * wbio_ (write BIO) — CIPHERTEXT OUTBOUND. SSL_write encrypts APP plaintext into wbio_;
//     the reactor drains ciphertext out (drain_ciphertext <- BIO_read(wbio_)) and writes it
//     to the socket via the EXISTING pump_write path.
//
// So the data flow is:  app frame --SSL_write--> wbio_ --[socket out]--> wire
//                        wire --[socket in]--> rbio_ --SSL_read--> app frame
//
// NON-BLOCKING + REACTOR-INTEGRATED. The session is created with SSL_set_app_data and
// driven purely by feeding/draining the memory BIOs — it NEVER blocks. On WANT_READ the
// session needs more inbound ciphertext (the reactor keeps EPOLLIN armed; when the socket
// delivers more bytes we feed rbio_ and retry). On WANT_WRITE there is outbound ciphertext
// in wbio_ to flush (the network arms EPOLLOUT; pump_write drains it). The handshake is the
// SAME loop: SSL_do_handshake drives bytes into wbio_ (flushed to the peer) and consumes
// rbio_; WANT_READ/WANT_WRITE just mean "more wire IO needed", resumed on the next reactor
// fd-ready event. This is exactly the WANT_READ/WANT_WRITE deferral the plaintext path does
// for a partial socket read/write — one uniform reactor model.
//
// mTLS (peers) vs TLS (clients):
//   * A peer-to-peer context verifies the PEER cert against the shared CA and REQUIRES it
//     (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT) on BOTH the server and client
//     side — mutual auth. A peer with no cert / a wrong-CA cert FAILS the handshake.
//   * A client-facing server context presents its server cert (clients verify it against
//     the CA) and OPTIONALLY requests a client cert (SSL_VERIFY_PEER, but it does not fail
//     if absent for the admin client — server-auth TLS). The admin CLIENT verifies the
//     server cert against the CA (SSL_VERIFY_PEER); a wrong-CA server is REJECTED.
//
// LIFETIME (V-RKV1): the SSL/SSL_CTX are RAII (ProdTlsSession / ProdTlsContext own them and
// free in the dtor — no leak under ASan/LSan). DECRYPTED app bytes are copied into the
// caller's framing buffer the instant SSL_read returns them, so no live span points into an
// OpenSSL-internal buffer across a suspend. SINGLE-THREAD per reactor: each shard's reactor
// owns its TLS sessions; an SSL object is NEVER shared across threads (no cross-thread SSL
// use — the OpenSSL per-object single-thread contract).
//
// LINUX-ONLY (prod transport). Compiled only under __linux__ AND when LOCKSTEP_TLS is
// defined (the build defines it on Linux when OpenSSL is found). When OpenSSL is absent
// (e.g. a Mac host that somehow compiles this), the whole header is empty — the macOS host
// never builds the prod transport anyway (the targets are if(UNIX AND NOT APPLE)-guarded).
//
// providers/prod is the lint-exempt + now real-CRYPTO boundary: OpenSSL calls are permitted
// HERE (and only here). No crypto leaks into core/cli (those pass cert PATHS as strings).

#if defined(__linux__) && defined(LOCKSTEP_TLS)

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>  // X509 peer-cert subject CN extraction (the PRINCIPAL)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lockstep::prod {

// TLS configuration threaded from the daemon/admin config down to the network bus. Paths
// are plain strings (set by cli, which never touches OpenSSL). `enabled` gates TLS on; when
// false the transport is plaintext (the existing tests' --no-tls path). `require_client_cert`
// distinguishes mTLS (peers — required) from server-auth TLS (clients — optional cert).
struct TlsConfig {
    bool enabled = false;
    std::string cert_path;   // this node's certificate (PEM)
    std::string key_path;    // this node's private key (PEM)
    std::string ca_path;     // the CA bundle used to verify the PEER (PEM)
};

// Role of a TLS endpoint: a connection is either the ACCEPTING side (server — runs the TLS
// server handshake) or the CONNECTING side (client — runs the client handshake).
enum class TlsRole : std::uint8_t { Server, Client };

// What kind of authentication this endpoint enforces.
//   * MutualPeer  — mTLS: REQUIRE + verify the peer cert against the CA (consensus peers).
//   * ServerAuth  — server presents its cert; the client verifies it; the server only
//     OPTIONALLY requests a client cert (admin/wire clients — server-auth TLS).
enum class TlsAuth : std::uint8_t { MutualPeer, ServerAuth };

// ---------------------------------------------------------------------------
// ProdTlsContext — RAII wrapper around SSL_CTX. One per (role, auth) combination on a
// reactor; reused for every connection of that kind (SSL_CTX is meant to be shared by many
// SSL objects on the SAME thread). Loads the cert/key/CA and sets the verify policy.
// ---------------------------------------------------------------------------
class ProdTlsContext {
public:
    ProdTlsContext() = default;

    ProdTlsContext(const ProdTlsContext&) = delete;
    ProdTlsContext& operator=(const ProdTlsContext&) = delete;
    ProdTlsContext(ProdTlsContext&&) = delete;
    ProdTlsContext& operator=(ProdTlsContext&&) = delete;

    ~ProdTlsContext() {
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    // Build the SSL_CTX for `role` enforcing `auth`. Loads the cert chain + private key (a
    // server ALWAYS needs them; a peer client needs them for mutual auth; a server-auth
    // client loads them only if present). Loads the CA for peer verification. Returns false
    // (ctx stays null / valid()==false) on any setup failure — the caller surfaces it
    // rather than running an unauthenticated/cleartext connection.
    bool init(TlsRole role, TlsAuth auth, const TlsConfig& cfg) {
        if (!cfg.enabled) {
            return false;
        }
        const SSL_METHOD* method =
            (role == TlsRole::Server) ? TLS_server_method() : TLS_client_method();
        ctx_ = SSL_CTX_new(method);
        if (ctx_ == nullptr) {
            return false;
        }
        // Floor at TLS 1.2; prefer 1.3 (OpenSSL negotiates the highest mutually supported).
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        // CA for verifying the peer's certificate (both directions use the shared dev CA).
        if (!cfg.ca_path.empty()) {
            if (SSL_CTX_load_verify_locations(ctx_, cfg.ca_path.c_str(), nullptr) != 1) {
                return fail();
            }
        }

        // Our own certificate chain + private key.
        const bool need_own_cert =
            (role == TlsRole::Server) || (auth == TlsAuth::MutualPeer);
        const bool have_own_cert = !cfg.cert_path.empty() && !cfg.key_path.empty();
        if (need_own_cert || have_own_cert) {
            if (have_own_cert) {
                if (SSL_CTX_use_certificate_chain_file(ctx_, cfg.cert_path.c_str()) != 1) {
                    return fail();
                }
                if (SSL_CTX_use_PrivateKey_file(ctx_, cfg.key_path.c_str(),
                                                SSL_FILETYPE_PEM) != 1) {
                    return fail();
                }
                if (SSL_CTX_check_private_key(ctx_) != 1) {
                    return fail();
                }
            } else if (need_own_cert) {
                return fail();  // a server / mTLS peer MUST have a cert+key.
            }
        }

        // Verify policy:
        //   * mTLS peer — BOTH sides REQUIRE + verify the peer cert (no cert => handshake
        //     fails). This is the auth TEETH: a wrong-CA / no-cert peer is REFUSED.
        //   * server-auth client — the CLIENT verifies the server cert (SSL_VERIFY_PEER):
        //     a wrong-CA server cert FAILS verification (the client rejects it).
        //   * server-auth server — REQUEST a client cert but do NOT fail if absent
        //     (optional client cert); a presented client cert is still verified vs the CA.
        int verify = SSL_VERIFY_NONE;
        if (auth == TlsAuth::MutualPeer) {
            verify = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        } else {  // ServerAuth
            if (role == TlsRole::Client) {
                verify = SSL_VERIFY_PEER;  // client always verifies the server cert.
            } else {
                verify = SSL_VERIFY_PEER;  // server requests a client cert (optional).
            }
        }
        SSL_CTX_set_verify(ctx_, verify, nullptr);
        SSL_CTX_set_verify_depth(ctx_, 4);
        role_ = role;
        return true;
    }

    [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] SSL_CTX* ctx() const noexcept { return ctx_; }
    [[nodiscard]] TlsRole role() const noexcept { return role_; }

private:
    bool fail() {
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        return false;
    }

    SSL_CTX* ctx_ = nullptr;
    TlsRole role_ = TlsRole::Server;
};

// ---------------------------------------------------------------------------
// ProdTlsSession — RAII wrapper around one SSL object + its two memory BIOs. One per
// ProdConnection. Drives the handshake + read/write transform via the memory BIOs only
// (never a socket syscall inside OpenSSL). The reactor feeds inbound ciphertext, drains
// outbound ciphertext, and retries on WANT_READ/WANT_WRITE when the socket is ready.
// ---------------------------------------------------------------------------
class ProdTlsSession {
public:
    // Create the SSL from `ctx` and wire two memory BIOs. `role` decides the handshake
    // direction (server vs client). On any failure valid()==false and the connection is
    // dropped (no cleartext fallback for a TLS-enabled link).
    ProdTlsSession(const ProdTlsContext& ctx, TlsRole role) {
        if (!ctx.valid()) {
            return;
        }
        ssl_ = SSL_new(ctx.ctx());
        if (ssl_ == nullptr) {
            return;
        }
        rbio_ = BIO_new(BIO_s_mem());  // inbound ciphertext (we BIO_write into it)
        wbio_ = BIO_new(BIO_s_mem());  // outbound ciphertext (we BIO_read out of it)
        if (rbio_ == nullptr || wbio_ == nullptr) {
            cleanup();
            return;
        }
        // SSL takes ownership of both BIOs (freed by SSL_free) — do NOT free them here.
        SSL_set_bio(ssl_, rbio_, wbio_);
        if (role == TlsRole::Server) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
        }
        valid_ = true;
    }

    ProdTlsSession(const ProdTlsSession&) = delete;
    ProdTlsSession& operator=(const ProdTlsSession&) = delete;
    ProdTlsSession(ProdTlsSession&&) = delete;
    ProdTlsSession& operator=(ProdTlsSession&&) = delete;

    ~ProdTlsSession() { cleanup(); }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool handshake_done() const noexcept { return handshake_done_; }

    // The PRINCIPAL — the VERIFIED peer certificate's subject Common Name (CN). This is
    // the application identity RBAC maps to a role. Returns "" when no peer cert was
    // presented (server-auth TLS with an optional/absent client cert) or before the
    // handshake completes. The cert here is already CA-VERIFIED by the handshake
    // (SSL_VERIFY_PEER on the server ctx + the CA load), so the CN is trustworthy — an
    // attacker cannot present an arbitrary CN without a CA-signed cert (mTLS is the gate
    // BELOW this; the CN is just the authenticated identity it carries). Cached after the
    // first extraction; cheap thereafter. providers/prod is the OpenSSL boundary — this
    // X509 call is permitted HERE and nowhere else; callers receive a plain std::string.
    [[nodiscard]] std::string peer_cn() const {
        if (!valid_ || !handshake_done_) {
            return {};
        }
        if (peer_cn_cached_) {
            return peer_cn_;
        }
        peer_cn_cached_ = true;
        X509* cert = SSL_get_peer_certificate(ssl_);  // bumps the refcount — free below.
        if (cert == nullptr) {
            return {};  // no peer cert presented (server-auth TLS, optional client cert).
        }
        X509_NAME* subj = X509_get_subject_name(cert);
        if (subj != nullptr) {
            char buf[256];
            const int len =
                X509_NAME_get_text_by_NID(subj, NID_commonName, buf, sizeof(buf));
            if (len > 0) {
                peer_cn_.assign(buf, static_cast<std::size_t>(len));
            }
        }
        X509_free(cert);  // release the refcount SSL_get_peer_certificate took (no leak).
        return peer_cn_;
    }

    // FEED inbound ciphertext (read off the socket) into the read BIO. The next SSL_read /
    // handshake step decrypts from it. Returns false on a BIO error (drop the connection).
    bool feed_ciphertext(std::span<const std::byte> bytes) {
        if (!valid_ || bytes.empty()) {
            return valid_;
        }
        const int n = BIO_write(rbio_, bytes.data(), static_cast<int>(bytes.size()));
        // The memory BIO grows; a short write should not happen, but guard it.
        return n == static_cast<int>(bytes.size());
    }

    // DRAIN any pending outbound ciphertext from the write BIO into `out` (appended). The
    // reactor then flushes `out` to the socket via its existing pump_write path. Always
    // succeeds (memory BIO read); appends nothing when there is no pending ciphertext.
    void drain_ciphertext(std::vector<std::byte>& out) {
        if (!valid_) {
            return;
        }
        for (;;) {
            std::byte tmp[4096];
            const int n = BIO_read(wbio_, tmp, static_cast<int>(sizeof(tmp)));
            if (n <= 0) {
                break;  // no more pending ciphertext (BIO_read returns <=0 when empty).
            }
            out.insert(out.end(), tmp, tmp + n);
        }
    }

    // True iff the write BIO has outbound ciphertext waiting to be flushed to the socket
    // (so the reactor must arm EPOLLOUT). Pending bytes mean WANT_WRITE-style readiness.
    [[nodiscard]] bool wants_flush() const noexcept {
        return valid_ && BIO_ctrl_pending(wbio_) > 0;
    }

    // Drive the TLS handshake one step. Returns:
    //   * +1  handshake COMPLETE (encrypted app IO can now proceed).
    //   *  0  handshake still in progress (WANT_READ/WANT_WRITE — more wire IO needed; the
    //         reactor re-arms and resumes on the next fd-ready event). Any outbound
    //         handshake ciphertext is now in wbio_ (caller drains+flushes it).
    //   * -1  handshake FAILED (cert verify failure / protocol error) — DROP the connection.
    //         This is the auth gate: a wrong-CA / no-cert peer lands here.
    int do_handshake() {
        if (!valid_) {
            return -1;
        }
        if (handshake_done_) {
            return 1;
        }
        const int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            handshake_done_ = true;
            return 1;
        }
        const int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return 0;  // need more wire IO — resume on the next reactor turn.
        }
        return -1;  // SSL_ERROR_SSL (verify/protocol fail), SSL_ERROR_SYSCALL, etc.
    }

    // ENCRYPT `plaintext` (one app frame's bytes) into the write BIO. The caller then
    // drains the write BIO to the socket. Returns false on a fatal TLS error (drop the
    // connection); WANT_WRITE here cannot happen with an unbounded memory wbio_, but a
    // WANT_READ (rare mid-renegotiation) returns true with nothing consumed (the caller
    // retries after feeding more inbound ciphertext). On success ALL bytes are encrypted.
    bool encrypt(std::span<const std::byte> plaintext) {
        if (!valid_ || !handshake_done_) {
            return valid_;
        }
        std::size_t off = 0;
        while (off < plaintext.size()) {
            const int n = SSL_write(ssl_, plaintext.data() + off,
                                    static_cast<int>(plaintext.size() - off));
            if (n > 0) {
                off += static_cast<std::size_t>(n);
                continue;
            }
            const int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // Memory wbio_ is unbounded so WANT_WRITE is not expected; WANT_READ means a
                // renegotiation needs inbound bytes — defer the rest (the caller retries).
                pending_plain_.assign(plaintext.begin() + static_cast<std::ptrdiff_t>(off),
                                      plaintext.end());
                return true;
            }
            return false;  // fatal
        }
        return true;
    }

    // DECRYPT available app plaintext out of the read BIO into `out` (appended). Call after
    // feed_ciphertext. Returns true on success (possibly appending zero bytes when only a
    // partial TLS record has arrived — WANT_READ), false on a fatal TLS error or a clean
    // peer close_notify (drop the connection). Decrypted bytes are COPIED into `out`
    // immediately, so no span points into an OpenSSL buffer across a suspend (V-RKV1).
    bool decrypt(std::vector<std::byte>& out) {
        if (!valid_ || !handshake_done_) {
            return valid_;
        }
        for (;;) {
            std::byte tmp[4096];
            const int n = SSL_read(ssl_, tmp, static_cast<int>(sizeof(tmp)));
            if (n > 0) {
                out.insert(out.end(), tmp, tmp + n);
                continue;
            }
            const int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return true;  // need more inbound ciphertext — not an error.
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                return false;  // peer sent close_notify — orderly TLS shutdown.
            }
            return false;  // fatal TLS/protocol error.
        }
    }

    // Retry any plaintext deferred by a mid-write WANT_READ (rare). Returns false on fatal.
    bool flush_pending_plain() {
        if (pending_plain_.empty()) {
            return true;
        }
        std::vector<std::byte> pend;
        pend.swap(pending_plain_);
        return encrypt(std::span<const std::byte>(pend.data(), pend.size()));
    }

private:
    void cleanup() {
        if (ssl_ != nullptr) {
            SSL_free(ssl_);  // frees the attached rbio_/wbio_ too (SSL_set_bio took them).
            ssl_ = nullptr;
            rbio_ = nullptr;
            wbio_ = nullptr;
        } else {
            // ssl_ never created: free any BIOs we did create.
            if (rbio_ != nullptr) {
                BIO_free(rbio_);
                rbio_ = nullptr;
            }
            if (wbio_ != nullptr) {
                BIO_free(wbio_);
                wbio_ = nullptr;
            }
        }
        valid_ = false;
    }

    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;  // inbound ciphertext (owned by ssl_ once SSL_set_bio runs)
    BIO* wbio_ = nullptr;  // outbound ciphertext (owned by ssl_ once SSL_set_bio runs)
    bool valid_ = false;
    bool handshake_done_ = false;
    std::vector<std::byte> pending_plain_{};  // plaintext deferred by a WANT_READ mid-write
    mutable bool peer_cn_cached_ = false;     // the principal (peer CN) extracted yet?
    mutable std::string peer_cn_{};           // cached verified peer-cert subject CN
};

}  // namespace lockstep::prod

#endif  // __linux__ && LOCKSTEP_TLS
