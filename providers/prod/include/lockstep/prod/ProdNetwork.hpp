#pragma once

// ProdNetwork.hpp — Phase 7 S4b. The PRODUCTION INetwork provider: real TCP over
// loopback, registered on the S4a ProdReactor's epoll loop, so the verified async
// core moves bytes over real sockets UNCHANGED. It is the HONEST counterpart to
// sim::SimNetwork — same frozen core::INetwork contract, but real sockets + framing
// + async connect + partial read/write + reconnect underneath.
//
// V-PROD-CONTRACT: ProdNetwork passes the SAME universal INetwork conformance
// checks as SimNetwork (local() stable; send accepted on a live link; recv delivers
// the right from+payload; per-link delivery in send order). TCP is an HONEST,
// ORDERED transport: it does NOT inject drop/dup/reorder/partition — those are the
// SIM-ONLY fault model (tier B), DELIBERATELY ABSENT here. Per-link order is a TCP
// GUARANTEE (a single connection delivers bytes in send order), which resolves the
// S1 per-link-order flag for the TCP-scoped prod Network.
//
// ----------------------------------------------------------------------------
// MODEL (single-threaded, reactor-driven, never a blocking syscall)
// ----------------------------------------------------------------------------
//   * One ProdReactor drives every node in-process (S4b is in-process; multi-
//     process is S5). Each node owns a non-blocking LISTEN socket bound to an
//     ephemeral loopback port, registered on the reactor (EPOLLIN -> accept).
//   * A node maps each peer Endpoint id -> (host, port) via the cluster config
//     (NodeConfig). The first send() to a peer ESTABLISHES a non-blocking connect
//     (EPOLLOUT completes it); the connection is then REUSED for later sends.
//   * FRAMING: every message is a 4-byte little-endian LENGTH prefix + the payload
//     bytes. A read buffer per connection re-assembles frames split/coalesced
//     across EPOLLIN events. A HELLO frame (the connecting node's 8-byte id, sent
//     first) lets the ACCEPTING side learn the peer's Endpoint, so recv()'s `from`
//     is correct even on an inbound connection.
//   * send(to, payload): frame it, queue it on the connection's WRITE buffer, drive
//     EPOLLOUT flushing (handle PARTIAL writes). The returned Future completes when
//     the message's bytes are fully ACCEPTED into the socket (the "accepted for
//     delivery" contract — NOT once the peer received them).
//   * recv(): completes with the next fully-reassembled inbound frame (its `from` is
//     the connection's learned peer id). Frames arriving with no parked waiter queue
//     up; a recv with a queued frame completes immediately (FIFO).
//   * Message bytes the INetwork contract says are valid ONLY during the callback
//     are COPIED into a per-node retained buffer before the span is handed out, so a
//     later read into the connection buffer can never invalidate a delivered span
//     (V-RKV1 — no live span into a growable container across a suspend).
//
// LINUX-ONLY. epoll + the socket plumbing are Linux-scoped this phase; the whole
// class is compiled only under __linux__ (the CMake target + its test are added
// only on Linux), so the macOS host build stays green (this target simply absent).
//
// providers/prod/ is the lint-exempt boundary zone: socket/epoll syscalls are
// permitted HERE (and only here). Single-threaded; no real threads; every socket is
// non-blocking (never a blocking accept/connect/read/write that stalls the loop).
// Every fd is RAII-closed (no leaks under ASan/LSan).

#ifdef __linux__

#include <arpa/inet.h>  // htons, inet_pton — ALLOWED only under providers/
#include <fcntl.h>      // fcntl, O_NONBLOCK
#include <netinet/in.h> // sockaddr_in
#include <netinet/tcp.h>// TCP_NODELAY
#include <sys/socket.h> // socket, bind, listen, accept4, connect, send, recv
#include <sys/epoll.h>  // EPOLLIN/EPOLLOUT masks
#include <unistd.h>     // close

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdTls.hpp>  // TLS transport (no-op header unless LOCKSTEP_TLS)

namespace lockstep::prod {

using core::Endpoint;
using core::Error;
using core::ErrorCode;
using core::Future;
using core::INetwork;
using core::Message;
using core::Promise;

// Cluster address map: Endpoint id -> loopback port. Built as nodes bind their
// listen sockets (each gets an ephemeral port the bus records here), so an outbound
// connect knows where to dial. Sorted-by-id vector — deterministic, no unordered.
struct NodeAddr {
    std::uint64_t id = 0;
    std::uint16_t port = 0;
};

// The 4-byte LE length prefix size + the HELLO id size (8-byte LE node id sent as
// the connecting side's first bytes so the accepting side learns the peer).
inline constexpr std::size_t kLenPrefix = 4;
inline constexpr std::size_t kHelloLen = 8;

// ---------------------------------------------------------------------------
// A single TCP connection (inbound or outbound). Owns one fd (RAII-closed), a
// write buffer (with per-message completion promises), and a read buffer that
// re-assembles framed messages. Non-blocking throughout.
// ---------------------------------------------------------------------------
class ProdConnection {
public:
    // Outbound: `peer` is known up front; we send a HELLO first so the far side
    // learns OUR id. Inbound: peer id is learned from the inbound HELLO (peer_known
    // stays false until then). connected==false until a non-blocking connect lands.
    ProdConnection(int fd, Endpoint peer, bool peer_known, bool connected) noexcept
        : fd_(fd), peer_(peer), peer_known_(peer_known), connected_(connected) {}

    ProdConnection(const ProdConnection&) = delete;
    ProdConnection& operator=(const ProdConnection&) = delete;
    ProdConnection(ProdConnection&&) = delete;
    ProdConnection& operator=(ProdConnection&&) = delete;

    ~ProdConnection() { close_fd(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] Endpoint peer() const noexcept { return peer_; }
    [[nodiscard]] bool peer_known() const noexcept { return peer_known_; }
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    [[nodiscard]] bool dead() const noexcept { return fd_ < 0; }
    [[nodiscard]] bool wants_write() const noexcept {
        if (!out_.empty() || !connected_) {
            return true;
        }
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        // TLS: pending handshake/app ciphertext, or staged app frames not yet flushed, or a
        // queued client HELLO awaiting the handshake — all need EPOLLOUT to make progress.
        if (tls_ != nullptr) {
            if (tls_->wants_flush() || !tls_pending_.empty() || !hello_pending_.empty()) {
                return true;
            }
            if (!tls_->handshake_done()) {
                return true;  // keep driving the handshake (it may need to flush).
            }
        }
#endif
        return false;
    }

    void mark_connected() noexcept { connected_ = true; }
    void learn_peer(Endpoint p) noexcept {
        peer_ = p;
        peer_known_ = true;
    }

    // A pending send: when the cumulative bytes flushed reach `done_at`, the
    // promise completes (the message is fully accepted into the socket).
    struct PendingSend {
        std::size_t done_at = 0; // out_total_ value at which this send is complete
        Promise<Error> promise;  // completed when flushed past done_at
    };

    // PERF (S8.6): frame `payload` DIRECTLY into the connection's out_ buffer (4-byte
    // LE length prefix + the bytes), with NO intermediate `framed` vector and NO
    // byte-by-byte push_back — one reserve + a bulk insert. This removes the per-send
    // temporary allocation AND the double copy that send() previously paid (build a
    // framed vector, then copy it byte-wise into out_). Records the completion promise
    // to fire when this message is fully written. Behavior-identical wire bytes.
    void queue_framed(std::span<const std::byte> payload, Promise<Error> p) {
        const std::size_t n = payload.size();
        const std::size_t total = kLenPrefix + n;
        out_.reserve(out_.size() + total);
        out_.push_back(static_cast<std::byte>(n & 0xFF));
        out_.push_back(static_cast<std::byte>((n >> 8) & 0xFF));
        out_.push_back(static_cast<std::byte>((n >> 16) & 0xFF));
        out_.push_back(static_cast<std::byte>((n >> 24) & 0xFF));
        out_.insert(out_.end(), payload.begin(), payload.end());
        const std::size_t done_at = out_base_ + out_.size();
        pending_.push_back(PendingSend{done_at, std::move(p)});
    }

    // Queue raw bytes with NO completion promise (the HELLO handshake). Bulk insert.
    void queue_raw(std::span<const std::byte> bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
    }

    // Mutable access for the node's IO pump (kept inside the same TU).
    std::vector<std::byte>& out() noexcept { return out_; }
    std::vector<std::byte>& in() noexcept { return in_; }
    std::size_t& out_base() noexcept { return out_base_; }
    std::vector<PendingSend>& pending() noexcept { return pending_; }

#if defined(__linux__) && defined(LOCKSTEP_TLS)
    // ---- TLS (transport encryption) -------------------------------------
    // When TLS is on, in_ holds CIPHERTEXT read off the socket (fed into the session's
    // read BIO) and out_ holds CIPHERTEXT to write to the socket (drained from the write
    // BIO). app_in_ holds the DECRYPTED app bytes awaiting frame reassembly (the plaintext
    // analogue of in_ in the cleartext path). A non-TLS connection leaves tls_ null and
    // app bytes flow through in_ directly (byte-identical to the original).
    void attach_tls(std::unique_ptr<ProdTlsSession> s) noexcept { tls_ = std::move(s); }
    [[nodiscard]] ProdTlsSession* tls() noexcept { return tls_.get(); }
    [[nodiscard]] bool has_tls() const noexcept { return tls_ != nullptr; }
    std::vector<std::byte>& app_in() noexcept { return app_in_; }
    // A connecting (client-role) TLS endpoint must send its HELLO only AFTER the handshake
    // completes (it cannot be queued as cleartext into out_). hello_pending_ stashes it.
    void stash_hello(std::vector<std::byte> h) { hello_pending_ = std::move(h); }
    [[nodiscard]] std::vector<std::byte>& hello_pending() noexcept { return hello_pending_; }

    // A framed app message awaiting TLS encryption + flush. `plain` is the framed bytes
    // (length prefix + payload); `encrypted` flips true once SSL_write has consumed it;
    // `cipher_done_at` is the out_ (ciphertext) cumulative offset at which its ciphertext
    // is fully flushed (set when encrypted). The promise fires once flushed past it.
    struct TlsPendingSend {
        std::vector<std::byte> plain{};
        bool encrypted = false;
        std::size_t cipher_done_at = 0;
        Promise<Error> promise;
    };
    std::vector<TlsPendingSend>& tls_pending() noexcept { return tls_pending_; }
#endif

    void close_fd() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    Endpoint peer_{};
    bool peer_known_ = false;
    bool connected_ = false;
    std::vector<std::byte> out_{};            // pending outbound bytes (framed / ciphertext)
    std::vector<std::byte> in_{};             // inbound bytes awaiting reassembly / ciphertext
    std::size_t out_base_ = 0;                // bytes already drained from out_ front
    std::vector<PendingSend> pending_{};      // per-message send completions
#if defined(__linux__) && defined(LOCKSTEP_TLS)
    std::unique_ptr<ProdTlsSession> tls_{};   // null == plaintext; else owns the SSL session
    std::vector<std::byte> app_in_{};         // DECRYPTED app bytes awaiting reassembly
    std::vector<std::byte> hello_pending_{};  // client HELLO queued until handshake done
    std::vector<TlsPendingSend> tls_pending_{};  // app frames awaiting encrypt+flush
#endif
};

// ---------------------------------------------------------------------------
// ProdNetworkBus — owns the reactor binding + the cluster address map + every
// node. Hands out ProdNetwork per-node handles (core::INetwork). One bus per
// in-process cluster (two nodes on loopback for S4b). NOT part of the INetwork
// surface — it is the prod analogue of SimNetworkBus's cluster-wide setup.
// ---------------------------------------------------------------------------
class ProdNetwork; // fwd

class ProdNetworkBus {
public:
    // Ctor + dtor are out-of-line (defined below, after ProdNetwork is complete):
    // both touch a vector<unique_ptr<ProdNetwork>> whose construction/destruction
    // needs the full ProdNetwork type, which is only declared at this point.
    explicit ProdNetworkBus(ProdReactor& reactor) noexcept;
    ~ProdNetworkBus();

    ProdNetworkBus(const ProdNetworkBus&) = delete;
    ProdNetworkBus& operator=(const ProdNetworkBus&) = delete;
    ProdNetworkBus(ProdNetworkBus&&) = delete;
    ProdNetworkBus& operator=(ProdNetworkBus&&) = delete;

    [[nodiscard]] ProdReactor& reactor() noexcept { return *reactor_; }

#if defined(__linux__) && defined(LOCKSTEP_TLS)
    // ---- TLS transport configuration (provider-layer; no core change) ----
    // Enable TLS on this bus's transport with the given cert/key/CA. `auth` selects mTLS
    // (consensus peers — both sides require + verify the peer cert) vs server-auth TLS
    // (admin/wire clients — server presents its cert, client verifies it, client cert
    // optional). Builds the SERVER + CLIENT SSL_CTX once; every connection on this bus then
    // wraps its socket in a TLS session of the right role. Must be called BEFORE add_node /
    // any connect. Returns false if the contexts fail to build (bad cert/key/CA) — the
    // caller then refuses to run rather than fall back to cleartext.
    bool enable_tls(const TlsConfig& cfg, TlsAuth auth) {
        tls_cfg_ = cfg;
        tls_auth_ = auth;
        if (!cfg.enabled) {
            tls_on_ = false;
            return true;  // plaintext requested — nothing to build.
        }
        if (!tls_server_ctx_.init(TlsRole::Server, auth, cfg)) {
            // A pure CLIENT (the admin client) has no listen socket and needs only the
            // client ctx; a server-side build failure there is non-fatal IF the client
            // ctx builds. But a daemon (which listens) needs the server ctx — surface it.
            // We still try the client ctx so an admin client without a server cert works.
        }
        if (!tls_client_ctx_.init(TlsRole::Client, auth, cfg)) {
            return false;
        }
        tls_on_ = true;
        return true;
    }

    [[nodiscard]] bool tls_on() const noexcept { return tls_on_; }
    [[nodiscard]] const ProdTlsContext& tls_server_ctx() const noexcept {
        return tls_server_ctx_;
    }
    [[nodiscard]] const ProdTlsContext& tls_client_ctx() const noexcept {
        return tls_client_ctx_;
    }
#endif

    // Register a node: open + bind a non-blocking loopback listen socket on an
    // EPHEMERAL port, record its port in the address map, register it on the reactor
    // for accept. Idempotent on id. Returns false on a socket setup failure.
    bool add_node(std::uint64_t id);

    // S5b-2 (multi-PROCESS): register a node LISTENING on a FIXED loopback port. In a
    // single process the bus mints ephemeral ports (peers learn them in-process), but
    // ACROSS processes a peer must know the listen port a priori — so a real cluster
    // binds each node to a fixed, agreed port. `port` must be > 0. Otherwise identical
    // to add_node (idempotent on id; non-blocking listen; armed for accept). Returns
    // false if the bind/listen fails (e.g. the port is already taken — fail fast, do
    // NOT spin trying alternates).
    bool add_node_on_port(std::uint64_t id, std::uint16_t port);

    // S5b-2: record a PEER's known (id -> fixed loopback port) WITHOUT binding a listen
    // socket here. A node's outbound send() to that peer dials 127.0.0.1:port via this
    // address-map entry (the peer LISTENS in its OWN process). Used to teach this
    // process where its peers live. Idempotent / overwrites the recorded port.
    void add_peer(std::uint64_t id, std::uint16_t port) { record_port(id, port); }

    // Per-node INetwork handle. The node must already be add_node()'d.
    [[nodiscard]] ProdNetwork* node(std::uint64_t id);

    // The recorded loopback port of a peer (0 if unknown). Used by connect().
    [[nodiscard]] std::uint16_t port_of(std::uint64_t id) const noexcept {
        for (const NodeAddr& a : addrs_) {
            if (a.id == id) {
                return a.port;
            }
        }
        return 0;
    }

    void record_port(std::uint64_t id, std::uint16_t port) {
        for (NodeAddr& a : addrs_) {
            if (a.id == id) {
                a.port = port;
                return;
            }
        }
        // keep sorted by id
        std::size_t pos = 0;
        while (pos < addrs_.size() && addrs_[pos].id < id) {
            ++pos;
        }
        addrs_.insert(addrs_.begin() + static_cast<std::ptrdiff_t>(pos),
                      NodeAddr{id, port});
    }

private:
    ProdReactor* reactor_;
    std::vector<NodeAddr> addrs_{};                       // id -> loopback port
    std::vector<std::unique_ptr<ProdNetwork>> nodes_{};   // owned per-node handles
#if defined(__linux__) && defined(LOCKSTEP_TLS)
    TlsConfig tls_cfg_{};                                 // cert/key/CA paths
    TlsAuth tls_auth_ = TlsAuth::MutualPeer;              // mTLS vs server-auth
    bool tls_on_ = false;                                 // TLS wrapping active?
    ProdTlsContext tls_server_ctx_{};                     // accept-side SSL_CTX
    ProdTlsContext tls_client_ctx_{};                     // connect-side SSL_CTX
#endif
};

// ---------------------------------------------------------------------------
// ProdNetwork — one node's core::INetwork over real TCP. Owns its listen fd and
// its connections; drives all IO on the bus's reactor via fd handlers. local()
// returns this node's stable Endpoint.
// ---------------------------------------------------------------------------
class ProdNetwork final : public INetwork {
public:
    ProdNetwork(ProdNetworkBus& bus, Endpoint self, int listen_fd) noexcept
        : bus_(&bus), self_(self), listen_fd_(listen_fd) {}

    ProdNetwork(const ProdNetwork&) = delete;
    ProdNetwork& operator=(const ProdNetwork&) = delete;
    ProdNetwork(ProdNetwork&&) = delete;
    ProdNetwork& operator=(ProdNetwork&&) = delete;

    ~ProdNetwork() override {
        // Unregister + close the listen fd and every connection (no fd leak).
        if (listen_fd_ >= 0) {
            bus_->reactor().remove_fd(listen_fd_);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        for (auto& c : conns_) {
            if (c && !c->dead()) {
                bus_->reactor().remove_fd(c->fd());
            }
        }
        conns_.clear();
    }

    [[nodiscard]] Endpoint local() const noexcept override { return self_; }

    // Register this node's listen fd on the reactor (EPOLLIN -> accept). Called by
    // the bus right after construction. Kept separate so the handler can capture a
    // stable `this` (the node is heap-owned by the bus, so `this` is stable).
    void arm_listen() {
        bus_->reactor().add_fd(listen_fd_, EPOLLIN,
                               [this](std::uint32_t /*revents*/) { on_accept(); });
    }

    [[nodiscard]] Future<Error> send(Endpoint to,
                                     std::span<const std::byte> payload) override {
        Promise<Error> p = core::make_promise<Error>(&bus_->reactor());
        Future<Error> f = p.get_future();

        ProdConnection* c = ensure_connection(to);
        if (c == nullptr) {
            p.set_value(Error{ErrorCode::Unavailable, "no route to peer"});
            return f;
        }

#if defined(__linux__) && defined(LOCKSTEP_TLS)
        if (c->has_tls()) {
            // TLS path: stage the framed app bytes; encrypt+flush whatever the handshake
            // state allows (encrypt() is a no-op until the handshake completes — the frame
            // stays staged and is encrypted the moment the handshake lands). The promise
            // fires when this frame's CIPHERTEXT is fully accepted into the socket.
            queue_framed_tls(*c, payload, std::move(p));
            drive_tls(*c);
            arm_io(*c);
            pump_write(*c);
            return f;
        }
#endif
        // PERF (S8.6): frame the payload DIRECTLY into the connection's out_ buffer
        // (length prefix + bytes), no intermediate `framed` vector. The payload is
        // copied into out_ now (the caller need not keep it alive — INetwork contract).
        c->queue_framed(payload, std::move(p));

        // Drive a flush attempt now; arm EPOLLOUT so the reactor finishes partial
        // writes. The promise completes when the message is fully accepted.
        arm_io(*c);
        pump_write(*c);
        return f;
    }

    // Test/control hook: forcibly tear down every live connection (simulating a
    // peer reset / link drop). Pending sends fail with Unavailable; a subsequent
    // send() to a peer re-establishes a fresh connection -> RECONNECT. NOT part of
    // the INetwork surface — the prod analogue of SimNetworkBus's partition control.
    void drop_all_connections() {
        for (auto& c : conns_) {
            if (c && !c->dead()) {
                drop_conn(*c);
            }
        }
        // Erase the dead connection objects (their fds are already closed).
        std::vector<std::unique_ptr<ProdConnection>> live;
        for (auto& c : conns_) {
            if (c && !c->dead()) {
                live.push_back(std::move(c));
            }
        }
        conns_ = std::move(live);
    }

    [[nodiscard]] Future<Message> recv() override {
        Promise<Message> p = core::make_promise<Message>(&bus_->reactor());
        Future<Message> f = p.get_future();
        if (!ready_.empty()) {
            deliver_front(std::move(p));
        } else {
            waiters_.push_back(std::move(p));
        }
        return f;
    }

    // The PRINCIPAL (mTLS client cert subject CN) of the connection that delivered the
    // MOST RECENTLY recv()'d message. core::Message is FROZEN (no principal field — core
    // is unchanged), so the prod admin handler / wire::Server read the identity off THIS
    // side-channel right after recv() returns. SOUND because the single reactor thread
    // drives recv()->handle()->send() strictly one message at a time (the same
    // single-consumer discipline that makes retained_one_ safe): the value is exactly the
    // principal of the just-delivered Message at the point the synchronous handler runs,
    // before the next recv(). Empty string == no peer cert (plaintext or optional-absent
    // client cert) == the anonymous principal.
    [[nodiscard]] const std::string& last_principal() const noexcept {
        return last_principal_;
    }

private:
    // A reassembled inbound message awaiting recv() consumption. `principal` is the peer
    // cert CN of the connection it arrived on (empty for plaintext / no client cert).
    struct InboundMsg {
        Endpoint from{};
        std::vector<std::byte> bytes{};
        std::string principal{};
    };

    // ---- connection management ------------------------------------------

    // Get the reusable outbound connection to `to`, creating + connecting it (non-
    // blocking) on first use. Returns null if the peer's port is unknown / setup
    // fails. A dead connection (peer reset) is replaced -> RECONNECT.
    ProdConnection* ensure_connection(Endpoint to) {
        for (auto& c : conns_) {
            if (c && c->peer_known() && c->peer().id == to.id && !c->dead()) {
                return c.get();
            }
        }
        // (re)connect: open a non-blocking TCP socket and connect to loopback:port.
        const std::uint16_t port = bus_->port_of(to.id);
        if (port == 0) {
            return nullptr;
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return nullptr;
        }
        set_nonblock_nodelay(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        const bool connected_now = (rc == 0);
        if (rc != 0 && errno != EINPROGRESS) {
            ::close(fd);
            return nullptr;
        }
        auto conn =
            std::make_unique<ProdConnection>(fd, to, /*peer_known=*/true,
                                             /*connected=*/connected_now);
        // HELLO: our 8-byte LE id, the connecting side's first bytes, so the far
        // side learns who we are (its accepted conn has no peer id otherwise).
        std::vector<std::byte> hello(kHelloLen, std::byte{0});
        std::uint64_t myid = self_.id;
        for (std::size_t i = 0; i < kHelloLen; ++i) {
            hello[i] = static_cast<std::byte>((myid >> (8 * i)) & 0xFF);
        }
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        if (bus_->tls_on()) {
            // CLIENT-role TLS session: the connecting side runs the TLS client handshake.
            // The HELLO is APP data and must be sent ENCRYPTED after the handshake — stash
            // it; drive_tls() sends it the moment the handshake completes.
            auto sess = std::make_unique<ProdTlsSession>(bus_->tls_client_ctx(),
                                                         TlsRole::Client);
            if (!sess->valid()) {
                ::close(fd);
                return nullptr;
            }
            conn->attach_tls(std::move(sess));
            conn->stash_hello(std::move(hello));
            ProdConnection* raw = conn.get();
            conns_.push_back(std::move(conn));
            register_conn(*raw);
            // Kick the handshake (it may emit ClientHello ciphertext into out_).
            drive_tls(*raw);
            return raw;
        }
#endif
        conn->queue_raw(std::span<const std::byte>(hello.data(), hello.size()));
        ProdConnection* raw = conn.get();
        conns_.push_back(std::move(conn));
        register_conn(*raw);
        return raw;
    }

    // Accept every pending inbound connection (level-triggered, loop until EAGAIN).
    void on_accept() {
        for (;;) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
            if (fd < 0) {
                break; // EAGAIN / EWOULDBLOCK: drained the backlog.
            }
            set_nodelay(fd);
            // Inbound: peer id UNKNOWN until its HELLO arrives. Connected already.
            auto conn = std::make_unique<ProdConnection>(
                fd, Endpoint{}, /*peer_known=*/false, /*connected=*/true);
#if defined(__linux__) && defined(LOCKSTEP_TLS)
            if (bus_->tls_on()) {
                // SERVER-role TLS session: the accepting side runs the TLS server
                // handshake (presents our cert; for mTLS it REQUIRES + verifies the peer
                // cert — a no-cert / wrong-CA peer fails the handshake here = the teeth).
                auto sess = std::make_unique<ProdTlsSession>(bus_->tls_server_ctx(),
                                                             TlsRole::Server);
                if (!sess->valid()) {
                    ::close(fd);
                    continue;
                }
                conn->attach_tls(std::move(sess));
            }
#endif
            ProdConnection* raw = conn.get();
            conns_.push_back(std::move(conn));
            register_conn(*raw);
#if defined(__linux__) && defined(LOCKSTEP_TLS)
            if (raw->has_tls()) {
                drive_tls(*raw);  // begin the server handshake (await ClientHello).
                arm_io(*raw);
            }
#endif
        }
    }

    // Register a connection fd on the reactor with a handler that pumps read+write.
    void register_conn(ProdConnection& c) {
        const int fd = c.fd();
        std::uint32_t ev = EPOLLIN;
        if (c.wants_write()) {
            ev |= EPOLLOUT;
        }
        bus_->reactor().add_fd(fd, ev, [this, fd](std::uint32_t revents) {
            on_conn_ready(fd, revents);
        });
    }

    // Re-arm a connection's interest mask (EPOLLOUT on iff it has pending writes or
    // is still connecting).
    void arm_io(ProdConnection& c) {
        if (c.dead()) {
            return;
        }
        std::uint32_t ev = EPOLLIN;
        if (c.wants_write()) {
            ev |= EPOLLOUT;
        }
        bus_->reactor().mod_fd(c.fd(), ev);
    }

    // The connection fd handler: dispatch read + write readiness.
    void on_conn_ready(int fd, std::uint32_t revents) {
        ProdConnection* c = conn_by_fd(fd);
        if (c == nullptr || c->dead()) {
            return;
        }
        if ((revents & (EPOLLERR | EPOLLHUP)) != 0) {
            // Peer reset / half-open: drop the connection. A later send re-connects.
            drop_conn(*c);
            return;
        }
        if ((revents & EPOLLOUT) != 0) {
            if (!c->connected()) {
                c->mark_connected();
            }
#if defined(__linux__) && defined(LOCKSTEP_TLS)
            if (c->has_tls()) {
                drive_tls(*c);  // advance handshake / encrypt staged frames into out_.
                if (c->dead()) {
                    return;
                }
            }
#endif
            pump_write(*c);
            if (c->dead()) {
                return;
            }
        }
        if ((revents & EPOLLIN) != 0) {
            pump_read(*c);
            if (c->dead()) {
                return;
            }
        }
        arm_io(*c);
    }

    // Flush as much of the write buffer as the socket accepts (handle PARTIAL
    // writes). Complete every send whose bytes are now fully accepted (its promise
    // fires — "accepted for delivery"). Drops the connection on a hard write error.
    void pump_write(ProdConnection& c) {
        if (c.dead() || !c.connected()) {
            return;
        }
        std::vector<std::byte>& out = c.out();
        std::size_t& base = c.out_base();
        while (base < out.size()) {
            const std::size_t remaining = out.size() - base;
            const ssize_t w = ::send(c.fd(), out.data() + base, remaining, MSG_NOSIGNAL);
            if (w > 0) {
                base += static_cast<std::size_t>(w);
                complete_sends(c);
#if defined(__linux__) && defined(LOCKSTEP_TLS)
                if (c.has_tls()) {
                    complete_tls_sends(c);
                }
#endif
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; // socket buffer full: finish later on the next EPOLLOUT.
            }
            // Hard error (EPIPE/ECONNRESET): fail the pending sends, drop the conn.
            drop_conn(c);
            return;
        }
        // Fully drained: compact the buffer so it does not grow unboundedly.
        if (base >= out.size() && !out.empty()) {
            out.clear();
            base = 0;
        }
    }

    // Fire the completion promise of every queued send now fully flushed.
    void complete_sends(ProdConnection& c) {
        std::vector<ProdConnection::PendingSend>& pend = c.pending();
        const std::size_t flushed = c.out_base();
        // pending are in queue order; their done_at is monotonic increasing.
        std::size_t done = 0;
        while (done < pend.size() && pend[done].done_at <= flushed) {
            ++done;
        }
        if (done == 0) {
            return;
        }
        // Move out the completed promises BEFORE erasing (no live ref across the
        // set_value -> schedule -> potential re-entrancy; V-RKV1).
        std::vector<Promise<Error>> firing;
        firing.reserve(done);
        for (std::size_t i = 0; i < done; ++i) {
            firing.push_back(std::move(pend[i].promise));
        }
        pend.erase(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(done));
        for (Promise<Error>& pr : firing) {
            pr.set_value(Error{}); // accepted into the socket
        }
    }

#if defined(__linux__) && defined(LOCKSTEP_TLS)
    // ---- TLS IO helpers (only compiled when LOCKSTEP_TLS) ----------------

    // Stage a framed app message for TLS encryption + flush. Builds the length-prefixed
    // frame (same wire framing as the plaintext path — just carried INSIDE the TLS records)
    // and records its completion promise. drive_tls() encrypts staged frames into out_.
    void queue_framed_tls(ProdConnection& c, std::span<const std::byte> payload,
                          Promise<Error> p) {
        const std::size_t n = payload.size();
        ProdConnection::TlsPendingSend ps;
        ps.plain.reserve(kLenPrefix + n);
        ps.plain.push_back(static_cast<std::byte>(n & 0xFF));
        ps.plain.push_back(static_cast<std::byte>((n >> 8) & 0xFF));
        ps.plain.push_back(static_cast<std::byte>((n >> 16) & 0xFF));
        ps.plain.push_back(static_cast<std::byte>((n >> 24) & 0xFF));
        ps.plain.insert(ps.plain.end(), payload.begin(), payload.end());
        ps.promise = std::move(p);
        c.tls_pending().push_back(std::move(ps));
    }

    // Drive the TLS state machine for `c`: advance the handshake; once done, send the
    // stashed client HELLO + encrypt any staged app frames into out_ (ciphertext); always
    // drain pending handshake/app ciphertext from the write BIO into out_. The reactor
    // then flushes out_ to the socket (pump_write). A handshake / TLS failure DROPS the
    // connection (the auth gate: a wrong-CA / no-cert peer dies here).
    void drive_tls(ProdConnection& c) {
        if (c.dead() || !c.has_tls() || !c.connected()) {
            return;
        }
        ProdTlsSession* s = c.tls();
        if (!s->handshake_done()) {
            const int hs = s->do_handshake();
            // Always drain any handshake ciphertext (ClientHello / ServerHello / ...).
            s->drain_ciphertext(c.out());
            if (hs < 0) {
                drop_conn(c);  // handshake FAILED — cert verify / protocol error.
                return;
            }
            if (hs == 0) {
                return;  // handshake in progress — resume on the next fd-ready event.
            }
            // hs == 1: handshake COMPLETE. Send the stashed client HELLO (encrypted) first.
            if (!c.hello_pending().empty()) {
                std::vector<std::byte> hello;
                hello.swap(c.hello_pending());
                if (!s->encrypt(std::span<const std::byte>(hello.data(), hello.size()))) {
                    drop_conn(c);
                    return;
                }
            }
        }
        // Handshake done: retry any plaintext deferred mid-write, then encrypt staged frames.
        if (!s->flush_pending_plain()) {
            drop_conn(c);
            return;
        }
        encrypt_staged(c);
        s->drain_ciphertext(c.out());
    }

    // Encrypt every not-yet-encrypted staged app frame into the TLS write BIO, then drain
    // the resulting ciphertext into out_ and mark each frame's ciphertext flush offset so
    // its completion promise fires once flushed (complete_tls_sends). Encrypt in queue order
    // so the wire byte order matches the app frame order (per-link order — the TCP/INetwork
    // contract). A fatal encrypt error drops the connection.
    void encrypt_staged(ProdConnection& c) {
        ProdTlsSession* s = c.tls();
        if (!s->handshake_done()) {
            return;  // cannot encrypt before the handshake — frames stay staged.
        }
        for (auto& ps : c.tls_pending()) {
            if (ps.encrypted) {
                continue;
            }
            if (!s->encrypt(std::span<const std::byte>(ps.plain.data(), ps.plain.size()))) {
                drop_conn(c);
                return;
            }
            ps.encrypted = true;
            // Drain the freshly produced ciphertext into out_; this frame is fully flushed
            // once out_base reaches the current cumulative out_ length (out_base + out_.size).
            s->drain_ciphertext(c.out());
            ps.cipher_done_at = c.out_base() + c.out().size();
            ps.plain.clear();
            ps.plain.shrink_to_fit();
        }
    }

    // Fire the completion promise of every staged TLS frame whose ciphertext is now fully
    // flushed to the socket (out_base has passed its cipher_done_at). Same "accepted into
    // the socket" contract as the plaintext complete_sends. Moves promises out before
    // firing (V-RKV1 — no live ref into tls_pending across set_value's re-entrancy).
    void complete_tls_sends(ProdConnection& c) {
        std::vector<ProdConnection::TlsPendingSend>& pend = c.tls_pending();
        const std::size_t flushed = c.out_base();
        std::size_t done = 0;
        while (done < pend.size() && pend[done].encrypted &&
               pend[done].cipher_done_at <= flushed) {
            ++done;
        }
        if (done == 0) {
            return;
        }
        std::vector<Promise<Error>> firing;
        firing.reserve(done);
        for (std::size_t i = 0; i < done; ++i) {
            firing.push_back(std::move(pend[i].promise));
        }
        pend.erase(pend.begin(), pend.begin() + static_cast<std::ptrdiff_t>(done));
        for (Promise<Error>& pr : firing) {
            pr.set_value(Error{});  // ciphertext accepted into the socket.
        }
    }

    // Feed inbound ciphertext (just read into in_) to the TLS read BIO, advance the
    // handshake / decrypt app bytes into app_in_, frame them, and flush any outbound
    // ciphertext the read step produced (handshake replies, etc.). Drops the connection on
    // a TLS failure (auth gate) or an orderly close_notify.
    void tls_ingest(ProdConnection& c) {
        ProdTlsSession* s = c.tls();
        if (!c.in().empty()) {
            const bool ok =
                s->feed_ciphertext(std::span<const std::byte>(c.in().data(), c.in().size()));
            c.in().clear();
            if (!ok) {
                drop_conn(c);
                return;
            }
        }
        if (!s->handshake_done()) {
            // Advance the handshake with the bytes just fed; drive_tls drains its ciphertext
            // and, on completion, sends the stashed HELLO + encrypts staged frames.
            drive_tls(c);
            if (c.dead() || !s->handshake_done()) {
                return;  // still handshaking (or dropped) — no app bytes yet.
            }
        }
        // Handshake complete: decrypt available app plaintext into app_in_, then frame it.
        if (!s->decrypt(c.app_in())) {
            drop_conn(c);  // fatal TLS error or peer close_notify.
            return;
        }
        extract_frames(c);
        // A decrypt step (renegotiation) can produce outbound ciphertext — flush it.
        s->drain_ciphertext(c.out());
        if (!c.out().empty() || !c.tls_pending().empty()) {
            arm_io(c);
            pump_write(c);
        }
    }
#endif  // __linux__ && LOCKSTEP_TLS

    // Read available bytes, learn the peer from the HELLO, re-assemble complete
    // frames, and deliver/queue each. Handles frames split/coalesced across reads.
    void pump_read(ProdConnection& c) {
        std::vector<std::byte>& in = c.in();
        for (;;) {
            std::byte tmp[4096];
            const ssize_t r = ::recv(c.fd(), tmp, sizeof(tmp), 0);
            if (r > 0) {
                // PERF (S8.6): bulk-append the chunk (one insert) instead of a per-byte
                // push_back loop — same bytes, far less overhead under a read burst.
                in.insert(in.end(), tmp, tmp + r);
                continue;
            }
            if (r == 0) {
                // Orderly peer close (FIN). Process any buffered frames, then drop.
                extract_frames(c);
                drop_conn(c);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // drained for now
            }
            drop_conn(c); // hard read error
            return;
        }
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        if (c.has_tls()) {
            // in_ now holds inbound CIPHERTEXT. Feed it to the TLS read BIO, advance the
            // handshake / decrypt app bytes into app_in_, then frame from app_in_.
            tls_ingest(c);
            return;  // tls_ingest drives extract_frames + flush itself.
        }
#endif
        extract_frames(c);
    }

    // Pull the HELLO (once) + every complete length-framed message out of the
    // connection's read buffer; deliver each reassembled payload. The buffer is the raw
    // socket buffer in_ for plaintext, or the DECRYPTED app buffer app_in_ for TLS (the
    // socket in_ then holds ciphertext, consumed by the TLS layer before this runs).
    void extract_frames(ProdConnection& c) {
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        std::vector<std::byte>& in = c.has_tls() ? c.app_in() : c.in();
#else
        std::vector<std::byte>& in = c.in();
#endif
        std::size_t pos = 0;
        // HELLO: the connecting side's 8-byte LE id, sent first. Learn it once.
        if (!c.peer_known()) {
            if (in.size() - pos < kHelloLen) {
                drain_front(in, pos);
                return;
            }
            std::uint64_t pid = 0;
            for (std::size_t i = 0; i < kHelloLen; ++i) {
                pid |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(in[pos + i]))
                       << (8 * i);
            }
            pos += kHelloLen;
            c.learn_peer(Endpoint{pid});
        }
        for (;;) {
            if (in.size() - pos < kLenPrefix) {
                break; // incomplete length prefix
            }
            const std::size_t len =
                static_cast<std::size_t>(std::to_integer<unsigned char>(in[pos])) |
                (static_cast<std::size_t>(std::to_integer<unsigned char>(in[pos + 1])) << 8) |
                (static_cast<std::size_t>(std::to_integer<unsigned char>(in[pos + 2])) << 16) |
                (static_cast<std::size_t>(std::to_integer<unsigned char>(in[pos + 3])) << 24);
            if (in.size() - pos - kLenPrefix < len) {
                break; // full payload not yet arrived
            }
            const std::size_t start = pos + kLenPrefix;
            std::vector<std::byte> payload(in.begin() + static_cast<std::ptrdiff_t>(start),
                                           in.begin() + static_cast<std::ptrdiff_t>(start + len));
            pos = start + len;
            enqueue_inbound(InboundMsg{c.peer(), std::move(payload), principal_of(c)});
        }
        drain_front(in, pos);
    }

    // The PRINCIPAL carried by a connection: its TLS session's verified peer-cert CN.
    // Empty for a plaintext connection (no TLS session) or when no client cert was
    // presented. Provider-layer (OpenSSL) extraction stays inside ProdTls; here we just
    // forward the resulting string.
    [[nodiscard]] static std::string principal_of(ProdConnection& c) {
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        if (c.has_tls() && c.tls() != nullptr) {
            return c.tls()->peer_cn();
        }
#else
        (void)c;
#endif
        return {};
    }

    static void drain_front(std::vector<std::byte>& in, std::size_t pos) {
        if (pos == 0) {
            return;
        }
        in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    // Deliver to a parked recv() waiter (FIFO) or queue for a later recv().
    void enqueue_inbound(InboundMsg m) {
        if (!waiters_.empty()) {
            Promise<Message> p = std::move(waiters_.front());
            waiters_.erase(waiters_.begin());
            deliver(std::move(p), std::move(m));
        } else {
            ready_.push_back(std::move(m));
        }
    }

    // Complete a recv promise from the front of the ready queue (FIFO).
    void deliver_front(Promise<Message> p) {
        InboundMsg m = std::move(ready_.front());
        ready_.erase(ready_.begin());
        deliver(std::move(p), std::move(m));
    }

    // Hand the message to a waiter. The payload bytes are copied into a per-node
    // RETAINED buffer so the non-owning span stays valid for the recv callback even
    // as connection read buffers churn (V-RKV1 — the INetwork "valid only during the
    // callback" contract, honored with a stable backing store).
    void deliver(Promise<Message> p, InboundMsg m) {
        // PERF (S8.6): a SINGLE reusable retained buffer instead of an unbounded
        // vector-of-vectors that grew by one entry per delivered message and was NEVER
        // freed (a steady leak + repeated reallocation of the outer vector). Only ONE
        // delivered span is live at a time: deliver() schedules the waiter (L1, NOT
        // inline), so the single consumer coroutine (recv_loop / admin_serve) does not
        // run until control returns to the reactor; it then consumes msg.payload
        // SYNCHRONOUSLY (decode / handle_admin — no co_await holding the span) before
        // calling recv() again, which is the only path back into deliver(). A parked
        // waiter is delivered to at most once (waiters_ then empty; later inbound queues
        // in ready_). So overwriting retained_one_ on the next deliver cannot clobber a
        // span still in use (V-RKV1 preserved — the backing store outlives every live
        // span; moving the new bytes in only AFTER the previous consumer has drained).
        retained_one_ = std::move(m.bytes);
        // Stamp the principal of THIS delivered message on the side-channel before the
        // waiter runs (the synchronous handler reads last_principal() right after recv()).
        last_principal_ = std::move(m.principal);
        std::span<const std::byte> view(retained_one_.data(), retained_one_.size());
        p.set_value(Message{m.from, view});
    }

    // ---- teardown ------------------------------------------------------

    // Drop a connection: fail its pending sends (Unavailable), unregister + close
    // its fd. A subsequent send() to the same peer re-creates it -> RECONNECT.
    void drop_conn(ProdConnection& c) {
        if (c.dead()) {
            return;
        }
        std::vector<Promise<Error>> firing;
        for (auto& ps : c.pending()) {
            firing.push_back(std::move(ps.promise));
        }
        c.pending().clear();
#if defined(__linux__) && defined(LOCKSTEP_TLS)
        for (auto& ps : c.tls_pending()) {
            firing.push_back(std::move(ps.promise));
        }
        c.tls_pending().clear();
#endif
        bus_->reactor().remove_fd(c.fd());
        c.close_fd();
        for (Promise<Error>& pr : firing) {
            pr.set_value(Error{ErrorCode::Unavailable, "connection reset"});
        }
    }

    [[nodiscard]] ProdConnection* conn_by_fd(int fd) noexcept {
        for (auto& c : conns_) {
            if (c && c->fd() == fd) {
                return c.get();
            }
        }
        return nullptr;
    }

    // ---- non-blocking socket helpers ------------------------------------

    static void set_nonblock_nodelay(int fd) noexcept {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl >= 0) {
            ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        set_nodelay(fd);
    }
    static void set_nodelay(int fd) noexcept {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    ProdNetworkBus* bus_;
    Endpoint self_;
    int listen_fd_ = -1;
    std::vector<std::unique_ptr<ProdConnection>> conns_{}; // inbound + outbound
    std::vector<InboundMsg> ready_{};                      // reassembled, awaiting recv
    std::vector<Promise<Message>> waiters_{};              // parked recv promises (FIFO)
    std::vector<std::byte> retained_one_{};                // stable backing for the one live delivered span
    std::string last_principal_{};                         // principal (peer CN) of the last delivered Message
};

// ---- ProdNetworkBus out-of-line members (need the full ProdNetwork type) ----

inline ProdNetworkBus::ProdNetworkBus(ProdReactor& reactor) noexcept
    : reactor_(&reactor) {}
inline ProdNetworkBus::~ProdNetworkBus() = default;

// Shared bind+listen helper for add_node / add_node_on_port. `want_port`==0 binds an
// EPHEMERAL port (single-process); a non-zero `want_port` binds that FIXED port (the
// cross-process cluster, where peers must know the listen port a priori). Records the
// actual bound port in the address map + arms the node for accept. Idempotent on id.
inline bool ProdNetworkBus::add_node(std::uint64_t id) {
    return add_node_on_port(id, 0);
}

inline bool ProdNetworkBus::add_node_on_port(std::uint64_t id, std::uint16_t want_port) {
    for (const auto& n : nodes_) {
        if (n && n->local().id == id) {
            return true; // idempotent
        }
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // non-blocking listen socket
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(want_port); // 0 == ephemeral; >0 == fixed (cross-process)
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); // fixed port already taken: fail fast (the caller does NOT spin)
        return false;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return false;
    }
    // Read back the actual bound port (== want_port when fixed, else the ephemeral one).
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        ::close(fd);
        return false;
    }
    const std::uint16_t port = ::ntohs(bound.sin_port);
    record_port(id, port);
    auto node = std::make_unique<ProdNetwork>(*this, Endpoint{id}, fd);
    node->arm_listen();
    nodes_.push_back(std::move(node));
    return true;
}

inline ProdNetwork* ProdNetworkBus::node(std::uint64_t id) {
    for (auto& n : nodes_) {
        if (n && n->local().id == id) {
            return n.get();
        }
    }
    return nullptr;
}

} // namespace lockstep::prod

#endif // __linux__
