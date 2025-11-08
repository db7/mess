#ifndef WRAPPER_H
#define WRAPPER_H

#include "nav.h"

#include <termios.h>

typedef void (*pager_termios_hook_fn)(const struct termios *term, char stage);

// Reason: Shared configuration for both pager frontends so they can choose the
// navigation backend while reusing the same PTY harness.
struct wrapper_config {
    int argc;
    char **argv;
    nav_mode_t mode;
};

int wrapper_run(const struct wrapper_config *config);
void wrapper_set_termios_hook(pager_termios_hook_fn hook);

#endif // WRAPPER_H
