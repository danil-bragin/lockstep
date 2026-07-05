// sql_cancel_test.cpp — W3.2 cooperative cancellation seam (engine side).
//
// A SqlEngine polls an externally-owned std::atomic<bool> cancel flag during execution
// (statement entry, before each intermediate materialization, at coarse row-loop
// boundaries). When it reads true, the current statement aborts with "query canceled".
// Null flag (the default) = no cancellation, zero overhead, deterministic in sim.
//
// This gate is the engine-side foundation; the prod side (a statement-timeout deadline
// timer and a PG CancelRequest handler flipping the flag from the reactor thread, aborting
// a genuinely long-running query mid-scan) is verified by the Linux/Docker prod test.
//
// Non-provider TU → forbidden-call lint scans it; no time/threads/random of its own.

#include <atomic>
#include <cstdio>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

namespace {

using lockstep::query::sql::ExecResult;
using lockstep::query::sql::SqlEngine;

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        g_fail = 1;
    }
}

bool is_canceled(const ExecResult& r) {
    return !r.ok && r.error.find("query canceled") != std::string::npos;
}

}  // namespace

int main() {
    std::printf("=== sql_cancel_test (W3.2 cancellation seam) ===\n");

    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, v INT, PRIMARY KEY (id))");
    for (int i = 0; i < 50; ++i) e.exec("INSERT INTO t (id, v) VALUES (" + std::to_string(i) +
                                        ", " + std::to_string(i) + ")");

    std::atomic<bool> cancel{false};
    e.set_cancel_flag(&cancel);

    // (A) with the flag clear, queries run normally.
    check(e.exec("SELECT id, v FROM t").rows.size() == 50, "(A) flag clear: query runs");

    // (B) with the flag set, a statement aborts at entry with 'query canceled'.
    cancel.store(true);
    check(is_canceled(e.exec("SELECT id, v FROM t")), "(B) flag set: SELECT is canceled");
    check(is_canceled(e.exec("SELECT id FROM (SELECT id FROM t) s")),
          "(B) flag set: a derived-table query is canceled (materialize poll)");
    // DML is canceled too (entry poll) — and must NOT apply.
    check(is_canceled(e.exec("UPDATE t SET v = 999 WHERE id = 1")), "(B) flag set: UPDATE is canceled");

    // (C) clearing the flag restores normal execution; the canceled UPDATE never applied.
    cancel.store(false);
    check(e.exec("SELECT v FROM t WHERE id = 1").rows[0].cells[0].second.i == 1,
          "(C) flag clear: query runs again, and the canceled UPDATE never applied (v stayed 1)");

    // (D) detaching the flag (nullptr) disables cancellation even while the atomic is true.
    cancel.store(true);
    e.set_cancel_flag(nullptr);
    check(e.exec("SELECT id FROM t").rows.size() == 50, "(D) detached flag: no cancellation");

    if (g_fail != 0) {
        std::printf("sql_cancel_test: FAILURES\n");
        return 1;
    }
    std::printf("sql_cancel_test: ALL PASS\n");
    return 0;
}
