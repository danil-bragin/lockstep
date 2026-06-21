// DIRTY: real threads. Categories: std::thread, std::jthread, <thread>.
#include <thread>

namespace lockstep::bad {
void spawn() {
    std::thread worker([] {});
    worker.join();
    std::jthread auto_worker([] {});
}
}  // namespace lockstep::bad
