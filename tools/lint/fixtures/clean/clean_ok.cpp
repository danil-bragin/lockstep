// Clean fixture: uses ONLY allowed constructs. Must pass the lint.
// All nondeterminism would flow through the lockstep::core abstraction boundary.
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>  // bare std::atomic is GRAY/allowed; only ordering ops are flagged.

#include <lockstep/core/IClock.hpp>
#include <lockstep/core/IRandom.hpp>

namespace lockstep::demo {

// A plain counter using bare std::atomic with no memory order — allowed.
std::atomic<int> plain_counter{0};

// Comments that MENTION forbidden tokens must NOT trip the lint:
//   std::chrono::system_clock::now();  socket(  ::open(  std::memory_order_seq_cst
/* block comment: std::thread std::rand std::random_device recvfrom( fsync( */

// String literals that contain forbidden tokens must NOT trip the lint either.
const char* doc = "do not use std::chrono or std::thread or socket( here";
const char* raw = R"(neither ::write( nor std::memory_order_relaxed in raw strings)";

std::uint64_t bump(lockstep::core::IRandom& rng) {
    plain_counter.fetch_add(1);  // bare atomic op, no explicit memory order: allowed.
    return rng.next();
}

// A member call whose name collides with a socket syscall must NOT be flagged:
// member access (`.`/`->`) disambiguates it from the raw `send(` syscall.
struct Channel {
    void deliver(const std::string& msg);
};
void route(Channel& ch) {
    ch.deliver("hello");   // ordinary member call
    Channel* p = &ch;
    p->deliver("again");   // ordinary member call via ->
}

}  // namespace lockstep::demo
