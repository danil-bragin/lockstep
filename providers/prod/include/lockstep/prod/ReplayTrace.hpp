#pragma once

// ReplayTrace.hpp — Phase 7 S2. The record-replay TRACE format + the first two
// recording/replaying providers (Random, Clock). This is the SCAFFOLDING for
// V-PROD-REPLAY: "a prod node records all boundary IO to a trace; any prod
// incident replays byte-identical in sim."
//
// ----------------------------------------------------------------------------
// TRACE FORMAT (stable, append-only, extensible — Disk S3 / Network S4 extend it)
// ----------------------------------------------------------------------------
// A trace is an ordered list of RECORDS. Each record is TAGGED with a provider
// KIND and an OP so adding a new provider kind never changes the meaning of the
// existing ones (the §B WAL-torn lesson: tag everything, never positional-only).
// One record per line, space-separated decimal/text tokens, ASCII, no floats, no
// pointers, no locale — so it diffs cleanly and is byte-identical across stdlibs
// (same discipline as core/Trace.hpp, kept SEPARATE because that trace records
// the SCHEDULER's internal events whereas THIS trace records boundary IO
// observations for offline replay — different consumers, different lifetime):
//
//     <kind> <op> <args...>
//
//   kind = a stable lowercase token: "random" | "clock"   (S3 adds "disk",
//          S4 adds "network"; never renumber/rename an existing one).
//   op   = a stable lowercase token scoped to the kind.
//   args = op-specific decimal tokens.
//
// Records currently emitted:
//   random seed <u64>            — the PRNG seed (the whole stream replays from it)
//   clock now <i64-tick>         — one observed now() return value, in arm order
//
// A REPLAY provider consumes the records of its kind IN ORDER and reproduces the
// IDENTICAL observations. Because ProdRandom reuses sim's exact splitmix64, the
// Random replay only needs the seed; the Clock replay needs each now() sample
// (real time isn't reproducible, so we replay the recorded samples verbatim).
//
// EXTENSIBILITY CONTRACT (so S3/S4 bolt on without breaking S2):
//   * append new (kind, op) rows; never repurpose an existing token.
//   * a replay consumer filters by its own kind and ignores unknown kinds, so a
//     mixed Clock+Random+Disk+Network trace replays each provider independently.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IRandom.hpp>

namespace lockstep::prod {

// Stable provider-kind tags. Append-only. S3 -> Disk, S4 -> Network.
enum class TraceKind : std::uint8_t {
    Random,
    Clock,
    Disk,    // reserved for S3 (no records emitted yet)
    Network, // reserved for S4 (no records emitted yet)
};

[[nodiscard]] constexpr const char* to_token(TraceKind k) noexcept {
    switch (k) {
    case TraceKind::Random:  return "random";
    case TraceKind::Clock:   return "clock";
    case TraceKind::Disk:    return "disk";
    case TraceKind::Network: return "network";
    }
    return "?";
}

// One recorded boundary observation. `op` and `args` are interpreted per `kind`.
// Kept stringly + decimal-only so render() is byte-stable across platforms.
struct TraceRecord {
    TraceKind kind{};
    std::string op{};
    std::vector<std::int64_t> args{}; // decimal args; u64 seeds stored bit-cast

    [[nodiscard]] std::string render() const {
        std::string out = to_token(kind);
        out += ' ';
        out += op;
        for (std::int64_t a : args) {
            out += ' ';
            out += std::to_string(a);
        }
        return out;
    }
};

// The trace: an ordered, append-only buffer of records. render() is the canonical
// byte stream the byte-identical replay proof diffs.
class ReplayTrace {
public:
    void append(TraceRecord r) { records_.push_back(std::move(r)); }

    [[nodiscard]] const std::vector<TraceRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    [[nodiscard]] std::string render() const {
        std::string out;
        out.reserve(records_.size() * 24);
        for (const TraceRecord& r : records_) {
            out += r.render();
            out += '\n';
        }
        return out;
    }

private:
    std::vector<TraceRecord> records_{};
};

// ---------------------------------------------------------------------------
// RECORD side — thin wrappers over a prod provider that log boundary outputs into
// a shared ReplayTrace. They delegate to the wrapped provider for the real value;
// the wrapper only observes. (Random records the seed ONCE — the stream is a pure
// function of it; Clock records EACH now() because real time isn't reproducible.)
// ---------------------------------------------------------------------------

// Wraps any IRandom (a ProdRandom in prod) and records its seed once, up front.
// The seed alone reproduces the entire stream on the replay side (sim==prod algo).
class RecordingRandom final : public core::IRandom {
public:
    RecordingRandom(core::IRandom& inner, std::uint64_t seed, ReplayTrace& trace)
        : inner_(&inner) {
        trace.append(TraceRecord{TraceKind::Random, "seed",
                                 {static_cast<std::int64_t>(seed)}});
    }

    [[nodiscard]] std::uint64_t next() noexcept override { return inner_->next(); }
    [[nodiscard]] std::uint64_t uniform(std::uint64_t b) noexcept override {
        return inner_->uniform(b);
    }
    [[nodiscard]] std::int64_t uniform_range(std::int64_t lo, std::int64_t hi) noexcept override {
        return inner_->uniform_range(lo, hi);
    }
    [[nodiscard]] bool chance(double p) noexcept override { return inner_->chance(p); }

private:
    core::IRandom* inner_;
};

// Wraps any IClock and records each now() observation (in call order). delay() is
// forwarded untouched (deferred to S4 on prod).
class RecordingClock final : public core::IClock {
public:
    RecordingClock(core::IClock& inner, ReplayTrace& trace) noexcept
        : inner_(&inner), trace_(&trace) {}

    [[nodiscard]] core::Tick now() const noexcept override {
        core::Tick t = inner_->now();
        trace_->append(TraceRecord{TraceKind::Clock, "now", {t}});
        return t;
    }
    [[nodiscard]] core::Future<void> delay(core::Duration d) override {
        return inner_->delay(d);
    }

private:
    core::IClock* inner_;
    ReplayTrace* trace_;
};

// ---------------------------------------------------------------------------
// REPLAY side (sim-usable — pure, no syscalls). Consumes a recorded trace and
// reproduces the IDENTICAL observations offline. These satisfy the same IRandom /
// IClock interfaces, so the rest of the system is unaware it is replaying.
// ---------------------------------------------------------------------------

// Replays a Random stream: reads the recorded seed and runs the SAME splitmix64
// engine (via the supplied factory) — so every draw matches the prod run bit for
// bit. Templated on a factory `unique_ptr<IRandom> make(uint64_t)` so it works
// with sim::SeededRandom OR prod::ProdRandom (both share the engine).
template <class RandomFactory>
class ReplayRandom final : public core::IRandom {
public:
    ReplayRandom(const ReplayTrace& trace, RandomFactory& factory) {
        std::uint64_t seed = 0;
        for (const TraceRecord& r : trace.records()) {
            if (r.kind == TraceKind::Random && r.op == "seed" && !r.args.empty()) {
                seed = static_cast<std::uint64_t>(r.args[0]);
                break;
            }
        }
        inner_ = factory.make(seed);
    }

    [[nodiscard]] std::uint64_t next() noexcept override { return inner_->next(); }
    [[nodiscard]] std::uint64_t uniform(std::uint64_t b) noexcept override {
        return inner_->uniform(b);
    }
    [[nodiscard]] std::int64_t uniform_range(std::int64_t lo, std::int64_t hi) noexcept override {
        return inner_->uniform_range(lo, hi);
    }
    [[nodiscard]] bool chance(double p) noexcept override { return inner_->chance(p); }

private:
    std::unique_ptr<core::IRandom> inner_{};
};

// Replays a Clock: hands back the recorded now() samples in order. Real time is
// not reproducible, so we replay the captured values verbatim (this is exactly
// what makes a prod incident reproducible offline). delay() is replay-inert
// (mirrors the prod S2 deferral) — S4 will extend the trace + this replay.
class ReplayClock final : public core::IClock {
public:
    explicit ReplayClock(const ReplayTrace& trace) {
        for (const TraceRecord& r : trace.records()) {
            if (r.kind == TraceKind::Clock && r.op == "now" && !r.args.empty()) {
                samples_.push_back(static_cast<core::Tick>(r.args[0]));
            }
        }
    }

    [[nodiscard]] core::Tick now() const noexcept override {
        if (cursor_ < samples_.size()) {
            return samples_[cursor_++];
        }
        // Trace exhausted: hold the last observed value (monotonic, never invents).
        return samples_.empty() ? core::Tick{0} : samples_.back();
    }
    [[nodiscard]] core::Future<void> delay(core::Duration /*d*/) override {
        core::Promise<void> p;
        core::Future<void> f = p.get_future();
        p.set_error(core::Error{core::ErrorCode::Unavailable,
                                "ReplayClock::delay deferred to S4"});
        return f;
    }

private:
    std::vector<core::Tick> samples_{};
    mutable std::size_t cursor_ = 0;
};

} // namespace lockstep::prod
