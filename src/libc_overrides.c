#include <termios.h>
#include <unistd.h>

// These can generally be no-ops.

int __wrap_tcgetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetpgrp(int fd, pid_t pgrp) {
    // TODO: Spit out JSON
}
