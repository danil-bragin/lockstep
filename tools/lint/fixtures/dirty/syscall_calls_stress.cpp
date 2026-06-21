// DIRTY stress: ensure the declaration-guard never hides a real CALL.
// Every line below is a genuine forbidden syscall *call* and MUST be flagged.
#include <unistd.h>
#include <sys/socket.h>

namespace lockstep::bad {
int caller(int fd) {
    auto n = send(fd, "x", 1, 0);          // assignment, not a declaration
    return recv(fd, nullptr, 0, 0);        // `return recv(` must still flag
}
int more(int fd) {
    if (connect(fd, nullptr, 0)) {}        // call inside if-condition
    while (accept(fd, nullptr, nullptr)) break;  // call in while-condition
    ::write(fd, "y", 1);                   // qualified syscall
    return fsync(fd) + lseek(fd, 0, 0);    // two syscalls after `return`
}
}  // namespace lockstep::bad
