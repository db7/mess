#include "dispatcher.h"
#include "pager.h"
#include "version.h"

#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Print usage instructions for the CLI entry point.
static void
usage_(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [file|uri]\n"
            "  %s --open <url>\n"
            "  %s [-m] [-o] < pager-input\n",
            prog, prog, prog);
}

int
main(int argc, char *argv[])
{
    dispatcher_set_self_path(argv[0]);
    bool enable_osc8     = false;
    bool enable_man      = false;
    const char *open_arg = NULL;

    // Supported long-form command-line options for getopt_long().
    static const struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {"open", required_argument, NULL, 'O'},
        {"osc8", no_argument, NULL, 'o'},
        {"man", no_argument, NULL, 'm'},
        {NULL, 0, NULL, 0}};

    opterr = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "hVOom", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'h':
                usage_(argv[0]);
                return 0;
            case 'V':
                printf("mess %s\n", MESS_VERSION);
                return 0;
            case 'O':
                open_arg = optarg;
                break;
            case 'o':
                enable_osc8 = true;
                break;
            case 'm':
                enable_man = true;
                break;
            default:
                usage_(argv[0]);
                return EXIT_FAILURE;
        }
    }

    bool have_target = (optind < argc) || (open_arg != NULL);
    if (have_target && (enable_osc8 || enable_man)) {
        fprintf(stderr,
                "mess: -m/-o may only be used when reading from stdin\n");
        return EXIT_FAILURE;
    }

    if (open_arg)
        return dispatcher_run(open_arg);

    if (optind < argc)
        return dispatcher_run(argv[optind]);

    int parse_flags = 0;
    if (enable_osc8)
        parse_flags |= PAGER_PARSE_OSC8;
    if (enable_man)
        parse_flags |= PAGER_PARSE_MAN;
    if (parse_flags == 0)
        parse_flags = PAGER_PARSE_OSC8;

    return pager_run(parse_flags);
}
