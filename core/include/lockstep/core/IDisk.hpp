#pragma once

// IDisk.hpp — async durable-storage surface. No raw file IO: this quarantines
// all disk nondeterminism and durability semantics behind the boundary. The
// Phase 2 sim provider (spec C2.2) models latency, IO faults, TORN WRITES
// (partial page), and LYING FSYNC (ack-before-durable, then lose on crash); the
// prod provider (Phase 7) implements it over io_uring/NVMe. Critically, sync()
// is an EXPLICIT, separate operation from append/write so the durability barrier
// can be modelled (data written but not yet synced may be lost on a crash).

#include <cstddef>
#include <cstdint>
#include <span>

#include <lockstep/core/Error.hpp>

namespace lockstep::core {

// Byte offset within a disk handle's address space. Unsigned; addresses bytes,
// not pages — paging/torn-write granularity is the provider's concern.
using Offset = std::uint64_t;

// Forward declaration only; concrete awaitable defined in Phase 1. See IClock.hpp.
template <class T> class Future;

// Pure-virtual block/log device. No file descriptor, no buffer, no ownership.
// A single IDisk is one append-structured object (e.g. a WAL segment or SSTable
// backing); higher layers compose many.
class IDisk {
public:
    virtual ~IDisk() = default;

    // Appends `data` to the end of the device. Completes with an ok Error and
    // the offset at which the data was placed (via `out_offset`, written before
    // completion), or with a failure Error. Appended data is NOT durable until a
    // subsequent successful sync(); a crash before that sync may lose it, and the
    // sim may model a torn (partial) append. Returns Future<Error> for the
    // completion status; the offset is reported through the out-parameter, which
    // must outlive the awaited completion.
    [[nodiscard]] virtual Future<Error> append(std::span<const std::byte> data, Offset& out_offset) = 0;

    // Reads up to `into.size()` bytes starting at `at` into `into`. Completes
    // with an ok Error once `into` is filled (short reads past end-of-device
    // report NotFound / the provider's documented behavior), or a failure Error
    // (e.g. Corruption on bit-rot, IoFault on an injected fault). `into` must
    // outlive the awaited completion.
    [[nodiscard]] virtual Future<Error> read(Offset at, std::span<std::byte> into) = 0;

    // Explicit durability barrier. Completes with an ok Error only once every
    // append acknowledged before this call is durable (survives a crash), or a
    // failure Error. This is the ONLY operation that establishes durability —
    // the sim's "lying fsync" fault deliberately violates it to test recovery.
    [[nodiscard]] virtual Future<Error> sync() = 0;
};

} // namespace lockstep::core
