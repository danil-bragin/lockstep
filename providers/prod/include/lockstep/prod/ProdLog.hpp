#pragma once

// ProdLog.hpp — Phase 10 OBSERVABILITY. A tiny PROD-LAYER STRUCTURED LOGGER for the
// lockstepd lifecycle events. Replaces ad-hoc std::printf prose with ONE parseable
// event per line in `key=value` form, prefixed with a stable `LSEVENT` tag + a
// monotonic event timestamp + the event name, e.g.
//
//   LSEVENT ts_ms=12345 ev=startup node=1 shard=0 listen_port=19101 admin_port=19201
//   LSEVENT ts_ms=12380 ev=became_leader node=1 shard=0 term=2
//   LSEVENT ts_ms=20000 ev=shutdown node=1 shard=0 role=Leader term=2 commit_index=4
//
// DESIGN:
//   * One event per line, flushed, so a tail/grep over the daemon's stdout yields a
//     structured event stream a log shipper can parse without regex gymnastics.
//   * BOUNDED by construction: the daemon emits a fixed, small set of LIFECYCLE events
//     (startup / became_leader / stepped_down / commit_milestone / shutdown / crash /
//     recover). There is NO per-op logging by default; an optional verbosity flag
//     (LOCKSTEP_LOG_VERBOSE / a ctor bool) enables debug-level events but the default
//     stays quiet so the daemon never spams.
//   * Values are stamped as-is (the daemon controls them; they are ids/terms/ports —
//     no untrusted strings, no embedded spaces). A key=value pair never contains a
//     space in practice; the encoder does not quote (kept minimal + auditable).
//
// providers/prod is the lint-exempt boundary; this header touches NO syscall beyond
// std::fprintf/fflush to a FILE* the caller supplies (default stderr-free: stdout), the
// same surface lockstepd already used for its prose lines. Pure C++, compiles everywhere.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace lockstep::prod {

// One key=value field. Value is rendered from an integer or a short string token.
struct LogField {
    std::string key;
    std::string value;

    LogField(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}
    LogField(std::string k, std::uint64_t v)
        : key(std::move(k)), value(std::to_string(v)) {}
    LogField(std::string k, const char* v) : key(std::move(k)), value(v) {}
};

// The structured logger. Holds a stable FILE* (default stdout) + a `verbose` flag that
// gates debug-level events. The CALLER supplies the ts_ms timestamp per event (read from
// the reactor's now()/1e6 — the prod clock — so the daemon's lint-scanned TU never makes
// a raw wall-clock call directly; the time comes through the provider surface).
class ProdLog {
public:
    explicit ProdLog(bool verbose = false, std::FILE* out = nullptr)
        : out_(out == nullptr ? stdout : out), verbose_(verbose) {}

    [[nodiscard]] bool verbose() const noexcept { return verbose_; }

    // Emit ONE structured lifecycle line: LSEVENT ts_ms=<t> ev=<name> <fields...>.
    void event(std::uint64_t ts_ms, const char* name,
               const std::vector<LogField>& fields) const {
        std::string line = "LSEVENT ts_ms=";
        line += std::to_string(ts_ms);
        line += " ev=";
        line += name;
        for (const LogField& f : fields) {
            line += ' ';
            line += f.key;
            line += '=';
            line += f.value;
        }
        line += '\n';
        std::fputs(line.c_str(), out_);
        std::fflush(out_);
    }

    // A DEBUG-level event: emitted ONLY when verbose is on (the per-op gate). Default-off
    // so the shipping daemon never spams a line per operation.
    void debug(std::uint64_t ts_ms, const char* name,
               const std::vector<LogField>& fields) const {
        if (verbose_) {
            event(ts_ms, name, fields);
        }
    }

private:
    std::FILE* out_ = nullptr;
    bool verbose_ = false;
};

}  // namespace lockstep::prod
