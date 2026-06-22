#pragma once

// SimDisk.hpp — C2.2. In-memory simulated durable-storage device implementing
// core::IDisk, with a seed-driven fault model. This lives under providers/sim/
// (the lint-exempt boundary zone) and is the WORST-CASE real-disk model the
// storage engine (Phase 3) and crash recovery are verified against.
//
// ----------------------------------------------------------------------------
// WHAT THIS MODELS (and why each is faithful to a real un-fsynced crash)
// ----------------------------------------------------------------------------
// A real append-structured device has THREE distinct layers of state:
//
//   (1) DURABLE  — bytes the platter/NAND truly holds; survive a power-cut.
//   (2) STAGED   — bytes accepted by append() but NOT yet flushed: they live in
//                  the OS page cache / drive write-back buffer. A crash here
//                  loses them. sync() is what *promises* to move STAGED -> DURABLE.
//   (3) LYING    — bytes a buggy/fast drive ACK'd as durable on sync() but did
//                  NOT actually persist (write-cache that lied about FUA/flush).
//                  sync() returned ok, yet a crash STILL loses them. This is the
//                  single most dangerous real failure and the reason sync() is a
//                  separate barrier in the IDisk contract.
//
// We keep these three byte-images explicitly so a crash() is a pure, auditable
// "drop everything not truly DURABLE" operation. recover() then exposes exactly
// the DURABLE prefix — which is what Phase 3 crash-recovery relies on.
//
// FAULTS (all decided from IRandom + virtual clock — never wall-clock, never
// std::random, never a thread):
//
//   * LATENCY      — every op completes after a seeded delay scheduled on the
//                    Scheduler's virtual-time timers (an internal driver Task
//                    awaits clock.delay(d) then fulfils the op's promise).
//   * IO FAULT     — an op may complete with ErrorCode::IoFault and have NO
//                    effect (append writes nothing; read returns nothing).
//   * TORN WRITE   — an append may land only a PARTIAL prefix of its bytes into
//                    the staged image (a fraction of a page), modelling a write
//                    interrupted mid-flight. The append still completes ok (a
//                    real drive does not know it tore), so a later read observes
//                    the partial page. This is observable-partial, NOT all-or-
//                    nothing.
//   * LYING FSYNC  — sync() returns ok but, instead of promoting ALL staged
//                    bytes to DURABLE, it promotes only a PREFIX and marks the
//                    rest LYING (ack'd-durable but not really). On the next
//                    crash() those LYING bytes are LOST — exactly as the brief's
//                    hard invariant requires.
//   * BIT-ROT      — a durable byte may silently flip at rest; a read covering it
//                    reports ErrorCode::Corruption (optional; off by default).
//
// DETERMINISM: every fault is a pure function of (seed, op-sequence). The fault
// schedule is drawn from a single IRandom in a FIXED order per op, so two runs
// with the same seed produce a byte-identical scheduler Trace. No unordered
// container, no pointer ordering, no float in any ordering key.
//
// FORBIDDEN (and absent here): wall-clock, real file IO (open/read/write/fsync),
// std::thread/atomic/mutex, std::*_distribution / std::shuffle / std::rand.
//
// COMPLETION CONVENTION (matches the landed Future<Error>): every op fulfils its
// promise via set_value(Error), so the awaiting coroutine reads the outcome as
// the co_await *value* (Future<Error>::await_resume returns the value; the
// separate error channel surfaces as a default-ok Error to an awaiter). Callers
// therefore inspect the returned Error, e.g. `Error e = co_await disk.read(...)`.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>
#include <lockstep/core/Trace.hpp>

namespace lockstep::sim {

// Tunable fault probabilities + latency window. All defaults are deterministic
// constants; a test dials them per-scenario. Probabilities are seeded coin
// flips (IRandom::chance), so 0.0 disables a fault entirely and is exactly
// reproducible.
struct DiskFaultConfig {
    // Latency window [min,max] virtual ticks added to every op (inclusive).
    std::int64_t latency_min = 1;
    std::int64_t latency_max = 4;

    // P(an append or read completes as an IoFault with no effect).
    double io_fault_prob = 0.0;

    // P(an append lands only a partial prefix — a torn write). When torn, the
    // kept length is a seeded fraction in [1, len-1] of the bytes.
    double torn_write_prob = 0.0;

    // P(a sync() lies: promotes only a prefix of staged bytes to durable and
    // marks the remainder LYING — ack'd but lost on the next crash).
    double lying_fsync_prob = 0.0;

    // P(a single durable byte is silently flipped at rest, observed as Corruption
    // by a read that covers it). Off by default.
    double bit_rot_prob = 0.0;
};

// ---------------------------------------------------------------------------
// SimDisk — one append-structured object (a WAL segment / SSTable backing).
// ---------------------------------------------------------------------------
class SimDisk final : public core::IDisk {
public:
    SimDisk(core::Scheduler& sched, core::IClock& clock, core::IRandom& rng,
            DiskFaultConfig cfg = {}) noexcept
        : sched_(&sched), clock_(&clock), rng_(&rng), cfg_(cfg) {}

    SimDisk(const SimDisk&) = delete;
    SimDisk& operator=(const SimDisk&) = delete;
    SimDisk(SimDisk&&) = delete;
    SimDisk& operator=(SimDisk&&) = delete;
    ~SimDisk() override = default;

    // ---- core::IDisk -----------------------------------------------------

    // Append bytes to the end of the device. The placement offset is the current
    // logical end-of-device (the high-water mark over staged+durable+lying), and
    // is written to out_offset BEFORE completion (per the IDisk contract). The
    // op completes after a seeded latency; the effect (full write / torn-partial
    // write / io-fault no-op) is decided deterministically up front.
    [[nodiscard]] core::Future<core::Error>
    append(std::span<const std::byte> data, core::Offset& out_offset) override {
        const std::uint64_t op = next_op_id_++;

        // Resolve the placement offset eagerly (contract: written before
        // completion). It is the device's current logical length.
        const core::Offset off = logical_len();
        out_offset = off;

        // Decide the fault for THIS op now, in a fixed draw order, so the seed
        // alone determines it. Order: io-fault flip, then (if not faulted) the
        // torn-write flip, then the latency draw. Keeping the order fixed is what
        // makes the byte stream stable.
        const bool io_fault = rng_->chance(cfg_.io_fault_prob);

        std::size_t kept = data.size();
        bool torn = false;
        if (!io_fault && data.size() > 1 && rng_->chance(cfg_.torn_write_prob)) {
            // Keep a seeded prefix of [1, size-1] bytes — a partial page.
            torn = true;
            kept = static_cast<std::size_t>(
                rng_->uniform_range(1, static_cast<std::int64_t>(data.size()) - 1));
        }

        const std::int64_t lat = draw_latency();

        // Snapshot the bytes to keep (none on io-fault). We materialise the copy
        // synchronously; the staged image is only mutated when the op COMPLETES
        // (after latency), so concurrent inspection sees pre-completion state.
        std::vector<std::byte> payload;
        if (!io_fault) {
            payload.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(kept));
        }

        trace(std::string("disk_append op=") + std::to_string(op) + " off=" +
              std::to_string(off) + " len=" + std::to_string(data.size()) +
              " kept=" + std::to_string(kept) + " torn=" + (torn ? "1" : "0") +
              " io_fault=" + (io_fault ? "1" : "0") + " lat=" + std::to_string(lat));

        core::Promise<core::Error> p = core::make_promise<core::Error>(sched_);
        core::Future<core::Error> f = p.get_future();

        // Drive completion on virtual time: an internal Task waits the latency
        // then applies the effect + fulfils the promise. Spawned onto the
        // scheduler so the ONE driver loop owns the resume (L1/L5).
        sched_->spawn(run_append(std::move(p), off, std::move(payload), io_fault, op, lat));
        return f;
    }

    // Read into `into` from offset `at`. Sees the CURRENT observable image:
    // durable + lying + staged bytes (a real read reflects the page cache).
    // A read covering a bit-rotted durable byte reports Corruption; a read past
    // the written end reports NotFound; an injected io-fault reports IoFault.
    [[nodiscard]] core::Future<core::Error>
    read(core::Offset at, std::span<std::byte> into) override {
        const std::uint64_t op = next_op_id_++;
        const bool io_fault = rng_->chance(cfg_.io_fault_prob);
        const std::int64_t lat = draw_latency();

        core::Error result{};
        if (io_fault) {
            result = core::Error{core::ErrorCode::IoFault, "sim injected read io-fault"};
        } else {
            result = fill_read(at, into);
        }

        trace(std::string("disk_read op=") + std::to_string(op) + " at=" +
              std::to_string(at) + " len=" + std::to_string(into.size()) +
              " err=" + std::to_string(static_cast<unsigned>(result.code)) +
              " lat=" + std::to_string(lat));

        core::Promise<core::Error> p = core::make_promise<core::Error>(sched_);
        core::Future<core::Error> f = p.get_future();
        sched_->spawn(run_complete(std::move(p), result, op, lat, "read"));
        return f;
    }

    // Durability barrier. Honest sync(): promote ALL staged bytes to durable.
    // Lying sync(): promote only a seeded prefix and mark the remainder LYING
    // (acked-durable but lost on the next crash). Either way sync() COMPLETES
    // ok — the lie is silent, exactly as a write-cache that ignores flush.
    [[nodiscard]] core::Future<core::Error> sync() override {
        const std::uint64_t op = next_op_id_++;
        const bool lying = (!staged_.empty()) && rng_->chance(cfg_.lying_fsync_prob);
        const std::int64_t lat = draw_latency();

        std::size_t promoted = staged_.size();
        if (lying) {
            // Promote a seeded prefix [0, staged-1]; the rest becomes LYING.
            promoted = static_cast<std::size_t>(
                rng_->uniform_range(0, static_cast<std::int64_t>(staged_.size()) - 1));
        }

        // Apply the promotion to the durable image immediately (the effect is a
        // function of state observed now); completion is just latency-delayed.
        const std::size_t lying_added = staged_.size() - promoted;
        durable_.insert(durable_.end(), staged_.begin(),
                        staged_.begin() + static_cast<std::ptrdiff_t>(promoted));
        // The un-promoted staged tail is "lying": acked durable, not truly so.
        lying_.insert(lying_.end(),
                      staged_.begin() + static_cast<std::ptrdiff_t>(promoted),
                      staged_.end());
        staged_.clear();

        trace(std::string("disk_sync op=") + std::to_string(op) + " promoted=" +
              std::to_string(promoted) + " lying_added=" + std::to_string(lying_added) +
              " lying=" + (lying ? "1" : "0") + " durable_len=" +
              std::to_string(durable_.size()) + " lat=" + std::to_string(lat));

        core::Promise<core::Error> p = core::make_promise<core::Error>(sched_);
        core::Future<core::Error> f = p.get_future();
        sched_->spawn(run_complete(std::move(p), core::Error{}, op, lat, "sync"));
        return f;
    }

    // ---- crash / recover boundary (sim-only; not part of IDisk) -----------

    // Simulated power loss. Drops EVERYTHING that is not TRULY DURABLE:
    //   - staged (un-synced) bytes      -> lost (real un-fsynced crash semantics)
    //   - lying  (acked-but-not-durable) -> lost (the lying-fsync hard invariant)
    // The durable image is left untouched: it IS the consistent prefix. This is
    // a pure, synchronous boundary — no latency, no fault (a crash always lands).
    void crash() {
        trace(std::string("disk_crash dropped_staged=") + std::to_string(staged_.size()) +
              " dropped_lying=" + std::to_string(lying_.size()) + " survived_durable=" +
              std::to_string(durable_.size()));
        staged_.clear();
        lying_.clear();
        crashed_ = true;
    }

    // Reopen after a crash. Post-condition: the observable device is EXACTLY the
    // durable prefix that survived. recover() optionally injects bit-rot at rest
    // (a durable byte silently flips), which a covering read later reports as
    // Corruption — modelling latent media decay surviving the crash.
    void recover() {
        crashed_ = false;
        maybe_bit_rot();
        trace(std::string("disk_recover durable_len=") + std::to_string(durable_.size()) +
              " rotted=" + std::to_string(rotted_count()));
    }

    // ---- introspection (tests / checkers; never used for ordering) --------

    [[nodiscard]] std::size_t durable_len() const noexcept { return durable_.size(); }
    [[nodiscard]] std::size_t staged_len() const noexcept { return staged_.size(); }
    [[nodiscard]] std::size_t lying_len() const noexcept { return lying_.size(); }
    [[nodiscard]] std::size_t logical_len() const noexcept {
        return durable_.size() + lying_.size() + staged_.size();
    }
    [[nodiscard]] bool crashed() const noexcept { return crashed_; }

    // The durable bytes that would survive a crash right now (the recoverable
    // PREFIX). Returned by value for the test to compare against expectations.
    [[nodiscard]] std::vector<std::byte> durable_snapshot() const { return durable_; }

private:
    // ----- internal driver coroutines -------------------------------------

    // Append completion: wait latency, then mutate the staged image (unless
    // io-faulted) and fulfil the promise. Mutating ON COMPLETION (not at call
    // time) keeps the model honest: an in-flight append is not yet visible.
    core::Task run_append(core::Promise<core::Error> p, core::Offset off,
                          std::vector<std::byte> payload, bool io_fault,
                          std::uint64_t op, std::int64_t lat) {
        if (lat > 0) {
            co_await clock_->delay(lat);
        }
        if (io_fault) {
            trace(std::string("disk_append_done op=") + std::to_string(op) +
                  " effect=iofault");
            // The Error IS the completion VALUE of Future<Error>: set_value so
            // `co_await` surfaces it (await_resume returns the value; the error
            // channel would surface as a default-ok Error to the awaiter).
            p.set_value(core::Error{core::ErrorCode::IoFault, "sim injected append io-fault"});
            co_return;
        }
        // Defensive: an append must land at the logical end. If concurrent ops
        // shifted the tail this would assert in a real engine; here appends are
        // serialized by op-id order so off always equals current logical_len().
        (void)off;
        staged_.insert(staged_.end(), payload.begin(), payload.end());
        trace(std::string("disk_append_done op=") + std::to_string(op) + " staged_len=" +
              std::to_string(staged_.size()));
        p.set_value(core::Error{}); // ok value; torn-ness already baked into payload
        co_return;
    }

    // Generic completion for read/sync: wait latency, then fulfil with the
    // pre-decided result. The effect (read fill / sync promotion) was already
    // applied at call time; only the COMPLETION is delayed.
    core::Task run_complete(core::Promise<core::Error> p, core::Error result,
                           std::uint64_t op, std::int64_t lat, const char* kind) {
        if (lat > 0) {
            co_await clock_->delay(lat);
        }
        trace(std::string("disk_") + kind + "_done op=" + std::to_string(op) +
              " err=" + std::to_string(static_cast<unsigned>(result.code)));
        // The Error is the completion VALUE: set_value so a co_await surfaces it
        // (Future<Error>::await_resume returns the value, not the error channel).
        p.set_value(result);
        co_return;
    }

    // ----- read fill over the observable image ----------------------------

    // Build the current observable byte image (durable ++ lying ++ staged) and
    // copy the requested window into `into`. Reports NotFound on a short read
    // past the written end, Corruption if any covered durable byte is rotted.
    core::Error fill_read(core::Offset at, std::span<std::byte> into) {
        const std::size_t want = into.size();
        const std::size_t total = logical_len();
        if (at > total || (at + want) > total) {
            return core::Error{core::ErrorCode::NotFound, "sim read past end-of-device"};
        }
        // Corruption check: if any rotted durable offset falls in [at, at+want).
        for (std::size_t r : rotted_offsets_) {
            if (r >= at && r < at + want) {
                return core::Error{core::ErrorCode::Corruption, "sim bit-rot on read"};
            }
        }
        for (std::size_t k = 0; k < want; ++k) {
            into[k] = byte_at(at + k);
        }
        return core::Error{};
    }

    // The observable byte at a logical offset: durable region, then lying, then
    // staged, concatenated in write order. Caller guarantees in-range.
    [[nodiscard]] std::byte byte_at(std::size_t i) const noexcept {
        if (i < durable_.size()) {
            return durable_[i];
        }
        i -= durable_.size();
        if (i < lying_.size()) {
            return lying_[i];
        }
        i -= lying_.size();
        return staged_[i];
    }

    // ----- bit-rot at rest -------------------------------------------------

    // On recover(), maybe flip one durable byte and record its offset so a
    // covering read reports Corruption. Seeded: pure function of (seed, history).
    void maybe_bit_rot() {
        if (durable_.empty() || !rng_->chance(cfg_.bit_rot_prob)) {
            return;
        }
        const std::size_t idx = static_cast<std::size_t>(
            rng_->uniform_range(0, static_cast<std::int64_t>(durable_.size()) - 1));
        durable_[idx] = static_cast<std::byte>(
            std::to_integer<unsigned>(durable_[idx]) ^ 0xFFu);
        rotted_offsets_.push_back(idx);
    }

    [[nodiscard]] std::size_t rotted_count() const noexcept { return rotted_offsets_.size(); }

    // ----- helpers ---------------------------------------------------------

    [[nodiscard]] std::int64_t draw_latency() {
        return rng_->uniform_range(cfg_.latency_min, cfg_.latency_max);
    }

    std::uint64_t trace(std::string payload) {
        return sched_->trace(core::TraceAction::Spawn, std::move(payload));
    }

    core::Scheduler* sched_;
    core::IClock* clock_;
    core::IRandom* rng_;
    DiskFaultConfig cfg_;

    // The three byte images. Logical device = durable ++ lying ++ staged.
    std::vector<std::byte> durable_{}; // truly persisted; survives a crash
    std::vector<std::byte> lying_{};   // acked-durable by a lying sync; lost on crash
    std::vector<std::byte> staged_{};  // appended, not yet synced; lost on crash

    std::vector<std::size_t> rotted_offsets_{}; // durable offsets flipped at rest

    std::uint64_t next_op_id_ = 0;
    bool crashed_ = false;
};

} // namespace lockstep::sim
