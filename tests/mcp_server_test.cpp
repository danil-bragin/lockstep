// mcp_server_test.cpp — K11.1: the agent-memory MCP server gate. Drives newline-
// delimited JSON-RPC through McpSession over a real SqlEngine and checks:
// handshake, tool listing, remember -> hybrid recall (RRF over IVFFLAT + BM25 —
// ranking must equal the direct SQL the recipe documents), history AS OF SEQ
// (the agent's exact earlier world), and the protocol teeth (parse error,
// unknown method, unknown tool, SQL error as isError-not-protocol-error).
#include <cstdio>
#include <string>

#include <lockstep/query/mcp/McpServer.hpp>

using namespace lockstep::query;
using lockstep::query::sql::SqlEngine;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
std::string call(mcp::McpSession& s, int id, const std::string& tool, const std::string& args) {
    return s.handle_line(R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                         R"(,"method":"tools/call","params":{"name":")" + tool +
                         R"(","arguments":)" + args + "}}");
}
}  // namespace

int main() {
    std::printf("=== mcp_server_test (K11 agent memory) ===\n");
    SqlEngine e;
    mcp::McpSession s(e);

    // (1) Handshake + listing.
    const std::string init =
        s.handle_line(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    check(has(init, "protocolVersion") && has(init, "lockstep-agent-memory"), "initialize");
    check(s.handle_line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty(),
          "notification -> no response");
    const std::string tools = s.handle_line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    for (const char* t : {"\"query\"", "\"schema\"", "\"remember\"", "\"recall\"", "\"history\""})
        check(has(tools, t), std::string("tools/list has ") + t);

    // (2) remember with embeddings -> auto-provisioned hybrid store.
    check(!has(call(s, 3, "remember",
                    R"({"content":"raft leader election uses randomized timeouts",
                        "embedding":[1,0,0,0],"kind":"fact"})"),
               "isError\":true"),
          "remember #1");
    check(!has(call(s, 4, "remember",
                    R"({"content":"the deploy pipeline runs conformance gates",
                        "embedding":[0,1,0,0]})"),
               "isError\":true"),
          "remember #2");
    check(!has(call(s, 5, "remember",
                    R"({"content":"vector search beats pgvector on recall",
                        "embedding":[0.9,0.1,0,0]})"),
               "isError\":true"),
          "remember #3");

    // (3) hybrid recall == the documented SQL recipe, byte-for-byte ranking.
    const std::string rec = call(
        s, 6, "recall", R"({"query":"vector recall","embedding":[1,0,0,0],"k":2})");
    check(!has(rec, "isError\":true"), "hybrid recall ok");
    const auto direct = e.exec(
        "SELECT id, kind, content FROM agent_memory ORDER BY "
        "rrf_score(embedding, '[1,0,0,0]', content, 'vector recall') DESC LIMIT 2");
    check(direct.ok && direct.rows.size() == 2, "recipe SQL runs");
    // The MCP ranking must contain the recipe's top hit first.
    if (direct.ok && !direct.rows.empty()) {
        check(has(rec, direct.rows[0].cells[2].second.s.substr(0, 20)),
              "MCP recall top hit == recipe SQL top hit (deterministic ranking)");
    }

    // (4) history: the world as of an earlier step.
    const auto seq_res = e.exec("SELECT COUNT(*) FROM agent_memory");
    (void)seq_res;
    const std::string before = call(s, 7, "query",
                                    R"({"sql":"SELECT MAX(id) FROM agent_memory"})");
    check(!has(before, "isError\":true"), "query tool");
    // CHANGES works over the memory store (the CDC line, storage seqs).
    const auto tip_rows = e.exec("CHANGES agent_memory SINCE 0");
    check(tip_rows.ok && !tip_rows.rows.empty(), "CHANGES over the memory table works");
    // history counts in STATEMENT versions (each remember = one step): 3 remembers = 3.
    const std::int64_t tip = 3;
    check(!has(call(s, 8, "remember", R"({"content":"post-checkpoint memory",
                    "embedding":[0,0,1,0]})"),
               "isError\":true"),
          "remember #4 (after checkpoint)");
    const std::string hist = call(
        s, 9, "history",
        R"({"sql":"SELECT COUNT(*) FROM agent_memory WHERE id > 0","seq":)" + std::to_string(tip) + "}");
    check(has(hist, "\\\"3\\\""), "history AS OF the checkpoint sees exactly 3 memories");
    const std::string now = call(s, 10, "query",
                                 R"({"sql":"SELECT COUNT(*) FROM agent_memory"})");
    check(has(now, "\\\"4\\\""), "the present sees 4");

    // (5) schema tool sees the provisioned store.
    check(has(call(s, 11, "schema", "{}"), "agent_memory"), "schema lists agent_memory");

    // (6) Teeth.
    check(has(s.handle_line("{not json"), "-32700"), "parse error -> -32700");
    check(has(s.handle_line(R"({"jsonrpc":"2.0","id":12,"method":"nope"})"), "-32601"),
          "unknown method -> -32601");
    check(has(call(s, 13, "flywheel", "{}"), "unknown tool"), "unknown tool -> isError");
    check(has(call(s, 14, "query", R"({"sql":"SELEKT 1"})"), "isError\":true"),
          "SQL error is a tool error, not a protocol error");
    check(has(call(s, 15, "remember", R"({"content":"bad dim","embedding":[1,2]})"),
              "isError\":true"),
          "wrong embedding dim -> clean tool error");

    // (7) K11.4 per-agent isolation: two agents on ONE engine see disjoint memory.
    {
        SqlEngine iso;
        mcp::McpSession alice(iso, "alice");
        mcp::McpSession bob(iso, "bob");
        check(!has(call(alice, 20, "remember", R"({"content":"alice private fact"})"),
                   "isError\":true"),
              "alice remembers");
        check(!has(call(bob, 21, "remember", R"({"content":"bob private fact"})"),
                   "isError\":true"),
              "bob remembers");
        const std::string ar = call(alice, 22, "recall", R"({"query":"private fact"})");
        check(has(ar, "alice private") && !has(ar, "bob private"),
              "alice recalls only her memory");
        const std::string br = call(bob, 23, "recall", R"({"query":"private fact"})");
        check(has(br, "bob private") && !has(br, "alice private"),
              "bob recalls only his memory");
    }

    // (8) CURRENT_VERSION(): capture "now" on the SAME line AS OF reads — the audit loop.
    {
        SqlEngine v;
        mcp::McpSession vs(v, "");
        (void)call(vs, 30, "remember", R"({"content":"step one"})");
        const std::string now = call(vs, 31, "query", R"js({"sql":"SELECT CURRENT_VERSION()"})js");
        check(!has(now, "isError\":true"), "CURRENT_VERSION() runs");
        // Extract the number out of ...\"CURRENT_VERSION()\":\"N\"...
        const std::size_t at = now.rfind(":\\\"");
        std::string n;
        for (std::size_t i = at + 3; i < now.size() && isdigit(static_cast<unsigned char>(now[i])); ++i)
            n += now[i];
        check(!n.empty(), "version extracted");
        (void)call(vs, 32, "remember", R"({"content":"step two"})");
        const std::string audit = call(
            vs, 33, "history",
            R"({"sql":"SELECT COUNT(*) FROM agent_memory WHERE id > 0","seq":)" + n + "}");
        check(has(audit, "\\\"1\\\""), "audit at the captured version sees exactly step one");
    }

    if (g_fail != 0) { std::printf("mcp_server_test: FAILURES\n"); return 1; }
    std::printf("mcp_server_test: ALL PASS\n");
    return 0;
}
