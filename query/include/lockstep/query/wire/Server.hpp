#pragma once

// Server.hpp — Phase 6 Stage B, C6.3. THE PROTOCOL SERVER.
//
// Source of truth: briefs/phase6.md C6.3. A deterministic server that runs on the
// core::Scheduler, RECEIVES wire requests over a core::INetwork (SimNetwork in
// sim), DISPATCHES each to the Stage-F Database surface (Database.hpp), and SENDS
// the encoded response back to the requester. It REINVENTS no txn/query logic: a
// Submit is wrapped into a Database::submit (the verified deterministic executor)
// and a Query into Database::run (the typed D5 read path).
//
// ============================================================================
// EXACTLY-ONCE UNDER DUP / RETRY (the cardinal invariant).
// SimNetwork may DUPLICATE, REORDER, and DROP frames; a dropped reply makes the
// client RETRY the SAME Submit. So the server must apply each Submit's effect
// EXACTLY ONCE. It does this with a DEDUP TABLE keyed by the idempotent
// `submit_key`: the FIRST time a submit_key is seen, the txn is executed and its
// committed effect appended to the live history (advancing the query-visible
// tip); the response is MEMOIZED. Every later Submit with that key (a duplicate
// or a retry) returns the MEMOIZED response WITHOUT re-applying — so a re-
// delivered Submit produces NO duplicate txn effect (reuses the deterministic-
// transferId idea: the submit_key IS the transfer id). Pure fn of the request
// stream.
//
// ============================================================================
// DETERMINISM (query/ is NOT lint-exempt): NO wall-clock, NO threads, NO
// std::*_distribution, NO ambient randomness. The dedup table + the live history
// are ordered std::map / std::vector; the op catalogue maps a named SubmitOp to a
// FIXED deterministic body. The whole server is a pure function of (the ordered
// request stream it recv()s) — itself a pure fn of the sim seed.

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lockstep/core/Future.hpp>
#include <lockstep/core/IDisk.hpp>
#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Scheduler.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/query/Database.hpp>
#include <lockstep/query/Query.hpp>
#include <lockstep/query/wire/Protocol.hpp>

#include <lockstep/txn/Transaction.hpp>

namespace lockstep::query::wire {

using core::Endpoint;
using core::INetwork;
using core::Message;
using core::Task;

// ----------------------------------------------------------------------------
// Materialize a named SubmitOp + its params into a deterministic one-shot TxnFn
// over the Stage-F surface (Database.hpp). The wire ships ONLY a named op + params
// (never executable code) — so V-DET-USER is preserved: the body is one of a fixed
// catalogue of pure functions of its declared reads.
// ----------------------------------------------------------------------------
[[nodiscard]] inline TxnFn materialize(const Request& req) {
    TxnFn fn;
    fn.id = req.submit_key;  // the txn's stable client tag IS the idempotent key
    switch (req.op) {
        case SubmitOp::Put: {
            // Write params[0].key = params[0].value. No reads needed.
            const OpParam p = req.params.empty() ? OpParam{} : req.params[0];
            fn.body = [p](TxnContext& ctx) {
                ctx.write(p.key, p.value);
                ctx.result("put:" + p.key);
            };
            return fn;
        }
        case SubmitOp::Transfer: {
            // Move `amount` from a -> b. Reads a,b at strict; writes both.
            const OpParam pa = req.params.size() > 0 ? req.params[0] : OpParam{};
            const OpParam pb = req.params.size() > 1 ? req.params[1] : OpParam{};
            const std::int64_t amount = pa.amount;
            fn.declared = reads(declare::strict(pa.key), declare::strict(pb.key));
            const Key ka = pa.key;
            const Key kb = pb.key;
            fn.body = [ka, kb, amount](TxnContext& ctx) {
                const std::int64_t va = parse_balance(ctx.read(ka));
                const std::int64_t vb = parse_balance(ctx.read(kb));
                ctx.write(ka, encode_balance(va - amount));
                ctx.write(kb, encode_balance(vb + amount));
                ctx.result("xfer:" + ka + "->" + kb);
            };
            return fn;
        }
        case SubmitOp::Increment: {
            // Read-modify-write: key += delta (params[0].amount).
            const OpParam p = req.params.empty() ? OpParam{} : req.params[0];
            const std::int64_t delta = p.amount;
            const Key k = p.key;
            fn.declared = reads(declare::strict(p.key));
            fn.body = [k, delta](TxnContext& ctx) {
                const std::int64_t cur = parse_balance(ctx.read(k));
                ctx.write(k, encode_balance(cur + delta));
                ctx.result("inc:" + k);
            };
            return fn;
        }
    }
    return fn;
}

// ----------------------------------------------------------------------------
// THE SERVER. Owns a Database (the verified executor + the standalone read path)
// and the dedup table. Drives a recv-loop Task on the scheduler.
// ----------------------------------------------------------------------------
class Server {
public:
    // Network-driven server (the normal mode): serve() recv-loops on `net`. The
    // committed query state uses the Database's DEFAULT in-memory backing.
    explicit Server(INetwork& net) : net_(&net) {}

    // Network-driven server backed by a DURABLE IDisk (Phase 7 S5a closure): the
    // committed query state is a persistent WalEngine over `disk` driven on `dsched`
    // (the disk's own scheduler — distinct from the network scheduler), so a restart
    // over the SAME disk recovers it (call recover() after re-constructing).
    Server(INetwork& net, core::Scheduler& dsched, core::IDisk& disk)
        : net_(&net), db_(dsched, disk) {}

    // Dispatch-only server (no transport): for the round-trip oracle, which calls
    // dispatch() directly with no serve()/recv(). net_ stays null and is NEVER
    // dereferenced on this path.
    Server() : net_(nullptr) {}

    // Dispatch-only server backed by a durable IDisk (for an in-process recovery
    // test that drives dispatch() directly, no transport). `dsched` is the disk's
    // scheduler.
    Server(core::Scheduler& dsched, core::IDisk& disk) : net_(nullptr), db_(dsched, disk) {}

    // ---- AUTH/RBAC (Phase: auth-rbac) ----------------------------------------
    // The client-facing wire::Server is one of the two RBAC ENFORCEMENT POINTS (the prod
    // admin handler is the other). query/ is NOT lint-exempt and must stay OpenSSL- and
    // provider-FREE, so the identity + policy are injected as PORTABLE CALLBACKS by the
    // prod assembly (ProdServerNode), which binds them to ProdNetwork::last_principal()
    // (the mTLS cert CN) + a prod::AuthPolicy. With no hooks installed the server is OPEN
    // (every request allowed) — byte-identical to the pre-auth path, so the sim tests +
    // the LocalCluster harness keep working unchanged.
    //
    //   principal_fn : returns the authenticated principal of the CURRENT request (read
    //                  right after recv(); empty == anonymous/plaintext).
    //   authz_fn     : (principal, is_write) -> allowed? A Submit is a WRITE; a Query /
    //                  Ping is a READ. DEFAULT-DENY is the caller's (AuthPolicy's) job —
    //                  here we simply refuse to dispatch when authz_fn returns false.
    using PrincipalFn = std::function<std::string()>;
    using AuthzFn = std::function<bool(const std::string& principal, bool is_write)>;
    void set_auth_hooks(PrincipalFn principal_fn, AuthzFn authz_fn) {
        principal_fn_ = std::move(principal_fn);
        authz_fn_ = std::move(authz_fn);
    }
    [[nodiscard]] std::uint64_t auth_denied() const noexcept { return auth_denied_; }

    // The recv-loop. Receives `max_msgs` frames (a bounded budget so the sim
    // quiesces — NEVER an unbounded loop), decodes+dispatches each, and replies.
    // A torn / corrupt frame is REJECTED at decode and DROPPED (no reply, no
    // effect) — the client retries. Bounded by `max_msgs` (V: no unbounded loop).
    Task serve(int max_msgs) {
        for (int i = 0; i < max_msgs; ++i) {
            Message m = co_await net_->recv();
            // Copy the bytes out before any further await (V-RKV1: the Message
            // payload view is non-owning and valid only until the next recv).
            std::vector<std::byte> frame(m.payload.begin(), m.payload.end());
            const Endpoint from = m.from;

            Request req;
            if (!decode_request(std::span<const std::byte>(frame.data(), frame.size()),
                                req)) {
                // Torn / corrupt / truncated: drop it (no fabrication). The client
                // will time out and retry. Count it for diagnostics.
                ++rejected_;
                continue;
            }

            // AUTH/RBAC ENFORCEMENT — BEFORE dispatch (never execute on a permission
            // miss). The op class is read off the DECODED request (a malformed frame was
            // already rejected above, so the gate can never be bypassed by garbage). A
            // Submit is a WRITE; Query/Ping are READ. authz_fn is the policy; when no
            // hooks are installed the request is allowed (legacy open path).
            if (authz_fn_) {
                const bool is_write = (req.kind == MsgKind::Submit);
                const std::string principal = principal_fn_ ? principal_fn_() : std::string{};
                if (!authz_fn_(principal, is_write)) {
                    ++auth_denied_;
                    Response denied;
                    denied.kind = MsgKind::Error;
                    denied.req_id = req.req_id;
                    denied.error = "AUTH-DENIED: principal '" + principal +
                                   "' lacks permission";
                    std::vector<std::byte> dbytes = encode_response(denied);
                    (void)co_await net_->send(
                        from, std::span<const std::byte>(dbytes.data(), dbytes.size()));
                    continue;  // op NOT dispatched / NOT applied.
                }
            }

            const Response resp = dispatch(req);
            std::vector<std::byte> out = encode_response(resp);
            (void)co_await net_->send(from,
                                      std::span<const std::byte>(out.data(), out.size()));
        }
        co_return;
    }

    [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::uint64_t applied_submits() const noexcept { return applied_; }
    [[nodiscard]] Seq tip() const noexcept { return tip_; }

    // RECOVER the committed query state from the durable IDisk after a restart (the
    // server object was re-constructed over the SAME disk). Rebuilds the persistent
    // engine from the durable WAL prefix so a Query returns every committed value
    // WITHOUT replaying the consensus log. `durable_len` is the durable WAL byte
    // length on the disk image. The query-visible tip is restored from the engine.
    void recover(std::size_t durable_len) {
        db_.recover(durable_len);
        tip_ = db_.tip();
    }

    // Direct (no-wire) dispatch — used by the round-trip oracle to compute the
    // SAME effect the wire path would, against the SAME server state. Pure fn.
    [[nodiscard]] Response dispatch(const Request& req) {
        switch (req.kind) {
            case MsgKind::Ping: {
                Response r;
                r.kind = MsgKind::Pong;
                r.req_id = req.req_id;
                return r;
            }
            case MsgKind::Submit:
                return handle_submit(req);
            case MsgKind::Query:
                return handle_query(req);
            default: {
                Response r;
                r.kind = MsgKind::Error;
                r.req_id = req.req_id;
                r.error = "unexpected request kind";
                return r;
            }
        }
    }

private:
    // EXACTLY-ONCE: if submit_key already applied, return the MEMOIZED response
    // WITHOUT re-executing (a duplicate / retried Submit has NO extra effect). Else
    // APPEND the materialized TxnFn to the ordered committed-txn batch and re-run
    // the WHOLE batch through the verified executor (Database::submit). The
    // executor applies the batch sequentially in seqLog order, so this txn's reads
    // see EVERY prior committed write (correct read-your-prior-commits semantics,
    // judged by the strict-serializable oracle). The LAST commit is this txn's
    // result; its committed write-set is appended to the query-visible history.
    Response handle_submit(const Request& req) {
        const auto it = dedup_.find(req.submit_key);
        if (it != dedup_.end()) {
            Response cached = it->second;
            cached.req_id = req.req_id;  // answer THIS request id (a retry/dup)
            return cached;
        }

        // WRITE-ONLY FAST PATH (kills the O(n^2)). A Put writes its key UNCONDITIONALLY
        // (declares no reads), so its committed write-set is INDEPENDENT of prior state —
        // running it as a SINGLETON yields the identical effect as re-running it at the
        // tail of the whole batch. So submit just this txn (O(1)) instead of re-submitting
        // the GROWING batch_ (which re-executes ALL prior committed txns from an empty
        // engine every call ⇒ O(n) per submit ⇒ O(n^2) over a run). Read-modify-write ops
        // (Transfer/Increment) DO read prior state, so they keep the batch re-run (which
        // replays the seqLog so they observe every prior commit). Write-only txns are still
        // RECORDED in batch_ afterward, so a later read-modify-write's re-run sees them.
        const bool write_only = (req.op == SubmitOp::Put);
        TxnFn fn = materialize(req);
        SubmitResult rr;
        if (write_only) {
            std::vector<TxnFn> one;
            one.push_back(fn);
            rr = db_.submit(one);
        } else {
            batch_.push_back(fn);
            rr = db_.submit(batch_);
        }

        Response r;
        r.kind = MsgKind::SubmitOk;
        r.req_id = req.req_id;
        if (!rr.commits.empty()) {
            // The just-submitted txn is the LAST in the ordered batch (or the only one).
            const txn::CommitInfo& ci = rr.commits.back();
            r.status = static_cast<std::uint8_t>(ci.status);
            r.commit_version = ci.commit_version;
            r.result = ci.result;
            r.writes = ci.writes_committed;
            if (ci.status == txn::Status::Committed) {
                // Apply this txn's committed write-set to the DURABLE query store
                // EXACTLY ONCE (WAL'd + synced over the injected IDisk), advancing
                // the query-visible tip by one. Incremental durable apply (survives +
                // recovers on a restart). For the write-only path, the query-visible
                // commit version is the monotonic tip (the singleton executor's local
                // version is per-call, not a global sequence).
                tip_ = db_.apply_committed(ci.writes_committed);
                ++applied_;
                if (write_only) {
                    r.commit_version = tip_;
                    batch_.push_back(std::move(fn));  // record for later read-txn re-runs
                }
            } else if (!write_only) {
                // Aborted read-modify-write txn leaves no effect: drop it from the live
                // batch so a re-run does not keep re-attempting (deterministic, idempotent).
                batch_.pop_back();
            }
        }
        dedup_.emplace(req.submit_key, r);  // memoize (exactly-once on retry/dup)
        return r;
    }

    Response handle_query(const Request& req) {
        Response r;
        r.kind = MsgKind::QueryOk;
        r.req_id = req.req_id;
        // The durable query store is already live (committed write-sets were applied
        // incrementally as they committed) — no per-query rebuild needed.

        // Re-materialize the typed Query<L> from the wire level + steps and run it
        // at the call-site-visible D5 level. The Database::run template needs the
        // concrete tag type, so we switch on the wire level.
        QueryResult qr;
        switch (req.level) {
            case Level::StrictSerializable: {
                Query<Strict> q;
                add_steps(q, req.steps);
                qr = db_.run(q);
                break;
            }
            case Level::Snapshot: {
                Query<Snapshot> q{Snapshot{req.snapshot_version}};
                add_steps(q, req.steps);
                qr = db_.run(q);
                break;
            }
            case Level::BoundedStaleness: {
                Query<Bounded> q{Bounded{req.max_lag}};
                add_steps(q, req.steps);
                qr = db_.run(q, /*replica_lag=*/req.max_lag);
                break;
            }
            case Level::ReadYourWrites: {
                Query<RYW> q{RYW{req.session}};
                add_steps(q, req.steps);
                qr = db_.run(q);
                break;
            }
        }

        r.level = qr.level;
        r.served_version = qr.served_version;
        for (const PointResult& pr : qr.points) {
            PointWire pw;
            pw.key = pr.key;
            pw.present = pr.value.has_value();
            pw.value = pr.value.value_or(Value{});
            r.points.push_back(std::move(pw));
        }
        for (const RangeResult& rg : qr.ranges) {
            RangeWire rw;
            rw.lo = rg.lo;
            rw.hi = rg.hi;
            rw.hi_unbounded = rg.hi_unbounded;
            for (const auto& [k, v] : rg.rows) {
                rw.rows.emplace_back(k, v);
            }
            r.ranges.push_back(std::move(rw));
        }
        return r;
    }

    template <typename L>
    static void add_steps(Query<L>& q, const std::vector<Step>& steps) {
        for (const Step& s : steps) {
            if (s.kind == StepKind::Point) {
                q.get(s.key);
            } else if (s.hi_unbounded) {
                q.scan_from(s.key);
            } else {
                q.scan(s.key, s.hi);
            }
        }
    }

    INetwork* net_;
    Database db_;
    std::vector<TxnFn> batch_;                 // ordered committed txns (seqLog)
    std::map<std::uint64_t, Response> dedup_;  // submit_key -> memoized response
    Seq tip_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t applied_ = 0;

    // AUTH/RBAC hooks (portable; provider-injected). Null == OPEN (legacy allow-all).
    PrincipalFn principal_fn_{};
    AuthzFn authz_fn_{};
    std::uint64_t auth_denied_ = 0;  // count of requests refused by the RBAC gate
};

}  // namespace lockstep::query::wire
