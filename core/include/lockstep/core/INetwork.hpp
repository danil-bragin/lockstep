#pragma once

// INetwork.hpp — async message-bus surface. No real sockets: this quarantines
// all network nondeterminism behind the boundary. The Phase 2 sim provider
// (spec C2.1) implements it as an in-memory bus with per-link latency,
// reordering, duplication, drop, and partition — all derived from IRandom +
// IClock. The prod provider (Phase 7) implements it over real sockets. This
// header must never name a socket type.

#include <cstddef>
#include <cstdint>
#include <span>

#include <lockstep/core/Error.hpp>

namespace lockstep::core {

// Opaque, value-type address of a peer on the bus. Just an identifier; it owns
// nothing and carries no socket. The provider maps it to a concrete peer. Kept
// as a thin struct (not a bare integer) so it is strongly typed at call sites.
struct Endpoint {
    std::uint64_t id = 0;

    friend constexpr bool operator==(Endpoint, Endpoint) noexcept = default;
};

// A received message: the sender plus a non-owning view of the bytes. The view
// is valid only for the duration of the receive callback / awaited result; the
// provider owns the backing buffer. Copy out anything you need to retain.
struct Message {
    Endpoint from{};
    std::span<const std::byte> payload{};
};

// Forward declaration only; concrete awaitable defined in Phase 1. Referencing
// it by name keeps this header free of coroutine machinery (see IClock.hpp).
template <class T> class Future;

// Pure-virtual message bus. No connections, no buffers, no ownership here.
class INetwork {
public:
    virtual ~INetwork() = default;

    // This node's own address on the bus, used as the `from` of messages it
    // sends. Stable for the lifetime of the network handle.
    [[nodiscard]] virtual Endpoint local() const noexcept = 0;

    // Asynchronously sends `payload` to `to`. The returned future completes with
    // an ok Error once the message is accepted for delivery (NOT once it is
    // received — the bus may delay/reorder/drop it per its fault model), or with
    // a failure Error (e.g. Unavailable on a partitioned link). The bytes are
    // consumed during the call; the caller need not keep `payload` alive past it.
    [[nodiscard]] virtual Future<Error> send(Endpoint to, std::span<const std::byte> payload) = 0;

    // Asynchronously receives the next message addressed to this node. Completes
    // with the next Message when one arrives, in the (possibly reordered)
    // delivery order the bus chose. Delivery order across calls is determined by
    // the provider's deterministic model — never by OS nondeterminism.
    [[nodiscard]] virtual Future<Message> recv() = 0;
};

} // namespace lockstep::core
