// Providers-exempt fixture. This file DOES use forbidden constructs, but its
// path contains a `providers/` segment, so the lint MUST NOT flag it.
// This proves cardinal rule 1's exemption: nondeterminism lives in providers/.
#include <chrono>
#include <thread>
#include <random>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>

namespace lockstep::providers::prod {

void wall_clock_provider() {
    auto t = std::chrono::system_clock::now();  // exempt: providers/
    (void)t;
    std::random_device rd;                       // exempt
    std::mt19937 gen(rd());                       // exempt
    (void)gen;
    std::atomic_thread_fence(std::memory_order_seq_cst);  // exempt
    int fd = ::open("/dev/null", 0);              // exempt
    ::write(fd, "x", 1);                          // exempt
    ::close(fd);                                  // exempt
    int s = socket(0, 0, 0);                      // exempt
    (void)s;
    std::thread worker([] {});                    // exempt
    worker.join();
}

}  // namespace lockstep::providers::prod
