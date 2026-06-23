// lockstep_cli.cpp — Phase 6 Stage B, C6.5. THE LOCKSTEP CLI / ADMIN TOOL.
//
// Source of truth: briefs/phase6.md C6.5 ("a small command-driven admin/client
// tool ... drives the driver against an in-process sim server"). A deterministic,
// command-driven client: it parses a list of commands (from argv after a `--`
// separator, or one-per-line on stdin), drives the C6.4 reference Driver
// (Driver.hpp) against an in-process sim cluster (LocalCluster.hpp), and prints
// each command's result.
//
// COMMANDS (one logical driver call each):
//   put <k> <v>                          unconditional put k = v
//   transfer <a> <b> <amt>               move <amt> from a to b (exactly-once)
//   increment <k> <delta>                read-modify-write k += delta
//   get <k> [--level <L>] [--arg <n>]    point read at a D5 level
//   scan <lo> <hi> [--level <L>] [--arg <n>]   half-open range read [lo,hi)
//   ping                                 liveness probe
//   # ...                                a comment line (ignored)
//
//   <L> in {strict (default), snapshot, bounded, ryw}. --arg supplies the level's
//   parameter: snapshot version / bounded max_lag / ryw session (default 0). The
//   D5 level is CALL-SITE-VISIBLE on the wire (V-D5-SAFE): the CLI maps the name to
//   the typed Query<L> path in the Driver.
//
// GLOBAL FLAGS (before the commands):
//   --seed <n>     the sim seed (default 1). Determinism: same seed + same script
//                  => byte-identical output. NO wall-clock anywhere.
//   --faults       turn on a dup/reorder/drop net-fault profile (still exactly-
//                  once + deterministic; exercises the driver's retry path).
//   --trace        also print the deterministic scheduler event trace at the end.
//
// USAGE
//   lockstep_cli --seed 7 -- put acct:a 100 transfer acct:a acct:b 30 \
//                get acct:a get acct:b --level snapshot --arg 1
//   echo "put k v\nget k" | lockstep_cli --seed 7
//
// DETERMINISM (binding; this TU is NOT lint-exempt — forbidden-lint scans it): the
// ONLY entropy is --seed. NO std::chrono, NO std::thread, NO std::*_distribution,
// NO std::rand. All commands run in ONE LocalCluster.run() against ONE server
// state, so reads see prior writes. Output is a pure function of (seed, faults,
// script). printf is fine (the project's bug-focused-tidy style).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lockstep/core/Task.hpp>

#include <lockstep/query/Driver.hpp>
#include <lockstep/query/LocalCluster.hpp>
#include <lockstep/query/Query.hpp>

namespace {

namespace q = lockstep::query;

// ----------------------------------------------------------------------------
// A parsed command. We parse argv/stdin into a flat list FIRST (a pure data
// transform), then a single driver program replays them in order against one
// server. `level`/`arg` are meaningful only for get/scan.
// ----------------------------------------------------------------------------
enum class Verb { Put, Transfer, Increment, Get, Scan, Ping, Bad };

struct Command {
    Verb verb = Verb::Bad;
    std::string a;            // put/get/scan: key/lo ; transfer: from ; inc: key
    std::string b;            // put: value ; transfer: to ; scan: hi
    std::int64_t amount = 0;  // transfer amount / increment delta
    q::Level level = q::Level::StrictSerializable;
    q::Seq arg = 0;           // snapshot version / bounded max_lag / ryw session
    std::string error;        // set when verb == Bad (a parse diagnostic)
};

[[nodiscard]] std::int64_t parse_i64(const std::string& s, bool& ok) {
    ok = false;
    if (s.empty()) {
        return 0;
    }
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) {
            return 0;
        }
    }
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        v = v * 10 + (c - '0');
    }
    ok = true;
    return neg ? -v : v;
}

[[nodiscard]] bool parse_level(const std::string& s, q::Level& out) {
    if (s == "strict") {
        out = q::Level::StrictSerializable;
        return true;
    }
    if (s == "snapshot") {
        out = q::Level::Snapshot;
        return true;
    }
    if (s == "bounded") {
        out = q::Level::BoundedStaleness;
        return true;
    }
    if (s == "ryw") {
        out = q::Level::ReadYourWrites;
        return true;
    }
    return false;
}

// Pull optional `--level <L>` and `--arg <n>` flags out of a read command's tail.
// Advances `i` over any consumed tokens. Returns false on a malformed flag.
[[nodiscard]] bool parse_read_flags(const std::vector<std::string>& toks,
                                    std::size_t& i, Command& cmd) {
    while (i < toks.size()) {
        if (toks[i] == "--level") {
            if (i + 1 >= toks.size() || !parse_level(toks[i + 1], cmd.level)) {
                cmd.error = "bad --level (want strict|snapshot|bounded|ryw)";
                return false;
            }
            i += 2;
        } else if (toks[i] == "--arg") {
            bool ok = false;
            if (i + 1 >= toks.size()) {
                cmd.error = "missing --arg value";
                return false;
            }
            const std::int64_t v = parse_i64(toks[i + 1], ok);
            if (!ok || v < 0) {
                cmd.error = "bad --arg (want a non-negative integer)";
                return false;
            }
            cmd.arg = static_cast<q::Seq>(v);
            i += 2;
        } else {
            break;  // not a flag for us — the next command starts here
        }
    }
    return true;
}

// Parse the flat token stream into a command list. A command consumes a fixed
// arity (plus optional read flags). A malformed command becomes a Verb::Bad with
// an error (reported, not fatal — we keep going so the script is auditable).
[[nodiscard]] std::vector<Command> parse_commands(
    const std::vector<std::string>& toks) {
    std::vector<Command> cmds;
    std::size_t i = 0;
    while (i < toks.size()) {
        const std::string& t = toks[i];
        Command cmd;
        if (t == "put") {
            if (i + 2 >= toks.size()) {
                cmd.error = "put needs <key> <value>";
                cmds.push_back(cmd);
                break;
            }
            cmd.verb = Verb::Put;
            cmd.a = toks[i + 1];
            cmd.b = toks[i + 2];
            i += 3;
        } else if (t == "transfer") {
            bool ok = false;
            if (i + 3 >= toks.size()) {
                cmd.error = "transfer needs <from> <to> <amount>";
                cmds.push_back(cmd);
                break;
            }
            cmd.verb = Verb::Transfer;
            cmd.a = toks[i + 1];
            cmd.b = toks[i + 2];
            cmd.amount = parse_i64(toks[i + 3], ok);
            if (!ok) {
                cmd.verb = Verb::Bad;
                cmd.error = "transfer <amount> must be an integer";
            }
            i += 4;
        } else if (t == "increment" || t == "inc") {
            bool ok = false;
            if (i + 2 >= toks.size()) {
                cmd.error = "increment needs <key> <delta>";
                cmds.push_back(cmd);
                break;
            }
            cmd.verb = Verb::Increment;
            cmd.a = toks[i + 1];
            cmd.amount = parse_i64(toks[i + 2], ok);
            if (!ok) {
                cmd.verb = Verb::Bad;
                cmd.error = "increment <delta> must be an integer";
            }
            i += 3;
        } else if (t == "get") {
            if (i + 1 >= toks.size()) {
                cmd.error = "get needs <key>";
                cmds.push_back(cmd);
                break;
            }
            cmd.verb = Verb::Get;
            cmd.a = toks[i + 1];
            i += 2;
            if (!parse_read_flags(toks, i, cmd)) {
                cmd.verb = Verb::Bad;
            }
        } else if (t == "scan") {
            if (i + 2 >= toks.size()) {
                cmd.error = "scan needs <lo> <hi>";
                cmds.push_back(cmd);
                break;
            }
            cmd.verb = Verb::Scan;
            cmd.a = toks[i + 1];
            cmd.b = toks[i + 2];
            i += 3;
            if (!parse_read_flags(toks, i, cmd)) {
                cmd.verb = Verb::Bad;
            }
        } else if (t == "ping") {
            cmd.verb = Verb::Ping;
            i += 1;
        } else {
            cmd.error = "unknown command: " + t;
            i += 1;
        }
        cmds.push_back(cmd);
    }
    return cmds;
}

// Render an optional value for printing (absent == "<nil>"; values are printed
// raw — they are opaque bytes but the CLI workload uses printable ASCII).
[[nodiscard]] std::string show(const std::optional<std::string>& v) {
    return v.has_value() ? *v : std::string("<nil>");
}

[[nodiscard]] const char* level_label(q::Level l) {
    return lockstep::txn::level_name(l);
}

// ----------------------------------------------------------------------------
// THE DRIVER PROGRAM. Replays the parsed commands in order against ONE Connection
// (so reads see prior writes). Each command's result is printed deterministically.
// A relaxed-level read names its level + served version so the D5 contract is
// visible in the output.
// ----------------------------------------------------------------------------
lockstep::core::Task run_script(q::Connection& conn, const std::vector<Command>* cmds) {
    int idx = 0;
    for (const Command& c : *cmds) {
        ++idx;
        switch (c.verb) {
            case Verb::Put: {
                q::WriteOutcome w;
                co_await conn.put(c.a, c.b, w);
                std::printf("[%d] put %s=%s -> %s (v%llu)\n", idx, c.a.c_str(),
                            c.b.c_str(), w.committed ? "committed" : "FAILED",
                            static_cast<unsigned long long>(w.commit_version));
                break;
            }
            case Verb::Transfer: {
                q::WriteOutcome w;
                co_await conn.transfer(c.a, c.b, c.amount, w);
                std::printf("[%d] transfer %lld %s->%s -> %s (v%llu, %d attempt%s)\n",
                            idx, static_cast<long long>(c.amount), c.a.c_str(),
                            c.b.c_str(), w.committed ? "committed" : "FAILED",
                            static_cast<unsigned long long>(w.commit_version),
                            w.attempts, w.attempts == 1 ? "" : "s");
                break;
            }
            case Verb::Increment: {
                q::WriteOutcome w;
                co_await conn.increment(c.a, c.amount, w);
                std::printf("[%d] increment %s += %lld -> %s (v%llu)\n", idx,
                            c.a.c_str(), static_cast<long long>(c.amount),
                            w.committed ? "committed" : "FAILED",
                            static_cast<unsigned long long>(w.commit_version));
                break;
            }
            case Verb::Get: {
                q::ReadOutcome r;
                switch (c.level) {
                    case q::Level::StrictSerializable:
                        co_await conn.get(c.a, r);
                        break;
                    case q::Level::Snapshot:
                        co_await conn.get_snapshot(c.a, c.arg, r);
                        break;
                    case q::Level::BoundedStaleness:
                        co_await conn.get_bounded(c.a, c.arg, r);
                        break;
                    case q::Level::ReadYourWrites:
                        co_await conn.get_ryw(c.a, c.arg, r);
                        break;
                }
                const std::string val = r.rows.empty() ? std::string("<nil>")
                                                        : show(r.rows[0].value);
                std::printf("[%d] get %s [%s] = %s (served v%llu)\n", idx, c.a.c_str(),
                            level_label(r.level), val.c_str(),
                            static_cast<unsigned long long>(r.served_version));
                break;
            }
            case Verb::Scan: {
                q::ReadOutcome r;
                switch (c.level) {
                    case q::Level::Snapshot:
                        co_await conn.scan_snapshot(c.a, c.b, c.arg, r);
                        break;
                    case q::Level::BoundedStaleness:
                        co_await conn.scan_bounded(c.a, c.b, c.arg, r);
                        break;
                    case q::Level::StrictSerializable:
                    case q::Level::ReadYourWrites:
                        co_await conn.scan(c.a, c.b, r);
                        break;
                }
                std::printf("[%d] scan [%s,%s) [%s] (served v%llu):\n", idx,
                            c.a.c_str(), c.b.c_str(), level_label(r.level),
                            static_cast<unsigned long long>(r.served_version));
                if (!r.ranges.empty()) {
                    for (const auto& [k, v] : r.ranges[0].rows) {
                        std::printf("       %s = %s\n", k.c_str(), v.c_str());
                    }
                }
                break;
            }
            case Verb::Ping: {
                q::PingOutcome p;
                co_await conn.ping(p);
                std::printf("[%d] ping -> %s (%d attempt%s)\n", idx,
                            p.ok ? "pong" : "TIMEOUT", p.attempts,
                            p.attempts == 1 ? "" : "s");
                break;
            }
            case Verb::Bad:
                std::printf("[%d] bad command: %s\n", idx, c.error.c_str());
                break;
        }
    }
    co_return;
}

// Read whitespace-separated tokens from stdin, dropping `#`-prefixed comment
// lines. Deterministic: a pure transform of the byte stream.
[[nodiscard]] std::vector<std::string> tokens_from_stdin() {
    std::vector<std::string> toks;
    int ch = 0;
    std::string cur;
    bool comment = false;
    auto flush = [&]() {
        if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    };
    while ((ch = std::getchar()) != EOF) {
        const char c = static_cast<char>(ch);
        if (c == '\n') {
            flush();
            comment = false;
            continue;
        }
        if (comment) {
            continue;
        }
        if (c == '#') {
            flush();
            comment = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return toks;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1;
    bool faults = false;
    bool want_trace = false;

    // Parse global flags up to a `--` separator; everything after `--` is the
    // command script. If no `--` appears and no command tokens follow the flags,
    // read the script from stdin (so `echo ... | lockstep_cli` works).
    std::vector<std::string> script;
    int i = 1;
    bool saw_sep = false;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") {
            saw_sep = true;
            ++i;
            break;
        }
        if (a == "--seed" && i + 1 < argc) {
            seed = std::strtoull(argv[i + 1], nullptr, 10);
            ++i;
        } else if (a == "--faults") {
            faults = true;
        } else if (a == "--trace") {
            want_trace = true;
        } else {
            // A non-flag token before `--`: treat the rest as the inline script.
            break;
        }
    }
    for (; i < argc; ++i) {
        script.emplace_back(argv[i]);
    }

    // No inline script => read from stdin (the scripted-list mode).
    if (script.empty() && !saw_sep) {
        script = tokens_from_stdin();
    } else if (script.empty() && saw_sep) {
        script = tokens_from_stdin();
    }

    const std::vector<Command> cmds = parse_commands(script);

    q::LinkFaults lf{};
    lf.latency_min = 1;
    lf.latency_max = faults ? 8 : 1;
    if (faults) {
        lf.drop_prob = 0.25;
        lf.dup_prob = 0.30;
        lf.reorder_prob = 0.50;
        lf.reorder_jitter_max = 12;
    }

    std::printf("lockstep_cli seed=%llu faults=%s commands=%zu\n",
                static_cast<unsigned long long>(seed), faults ? "on" : "off",
                cmds.size());

    q::LocalCluster lc(seed, lf);
    lc.run([&cmds](q::Connection& conn) -> lockstep::core::Task {
        co_await run_script(conn, &cmds);
        co_return;
    });

    std::printf("done: applied=%llu rejected=%llu tip=v%llu\n",
                static_cast<unsigned long long>(lc.applied()),
                static_cast<unsigned long long>(lc.rejected()),
                static_cast<unsigned long long>(lc.tip()));

    if (want_trace) {
        std::printf("---- trace ----\n%s", lc.trace().c_str());
    }
    return 0;
}
