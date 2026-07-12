// cdc_bench_main.cpp — K4 CDC throughput: how fast can a consumer drain the changefeed?
// Deterministic workload: N single-row INSERTs (each its own committed op), then time
//   (a) full-feed drain      CHANGES t SINCE 0            (cold cursor, whole log)
//   (b) chunked consumption  CHANGES t SINCE c LIMIT 4096 (the real consumer loop)
// Reported number = ops/s DELIVERED to the consumer, decoded into full row images.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <lockstep/query/sql/Engine.hpp>

using namespace lockstep::query::sql;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? static_cast<std::size_t>(std::atoll(argv[1])) : 200000;
    SqlEngine e;
    e.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const std::string v = std::to_string(i);
        e.exec("INSERT INTO t (id,name,score) VALUES (" + v + ",'u" + v + "'," +
               std::to_string((i * 7) % 1000) + ")");
    }
    const auto t1 = Clock::now();
    ExecResult all = e.exec("CHANGES t SINCE 0");
    const auto t2 = Clock::now();
    if (!all.ok || all.rows.size() != n) {
        std::printf("full drain refused/short: ok=%d rows=%zu err=%s\n", all.ok ? 1 : 0,
                    all.rows.size(), all.error.c_str());
        return 1;
    }
    // Chunked loop: cursor = last _seq of the previous chunk (the real consumer shape).
    std::int64_t cur = 0;
    std::size_t got = 0;
    const auto t3 = Clock::now();
    for (;;) {
        ExecResult c = e.exec("CHANGES t SINCE " + std::to_string(cur) + " LIMIT 4096");
        if (!c.ok || c.rows.empty()) break;
        got += c.rows.size();
        cur = c.rows.back().cells[0].second.i;
    }
    const auto t4 = Clock::now();
    if (got != n) { std::printf("BAD chunked drain got=%zu\n", got); return 1; }
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
    };
    std::printf("rows=%zu ingest=%.0fms (%.0f ops/s)\n", n, ms(t0, t1), n / ms(t0, t1) * 1000);
    std::printf("full drain:    %.1fms  -> %.0f ops/s delivered\n", ms(t1, t2),
                n / ms(t1, t2) * 1000);
    std::printf("chunked drain: %.1fms  -> %.0f ops/s delivered (LIMIT 4096 cursor loop)\n",
                ms(t3, t4), n / ms(t3, t4) * 1000);

    // (c) The REAL consumer shape: tail the feed WHILE ingest runs (drain every 2048
    // writes). This is how a changefeed consumer avoids the flush horizon entirely —
    // the cursor stays inside the memtable window no matter how large the table grows.
    const std::size_t big = n * 4;
    SqlEngine e2;
    e2.exec("CREATE TABLE t (id INT, name TEXT, score INT, PRIMARY KEY (id))");
    std::int64_t c2 = 0;
    std::size_t seen = 0;
    double drain_ms = 0.0;
    const auto t5 = Clock::now();
    for (std::size_t i = 0; i < big; ++i) {
        const std::string v = std::to_string(i);
        e2.exec("INSERT INTO t (id,name,score) VALUES (" + v + ",'u" + v + "'," +
                std::to_string((i * 7) % 1000) + ")");
        if ((i + 1) % 2048 == 0 || i + 1 == big) {
            const auto d0 = Clock::now();
            for (;;) {
                ExecResult c = e2.exec("CHANGES t SINCE " + std::to_string(c2) + " LIMIT 4096");
                if (!c.ok) { std::printf("LIVE TAIL REFUSED at %zu\n", i); return 1; }
                if (c.rows.empty()) break;
                seen += c.rows.size();
                c2 = c.rows.back().cells[0].second.i;
            }
            drain_ms += ms(d0, Clock::now());
        }
    }
    const auto t6 = Clock::now();
    if (seen != big) { std::printf("BAD live tail seen=%zu\n", seen); return 1; }
    std::printf("live tail (%zu rows, ingest+consume interleaved): total=%.0fms, "
                "consumer share=%.0fms -> %.0f ops/s delivered\n",
                big, ms(t5, t6), drain_ms, big / drain_ms * 1000);
    return 0;
}
