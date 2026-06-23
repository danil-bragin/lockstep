#pragma once

// ContractConformance.hpp — Phase 7 S1. The REUSABLE boundary-provider contract
// conformance suite (V-PROD-CONTRACT). It encodes, as runnable checks, exactly
// what the FROZEN interface headers in core/include/lockstep/core/ promise:
//   IClock   (now/delay)         IRandom  (next/uniform/uniform_range/chance)
//   IDisk    (append/read/sync)  INetwork (local/send/recv)
//
// ----------------------------------------------------------------------------
// TWO TIERS — read this before adding a check.
// ----------------------------------------------------------------------------
// (A) UNIVERSAL CONTRACT  — properties EVERY impl of the interface MUST satisfy,
//     sim AND future prod. These live in `conformance::universal::*` and take the
//     provider through a small FACTORY abstraction, so the SAME functions run
//     against the sim providers today and against the prod providers in S2–S4.
//     A prod provider that diverges from the contract FAILS these. They assert
//     ONLY the documented happy-path + documented-error contract; never a sim
//     internal (no torn writes, no lying fsync, no drop/dup/reorder, no crash()).
//
// (B) SIM-ONLY fault behaviours — torn writes, lying fsync, drop/dup/reorder/
//     partition injection, latency models, crash()/recover(). These are sim
//     verification AIDS, NOT part of the universal contract a prod impl must (or
//     can) reproduce. They are DELIBERATELY ABSENT from this header. The driver
//     test keeps them in a clearly-labelled sim-only section that NEVER runs
//     against a prod factory. Do NOT add a (B) behaviour here.
//
// ----------------------------------------------------------------------------
// HOW A PROD IMPL PLUGS IN (S2–S4).
// ----------------------------------------------------------------------------
// Each universal check is templated on a FACTORY that knows how to construct the
// provider under test on a given deterministic Scheduler/SimClock (for the async
// IDisk/INetwork checks the SCHEDULER is the sim core — that is the harness, not
// the provider; a prod Disk/Network is still driven by awaiting its Futures on
// the sim scheduler in S1, matching how every existing sim test drives them).
// The sim driver builds a `SimClockFactory`, `SeededRandomFactory`,
// `SimDiskFactory`, `SimNetworkFactory`. A prod driver in S2–S4 builds a
// `ProdClockFactory`, ... and calls the IDENTICAL `conformance::universal::*`
// functions. No check changes; only the factory does.
//
// A factory is any type exposing the documented members below. They are passed
// by reference so they may own per-suite state (e.g. a temp dir for a prod Disk).
//
//   ClockFactory:   IClock&   clock(Scheduler&);              // bound to sched
//   RandomFactory:  unique_ptr<IRandom> make(uint64_t seed);  // fresh PRNG
//   DiskFactory:    unique_ptr<IDisk>   make(Scheduler&, IClock&, IRandom&);
//                   // an HONEST device (no injected faults) — the contract is
//                   // the no-fault durability promise; faults are tier (B).
//   NetworkFactory: a bus-like object exposing node(id)->INetwork& and
//                   add_nodes(...); see SimNetworkFactory for the shape. Built
//                   HONEST (no drop/dup/reorder/partition) — those are tier (B).
//
// ----------------------------------------------------------------------------
// Determinism: where randomness is involved every check is a pure function of a
// seed threaded in by the caller. No wall-clock, no threads, no std::* outside
// providers/. This is non-provider code — the forbidden-call lint scans it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lockstep/core/Error.hpp>
#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/IRandom.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

namespace conformance {

// Local alias so the contract checks read `core::X` like the rest of the tree.
namespace core = lockstep::core;

// ---------------------------------------------------------------------------
// A tiny in-process result accumulator. Each check appends one named outcome;
// the driver renders + tallies them. No exceptions, no globals: a value passed
// by reference so the suite can be run many times (e.g. determinism re-runs).
// ---------------------------------------------------------------------------
struct Report {
    struct Item {
        std::string name;
        bool pass = false;
        std::string detail; // populated only on failure (witness / expected-vs-got)
    };
    std::vector<Item> items{};

    void add(std::string name, bool pass, std::string detail = {}) {
        items.push_back(Item{std::move(name), pass, std::move(detail)});
    }
    [[nodiscard]] bool all_pass() const noexcept {
        for (const Item& it : items) {
            if (!it.pass) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] std::size_t failures() const noexcept {
        std::size_t n = 0;
        for (const Item& it : items) {
            if (!it.pass) {
                ++n;
            }
        }
        return n;
    }
};

// Span helper over a byte vector (non-owning view used by the disk/net checks).
[[nodiscard]] inline std::span<const std::byte> view_of(const std::vector<std::byte>& v) {
    return std::span<const std::byte>(v.data(), v.size());
}

// Make a deterministic byte payload of `n` bytes from a seed-ish base (pure).
[[nodiscard]] inline std::vector<std::byte> payload(std::size_t n, std::uint8_t base) {
    std::vector<std::byte> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back(static_cast<std::byte>(base + static_cast<std::uint8_t>(i)));
    }
    return v;
}

namespace universal {

// ===========================================================================
// IClock — UNIVERSAL CONTRACT
//   * now() is monotonic non-decreasing across calls (IClock.hpp: "Never
//     decreases across calls on the same clock").
//   * delay(d>0) completes only AFTER virtual time advanced by EXACTLY d, and
//     now() observes that advance (IClock.hpp: "completes once virtual time has
//     advanced by `d`").
//   * delay(d<=0) completes at the CURRENT virtual time, advancing nothing
//     (IClock.hpp: "`d <= 0` ... completes at the current virtual time").
//   * timers fire in time order: a shorter delay completes before a longer one
//     armed at the same instant (Scheduler L4 / the IClock ordering promise).
// ClockFactory must expose: IClock& clock(Scheduler&)  (bound to that scheduler).
// ===========================================================================
template <class ClockFactory>
void check_clock_contract(ClockFactory& factory, Report& rep) {
    // --- monotonic now() + exact-advance delay -----------------------------
    {
        core::Scheduler sched;
        core::IClock& clock = factory.clock(sched);

        struct State {
            bool ran = false;
            bool monotonic = true;
            core::Tick t_before = 0;
            core::Tick t_after = 0;
            core::Tick last = 0;
        } st;

        // A driver that samples now() repeatedly (monotonic), then delays an
        // exact span and re-checks the advance is EXACTLY that span.
        auto driver = [](core::IClock& c, State& s) -> core::Task {
            // Several now() reads with intervening yields must never decrease.
            s.last = c.now();
            for (int i = 0; i < 4; ++i) {
                core::Tick t = c.now();
                if (t < s.last) {
                    s.monotonic = false;
                }
                s.last = t;
            }
            s.t_before = c.now();
            constexpr core::Duration kSpan = 7;
            co_await c.delay(kSpan);
            s.t_after = c.now();
            s.ran = true;
            co_return;
        };

        sched.spawn(driver(clock, st));
        sched.run();

        rep.add("clock/now-monotonic", st.ran && st.monotonic,
                st.ran ? "now() decreased across calls" : "driver did not finish");
        const core::Tick advanced = st.t_after - st.t_before;
        rep.add("clock/delay-advances-exactly-d", st.ran && advanced == 7,
                std::string("expected advance 7, got ") + std::to_string(advanced));
    }

    // --- delay(d<=0) advances nothing --------------------------------------
    {
        core::Scheduler sched;
        core::IClock& clock = factory.clock(sched);
        struct State {
            bool ran = false;
            core::Tick before = 0;
            core::Tick after = 0;
        } st;
        auto driver = [](core::IClock& c, State& s) -> core::Task {
            s.before = c.now();
            co_await c.delay(0);
            co_await c.delay(-5);
            s.after = c.now();
            s.ran = true;
            co_return;
        };
        sched.spawn(driver(clock, st));
        sched.run();
        rep.add("clock/delay-nonpositive-no-advance", st.ran && st.before == st.after,
                std::string("clock moved on d<=0: before ") + std::to_string(st.before) +
                    " after " + std::to_string(st.after));
    }

    // --- timers fire in time order -----------------------------------------
    {
        core::Scheduler sched;
        core::IClock& clock = factory.clock(sched);
        // Two concurrent timers armed at the same instant: 3 ticks and 9 ticks.
        // The 3-tick timer MUST complete (record its order) before the 9-tick.
        struct State {
            std::vector<int> fire_order{};
        } st;
        auto timer_task = [](core::IClock& c, core::Duration d, int id,
                             State& s) -> core::Task {
            co_await c.delay(d);
            s.fire_order.push_back(id);
            co_return;
        };
        sched.spawn(timer_task(clock, 9, /*id=*/9, st));
        sched.spawn(timer_task(clock, 3, /*id=*/3, st));
        sched.run();
        const bool ok = st.fire_order.size() == 2 && st.fire_order[0] == 3 &&
                        st.fire_order[1] == 9;
        std::string got;
        for (int id : st.fire_order) {
            got += std::to_string(id) + " ";
        }
        rep.add("clock/timers-fire-in-time-order", ok,
                std::string("expected [3 9], got [") + got + "]");
    }
}

// ===========================================================================
// IRandom — UNIVERSAL CONTRACT
//   * determinism: same seed ⇒ identical next() sequence (IRandom.hpp: "Sequence
//     is fully determined by the seed").
//   * a different seed yields a different sequence (the seed is actually wired).
//   * uniform(bound) ∈ [0, bound) for bound>0 (IRandom.hpp).
//   * uniform_range(lo,hi) ∈ [lo,hi] for lo<=hi (IRandom.hpp).
//   * chance(p<=0)==false, chance(p>=1)==true (IRandom.hpp).
//   * no modulo bias — statistical, bounded sample: a small bound that does NOT
//     divide 2^64 produces a near-uniform histogram (a biased modulo impl skews
//     the low residues; this catches it without a distribution type).
// RandomFactory must expose: unique_ptr<IRandom> make(uint64_t seed).
// ===========================================================================
template <class RandomFactory>
void check_random_contract(RandomFactory& factory, Report& rep) {
    constexpr std::uint64_t kSeed = 0xA5A5'1234'DEAD'BEEFULL;

    // --- determinism: same seed ⇒ identical next() stream ------------------
    {
        auto r1 = factory.make(kSeed);
        auto r2 = factory.make(kSeed);
        bool identical = true;
        for (int i = 0; i < 256; ++i) {
            if (r1->next() != r2->next()) {
                identical = false;
                break;
            }
        }
        rep.add("random/same-seed-identical-next", identical,
                "two PRNGs from the same seed diverged");
    }

    // --- a different seed diverges (seed is actually threaded) --------------
    {
        auto ra = factory.make(kSeed);
        auto rb = factory.make(kSeed ^ 0xFFFF'FFFF'FFFF'FFFFULL);
        bool differs = false;
        for (int i = 0; i < 256; ++i) {
            if (ra->next() != rb->next()) {
                differs = true;
                break;
            }
        }
        rep.add("random/different-seed-differs", differs,
                "different seeds produced an identical stream");
    }

    // --- uniform(bound) ∈ [0, bound) ---------------------------------------
    {
        auto r = factory.make(kSeed);
        bool in_range = true;
        const std::uint64_t bounds[] = {1, 2, 7, 100, 1000, (1ULL << 40)};
        for (std::uint64_t b : bounds) {
            for (int i = 0; i < 500; ++i) {
                std::uint64_t v = r->uniform(b);
                if (v >= b) {
                    in_range = false;
                    break;
                }
            }
            if (!in_range) {
                break;
            }
        }
        rep.add("random/uniform-in-half-open-range", in_range,
                "uniform(bound) returned a value >= bound");
    }

    // --- uniform_range(lo,hi) ∈ [lo,hi] ------------------------------------
    {
        auto r = factory.make(kSeed);
        bool in_range = true;
        struct LH {
            std::int64_t lo, hi;
        };
        const LH cases[] = {{0, 0}, {-5, 5}, {1, 1}, {-1000, 1000},
                            {-9'000'000'000LL, 9'000'000'000LL}};
        for (const LH& c : cases) {
            for (int i = 0; i < 500; ++i) {
                std::int64_t v = r->uniform_range(c.lo, c.hi);
                if (v < c.lo || v > c.hi) {
                    in_range = false;
                    break;
                }
            }
            if (!in_range) {
                break;
            }
        }
        rep.add("random/uniform_range-in-closed-range", in_range,
                "uniform_range(lo,hi) returned a value outside [lo,hi]");
    }

    // --- chance(p<=0)==false, chance(p>=1)==true ---------------------------
    {
        auto r = factory.make(kSeed);
        bool boundaries_ok = true;
        for (int i = 0; i < 64; ++i) {
            if (r->chance(0.0) || r->chance(-1.0) || !r->chance(1.0) || !r->chance(2.0)) {
                boundaries_ok = false;
                break;
            }
        }
        rep.add("random/chance-boundaries", boundaries_ok,
                "chance(<=0) was true or chance(>=1) was false");
    }

    // --- chance(p) is roughly p over a bounded sample (non-vacuous) --------
    {
        auto r = factory.make(kSeed);
        constexpr int kN = 20000;
        int hits = 0;
        for (int i = 0; i < kN; ++i) {
            if (r->chance(0.25)) {
                ++hits;
            }
        }
        // Expect ~5000; a wide tolerance keeps it deterministic-yet-meaningful.
        const bool ok = hits > 4000 && hits < 6000;
        rep.add("random/chance-frequency-approx", ok,
                std::string("chance(0.25) hits ") + std::to_string(hits) +
                    " of 20000 (want 4000..6000)");
    }

    // --- no modulo bias: a near-uniform histogram for a non-divisor bound ---
    {
        auto r = factory.make(kSeed);
        // 2^64 mod 7 != 0, so a naive `next() % 7` over-weights residues 0..(2^64
        // mod 7 - 1). A bias-free impl keeps every bucket within a tight band.
        constexpr std::uint64_t kBound = 7;
        constexpr int kN = 70000; // 10000 expected per bucket
        std::vector<int> hist(kBound, 0);
        for (int i = 0; i < kN; ++i) {
            std::uint64_t v = r->uniform(kBound);
            if (v < kBound) {
                ++hist[static_cast<std::size_t>(v)];
            }
        }
        const int expected = kN / static_cast<int>(kBound);
        const int tol = expected / 5; // ±20% band: tight enough to catch skew
        bool uniform_ok = true;
        std::string detail;
        for (std::size_t b = 0; b < kBound; ++b) {
            if (hist[b] < expected - tol || hist[b] > expected + tol) {
                uniform_ok = false;
                detail += "bucket " + std::to_string(b) + "=" +
                          std::to_string(hist[b]) + " ";
            }
        }
        rep.add("random/uniform-no-modulo-bias", uniform_ok,
                detail.empty() ? "" : (std::string("skewed: ") + detail));
    }
}

// ===========================================================================
// IDisk — UNIVERSAL CONTRACT (HONEST device; faults are tier B, not here)
//   * append reports a placement offset via out_offset (IDisk.hpp).
//   * offsets are monotonic / append-structured: each append lands at the prior
//     logical end (IDisk.hpp: "Appends `data` to the END of the device").
//   * SYNC DURABILITY BARRIER + read-back: data appended-then-synced reads back
//     byte-identical (IDisk.hpp: sync() "establishes durability"; read() returns
//     the bytes). On an honest device the read path is consistent.
//   * a read past the written end reports the documented short-read (NotFound)
//     (IDisk.hpp: "short reads past end-of-device report NotFound").
// DiskFactory must expose: unique_ptr<IDisk> make(Scheduler&, IClock&, IRandom&)
//   constructing an HONEST device.
// ===========================================================================
template <class ClockFactory, class RandomFactory, class DiskFactory>
void check_disk_contract(ClockFactory& clock_factory, RandomFactory& random_factory,
                         DiskFactory& disk_factory, Report& rep) {
    core::Scheduler sched;
    core::IClock& clock = clock_factory.clock(sched);
    auto rng = random_factory.make(0xD15C'0FFE'E5EE'D001ULL);
    auto disk = disk_factory.make(sched, clock, *rng);

    struct State {
        bool ran = false;
        core::Error append0_err{};
        core::Error append1_err{};
        core::Error sync_err{};
        core::Offset off0 = ~0ULL;
        core::Offset off1 = ~0ULL;
        std::vector<std::byte> readback0{};
        std::vector<std::byte> readback1{};
        core::Error read0_err{};
        core::Error read1_err{};
        core::Error past_end_err{};
    } st;

    const std::vector<std::byte> rec0 = payload(16, 0x10);
    const std::vector<std::byte> rec1 = payload(24, 0x40);

    auto driver = [&](core::IDisk& d, State& s) -> core::Task {
        // Two appends; capture their reported offsets.
        s.append0_err = co_await d.append(view_of(rec0), s.off0);
        s.append1_err = co_await d.append(view_of(rec1), s.off1);
        // Durability barrier.
        s.sync_err = co_await d.sync();

        // Read each record back at its reported offset.
        s.readback0.assign(rec0.size(), std::byte{0});
        s.read0_err = co_await d.read(s.off0,
                                      std::span<std::byte>(s.readback0.data(),
                                                           s.readback0.size()));
        s.readback1.assign(rec1.size(), std::byte{0});
        s.read1_err = co_await d.read(s.off1,
                                      std::span<std::byte>(s.readback1.data(),
                                                           s.readback1.size()));

        // A read that runs past the written end must be a documented short read.
        std::vector<std::byte> over(rec0.size() + rec1.size() + 32, std::byte{0});
        s.past_end_err = co_await d.read(0, std::span<std::byte>(over.data(), over.size()));
        s.ran = true;
        co_return;
    };

    sched.spawn(driver(*disk, st));
    sched.run();

    rep.add("disk/append-reports-offset",
            st.ran && !st.append0_err && !st.append1_err && st.off0 != ~0ULL,
            "append did not report an ok offset");
    // Append-structured / monotonic: off0 == 0, off1 == off0 + |rec0|.
    const bool offsets_ok = st.off0 == 0 && st.off1 == st.off0 + rec0.size();
    rep.add("disk/offsets-monotonic-append-structured", offsets_ok,
            std::string("off0=") + std::to_string(st.off0) + " off1=" +
                std::to_string(st.off1) + " expected 0 and " +
                std::to_string(rec0.size()));
    rep.add("disk/sync-barrier-ok", st.ran && !st.sync_err, "honest sync() failed");
    rep.add("disk/readback-after-sync-matches",
            st.ran && !st.read0_err && !st.read1_err && st.readback0 == rec0 &&
                st.readback1 == rec1,
            "synced data did not read back byte-identical");
    rep.add("disk/read-past-end-notfound",
            st.ran && st.past_end_err.code == core::ErrorCode::NotFound,
            std::string("read past end gave code ") +
                std::to_string(static_cast<unsigned>(st.past_end_err.code)) +
                " (want NotFound)");
}

// ===========================================================================
// INetwork — UNIVERSAL CONTRACT (HONEST bus; faults/partition are tier B)
//   * local() is stable across calls (INetwork.hpp: "Stable for the lifetime").
//   * send() on a live link is accepted (ok Error) (INetwork.hpp: send completes
//     ok "once the message is accepted for delivery").
//   * recv() delivers a sent message, with the correct `from` and payload bytes
//     (INetwork.hpp: recv "Completes with the next Message when one arrives").
//   * per-link ORDER: on an honest (no-reorder) link, messages from one sender to
//     one receiver are delivered in send order (the documented per-link semantic
//     the prod TCP transport must honor; sim's reorder is tier B and OFF here).
// Takes an ALREADY-CONSTRUCTED, HONEST bus handle (no drop/dup/reorder/partition)
// plus a pacing clock bound to the bus's scheduler. The bus handle must expose:
//   core::Scheduler& scheduler();
//   core::INetwork&  node(uint64_t id);   // stable per-node handle on this bus
//   (the driver registers the nodes before calling, or the handle auto-registers)
// A prod driver in S2–S4 constructs its OWN honest bus the same way and calls
// this identical function. Templated on the bus type so sim and prod both fit.
// ===========================================================================
template <class BusHandle>
void check_network_contract_on(BusHandle& bus, core::IClock* clock_for_pacing,
                               Report& rep) {
    core::Scheduler& sched = bus.scheduler();

    // --- local() stable ----------------------------------------------------
    {
        core::INetwork& n0 = bus.node(0);
        const core::Endpoint a = n0.local();
        const core::Endpoint b = n0.local();
        rep.add("network/local-stable", a == b && a.id == 0,
                std::string("local() unstable or wrong id (") +
                    std::to_string(a.id) + ")");
    }

    // --- send accepted + recv delivers + per-link ORDER --------------------
    struct State {
        bool sender_done = false;
        bool recv_done = false;
        std::vector<core::Error> send_errs{};
        std::vector<std::string> got_texts{};
        std::vector<std::uint64_t> got_from{};
    } st;

    constexpr int kMsgs = 8;

    // Sender: node 0 -> node 1, kMsgs distinct payloads in send order, pacing one
    // tick between sends so the honest bus delivers them in that order.
    auto sender = [&](core::INetwork& net, core::Endpoint to, State& s) -> core::Task {
        for (int i = 0; i < kMsgs; ++i) {
            std::vector<std::byte> p = payload(4, static_cast<std::uint8_t>('A' + i));
            core::Error e = co_await net.send(to, view_of(p));
            s.send_errs.push_back(e);
            if (clock_for_pacing != nullptr) {
                co_await clock_for_pacing->delay(2);
            }
        }
        s.sender_done = true;
        co_return;
    };

    // Receiver: drain exactly kMsgs, recording `from` and the decoded payload.
    auto receiver = [&](core::INetwork& net, State& s) -> core::Task {
        for (int i = 0; i < kMsgs; ++i) {
            core::Message m = co_await net.recv();
            std::string text;
            for (std::byte byte : m.payload) {
                text.push_back(static_cast<char>(byte));
            }
            s.got_texts.push_back(text);
            s.got_from.push_back(m.from.id);
        }
        s.recv_done = true;
        co_return;
    };

    sched.spawn(receiver(bus.node(1), st));
    sched.spawn(sender(bus.node(0), core::Endpoint{1}, st));
    sched.run();

    bool all_accepted = st.send_errs.size() == static_cast<std::size_t>(kMsgs);
    for (const core::Error& e : st.send_errs) {
        if (e) {
            all_accepted = false;
        }
    }
    rep.add("network/send-accepted-on-live-link", st.sender_done && all_accepted,
            "a send on a live link was not accepted");

    rep.add("network/recv-delivers-message",
            st.recv_done && st.got_texts.size() == static_cast<std::size_t>(kMsgs),
            std::string("receiver got ") + std::to_string(st.got_texts.size()) +
                " of " + std::to_string(kMsgs));

    // `from` is always the sender (node 0) and payloads are the sent bytes.
    bool from_ok = true;
    bool payload_ok = st.got_texts.size() == static_cast<std::size_t>(kMsgs);
    for (std::size_t i = 0; i < st.got_from.size(); ++i) {
        if (st.got_from[i] != 0) {
            from_ok = false;
        }
    }
    for (std::size_t i = 0; i < st.got_texts.size(); ++i) {
        std::string want(4, static_cast<char>('A' + static_cast<int>(i)));
        // payload(4, 'A'+i) is 'A'+i, then +1,+2,+3 — reconstruct expected.
        want[0] = static_cast<char>('A' + static_cast<int>(i));
        want[1] = static_cast<char>('A' + static_cast<int>(i) + 1);
        want[2] = static_cast<char>('A' + static_cast<int>(i) + 2);
        want[3] = static_cast<char>('A' + static_cast<int>(i) + 3);
        if (st.got_texts[i] != want) {
            payload_ok = false;
        }
    }
    rep.add("network/recv-from-and-payload-correct", from_ok && payload_ok,
            "delivered `from` or payload bytes did not match the send");

    // Per-link order: the first byte of each payload is 'A','B','C',... in send
    // order; on an honest link the delivered sequence is exactly ascending.
    bool ordered = st.got_texts.size() == static_cast<std::size_t>(kMsgs);
    for (std::size_t i = 0; i < st.got_texts.size(); ++i) {
        if (st.got_texts[i].empty() ||
            st.got_texts[i][0] != static_cast<char>('A' + static_cast<int>(i))) {
            ordered = false;
        }
    }
    rep.add("network/per-link-delivery-order", ordered,
            "honest per-link delivery was not in send order");
}

} // namespace universal
} // namespace conformance
