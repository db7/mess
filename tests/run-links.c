// RUN: cat %S/data/sample1.md | %x | %check
//
// CHECK: [osc8] [0] (3,64)-(3,68)
// CHECK-SAME: [osc8] [0] {{.*}} "OSC8"
// CHECK-SAME: [osc8] [0] {{.*}} https://gist.github.com/egmontkob/
//
// CHECK: [osc8] [1] (4,4)-(4,13)
// CHECK-SAME: [osc8] [1] {{.*}} "sequences"
// CHECK-SAME: [osc8] [1] {{.*}} https://gist.github.com/egmontkob/
//
// CHECK: [osc8] [2] (6,48)-(6,52) "lynx" -> https://lynx.browser.org/
//
// CHECK: [osc8] [3] (6,58)-(6,65) "lowdown"
// CHECK-SAME: [osc8] [3] {{.*}} https://kristaps.bsd.lv/lowdown/
//
// CHECK: [man] [4] (8,34)-(8,44)
// CHECK-SAME: [man] [4] {{.*}} "wordexp(3)" -> man://wordexp.3

#include "links.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(void)
{
    struct offscr_opts opts = {
        .first_byte_timeout_ms = 0,
        .next_byte_timeout_ms  = 0,
    };
    int status = EXIT_FAILURE;

    struct offscr_ctx *ctx = offscr_new(&opts);
    if (ctx == NULL) {
        perror("offscr_new");
        goto out;
    }

    if (offscr_capture(ctx, STDIN_FILENO) < 0) {
        perror("offscr_capture");
        goto out;
    }

    struct offscr_view view      = offscr_view(ctx);
    const enum link_kind kinds[] = {LINKS_KIND_OSC8, LINKS_KIND_MAN};
    struct link_iter iter        = {0};
    for (size_t m = 0; m < sizeof(kinds) / sizeof(kinds[0]); ++m) {
        if (parse_links(&view, kinds[m], &iter) == -1) {
            perror("parse_links");
            goto out;
        }
    }

    if (iter.count > 0) {
        iter.index = 0;
        while (links_ok(&iter)) {
            size_t match_len = 0;
            size_t *matches  = links_match(&iter, &match_len);
            if (matches != NULL) {
                for (size_t mi = 0; mi < match_len; ++mi) {
                    const struct link_span *span =
                        links_get(&iter, matches[mi]);
                    if (span == NULL)
                        continue;
                    printf(
                        "[%s] [%zu] (%zu,%zu)-(%zu,%zu)"
                        " \"%s\" -> %s\n",
                        span->kind == LINKS_KIND_OSC8 ? "osc8" : "man",
                        matches[mi], span->row, span->columns.start, span->row,
                        span->columns.end, span->text, span->link);
                }
                free(matches);
            }
            if (!links_next(&iter))
                break;
        }
    }

    status = EXIT_SUCCESS;
out:
    link_iter_free(&iter);
    if (ctx)
        offscr_free(ctx);
    return status;
}
