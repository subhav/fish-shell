#include <termios.h>
#include <unistd.h>
#include <stdio.h>

// These can generally be no-ops.
// While this is dangerous, the amount of the terminal API that's used to do only job control is
// fairly narrow.

int __wrap_tcgetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetpgrp(int fd, pid_t pgrp) {
    printf("{\"Pgid\": %d}\n", pgrp);
    fflush(stdout);
    return 0;
}
