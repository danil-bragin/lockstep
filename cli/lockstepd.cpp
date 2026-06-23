// lockstepd.cpp — Phase 7 S5a. THE SINGLE-NODE PROD SERVER DAEMON (precursor to the
// full lockstepd; multi-node Raft is S5b). A thin main(): parse a tiny config
// (listen node id, data dir, seed, optional run-duration), assemble a
// prod::ProdServerNode on the PROD providers (ProdReactor epoll loop + the reactor's
// ONE shared ProdClock + ProdRandom(seed) + ProdDisk(data dir) + ProdNetwork), spawn
// the wire::Server's serve coroutine, and run the reactor's BOUNDED loop. Keep it
// thin — config parse + assemble + run; ALL the real plumbing lives in
// providers/prod/ProdServerNode.hpp.
//
// LINUX-ONLY (epoll/sockets). cli/CMakeLists.txt guards the target with
// if(UNIX AND NOT APPLE); the macOS host never builds it and stays green.
//
// This TU is NOT in the providers/ lint-exempt zone: the forbidden-call lint scans
// it. It touches NO raw socket/epoll/clock syscall of its own — only the provider
// surfaces (ProdServerNode) + plain argv parsing. The run loop is BOUNDED by an
// absolute reactor deadline (a hard wall guard) OR an explicit run-seconds, so it
// can never serve forever in a constrained / CI context.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <lockstep/prod/ProdNetwork.hpp>
#include <lockstep/prod/ProdReactor.hpp>
#include <lockstep/prod/ProdServerNode.hpp>

namespace {

namespace prod = lockstep::prod;
namespace core = lockstep::core;

struct Args {
    std::uint64_t node_id = 1;
    std::uint64_t seed = 0;
    std::string data_dir = ".";
    // Bounded run: serve at most this many real seconds, then exit cleanly. 0 means
    // "until the reactor quiesces" (a single-node node with a live listen socket does
    // not quiesce, so a 0 here would block — we default to a small positive bound so
    // a no-arg run terminates; pass --run-seconds N to extend).
    std::uint64_t run_seconds = 2;
};

std::uint64_t parse_u64(const char* s, std::uint64_t fallback) {
    if (s == nullptr || s[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    return (end != nullptr && *end == '\0') ? static_cast<std::uint64_t>(v) : fallback;
}

// Parse a tiny --key value config. Unknown flags are ignored (forward-compatible).
Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char* k = argv[i];
        const char* v = argv[i + 1];
        if (std::strcmp(k, "--node-id") == 0) {
            a.node_id = parse_u64(v, a.node_id);
        } else if (std::strcmp(k, "--seed") == 0) {
            a.seed = parse_u64(v, a.seed);
        } else if (std::strcmp(k, "--data-dir") == 0) {
            a.data_dir = v;
        } else if (std::strcmp(k, "--run-seconds") == 0) {
            a.run_seconds = parse_u64(v, a.run_seconds);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // --- assemble on the prod providers (the ProdServerNode does the wiring) ---
    prod::ProdReactor reactor;
    if (!reactor.valid()) {
        std::fprintf(stderr, "lockstepd: failed to create epoll reactor\n");
        return 1;
    }
    prod::ProdNetworkBus bus(reactor);
    if (!bus.add_node(args.node_id)) {
        std::fprintf(stderr, "lockstepd: failed to bind listen socket for node %llu\n",
                     static_cast<unsigned long long>(args.node_id));
        return 1;
    }

    prod::ProdServerConfig cfg;
    cfg.node_id = args.node_id;
    cfg.seed = args.seed;
    cfg.data_dir = args.data_dir;
    prod::ProdServerNode node(reactor, bus, cfg);
    if (!node.valid()) {
        std::fprintf(stderr, "lockstepd: failed to assemble server node\n");
        return 1;
    }

    const core::Endpoint ep = node.endpoint();
    std::printf("lockstepd: node %llu listening on 127.0.0.1 (ephemeral port), "
                "data-dir=%s seed=%llu disk=%s\n",
                static_cast<unsigned long long>(ep.id), cfg.data_dir.c_str(),
                static_cast<unsigned long long>(cfg.seed),
                node.disk_valid() ? "ok" : "UNAVAILABLE");

    // --- spawn the server serve-loop + run the BOUNDED reactor loop -----------
    // A generous bounded recv budget (the single node serves many requests before
    // quiescing; NEVER an unbounded loop). The reactor run is BOUNDED by an absolute
    // now()-deadline (run_seconds), a hard wall guard so the daemon always terminates
    // in a constrained context.
    constexpr int kServeBudget = 1 << 20;
    node.start(kServeBudget);

    const core::Tick deadline_ns =
        reactor.now() + static_cast<core::Tick>(args.run_seconds) * 1'000'000'000;
    node.run_with_deadline(deadline_ns);

    std::printf("lockstepd: node %llu shutting down — applied=%llu rejected=%llu tip=%llu\n",
                static_cast<unsigned long long>(ep.id),
                static_cast<unsigned long long>(node.applied_submits()),
                static_cast<unsigned long long>(node.rejected()),
                static_cast<unsigned long long>(node.tip()));
    return 0;
}
