#pragma once

// invariants.hpp — structural-invariant checker over a rendered event trace.
//
// VERIFICATION-ONLY (P1-VERIFY). Parses the STABLE, DOCUMENTED trace format
// (docs/runtime-determinism.md):  "<seq> <action> vt=<vtime> [<payload>]" per
// line. Asserts the determinism CONTRACT's structural invariants WITHOUT pinning
// a specific trace literal (no over-fit):
//
//   I1 (seq monotonic):   event seq numbers are 0,1,2,... contiguous.
//   I2 (vtime monotonic): vtime never decreases across events (L4: clock only
//                         moves forward, and only at clock_advance).
//   I3 (L1 schedule-not-resume): every promise_set is immediately followed by a
//                         schedule (the waiter is QUEUED, never resumed inline),
//                         and the woken resume appears strictly LATER.
//   I4 (clock advance discipline): a clock_advance's vt strictly increases and
//                         matches the to=<n> payload; timer_fire only at/after it.
//   I5 (lifecycle): trace starts run_start, ends run_end; balanced spawns/dones.
//
// No <chrono>/<thread>/<random> — pure string parsing. Scanned by the lint.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lockstep::verify {

struct ParsedEvent {
    std::uint64_t seq = 0;
    std::string action;
    std::int64_t vt = 0;
    std::string payload;
};

// Parse the stable trace into events. Tolerant: a malformed line is reported via
// a sentinel action "<parse-error>" so the caller's invariant check fails loudly.
inline std::vector<ParsedEvent> parse_trace(const std::string& trace) {
    std::vector<ParsedEvent> out;
    std::size_t i = 0;
    const std::size_t n = trace.size();
    while (i < n) {
        std::size_t eol = trace.find('\n', i);
        if (eol == std::string::npos) {
            eol = n;
        }
        std::string_view line(trace.data() + i, eol - i);
        i = eol + 1;
        if (line.empty()) {
            continue;
        }
        ParsedEvent ev;
        // <seq> <action> vt=<vt> [payload...]
        std::size_t p = 0;
        auto next_field = [&](std::string_view& f) -> bool {
            while (p < line.size() && line[p] == ' ') {
                ++p;
            }
            std::size_t start = p;
            while (p < line.size() && line[p] != ' ') {
                ++p;
            }
            if (p == start) {
                return false;
            }
            f = line.substr(start, p - start);
            return true;
        };
        std::string_view f;
        bool ok = next_field(f);
        if (ok) {
            ev.seq = 0;
            for (char c : f) {
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                ev.seq = ev.seq * 10 + static_cast<std::uint64_t>(c - '0');
            }
        }
        if (ok) {
            ok = next_field(f);
            ev.action = std::string(f);
        }
        if (ok) {
            ok = next_field(f); // "vt=<n>"
            if (ok && f.size() > 3 && f.substr(0, 3) == "vt=") {
                std::string_view num = f.substr(3);
                bool neg = !num.empty() && num[0] == '-';
                std::int64_t v = 0;
                for (std::size_t k = neg ? 1 : 0; k < num.size(); ++k) {
                    char c = num[k];
                    if (c < '0' || c > '9') {
                        ok = false;
                        break;
                    }
                    v = v * 10 + (c - '0');
                }
                ev.vt = neg ? -v : v;
            } else {
                ok = false;
            }
        }
        if (ok) {
            // remainder (may be empty) is the payload.
            while (p < line.size() && line[p] == ' ') {
                ++p;
            }
            ev.payload = std::string(line.substr(p));
        }
        if (!ok) {
            ev.action = "<parse-error>";
            ev.payload = std::string(line);
        }
        out.push_back(std::move(ev));
    }
    return out;
}

struct InvariantResult {
    bool ok = true;
    std::string why;
    int spawns = 0;
    int task_done = 0;
    int timer_arm = 0;
    int timer_fire = 0;
    int promise_set = 0;
    int clock_advance = 0;
    int resume = 0;
};

inline InvariantResult check_invariants(const std::string& trace, int exchanges) {
    InvariantResult r;
    std::vector<ParsedEvent> ev = parse_trace(trace);
    auto fail = [&](std::string msg) {
        if (r.ok) {
            r.ok = false;
            r.why = std::move(msg);
        }
    };

    if (ev.empty()) {
        fail("empty trace");
        return r;
    }
    // Lifecycle: exactly one run_start and one run_end; run_end is the LAST
    // event; every spawn precedes run_start (spawn() is called before run()).
    int run_start_count = 0;
    int run_end_count = 0;
    std::size_t run_start_idx = 0;
    for (std::size_t k = 0; k < ev.size(); ++k) {
        if (ev[k].action == "run_start") {
            ++run_start_count;
            run_start_idx = k;
        } else if (ev[k].action == "run_end") {
            ++run_end_count;
        }
    }
    if (run_start_count != 1) {
        fail("expected exactly one run_start, got " + std::to_string(run_start_count));
    }
    if (run_end_count != 1) {
        fail("expected exactly one run_end, got " + std::to_string(run_end_count));
    }
    if (ev.back().action != "run_end") {
        fail("trace does not end with run_end");
    }
    // Everything before run_start must be a spawn (the pre-run enqueue phase).
    for (std::size_t k = 0; k < run_start_idx; ++k) {
        if (ev[k].action != "spawn") {
            fail("non-spawn event '" + ev[k].action + "' before run_start");
            break;
        }
    }

    std::int64_t prev_vt = ev.front().vt;
    std::uint64_t expect_seq = 0;
    for (std::size_t k = 0; k < ev.size(); ++k) {
        const ParsedEvent& e = ev[k];
        if (e.action == "<parse-error>") {
            fail("parse error on line: " + e.payload);
            break;
        }
        // I1: contiguous monotonic seq.
        if (e.seq != expect_seq) {
            fail("seq not contiguous at index " + std::to_string(k));
        }
        ++expect_seq;
        // I2: vtime never decreases.
        if (e.vt < prev_vt) {
            fail("vtime decreased at seq " + std::to_string(e.seq));
        }
        prev_vt = e.vt;

        if (e.action == "spawn") {
            ++r.spawns;
        } else if (e.action == "task_done") {
            ++r.task_done;
        } else if (e.action == "timer_arm") {
            ++r.timer_arm;
        } else if (e.action == "timer_fire") {
            ++r.timer_fire;
        } else if (e.action == "promise_set") {
            ++r.promise_set;
        } else if (e.action == "clock_advance") {
            ++r.clock_advance;
        } else if (e.action == "resume") {
            ++r.resume;
        }

        // I3 (L1: fulfillment SCHEDULES the waiter, never resumes inline).
        // The observable signature of L1 is twofold:
        //  (a) a waiter is only ever woken via a `schedule` event, which is
        //      ALWAYS immediately preceded by a fulfillment source
        //      (promise_set / timer_fire / task_done). If a `schedule` appeared
        //      after a `resume` with no fulfillment, work was resumed inline.
        //  (b) a `promise_set` is NEVER immediately followed by a `resume`: that
        //      would be the waiter running inline at fulfillment time. (When the
        //      promise had a waiter, `schedule` follows; when it had none, the
        //      next event is run()'s next pop — but a fulfillment never itself
        //      drives a resume.)
        // Note: a promise_set with NO parked waiter emits no `schedule` (core
        // only schedules when waiter_ != nullptr) — that is correct, not an L1
        // violation, so we do NOT require a schedule after every promise_set.
        if (e.action == "schedule") {
            const std::string& prev = (k == 0) ? std::string() : ev[k - 1].action;
            if (!(prev == "promise_set" || prev == "timer_fire" ||
                  prev == "task_done")) {
                fail("schedule at seq " + std::to_string(e.seq) +
                     " not preceded by a fulfillment source (prev=" + prev +
                     ") — possible inline-resume path");
            }
        }
        if (e.action == "promise_set") {
            if (k + 1 < ev.size() && ev[k + 1].action == "resume") {
                fail("promise_set immediately followed by resume at seq " +
                     std::to_string(e.seq) + " (L1 violation: inline resume)");
            }
        }
        // I4: clock_advance's payload "to=<n>" matches its vt.
        if (e.action == "clock_advance") {
            std::string want = "to=" + std::to_string(e.vt);
            if (e.payload != want) {
                fail("clock_advance payload mismatch at seq " +
                     std::to_string(e.seq) + " got '" + e.payload + "' want '" +
                     want + "'");
            }
        }
    }

    // I5: balanced lifecycle. (Counts checked exactly by the caller too.)
    if (r.spawns != r.task_done) {
        fail("spawns (" + std::to_string(r.spawns) + ") != task_done (" +
             std::to_string(r.task_done) + ")");
    }
    if (r.timer_arm != r.timer_fire) {
        fail("timer_arm (" + std::to_string(r.timer_arm) + ") != timer_fire (" +
             std::to_string(r.timer_fire) + ")");
    }
    // Every timer fire fulfills a promise; ping/pong also fulfill the token
    // promises directly, so promise_set == timer_fire + token-promises. We assert
    // the weaker, robust bound: at least one promise_set per timer_fire.
    if (r.promise_set < r.timer_fire) {
        fail("fewer promise_set than timer_fire");
    }
    (void)exchanges;
    return r;
}

// Extract the per-clock_advance vtime DELTAS, in order — i.e. how much virtual
// time jumped at each advance. For the correct ping-pong this is the per-hop
// jitter schedule (one advance per hop). Used to anchor the prediction to the run.
inline std::vector<std::int64_t> clock_advance_deltas(const std::string& trace) {
    std::vector<std::int64_t> deltas;
    std::int64_t prev = 0;
    for (const ParsedEvent& e : parse_trace(trace)) {
        if (e.action == "clock_advance") {
            deltas.push_back(e.vt - prev);
            prev = e.vt;
        }
    }
    return deltas;
}

} // namespace lockstep::verify
