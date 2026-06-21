// DIRTY: raw file syscalls / raw IO. ::open ::read ::write ::close lseek fsync + headers.
#include <fcntl.h>
#include <unistd.h>

namespace lockstep::bad {
void io() {
    int fd = ::open("/tmp/x", 0);
    ::read(fd, nullptr, 0);
    ::write(fd, "x", 1);
    lseek(fd, 0, 0);
    fsync(fd);
    fdatasync(fd);
    ::close(fd);
}
}  // namespace lockstep::bad
