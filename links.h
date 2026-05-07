#ifndef LINKS_H
#define LINKS_H

#include "defs.h"
#include "offscr.h"

#include <stddef.h>

enum link_kind {
    LINKS_KIND_OSC8 = 0,
    LINKS_KIND_MAN  = 1,
};


/*
 * Metadata describing one detected link.
 *
 * link/text/text_len: canonical target URI, printable label, and its byte len.
 * row: zero-based row number in the captured view.
 * row_offset: absolute byte offset (within view->data) of the first byte that
 *             belongs to row `row`. Adding indices.* to this yields absolute
 *             byte offsets for the span.
 * columns: visible column range [start, end) covered by the link text.
 * indices: byte range [start, end) relative to row_offset that covers the raw
 *          bytes forming the link text (including any inline SGR sequences).
 * kind: which parser produced the span (OSC8 vs MAN token).
 */
struct link_span {
    char *link;
    char *text;
    size_t text_len;
    size_t row;
    size_t row_offset;
    struct range columns;
    struct range indices;
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
