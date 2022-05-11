#include <termios.h>
#include <unistd.h>
#include <stdio.h>

// These can generally be no-ops.
// While this is dangerous, the amount of the terminal API that's used in only job control is
// fairly narrow.

// TODO: I'm not sure all the tcgetattr/tcsetattr calls can actually be stubbed out. If they're
//       related to job control, it's fine. But they could called from builtins as well?
// It may also make more sense to stub out or refactor actual fish functions.
// ("terminal_return_from_job_group") A lot of that code is unnecessary anyway.

int __wrap_tcgetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetattr(int fd, struct termios* t) {
    return 0;
}

int __wrap_tcsetpgrp(int fd, pid_t pgrp) {
    dprintf(STDOUT_FILENO, "{\"Pgid\": %d}\n", pgrp);
    return 0;
}
