#ifndef PAGER_H
#define PAGER_H

#include <termios.h>

typedef void (*pager_termios_hook_fn)(const struct termios *term, char stage);

// Run the pager executable with the provided arguments.
int pager_run(int argc, char *argv[]);

// Register a callback to inspect or tweak terminal settings during setup/teardown.
void pager_set_termios_hook(pager_termios_hook_fn hook);

#endif // PAGER_H
