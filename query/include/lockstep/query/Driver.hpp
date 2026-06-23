#pragma once

// Driver.hpp — Phase 6 Stage B, C6.4. THE REFERENCE-LANGUAGE DRIVER (C++).
//
// Source of truth: briefs/phase6.md C6.4 ("one reference-language driver first,
// then fan out"). This is the clean, ergonomic HIGH-LEVEL CLIENT a developer
// actually programs against. It WRAPS wire::ClientStub (ClientStub.hpp) and HIDES
// the protocol plumbing the stub exposes — req-ids, the idempotent submit-key
// lifecycle, the timeout/retry loop, and the background reply pump — behind a
// handful of value-shaped, intention-revealing calls:
//
//   driver:                                wraps:
//   ------------------------------------   ------------------------------------
//   Connection conn(stub)                  one ClientStub (one logical client)
//   co_await conn.put(k, v)                Submit{Put}, one stable submit_key
//   co_await conn.transfer(a, b, amount)   Submit{Transfer}, one stable key
//   co_await conn.increment(k, delta)      Submit{Increment}, one stable key
//   co_await conn.get(k, Level)            Query<L>.get(k)
//   co_await conn.scan(lo, hi, Level)      Query<L>.scan(lo, hi)
//   co_await conn.ping()                   Ping
//
// EXACTLY-ONCE IS AUTOMATIC. Every logical submit allocates EXACTLY ONE submit_key
// (via stub.new_submit_key()) and reuses it across ALL of that call's transport
// retries — so a dropped reply that triggers a re-send is de-duplicated by the
// server (the submit_key IS the idempotent transfer id). The caller NEVER touches
// a submit_key: the driver owns the whole exactly-once contract. A relaxed-level
// get/scan is READ-ONLY (no submit_key, naturally idempotent) so a retry is free.
//
// PORTABILITY. The shape here is the binding "reference driver" contract a future
// driver in another language mirrors: open a connection, call a verb, get a typed
// Result or a typed Error. The C++ specifics (coroutines, the SimNetwork) are an
// implementation detail of THIS driver; the SURFACE — verbs in, results/errors out,
// exactly-once owned by the driver — is the portable part.
//
// FORBIDDEN (query/ is NOT lint-exempt): NO wall-clock, NO threads, NO
// std::*_distribution, NO ambient randomness. All time is the virtual SimClock the
// ClientStub already owns; the driver adds no entropy. Pure fn of (seed). No
// pointer parked across a co_await (V-RKV1): each call copies its result out of the
// CallResult into a plain value before returning.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Task.hpp>

#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/ClientStub.hpp>
#include <lockstep/query/wire/Protocol.hpp>

namespace lockstep::query {

// ----------------------------------------------------------------------------
// THE RESULT TYPES — value-shaped, no wire leakage. A developer programming the
// driver sees these, never a wire::Response. `ok` is the single success flag (a
// timed-out call after the stub's retry budget is ok=false). `attempts` surfaces
// how many sends it took (a diagnostic, e.g. to observe retries under faults).
// ----------------------------------------------------------------------------

// The outcome of a WRITE (put / transfer / increment): did the server commit it,
// what commit version, the result token, and the committed writes (ordered).
struct WriteOutcome {
    bool ok = false;            // false == call timed out (no reply within budget)
    bool committed = false;     // server status == Committed
    int attempts = 0;           // transport sends it took (>=1 when ok)
    Seq commit_version = kNoSeq;
    std::string result;                       // the body's observable token
    std::vector<std::pair<Key, Value>> writes;  // committed writes, key-ascending
};

// One point-read row in a ReadOutcome (the value is ∅ for an absent/tombstoned key).
struct ReadRow {
    Key key;
    std::optional<Value> value;  // ∅ == absent / tombstone
};

// One scan range in a ReadOutcome: the live (key, value) rows in [lo, hi),
// key-ascending.
struct ScanRange {
    Key lo;
    Key hi;
    bool hi_unbounded = false;
    std::vector<std::pair<Key, Value>> rows;
};

// The outcome of a READ (get / scan): the level it was served at, the committed
// prefix it read as-of, and the rows. A point get fills `rows`; a scan fills
// `ranges`.
struct ReadOutcome {
    bool ok = false;
    int attempts = 0;
    Level level = Level::StrictSerializable;
    Seq served_version = kNoSeq;   // the committed prefix the read served from
    std::vector<ReadRow> rows;     // point-read results
    std::vector<ScanRange> ranges; // scan results
};

// The outcome of a ping: did the server answer, and after how many sends.
struct PingOutcome {
    bool ok = false;
    int attempts = 0;
};

// ----------------------------------------------------------------------------
// THE CONNECTION — the single developer-facing driver object. It borrows ONE
// ClientStub (one logical client = one Connection). It owns NO transport state of
// its own: the stub owns the network handle, the clock, the req-id / submit-key
// counters, and the reply-cell table. The Connection is the ERGONOMIC verb layer.
//
// LIFECYCLE (mirrors the stub's): the caller spawns the stub's pump() ONCE on the
// scheduler, then spawns Connection calls as Tasks. Each call is a bounded,
// deterministic coroutine. `connect()` is the named constructor for symmetry with
// a future networked driver (open a connection to an endpoint); here it just binds
// the stub.
// ----------------------------------------------------------------------------
class Connection {
public:
    explicit Connection(wire::ClientStub& stub) noexcept : stub_(&stub) {}

    // Named constructor: "connect" to the server the stub already targets. Returns
    // a Connection bound to that stub. (The stub already holds net + endpoint; this
    // is the call-site-readable entry point the spec asks for — connect(net, ep)
    // is realized by constructing the stub, then connect(stub).)
    [[nodiscard]] static Connection connect(wire::ClientStub& stub) noexcept {
        return Connection{stub};
    }

    // ---- WRITES (each owns exactly ONE submit_key; exactly-once automatic) -----

    // Unconditional put: k = v. Returns the commit outcome.
    core::Task put(Key k, Value v, WriteOutcome& out) {
        wire::OpParam p;
        p.key = std::move(k);
        p.value = std::move(v);
        co_await do_submit(wire::SubmitOp::Put, {std::move(p)}, out);
        co_return;
    }

    // Move `amount` from `from` to `to` (the canonical idempotent money move). The
    // server reads both balances at strict and writes from-amount / to+amount. The
    // ONE submit_key makes a re-delivered transfer apply exactly once.
    core::Task transfer(Key from, Key to, std::int64_t amount, WriteOutcome& out) {
        wire::OpParam pa;
        pa.key = std::move(from);
        pa.amount = amount;
        wire::OpParam pb;
        pb.key = std::move(to);
        co_await do_submit(wire::SubmitOp::Transfer, {std::move(pa), std::move(pb)},
                           out);
        co_return;
    }

    // Read-modify-write: k += delta.
    core::Task increment(Key k, std::int64_t delta, WriteOutcome& out) {
        wire::OpParam p;
        p.key = std::move(k);
        p.amount = delta;
        co_await do_submit(wire::SubmitOp::Increment, {std::move(p)}, out);
        co_return;
    }

    // ---- READS (typed, call-site-visible D5 level; read-only, naturally idempotent)

    // Point get of `key` at a call-site-visible D5 level. The level rides on the
    // Query<L> TYPE (V-D5-SAFE) — the default overload is Strict (the strong
    // default). The relaxed overloads take the level's parameter explicitly.
    core::Task get(Key key, ReadOutcome& out) {  // Strict (default level)
        Query<Strict> q;
        q.get(std::move(key));
        co_await do_query(q, out);
        co_return;
    }
    core::Task get_snapshot(Key key, Seq version, ReadOutcome& out) {
        Query<Snapshot> q{Snapshot{version}};
        q.get(std::move(key));
        co_await do_query(q, out);
        co_return;
    }
    core::Task get_bounded(Key key, Seq max_lag, ReadOutcome& out) {
        Query<Bounded> q{Bounded{max_lag}};
        q.get(std::move(key));
        co_await do_query(q, out);
        co_return;
    }
    core::Task get_ryw(Key key, SessionId session, ReadOutcome& out) {
        Query<RYW> q{RYW{session}};
        q.get(std::move(key));
        co_await do_query(q, out);
        co_return;
    }

    // Half-open range scan [lo, hi) at a call-site-visible D5 level.
    core::Task scan(Key lo, Key hi, ReadOutcome& out) {  // Strict (default)
        Query<Strict> q;
        q.scan(std::move(lo), std::move(hi));
        co_await do_query(q, out);
        co_return;
    }
    core::Task scan_snapshot(Key lo, Key hi, Seq version, ReadOutcome& out) {
        Query<Snapshot> q{Snapshot{version}};
        q.scan(std::move(lo), std::move(hi));
        co_await do_query(q, out);
        co_return;
    }
    core::Task scan_bounded(Key lo, Key hi, Seq max_lag, ReadOutcome& out) {
        Query<Bounded> q{Bounded{max_lag}};
        q.scan(std::move(lo), std::move(hi));
        co_await do_query(q, out);
        co_return;
    }

    // Run an ALREADY-COMPOSED typed query (the escape hatch for a multi-step read:
    // a developer builds Query<L>().get(a).get(b).scan(lo,hi) and runs it whole).
    // The level rides on the type, so this stays V-D5-SAFE.
    template <typename L>
    core::Task run(const Query<L>& q, ReadOutcome& out) {
        co_await do_query(q, out);
        co_return;
    }

    // ---- LIVENESS --------------------------------------------------------------
    core::Task ping(PingOutcome& out) {
        wire::CallResult cr;
        co_await stub_->ping(cr);
        out.ok = cr.ok;
        out.attempts = cr.attempts;
        co_return;
    }

private:
    // The single place a WRITE goes out: allocate ONE submit_key, reuse it across
    // the stub's transport retries (exactly-once), then unpack the wire SubmitOk
    // into a value-shaped WriteOutcome. The caller NEVER sees a submit_key.
    core::Task do_submit(wire::SubmitOp op, std::vector<wire::OpParam> params,
                         WriteOutcome& out) {
        const std::uint64_t key = stub_->new_submit_key();  // ONE key per logical call
        wire::CallResult cr;
        co_await stub_->submit(op, std::move(params), key, cr);
        out.ok = cr.ok;
        out.attempts = cr.attempts;
        if (cr.ok) {
            const wire::Response& r = cr.response;
            out.committed =
                r.status == static_cast<std::uint8_t>(txn::Status::Committed);
            out.commit_version = r.commit_version;
            out.result = r.result;
            out.writes.clear();
            for (const auto& [k, v] : r.writes) {  // wire writes are ordered (map)
                out.writes.emplace_back(k, v);
            }
        }
        co_return;
    }

    // The single place a READ goes out: ship the typed Query<L> (level on the type),
    // then unpack the wire QueryOk into a value-shaped ReadOutcome.
    template <typename L>
    core::Task do_query(const Query<L>& q, ReadOutcome& out) {
        wire::CallResult cr;
        co_await stub_->query(q, cr);
        out.ok = cr.ok;
        out.attempts = cr.attempts;
        if (cr.ok) {
            const wire::Response& r = cr.response;
            out.level = r.level;
            out.served_version = r.served_version;
            out.rows.clear();
            for (const wire::PointWire& pw : r.points) {
                ReadRow row;
                row.key = pw.key;
                row.value = pw.present ? std::optional<Value>(pw.value) : std::nullopt;
                out.rows.push_back(std::move(row));
            }
            out.ranges.clear();
            for (const wire::RangeWire& rw : r.ranges) {
                ScanRange sr;
                sr.lo = rw.lo;
                sr.hi = rw.hi;
                sr.hi_unbounded = rw.hi_unbounded;
                for (const auto& [k, v] : rw.rows) {
                    sr.rows.emplace_back(k, v);
                }
                out.ranges.push_back(std::move(sr));
            }
        }
        co_return;
    }

    wire::ClientStub* stub_;  // borrowed; one stub == one logical client
};

}  // namespace lockstep::query
