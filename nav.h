#ifndef NAV_H
#define NAV_H

#include "offscr.h"
#include "pager.h"
#include "readq.h"

#include <stdbool.h>
#include <stddef.h>

struct nav;

struct nav_opts {
    int parse_flags;
    const char *self_cmd;
};

enum nav_result {
    NAV_RESULT_OK = 0,
    NAV_RESULT_STOP,
    NAV_RESULT_REFRESH,
    NAV_RESULT_CANCELLED,
    NAV_RESULT_QUIT,
    NAV_RESULT_ERROR,
};
typedef enum nav_result nav_result_t;

struct nav *nav_create(const struct nav_opts *);
void nav_destroy(struct nav *);
nav_result_t nav_run(struct nav *, const struct offscr_view *, struct readq *,
                     int out_fd, size_t width, bool redraw);
void nav_cancel(struct nav *);

#endif /* NAV_H */
