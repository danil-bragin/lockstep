#pragma once

// Error.hpp — the shared error type used by every abstraction-boundary
// interface for error completion. Deliberately tiny and stable: it is part of
// the frozen boundary, so widening it later risks Phase 4/5 ABI churn. Add new
// codes by appending to the enum (never renumber existing ones).

#include <cstdint>
#include <string_view>

namespace lockstep::core {

// Stable, append-only set of failure categories an interface call may report.
// Generic on purpose — providers map their concrete failures onto these.
enum class ErrorCode : std::uint16_t {
    Ok = 0,        // no error (a default-constructed Error is Ok)
    Cancelled,     // the operation was cancelled before completing
    Timeout,       // the operation did not complete within its deadline
    NotFound,      // a referenced entity (endpoint, handle, offset) does not exist
    Unavailable,   // the resource is currently unreachable (e.g. partitioned link)
    IoFault,       // a disk/network IO fault occurred (includes injected faults)
    Corruption,    // data failed an integrity check (e.g. bit-rot, torn read)
    InvalidArgument, // a caller-supplied argument was malformed or out of range
    Unknown,       // an error that does not map to any category above
};

// Value type carrying a result code plus an optional non-owning, static-lifetime
// detail string for diagnostics. No allocation, no ownership: trivially copyable
// so it can cross the boundary by value without churn. The pointed-to string,
// when present, must outlive the Error (typically a string literal).
struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string_view detail{};

    // True when this Error represents success. The natural "did it work?" check.
    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::Ok; }

    // True when this Error represents a failure. Convenience inverse of ok().
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return !ok(); }
};

} // namespace lockstep::core
