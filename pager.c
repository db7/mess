#include "pager.h"
#include "wrapper.h"

int
pager_run(int argc, char *argv[])
{
    // Reason: Legacy entry point now just pins the navigation mode to OSC8 and
    // hands control to the shared wrapper.
    struct wrapper_config cfg = {
        .argc = argc,
        .argv = argv,
        .mode = NAV_MODE_OSC8,
    };
    return wrapper_run(&cfg);
}

void
pager_set_termios_hook(pager_termios_hook_fn hook)
{
    // Reason: Preserve backwards compatibility for tests/tooling calling the
    // old pager API directly.
    wrapper_set_termios_hook(hook);
}
