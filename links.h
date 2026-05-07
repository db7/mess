#ifndef LINKS_H
#define LINKS_H

#include "offscr.h"

#include <stddef.h>

enum link_kind {
    LINKS_KIND_OSC8 = 0,
    LINKS_KIND_MAN  = 1,
};

struct link_pos {
    size_t row;
    size_t col;
};

struct link_span {
    char *link;
    char *text;
    size_t text_len;
    struct link_pos start;
    struct link_pos end;
    enum link_kind kind;
};

struct link_iter {
    struct link_span *spans;
    size_t count;
    size_t capacity;
    size_t index;
};

int parse_links(const struct offscr_view *, enum link_kind, struct link_iter *);
void link_iter_free(struct link_iter *);

int links_ok(const struct link_iter *);
int links_next(struct link_iter *);
int links_prev(struct link_iter *);
size_t *links_match(const struct link_iter *, size_t *out_len);
const struct link_span *links_get(const struct link_iter *, size_t idx);

#endif /* LINKS_H */
