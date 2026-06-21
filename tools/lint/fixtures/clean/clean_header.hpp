// Clean header fixture: allowed includes + pure interface shape. Must pass.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace lockstep::demo {

// Pure-virtual-ish surface; no nondeterministic includes, no syscalls.
// Methods deliberately named like socket/file syscalls: these are abstraction
// boundary DECLARATIONS (mirroring INetwork's send/recv) and must NOT be
// flagged — the lint distinguishes a declaration from a raw syscall CALL.
struct IThing {
    virtual ~IThing() = default;
    virtual std::size_t size() const = 0;
    virtual std::string_view name() const = 0;
    virtual void send(std::span<const std::byte> bytes) = 0;
    virtual int recv() = 0;
    virtual bool connect(int endpoint) = 0;
    virtual void close() = 0;
};

}  // namespace lockstep::demo
