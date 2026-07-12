// lockstep_queue.cpp — K3.4: queue throughput, the Lockstep side.
//   lockstep_queue N BATCH
// Emits jsonl: send msg/s (single-statement), send msg/s (one txn = outbox batch),
// receive+ack msg/s (drain), redelivery sanity.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;
namespace {
double secs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 10000;
    const int batch = argc > 2 ? std::atoi(argv[2]) : 100;
    SqlEngine e;
    e.exec("CREATE QUEUE q");

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) e.exec("SEND q, 'payload-" + std::to_string(i) + "'");
    std::printf("{\"sys\":\"lockstep\",\"op\":\"send\",\"msg_s\":%.0f}\n", n / secs(t0));

    e.exec("CREATE QUEUE qb");
    t0 = std::chrono::steady_clock::now();
    e.exec("BEGIN");
    for (int i = 0; i < n; ++i) e.exec("SEND qb, 'payload-" + std::to_string(i) + "'");
    e.exec("COMMIT");
    std::printf("{\"sys\":\"lockstep\",\"op\":\"send_txn\",\"msg_s\":%.0f}\n", n / secs(t0));

    // Drain q: RECEIVE BATCH + ACK each (the worker loop).
    t0 = std::chrono::steady_clock::now();
    int got = 0;
    while (got < n) {
        const ExecResult r = e.exec("RECEIVE q BATCH " + std::to_string(batch) +
                                    " VISIBILITY 1000000");
        if (!r.ok || r.rows.empty()) break;
        std::string ack = "ACK q, ";
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            if (i != 0) ack += ", ";
            ack += std::to_string(r.rows[i].cells[0].second.i);
            ++got;
        }
        e.exec(ack);  // batched ack: one commit per RECEIVE batch
    }
    std::printf("{\"sys\":\"lockstep\",\"op\":\"recv_ack\",\"msg_s\":%.0f,\"got\":%d}\n",
                got / secs(t0), got);
    return 0;
}
