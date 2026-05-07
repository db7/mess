/*
 * Styler helper for mess.
 * Provides utilities for wrapping visible substrings with custom ANSI codes.
 */
#ifndef MESS_STYLER_H
#define MESS_STYLER_H

#include "defs.h"

#include <stddef.h>

enum styler_style_flags {
    STYLER_STYLE_REPLACE = 1 << 0,
};

struct styler_style {
    struct range span;
    const char *enter_seq;
    const char *exit_seq;
    int flags;
};

/*
 * Produce a newly-allocated copy of `line` with ANSI escape sequences removed.
 * When out_len is non-NULL it receives the length of the visible portion.
 */
char *styler_strip(const char *line, size_t *out_len);

char *styler_apply_offsets(const char *, const struct styler_style *, size_t);

#endif /* MESS_STYLER_H */
