#pragma once

// Trace.hpp — the event-trace recorder. The scheduler records a replayable
// sequence of (seq, action, vtime, payload) events as the run proceeds. Same
// (seed, initial tasks) ⇒ byte-identical trace (L5); the self-test diffs two
// runs byte-for-byte.
//
// STABLE FORMAT (documented in docs/runtime-determinism.md — do not reorder
// columns or rename actions without a spec change; the verifier diffs this):
//   one event per line, fields separated by a single space:
//     <seq> <action> vt=<vtime> <payload>
//   where
//     seq     = global monotonic event counter (decimal, starts at 0)
//     action  = a fixed lowercase token from the Action enum below
//     vtime   = scheduler virtual time at the moment the event was recorded
//     payload = action-specific, space-free key=value tokens (may be empty)
//
// The format is line-oriented ASCII on purpose: a textual trace diffs cleanly,
// is grep-able, and is identical across stdlib implementations (no floats, no
// pointers, no locale). Numbers are plain decimal. Nothing here touches
// wall-clock or randomness.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <lockstep/core/IClock.hpp> // Tick

namespace lockstep::core {

// The fixed, stable set of trace actions. Append-only: never renumber or rename
// an existing action (that would change the byte stream for old inputs). Tokens
// are the lowercase strings emitted into the trace.
enum class TraceAction : std::uint8_t {
    Spawn,        // a Task was spawned onto the ready queue        -> "spawn"
    Resume,       // a ready continuation was resumed by run()      -> "resume"
    PromiseSet,   // a Promise was fulfilled (value or error)       -> "promise_set"
    Schedule,     // a waiter was scheduled onto the ready queue    -> "schedule"
    TimerArm,     // delay() armed a timer                          -> "timer_arm"
    TimerFire,    // a timer fired and its waiter was scheduled     -> "timer_fire"
    ClockAdvance, // virtual time jumped to the earliest timer      -> "clock_advance"
    TaskDone,     // a Task ran to completion (final_suspend)        -> "task_done"
    RunStart,     // run() entered                                  -> "run_start"
    RunEnd,       // run() returned (no work, no timers)            -> "run_end"
};

// Returns the stable lowercase token for an action. Used both to emit the trace
// and (potentially) to parse it back. switch with no default so adding an action
// without a token is a compile warning under -Wall (caught by the gate).
[[nodiscard]] constexpr std::string_view to_token(TraceAction a) noexcept {
    switch (a) {
    case TraceAction::Spawn:        return "spawn";
    case TraceAction::Resume:       return "resume";
    case TraceAction::PromiseSet:   return "promise_set";
    case TraceAction::Schedule:     return "schedule";
    case TraceAction::TimerArm:     return "timer_arm";
    case TraceAction::TimerFire:    return "timer_fire";
    case TraceAction::ClockAdvance: return "clock_advance";
    case TraceAction::TaskDone:     return "task_done";
    case TraceAction::RunStart:     return "run_start";
    case TraceAction::RunEnd:       return "run_end";
    }
    return "?"; // unreachable; keeps the function total for constexpr use
}

// One recorded event. POD-ish, no pointers in the serialized form: `payload`
// carries only deterministic, space-free key=value tokens (e.g. "id=3 seq=7").
struct TraceEvent {
    std::uint64_t seq = 0;        // global event sequence (0-based)
    TraceAction action{};         // what happened
    Tick vtime = 0;               // virtual time at record moment
    std::string payload{};        // action-specific, space-free tokens
};

// In-memory, append-only trace buffer. Deterministic: events are appended in the
// exact order the scheduler records them; the buffer never reorders. `render()`
// produces the canonical byte stream used for byte-identical comparison.
class Trace {
public:
    // Append an event. Assigns the next global sequence number. Returns the
    // assigned seq so callers can cross-reference (e.g. a spawn seq in a payload).
    std::uint64_t record(TraceAction action, Tick vtime, std::string payload = {}) {
        std::uint64_t s = next_seq_++;
        events_.push_back(TraceEvent{s, action, vtime, std::move(payload)});
        return s;
    }

    [[nodiscard]] const std::vector<TraceEvent>& events() const noexcept { return events_; }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

    // Render the whole trace to the canonical stable byte stream (see header
    // comment). One line per event, terminated by '\n'. No trailing locale or
    // platform dependence. This is what the self-test diffs.
    [[nodiscard]] std::string render() const {
        std::string out;
        out.reserve(events_.size() * 32);
        for (const TraceEvent& e : events_) {
            out += std::to_string(e.seq);
            out += ' ';
            out += to_token(e.action);
            out += " vt=";
            out += std::to_string(e.vtime);
            if (!e.payload.empty()) {
                out += ' ';
                out += e.payload;
            }
            out += '\n';
        }
        return out;
    }

private:
    std::vector<TraceEvent> events_{};
    std::uint64_t next_seq_ = 0;
};

} // namespace lockstep::core
