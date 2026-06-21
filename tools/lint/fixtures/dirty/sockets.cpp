// DIRTY: raw socket syscalls. socket/bind/connect/accept/listen/send/recv/sendto/recvfrom.
#include <sys/socket.h>
#include <netinet/in.h>

namespace lockstep::bad {
void net() {
    int s = socket(2, 1, 0);
    bind(s, nullptr, 0);
    connect(s, nullptr, 0);
    listen(s, 16);
    int c = accept(s, nullptr, nullptr);
    send(c, "x", 1, 0);
    recv(c, nullptr, 0, 0);
    sendto(c, "x", 1, 0, nullptr, 0);
    recvfrom(c, nullptr, 0, 0, nullptr, nullptr);
}
}  // namespace lockstep::bad
