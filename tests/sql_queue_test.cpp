// sql_queue_test.cpp — K3: SQL queues with exactly-once semantics. CREATE QUEUE / SEND /
// RECEIVE [BATCH n] [VISIBILITY v] / ACK / DLQ over hidden row tables; SEND is
// transactional (outbox); VISIBILITY is in Seq units (deterministic logical time).
// Gate: every message is delivered-and-ACKed exactly once by competing consumers; an
// unACKed message redelivers after its visibility deadline; the 6th delivery attempt
// dead-letters it.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <lockstep/core/Scheduler.hpp>
#include <lockstep/sim/SeededRandom.hpp>
#include <lockstep/sim/SimDisk.hpp>
#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); g_fail = 1; }
}
// Advance the engine Seq by n commits (the logical clock VISIBILITY counts in).
void tick(SqlEngine& e, int n) {
    for (int i = 0; i < n; ++i) e.exec("INSERT INTO _tick (v) VALUES (0)");
}
}  // namespace

int main() {
    std::printf("=== sql_queue_test (K3 queues) ===\n");
    {
        SqlEngine e;
        e.exec("CREATE TABLE _tick (id INT AUTO_INCREMENT, v INT DEFAULT 0, PRIMARY KEY (id))");
        check(e.exec("CREATE QUEUE jobs").ok, "CREATE QUEUE");
        check(!e.exec("CREATE QUEUE jobs").ok, "duplicate CREATE QUEUE rejected");

        // FIFO delivery in send order.
        e.exec("SEND jobs, 'a'");
        e.exec("SEND jobs, 'b'");
        e.exec("SEND jobs, 'c'");
        ExecResult r = e.exec("RECEIVE jobs BATCH 2 VISIBILITY 5");
        check(r.ok && r.rows.size() == 2 && r.rows[0].cells[1].second.s == "a" &&
                  r.rows[1].cells[1].second.s == "b",
              "RECEIVE BATCH 2 -> a, b in FIFO order");
        const std::int64_t mid_a = r.rows[0].cells[0].second.i;
        // In-flight messages are invisible before their deadline.
        r = e.exec("RECEIVE jobs BATCH 10 VISIBILITY 5");
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[1].second.s == "c",
              "in-flight a,b invisible; only c delivered");
        // ACK a; let the visibility deadline pass -> only b redelivers (deliveries=2).
        check(e.exec("ACK jobs, " + std::to_string(mid_a)).ok, "ACK a");
        tick(e, 8);
        r = e.exec("RECEIVE jobs BATCH 10 VISIBILITY 5");
        check(r.ok && r.rows.size() == 2, "b and c redeliver after the deadline");
        bool saw_a = false;
        for (const auto& row : r.rows) saw_a = saw_a || row.cells[1].second.s == "a";
        check(!saw_a, "ACKed a never redelivers");

        // Exactly-once under competing consumers: two consumers alternate RECEIVE/ACK;
        // every payload must be ACKed exactly once, none lost, none duplicated post-ACK.
        e.exec("CREATE QUEUE work");
        for (int i = 0; i < 20; ++i) e.exec("SEND work, 'm" + std::to_string(i) + "'");
        std::map<std::string, int> acked;
        for (int round = 0; round < 50 && static_cast<int>(acked.size()) < 20; ++round) {
            const ExecResult got = e.exec("RECEIVE work BATCH 3 VISIBILITY 4");
            for (const auto& row : got.rows) {  // consumer (round % 2) processes + ACKs
                ++acked[row.cells[1].second.s];
                e.exec("ACK work, " + std::to_string(row.cells[0].second.i));
            }
            tick(e, 2);
        }
        check(acked.size() == 20, "all 20 messages consumed");
        bool once = true;
        for (const auto& [m2, n] : acked) once = once && n == 1;
        check(once, "every message ACKed exactly once");
        check(e.exec("RECEIVE work BATCH 10").rows.empty(), "queue drained");

        // DLQ: an unACKed message dead-letters after 5 deliveries.
        e.exec("CREATE QUEUE poison");
        e.exec("SEND poison, 'bad'");
        for (int i = 0; i < 5; ++i) {
            const ExecResult g2 = e.exec("RECEIVE poison VISIBILITY 1");
            check(g2.rows.size() == 1, "poison delivery " + std::to_string(i + 1));
            tick(e, 3);
        }
        r = e.exec("RECEIVE poison VISIBILITY 1");  // 6th attempt -> DLQ, not delivered
        check(r.ok && r.rows.empty(), "6th attempt dead-letters instead of delivering");
        r = e.exec("RECEIVE poison DLQ");
        check(r.ok && r.rows.size() == 1 && r.rows[0].cells[1].second.s == "bad",
              "DLQ peek shows the dead letter");

        // Transactional outbox: SEND commits/rolls back atomically with data writes.
        e.exec("CREATE TABLE orders (id INT, note TEXT, PRIMARY KEY (id))");
        e.exec("CREATE QUEUE events");
        e.exec("BEGIN");
        e.exec("INSERT INTO orders (id, note) VALUES (1, 'x')");
        e.exec("SEND events, 'order-1'");
        e.exec("ROLLBACK");
        check(e.exec("SELECT id FROM orders").rows.empty(), "outbox ROLLBACK: no row");
        check(e.exec("RECEIVE events").rows.empty(), "outbox ROLLBACK: no message");
        e.exec("BEGIN");
        e.exec("INSERT INTO orders (id, note) VALUES (1, 'x')");
        e.exec("SEND events, 'order-1'");
        e.exec("COMMIT");
        check(e.exec("SELECT id FROM orders").rows.size() == 1, "outbox COMMIT: row present");
        r = e.exec("RECEIVE events");
        check(r.rows.size() == 1 && r.rows[0].cells[1].second.s == "order-1",
              "outbox COMMIT: message delivered");

        // Teeth.
        check(!e.exec("SEND nosuch, 'x'").ok, "SEND to unknown queue rejected");
        check(!e.exec("SEND jobs, 42").ok, "non-TEXT payload rejected");
        e.exec("BEGIN");
        check(!e.exec("RECEIVE jobs").ok, "RECEIVE inside a transaction rejected");
        check(!e.exec("ACK jobs, 1").ok, "ACK inside a transaction rejected");
        e.exec("ROLLBACK");
        check(e.exec("DROP QUEUE poison").ok, "DROP QUEUE");
        check(!e.exec("SEND poison, 'x'").ok, "dropped queue is gone");
    }

    // Durability: queues, in-flight state, and the DLQ survive a restart.
    {
        lockstep::core::Scheduler sched;
        lockstep::core::SimClock clock(sched);
        lockstep::sim::SeededRandom rng(0x0Bull);
        lockstep::sim::DiskFaultConfig dc;
        lockstep::sim::SimDisk data(sched, clock, rng, dc), cat(sched, clock, rng, dc);
        {
            SqlEngine e(sched, data, sched, cat);
            e.exec("CREATE QUEUE q");
            e.exec("SEND q, 'one'");
            e.exec("SEND q, 'two'");
            (void)e.exec("RECEIVE q BATCH 1 VISIBILITY 3");  // 'one' goes in-flight
        }
        {
            SqlEngine e(sched, data, sched, cat);
            e.recover(data.logical_len(), cat.logical_len());
            ExecResult r = e.exec("RECEIVE q BATCH 10 VISIBILITY 3");
            check(r.ok && r.rows.size() >= 1, "queue survives restart");
            bool saw_two = false;
            for (const auto& row : r.rows) saw_two = saw_two || row.cells[1].second.s == "two";
            check(saw_two, "'two' still deliverable after restart");
        }
    }

    if (g_fail != 0) { std::printf("sql_queue_test: FAILURES\n"); return 1; }
    std::printf("sql_queue_test: ALL PASS\n");
    return 0;
}
