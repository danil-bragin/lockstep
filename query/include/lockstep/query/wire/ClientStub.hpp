#pragma once

// ClientStub.hpp — Phase 6 Stage B, C6.3. THE CLIENT-SIDE PROTOCOL STUB.
//
// Source of truth: briefs/phase6.md C6.3. This is the thin, deterministic client
// the reference DRIVER (C6.4, a later agent) WRAPS. It ENCODES a wire Request,
// SENDS it over a core::INetwork to the server endpoint, and DECODES the matching
// wire Response — handling DUP / REORDER / DROP safely:
//
//   * idempotent ids: every call carries a monotonic `req_id`, and a Submit a
//     stable `submit_key` (so a retry of a dropped Submit is applied EXACTLY ONCE
//     server-side — the dedup key). A reply is matched by `req_id`; a duplicate /
//     late / out-of-order reply for an already-answered call is IGNORED.
//   * deterministic timeout + retry: a background REPLY PUMP recv()s frames and
//     drops each (decoded) Response into a per-req_id CELL. A call SENDS, then
//     POLLS its cell on a virtual-time clock grid up to a per-attempt deadline
//     (clock.delay() can NEVER be dropped → ALWAYS terminates, no livelock). On
//     the deadline it RE-SENDS the SAME bytes (same req_id + submit_key) up to
//     `max_attempts`. A drop thus surfaces as a deterministic retry; exhaustion as
//     a client-visible timeout (ok=false). A TORN reply fails to decode in the
//     pump and is dropped (never fabricated). Pure fn of (seed).
//
// FORBIDDEN (query/ is NOT lint-exempt): NO wall-clock, NO threads, NO
// std::*_distribution, NO ambient randomness. All time is the virtual SimClock;
// the retry schedule is a pure function of the deadline grid. Bytes are copied out
// of a recv() Message before the next await (V-RKV1: the payload view is non-
// owning). The cell map is an ordered std::map (deterministic iteration).

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/IClock.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/Protocol.hpp>

namespace lockstep::query::wire {

using core::Endpoint;
using core::IClock;
using core::INetwork;
using core::Message;
using core::Tick;

// The outcome of a client call: did a matching reply arrive (ok), how many send
// attempts it took, and the decoded Response (meaningful only when ok).
struct CallResult {
    bool ok = false;    // false == timed out after max_attempts (no reply)
    int attempts = 0;   // how many sends it took (>=1 when a send was made)
    Response response;  // the matched reply (kind echoes the request)
};

// Tunable, deterministic retry policy (all virtual ticks).
struct ClientConfig {
    Tick poll_grain = 1;         // clock grid the wait polls on (>=1)
    Tick attempt_deadline = 48;  // per-attempt wait budget before a re-send
    int max_attempts = 16;       // total sends before a client-visible timeout
};

// A per-request response cell: the pump fills it when a matching reply decodes.
struct ReplyCell {
    bool filled = false;
    Response response;
};

// ----------------------------------------------------------------------------
// THE CLIENT STUB. Holds its INetwork handle, the server endpoint, a clock for the
// deterministic timeout grid, the monotonic req_id / submit_key counters, and the
// req_id -> cell table the background pump fills. One stub per logical client.
//
// USAGE: spawn pump() once on the scheduler, then spawn ping()/submit()/query()
// calls. The pump runs a bounded recv budget so the sim quiesces.
// ----------------------------------------------------------------------------
class ClientStub {
public:
    ClientStub(INetwork& net, IClock& clock, Endpoint server, ClientConfig cfg = {})
        : net_(&net), clock_(&clock), server_(server), cfg_(cfg) {}

    // Allocate the NEXT idempotent submit key. The DRIVER ties one logical submit
    // to one key; a retry of that submit MUST reuse the same key — that is what
    // makes the server apply it exactly once (the deterministic-transferId idea).
    [[nodiscard]] std::uint64_t new_submit_key() noexcept { return ++submit_key_; }

    // ---- the background reply pump (spawn ONCE) -------------------------------
    // recv()s up to `budget` frames, decodes each, and routes a matched Response
    // into its cell. A TORN reply fails to decode and is dropped (never
    // fabricated). Bounded by `budget` (V: no unbounded loop) — size it >= the
    // total replies the client could receive (incl. duplicates) so it drains.
    core::Task pump(int budget) {
        for (int i = 0; i < budget; ++i) {
            Message m = co_await net_->recv();
            std::vector<std::byte> frame(m.payload.begin(), m.payload.end());
            Response resp;
            if (!decode_response(
                    std::span<const std::byte>(frame.data(), frame.size()), resp)) {
                ++rejected_;  // torn / corrupt reply: drop it, never fabricate
                continue;
            }
            auto it = cells_.find(resp.req_id);
            if (it == cells_.end()) {
                // A reply for a req_id we never issued / already cleared: ignore.
                continue;
            }
            if (!it->second->filled) {
                it->second->filled = true;
                it->second->response = std::move(resp);
            }
            // else: a DUPLICATE reply for an already-answered call — ignored
            // (exactly-once client-side: the second copy has no effect).
        }
        co_return;
    }

    // ---- the three client calls (each a bounded, deterministic Task) ----------

    core::Task ping(CallResult& out) {
        Request req;
        req.kind = MsgKind::Ping;
        co_await round_trip(req, out);
        co_return;
    }

    // Submit a named op with the GIVEN idempotent submit_key (reuse it across a
    // retry for exactly-once). Returns the SubmitOk (commit info) on success.
    core::Task submit(SubmitOp op, std::vector<OpParam> params,
                      std::uint64_t submit_key, CallResult& out) {
        Request req;
        req.kind = MsgKind::Submit;
        req.submit_key = submit_key;
        req.op = op;
        req.params = std::move(params);
        co_await round_trip(req, out);
        co_return;
    }

    // Run a typed query at a call-site-visible D5 level. The level + params + steps
    // ride from the typed Query<L> onto the wire.
    template <typename L>
    core::Task query(const Query<L>& q, CallResult& out) {
        Request req;
        req.kind = MsgKind::Query;
        req.level = L::level;
        fill_level_params(req, q);
        req.steps = q.steps();
        co_await round_trip(req, out);
        co_return;
    }

private:
    // Encode-send-poll-retry. Assigns the req_id, registers a cell, re-sends the
    // SAME bytes on each deadline (idempotent: same req_id + submit_key), and waits
    // by polling the cell on the clock grid. Cleans up the cell on return.
    core::Task round_trip(Request& req, CallResult& out) {
        req.req_id = ++req_id_;
        auto cell = std::make_shared<ReplyCell>();
        cells_[req.req_id] = cell;

        const std::vector<std::byte> frame = encode_request(req);
        out.ok = false;
        out.attempts = 0;

        for (int attempt = 0; attempt < cfg_.max_attempts && !cell->filled;
             ++attempt) {
            ++out.attempts;
            (void)co_await net_->send(
                server_, std::span<const std::byte>(frame.data(), frame.size()));

            Tick waited = 0;
            while (waited < cfg_.attempt_deadline && !cell->filled) {
                co_await clock_->delay(cfg_.poll_grain);
                waited += cfg_.poll_grain;
            }
            // Deadline → re-send the SAME request (idempotent). Loop exits early
            // the moment the pump fills the cell.
        }

        if (cell->filled) {
            out.ok = true;
            out.response = cell->response;
        }
        cells_.erase(req.req_id);  // a later duplicate reply now routes to nobody
        co_return;
    }

    template <typename L>
    static void fill_level_params(Request& req, const Query<L>& q) {
        if constexpr (std::is_same_v<L, Snapshot>) {
            req.snapshot_version = q.tag().version;
        } else if constexpr (std::is_same_v<L, Bounded>) {
            req.max_lag = q.tag().max_lag;
        } else if constexpr (std::is_same_v<L, RYW>) {
            req.session = q.tag().session;
        } else {
            (void)req;
            (void)q;
        }
    }

    INetwork* net_;
    IClock* clock_;
    Endpoint server_;
    ClientConfig cfg_;
    std::uint64_t req_id_ = 0;
    std::uint64_t submit_key_ = 0;
    std::map<std::uint64_t, std::shared_ptr<ReplyCell>> cells_;
    std::uint64_t rejected_ = 0;

public:
    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
};

}  // namespace lockstep::query::wire
