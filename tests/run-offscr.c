// RUN: cat %S/data/sample1.md | %x | %check
//
// CHECK: https://gist.github.com/egmontkob/
// CHECK: https://gist.github.com/egmontkob/
// CHECK: https://lynx.browser.org/
// CHECK-SAME: https://kristaps.bsd.lv/lowdown
// CHECK: wordexp(3)
// CHECK-NOT: wordexp.3

#include "offscr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(void)
{
    int status = EXIT_FAILURE;

    struct offscr_opts opts = {
        .first_byte_timeout_ms = 0,
        .next_byte_timeout_ms  = 0,
    };
    struct offscr_ctx *ctx = offscr_new(&opts);
    if (ctx == NULL) {
        perror("offscr_new");
        goto error;
    }

    int rc = offscr_capture(ctx, STDIN_FILENO);
    if (rc < 0) {
        perror("offscr_capture");
        goto error;
    }

    struct offscr_view view = offscr_view(ctx);
    if (view.len > 0) {
        if (write(STDOUT_FILENO, view.data, view.len) == -1) {
            perror("write");
            goto error;
        }
    }

    status = EXIT_SUCCESS;
error:
    if (ctx)
        offscr_free(ctx);
    return status;
}
