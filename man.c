#include "man.h"
#include "wrapper.h"

int
man_run(int argc, char *argv[])
{
    // Reason: Dedicated entry point for man-page navigation so we just flip the
    // wrapper into MAN mode and reuse the rest of the infrastructure.
    struct wrapper_config cfg = {
        .argc = argc,
        .argv = argv,
        .mode = NAV_MODE_MAN,
    };
    return wrapper_run(&cfg);
}
