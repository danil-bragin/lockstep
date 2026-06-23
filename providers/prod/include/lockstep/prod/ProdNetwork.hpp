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
        return !out_.empty() || !connected_;
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

    // Queue framed bytes (caller already prepended the length prefix). Records a
    // completion promise to fire when this message is fully written.
    void queue_send(const std::vector<std::byte>& framed, Promise<Error> p) {
        const std::size_t done_at = out_base_ + out_.size() + framed.size();
        for (std::byte b : framed) {
            out_.push_back(b);
        }
        pending_.push_back(PendingSend{done_at, std::move(p)});
    }

    // Queue raw bytes with NO completion promise (the HELLO handshake).
    void queue_raw(std::span<const std::byte> bytes) {
        for (std::byte b : bytes) {
            out_.push_back(b);
        }
    }

    // Mutable access for the node's IO pump (kept inside the same TU).
    std::vector<std::byte>& out() noexcept { return out_; }
    std::vector<std::byte>& in() noexcept { return in_; }
    std::size_t& out_base() noexcept { return out_base_; }
    std::vector<PendingSend>& pending() noexcept { return pending_; }

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
    std::vector<std::byte> out_{};            // pending outbound bytes (framed)
    std::vector<std::byte> in_{};             // inbound bytes awaiting reassembly
    std::size_t out_base_ = 0;                // bytes already drained from out_ front
    std::vector<PendingSend> pending_{};      // per-message send completions
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

    // Register a node: open + bind a non-blocking loopback listen socket on an
    // EPHEMERAL port, record its port in the address map, register it on the reactor
    // for accept. Idempotent on id. Returns false on a socket setup failure.
    bool add_node(std::uint64_t id);

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

        // Frame: 4-byte LE length prefix + payload. Copy the payload now (the caller
        // need not keep it alive past the call — INetwork contract).
        const std::size_t n = payload.size();
        std::vector<std::byte> framed;
        framed.reserve(kLenPrefix + n);
        framed.push_back(static_cast<std::byte>(n & 0xFF));
        framed.push_back(static_cast<std::byte>((n >> 8) & 0xFF));
        framed.push_back(static_cast<std::byte>((n >> 16) & 0xFF));
        framed.push_back(static_cast<std::byte>((n >> 24) & 0xFF));
        for (std::byte b : payload) {
            framed.push_back(b);
        }
        c->queue_send(framed, std::move(p));

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

private:
    // A reassembled inbound message awaiting recv() consumption.
    struct InboundMsg {
        Endpoint from{};
        std::vector<std::byte> bytes{};
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
            ProdConnection* raw = conn.get();
            conns_.push_back(std::move(conn));
            register_conn(*raw);
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

    // Read available bytes, learn the peer from the HELLO, re-assemble complete
    // frames, and deliver/queue each. Handles frames split/coalesced across reads.
    void pump_read(ProdConnection& c) {
        std::vector<std::byte>& in = c.in();
        for (;;) {
            std::byte tmp[4096];
            const ssize_t r = ::recv(c.fd(), tmp, sizeof(tmp), 0);
            if (r > 0) {
                for (ssize_t i = 0; i < r; ++i) {
                    in.push_back(tmp[i]);
                }
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
        extract_frames(c);
    }

    // Pull the HELLO (once) + every complete length-framed message out of the
    // connection's read buffer; deliver each reassembled payload.
    void extract_frames(ProdConnection& c) {
        std::vector<std::byte>& in = c.in();
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
            enqueue_inbound(InboundMsg{c.peer(), std::move(payload)});
        }
        drain_front(in, pos);
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
        retained_.push_back(std::move(m.bytes));
        std::span<const std::byte> view(retained_.back().data(), retained_.back().size());
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
    std::vector<std::vector<std::byte>> retained_{};       // stable spans for delivered msgs
};

// ---- ProdNetworkBus out-of-line members (need the full ProdNetwork type) ----

inline ProdNetworkBus::ProdNetworkBus(ProdReactor& reactor) noexcept
    : reactor_(&reactor) {}
inline ProdNetworkBus::~ProdNetworkBus() = default;

inline bool ProdNetworkBus::add_node(std::uint64_t id) {
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
    addr.sin_port = 0; // ephemeral
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return false;
    }
    // Read back the ephemeral port we were assigned.
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
