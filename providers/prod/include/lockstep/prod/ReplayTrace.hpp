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
//   disk read <at> <len> <errc> <hexbytes>
//                                — one observed READ: offset, requested length,
//                                  the returned ErrorCode (decimal), and the bytes
//                                  actually delivered, lower-hex, no separators
//                                  (empty on a non-ok read). Reads are the ONLY
//                                  nondeterministic boundary INPUT a disk replay
//                                  needs: appends/syncs are deterministic given
//                                  the data, so the read LOG is sufficient.
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
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/IScheduler.hpp>
#include <lockstep/core/Task.hpp>

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
// Kept stringly + decimal-only so render() is byte-stable across platforms. The
// optional `blob` is a lower-hex byte string appended AFTER the decimal args (the
// S3 disk read-bytes payload); it is empty for Random/Clock records, so their
// rendered lines are byte-identical to before this field existed (append-only).
struct TraceRecord {
    TraceKind kind{};
    std::string op{};
    std::vector<std::int64_t> args{}; // decimal args; u64 seeds stored bit-cast
    std::string blob{};               // optional lower-hex bytes (disk read data)

    [[nodiscard]] std::string render() const {
        std::string out = to_token(kind);
        out += ' ';
        out += op;
        for (std::int64_t a : args) {
            out += ' ';
            out += std::to_string(a);
        }
        if (!blob.empty()) {
            out += ' ';
            out += blob;
        }
        return out;
    }
};

// Hex codec for the disk read-bytes blob. Lower-hex, two chars per byte, no
// separators — byte-stable across platforms (no locale, no float).
[[nodiscard]] inline std::string bytes_to_hex(std::span<const std::byte> b) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (std::byte by : b) {
        const auto v = static_cast<unsigned>(std::to_integer<unsigned char>(by));
        out.push_back(kHex[(v >> 4) & 0xF]);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

[[nodiscard]] inline std::vector<std::byte> hex_to_bytes(const std::string& h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return 0;
    };
    std::vector<std::byte> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        const int v = (nib(h[i]) << 4) | nib(h[i + 1]);
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

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

// ---------------------------------------------------------------------------
// DISK record-replay (S3). On prod a READ is the nondeterministic boundary INPUT
// (the bytes the platter actually returns); append/sync are deterministic given
// the data stream, so the read LOG is exactly what an offline replay needs.
// ---------------------------------------------------------------------------

// RecordingDisk — wraps a real ProdDisk (any IDisk) and logs each READ's returned
// ErrorCode + delivered bytes into the shared trace, in call order. append/sync
// are forwarded untouched (the wrapper only OBSERVES reads). Because ProdDisk does
// inline synchronous IO, the wrapped Future is already-ready when it returns, so
// we can read its delivered bytes straight off `into` to log them — no co_await
// needed inside the wrapper (and none of the returned span is captured across a
// suspend, V-RKV1).
class RecordingDisk final : public core::IDisk {
public:
    RecordingDisk(core::IDisk& inner, ReplayTrace& trace) noexcept
        : inner_(&inner), trace_(&trace) {}

    [[nodiscard]] core::Future<core::Error>
    append(std::span<const std::byte> data, core::Offset& out_offset) override {
        return inner_->append(data, out_offset);
    }

    [[nodiscard]] core::Future<core::Error>
    read(core::Offset at, std::span<std::byte> into) override {
        const std::uint64_t want = into.size();
        core::Future<core::Error> f = inner_->read(at, into);
        // ProdDisk completes INLINE → f is already-ready here; read its completion
        // VALUE (the Error, set via set_value) to log the observation. We are not a
        // coroutine, so we resume the ready awaiter directly to pull the value out
        // (await_ready() is true; await_resume() returns the value).
        const core::Error e = completed_value(f);
        const auto errc = static_cast<std::int64_t>(static_cast<unsigned>(e.code));
        // On an ok read the bytes in `into` are the delivered payload; on a
        // non-ok read `into` is unspecified, so we log no bytes (empty blob).
        std::string hex;
        if (e.code == core::ErrorCode::Ok) {
            hex = bytes_to_hex(std::span<const std::byte>(into.data(), into.size()));
        }
        TraceRecord rec{TraceKind::Disk, "read",
                        {static_cast<std::int64_t>(at),
                         static_cast<std::int64_t>(want), errc},
                        std::move(hex)};
        trace_->append(std::move(rec));
        return f;
    }

    [[nodiscard]] core::Future<core::Error> sync() override { return inner_->sync(); }

private:
    // Pull the completion VALUE off an already-ready Future<Error> (the Error set
    // via set_value, the convention SimDisk/ProdDisk use). await_resume copies the
    // trivially-copyable Error and leaves the future's stored value intact, so the
    // real driver may still await `f` and observe the same outcome.
    static core::Error completed_value(core::Future<core::Error>& f) {
        auto awaiter = f.operator co_await();
        return awaiter.await_resume();
    }

    core::IDisk* inner_;
    ReplayTrace* trace_;
};

// ReplayDisk — reproduces the recorded READ observations offline (sim-usable,
// pure: no syscalls). It consumes the Disk read records IN ORDER and, for each
// read(), fills `into` with the recorded bytes and returns the recorded
// ErrorCode. append/sync are replay-inert no-ops (deterministic given the data;
// nothing to reproduce). This is what makes a prod disk incident replayable.
class ReplayDisk final : public core::IDisk {
public:
    explicit ReplayDisk(const ReplayTrace& trace, core::Scheduler& sched)
        : sched_(&sched) {
        for (const TraceRecord& r : trace.records()) {
            if (r.kind == TraceKind::Disk && r.op == "read") {
                reads_.push_back(ReadObs{r.args.size() > 2 ? r.args[2] : 0,
                                         hex_to_bytes(r.blob)});
            }
        }
    }

    [[nodiscard]] core::Future<core::Error>
    append(std::span<const std::byte> /*data*/, core::Offset& out_offset) override {
        out_offset = 0; // deterministic; appends are not replayed from the trace
        return ready(core::Error{});
    }

    [[nodiscard]] core::Future<core::Error>
    read(core::Offset /*at*/, std::span<std::byte> into) override {
        if (cursor_ >= reads_.size()) {
            return ready(core::Error{core::ErrorCode::NotFound, "replay trace exhausted"});
        }
        const ReadObs& obs = reads_[cursor_++];
        const auto code = static_cast<core::ErrorCode>(static_cast<std::uint16_t>(obs.errc));
        if (code == core::ErrorCode::Ok) {
            const std::size_t n = into.size() < obs.bytes.size() ? into.size()
                                                                 : obs.bytes.size();
            for (std::size_t i = 0; i < n; ++i) {
                into[i] = obs.bytes[i];
            }
        }
        return ready(core::Error{code, "replay"});
    }

    [[nodiscard]] core::Future<core::Error> sync() override { return ready(core::Error{}); }

private:
    struct ReadObs {
        std::int64_t errc = 0;
        std::vector<std::byte> bytes{};
    };

    [[nodiscard]] core::Future<core::Error> ready(core::Error e) {
        core::Promise<core::Error> p = core::make_promise<core::Error>(sched_);
        core::Future<core::Error> f = p.get_future();
        p.set_value(e);
        return f;
    }

    core::Scheduler* sched_;
    std::vector<ReadObs> reads_{};
    std::size_t cursor_ = 0;
};

// ---------------------------------------------------------------------------
// NETWORK record-replay (S4). On prod the RECV STREAM is the nondeterministic
// boundary INPUT (which message arrives next, from whom, with what bytes — TCP
// timing the core cannot reproduce). send() is deterministic given the inputs, so
// the RECV log is exactly what an offline replay needs. Trace record:
//
//     network recv <from-id> <hexbytes>
//
// from-id = the sender Endpoint id (decimal); hexbytes = the payload, lower-hex,
// no separators (empty payload -> empty blob). Append-only; mixes cleanly with the
// Random/Clock/Disk records (each replay consumer filters by its own kind).
// ---------------------------------------------------------------------------

// RecordingNetwork — wraps any INetwork (a ProdNetwork in prod) and logs each
// recv'd Message (from + bytes) into the shared trace, IN DELIVERY ORDER. send() is
// forwarded untouched (logged-only is unnecessary — send is deterministic given the
// inputs; only the recv stream is the nondeterministic boundary input we must
// reproduce). Because a prod recv() completes ASYNCHRONOUSLY on the reactor (not
// inline like a disk read), the wrapper spawns a tiny forwarder coroutine that
// awaits the inner recv, COPIES the delivered bytes out (the Message span is valid
// only during that callback — V-RKV1), logs them, and fulfils the outer promise.
class RecordingNetwork final : public core::INetwork {
public:
    RecordingNetwork(core::INetwork& inner, core::IScheduler& sched,
                     core::detail::SchedulerSink& sink, ReplayTrace& trace) noexcept
        : inner_(&inner), sched_(&sched), sink_(&sink), trace_(&trace) {}

    [[nodiscard]] core::Endpoint local() const noexcept override {
        return inner_->local();
    }

    [[nodiscard]] core::Future<core::Error>
    send(core::Endpoint to, std::span<const std::byte> payload) override {
        return inner_->send(to, payload);
    }

    [[nodiscard]] core::Future<core::Message> recv() override {
        core::Promise<core::Message> out = core::make_promise<core::Message>(sink_);
        core::Future<core::Message> f = out.get_future();
        sched_->spawn(forward(inner_, trace_, &retained_, std::move(out)));
        return f;
    }

private:
    // Await the inner recv, copy the bytes into a stable buffer, log the
    // observation, then fulfil the outer promise with a span over that buffer.
    static core::Task forward(core::INetwork* inner, ReplayTrace* trace,
                              std::vector<std::vector<std::byte>>* retained,
                              core::Promise<core::Message> out) {
        core::Message m = co_await inner->recv();
        std::vector<std::byte> bytes(m.payload.begin(), m.payload.end());
        trace->append(TraceRecord{
            TraceKind::Network, "recv",
            {static_cast<std::int64_t>(m.from.id)},
            bytes_to_hex(std::span<const std::byte>(bytes.data(), bytes.size()))});
        retained->push_back(std::move(bytes));
        std::span<const std::byte> view(retained->back().data(), retained->back().size());
        out.set_value(core::Message{m.from, view});
        co_return;
    }

    core::INetwork* inner_;
    core::IScheduler* sched_;
    core::detail::SchedulerSink* sink_;
    ReplayTrace* trace_;
    std::vector<std::vector<std::byte>> retained_{}; // stable spans for forwarded msgs
};

// ReplayNetwork — reproduces the recorded RECV observations offline (sim-usable,
// pure: no sockets). It consumes the Network recv records IN ORDER and, for each
// recv(), returns the recorded (from, bytes) as an already-ready Message. send() is
// replay-inert (deterministic given inputs; nothing to reproduce) — it just returns
// an ok already-ready Future. This is what makes a prod network incident replayable.
class ReplayNetwork final : public core::INetwork {
public:
    ReplayNetwork(const ReplayTrace& trace, core::detail::SchedulerSink& sink,
                  core::Endpoint self) noexcept
        : sink_(&sink), self_(self) {
        for (const TraceRecord& r : trace.records()) {
            if (r.kind == TraceKind::Network && r.op == "recv") {
                recvs_.push_back(RecvObs{r.args.empty() ? 0 : r.args[0],
                                         hex_to_bytes(r.blob)});
            }
        }
    }

    [[nodiscard]] core::Endpoint local() const noexcept override { return self_; }

    [[nodiscard]] core::Future<core::Error>
    send(core::Endpoint /*to*/, std::span<const std::byte> /*payload*/) override {
        core::Promise<core::Error> p = core::make_promise<core::Error>(sink_);
        core::Future<core::Error> f = p.get_future();
        p.set_value(core::Error{}); // replay-inert: send is deterministic
        return f;
    }

    [[nodiscard]] core::Future<core::Message> recv() override {
        core::Promise<core::Message> p = core::make_promise<core::Message>(sink_);
        core::Future<core::Message> f = p.get_future();
        if (cursor_ >= recvs_.size()) {
            p.set_error(core::Error{core::ErrorCode::NotFound, "replay recv exhausted"});
            return f;
        }
        const RecvObs& obs = recvs_[cursor_++];
        retained_.push_back(obs.bytes);
        std::span<const std::byte> view(retained_.back().data(), retained_.back().size());
        p.set_value(core::Message{core::Endpoint{static_cast<std::uint64_t>(obs.from)}, view});
        return f;
    }

private:
    struct RecvObs {
        std::int64_t from = 0;
        std::vector<std::byte> bytes{};
    };

    core::detail::SchedulerSink* sink_;
    core::Endpoint self_;
    std::vector<RecvObs> recvs_{};
    std::vector<std::vector<std::byte>> retained_{};
    std::size_t cursor_ = 0;
};

} // namespace lockstep::prod
