// lockstep_mcpd.cpp — K11.1: the agent-memory MCP server over stdio. Newline-delimited
// JSON-RPC 2.0 (the MCP stdio transport) on stdin/stdout, tools served by McpSession
// over a durable SqlEngine (ProdDisk WALs under --data-dir; a restart recovers the
// memory store byte-identically). Register in an MCP client as:
//   { "command": "lockstep_mcpd", "args": ["--data-dir", "/var/lib/lockstep-memory"] }
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/prod/ProdDisk.hpp>
#include <lockstep/query/mcp/McpServer.hpp>

using lockstep::query::sql::SqlEngine;
namespace core = lockstep::core;
namespace prod = lockstep::prod;

int main(int argc, char** argv) {
    std::string data_dir = ".";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0) data_dir = argv[i + 1];
    }
    core::Scheduler d_sched;
    core::Scheduler c_sched;
    prod::ProdDisk d_disk(d_sched, data_dir + "/mcp-data.wal");
    prod::ProdDisk c_disk(c_sched, data_dir + "/mcp-catalog.wal");
    SqlEngine engine(d_sched, d_disk, c_sched, c_disk);
    engine.recover(d_disk.logical_len(), c_disk.logical_len());
    engine.set_trace_enabled(false);  // prod posture

    lockstep::query::mcp::McpSession session(engine);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const std::string out = session.handle_line(line);
        if (!out.empty()) {
            std::fputs(out.c_str(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);  // MCP stdio: one response line per request, flushed
        }
    }
    return 0;
}
