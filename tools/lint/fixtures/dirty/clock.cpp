// DIRTY: clock / wall-clock time. Category: std::chrono.
#include <chrono>

namespace lockstep::bad {
void now() {
    // The canonical planted forbidden call from the brief / spec C0.2:
    auto t = std::chrono::system_clock::now();
    (void)t;
    auto d = std::chrono::milliseconds(5);
    (void)d;
}
}  // namespace lockstep::bad
