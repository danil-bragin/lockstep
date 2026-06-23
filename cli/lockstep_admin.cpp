// lockstep_admin.cpp — Phase 7 S5b-2. The CLUSTER ADMIN CLIENT: a thin CLI that talks
// the lockstepd admin protocol over REAL TCP. It dials a node's admin port, sends ONE
// request, awaits ONE reply, prints the decoded result, and exits — BOUNDED by an
// absolute reactor deadline so a dead/unreachable node can NEVER hang the client.
//
// Two verbs:
//   status --host PORT [--host PORT ...]
//       Query each given admin port's role/term/commit_index/committed-log digest and
//       print one machine-parseable line per node:
//         STATUS port=<P> ok=<0|1> role=<0|1|2> term=<T> commit=<C> log=<v1,v2,...>
//       (role 0=Follower 1=Candidate 2=Leader). A node that does not reply within the
//       deadline prints ok=0 (the orchestrator treats it as down / not-yet-up).
//
//   submit VALUE --host PORT [--host PORT ...]
//       SUBMIT VALUE to the nodes in order; on NotLeader, retry the next host until one
//       ACCEPTS (the LEADER-FIND client). Prints:
//         SUBMIT value=<V> accepted=<0|1> port=<P> term=<T> index=<I> leader_hint=<H>
//       accepted=1 means the leader appended it at <index>; accepted=0 means no host in
//       the list accepted within the per-host deadline (caller retries later).
//
// The client speaks the admin wire via the PROVIDER surface (ProdNetwork over the
// ProdReactor), so THIS TU does no raw socket/epoll syscall — exactly like lockstepd.
// It mints its OWN client endpoint (a ProdNetwork node on an ephemeral port) and
// records each target admin port as a PEER it dials.
//
// LINUX-ONLY (epoll/sockets): built only on Linux (if(UNIX AND NOT APPLE)).
// NOT in the providers/ lint-exempt zone: forbidden-lint scans it (no raw syscall — only
// the provider + admin-protocol surfaces). Every coroutine is a FREE FUNCTION over
// stable pointers (no [&] capture Task — avoids stack-use-after-scope).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/core/INetwork.hpp>
#include <lockstep/core/Task.hpp>

#include <lockstep/prod/ProdConsensusNode.hpp>  // admin protocol encode/decode
#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>

namespace {

namespace core = lockstep::core;
namespace prod = lockstep::prod;

// A distinct client endpoint id (well above any node/admin id so it never collides).
constexpr std::uint64_t kClientId = 9'000'000'001ULL;

// Per-request wall guard: how long the client waits for ONE admin round-trip before
// giving up (a down / not-yet-up node yields no reply). Bounded — never an unbounded
// spin. 1.5 s absorbs cross-process connect + reply latency while killing any hang.
constexpr core::Tick kReqWallNs = 1'500'000'000;

std::uint16_t parse_port(const char* s) {
    if (s == nullptr) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == nullptr || *end != '\0' || v == 0 || v > 65535) {
        return 0;
    }
    return static_cast<std::uint16_t>(v);
}

// ---- one STATUS round-trip (free function over stable pointers) ------------
core::Task status_rpc(core::INetwork* cli, core::Endpoint admin, prod::AdminStatus* out,
                      bool* ok, bool* done) {
    const std::vector<std::byte> req = prod::encode_status();
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    *ok = prod::decode_status(rep.payload, *out);
    *done = true;
    co_return;
}

// ---- one SUBMIT round-trip (free function over stable pointers) ------------
struct SubmitOutcome {
    bool replied = false;
    bool accepted = false;
    std::uint64_t term = 0;
    std::uint64_t index = 0;
    std::uint64_t leader_hint = 0;
};

core::Task submit_rpc(core::INetwork* cli, core::Endpoint admin, std::string value,
                      SubmitOutcome* out, bool* done) {
    const std::vector<std::byte> req = prod::encode_submit(value);
    co_await cli->send(admin, {req.data(), req.size()});
    core::Message rep = co_await cli->recv();
    prod::admin_detail::Reader r{rep.payload.data(), rep.payload.size(), 0, true};
    const auto kind = static_cast<prod::AdminKind>(r.u8());
    out->replied = true;
    if (kind == prod::AdminKind::SubmitOk) {
        out->accepted = true;
        out->term = r.u64();
        out->index = r.u64();
    } else if (kind == prod::AdminKind::NotLeader) {
        out->leader_hint = r.u64();
    }
    *done = true;
    co_return;
}

// A fresh client endpoint dialing one admin port. The admin endpoint id used for
// dialing is synthetic (admin ports map to distinct synthetic peer ids in the client's
// bus); each call recreates the reactor/bus so a prior connection never lingers.
struct Client {
    prod::ProdReactor reactor;
    prod::ProdNetworkBus bus{reactor};
    bool ok = false;
    std::uint64_t admin_peer_id = 0;

    explicit Client(std::uint16_t admin_port) {
        if (!reactor.valid()) {
            return;
        }
        // Synthetic peer id for the target admin endpoint (unique per port).
        admin_peer_id = 8'000'000'000ULL + admin_port;
        if (!bus.add_node(kClientId)) {  // our own ephemeral client listen socket
            return;
        }
        bus.add_peer(admin_peer_id, admin_port);  // record where the daemon's admin lives
        ok = true;
    }

    [[nodiscard]] core::INetwork* net() { return bus.node(kClientId); }
    [[nodiscard]] core::Endpoint admin_ep() const { return core::Endpoint{admin_peer_id}; }
};

// Run STATUS against one admin port; fill `out`. Returns true if a reply decoded.
bool do_status(std::uint16_t port, prod::AdminStatus& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool ok = false;
    bool done = false;
    c.reactor.spawn(status_rpc(c.net(), c.admin_ep(), &out, &ok, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && ok;
}

// Run SUBMIT against one admin port; fill `out`. Returns true if a reply was decoded.
bool do_submit(std::uint16_t port, const std::string& value, SubmitOutcome& out) {
    Client c(port);
    if (!c.ok) {
        return false;
    }
    bool done = false;
    c.reactor.spawn(submit_rpc(c.net(), c.admin_ep(), value, &out, &done));
    c.reactor.run_until([&done] { return done; }, c.reactor.now() + kReqWallNs);
    return done && out.replied;
}

void print_status(std::uint16_t port, bool ok, const prod::AdminStatus& st) {
    std::printf("STATUS port=%u ok=%d role=%u term=%llu commit=%llu log=", port,
                ok ? 1 : 0, static_cast<unsigned>(st.role),
                static_cast<unsigned long long>(st.term),
                static_cast<unsigned long long>(st.commit_index));
    for (std::size_t i = 0; i < st.committed.size(); ++i) {
        std::printf("%s%s", i == 0 ? "" : ",", st.committed[i].c_str());
    }
    std::printf("\n");
    std::fflush(stdout);
}

int cmd_status(const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        prod::AdminStatus st;
        const bool ok = do_status(port, st);
        print_status(port, ok, st);
    }
    return 0;
}

// LEADER-FIND submit: try each host in order; the LEADER accepts (accepted=1), a
// follower replies NotLeader (we try the next host). Print the outcome.
int cmd_submit(const std::string& value, const std::vector<std::uint16_t>& hosts) {
    for (std::uint16_t port : hosts) {
        SubmitOutcome so;
        const bool replied = do_submit(port, value, so);
        if (replied && so.accepted) {
            std::printf("SUBMIT value=%s accepted=1 port=%u term=%llu index=%llu "
                        "leader_hint=0\n",
                        value.c_str(), port, static_cast<unsigned long long>(so.term),
                        static_cast<unsigned long long>(so.index));
            std::fflush(stdout);
            return 0;
        }
        // NotLeader or no reply: try the next host (the leader-find retry loop).
    }
    std::printf("SUBMIT value=%s accepted=0 port=0 term=0 index=0 leader_hint=0\n",
                value.c_str());
    std::fflush(stdout);
    return 1;  // no host accepted (caller retries later)
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: lockstep_admin status --host PORT [--host PORT ...]\n"
                     "       lockstep_admin submit VALUE --host PORT [--host PORT ...]\n");
        return 2;
    }

    const std::string verb = argv[1];
    std::vector<std::uint16_t> hosts;
    std::string value;
    int i = 2;

    if (verb == "submit") {
        if (argc < 3) {
            std::fprintf(stderr, "lockstep_admin submit: missing VALUE\n");
            return 2;
        }
        value = argv[2];
        i = 3;
    }

    for (; i + 1 < argc + 1 && i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            const std::uint16_t p = parse_port(argv[i + 1]);
            if (p != 0) {
                hosts.push_back(p);
            }
            ++i;
        }
    }

    if (hosts.empty()) {
        std::fprintf(stderr, "lockstep_admin: no --host PORT given\n");
        return 2;
    }

    if (verb == "status") {
        return cmd_status(hosts);
    }
    if (verb == "submit") {
        return cmd_submit(value, hosts);
    }
    std::fprintf(stderr, "lockstep_admin: unknown verb '%s'\n", verb.c_str());
    return 2;
}
