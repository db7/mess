#ifndef MESS_OFFSCR_H
#define MESS_OFFSCR_H

#include <stdbool.h>
#include <stddef.h>

enum offscr_status { OFFSCR_CAPTURE_OK = 0, OFFSCR_CAPTURE_TIMEOUT = 1 };

struct offscr_opts {
    unsigned int first_byte_timeout_ms;
    unsigned int next_byte_timeout_ms;
    bool drain;
};

struct offscr_view {
    const char *data;
    size_t len;
};

struct offscr_ctx;

struct offscr_ctx *offscr_new(const struct offscr_opts *);
int offscr_capture(struct offscr_ctx *, int child_fd);
struct offscr_view offscr_view(const struct offscr_ctx *);
char *offscr_extract(const struct offscr_view *, size_t row);
void offscr_forget_first_row(struct offscr_ctx *);
void offscr_free(struct offscr_ctx *);

#endif /* MESS_OFFSCR_H */
