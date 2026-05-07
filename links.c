#include "links.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
ensure_capacity_(struct link_iter *res, size_t needed)
{
    if (needed <= res->capacity)
        return 0;
    size_t cap = res->capacity;
    if (cap == 0)
        cap = 4;
    while (cap < needed)
        cap *= 2;
    struct link_span *span = realloc(res->spans, cap * sizeof(*span));
    if (span == NULL)
        return -1;
    memset(span + res->capacity, 0, (cap - res->capacity) * sizeof(*span));
    res->spans    = span;
    res->capacity = cap;
    return 0;
}

static char *
dup_range_(const char *start, size_t len)
{
    char *out = malloc(len + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int
record_span_(struct link_iter *res, const char *link_begin, size_t link_len,
             const char *text_begin, size_t text_len, struct link_pos start,
             struct link_pos end, enum link_kind kind)
{
    if (ensure_capacity_(res, res->count + 1) == -1)
        return -1;

    struct link_span *span = &res->spans[res->count];
    span->link             = dup_range_(link_begin, link_len);
    if (span->link == NULL)
        return -1;
    span->text = dup_range_(text_begin, text_len);
    if (span->text == NULL)
        return -1;
    span->text_len  = text_len;
    span->start.row = start.row;
    span->start.col = start.col;
    span->end.row   = end.row;
    span->end.col   = end.col;
    span->kind      = kind;
    res->count++;
    return 0;
}

static void
line_advance_(char ch, struct link_pos *pos)
{
    if (ch == '\n') {
        pos->row++;
        pos->col = 0;
    } else if (ch == '\r') {
        pos->col = 0;
    } else {
        pos->col++;
    }
}

static int
man_is_name_char_(char ch)
{
    return isalnum((unsigned char)ch) || ch == '_' || ch == '-';
}

static int
man_is_section_char_(char ch)
{
    return isalnum((unsigned char)ch) || ch == '.' || ch == '_';
}

static size_t
man_format_link_(char *dest, size_t cap, const char *name, size_t name_len,
                 const char *section, size_t section_len)
{
    if (!dest || cap == 0)
        return 0;
    int written = snprintf(dest, cap, "man://%.*s.%.*s", (int)name_len, name,
                           (int)section_len, section);
    if (written <= 0 || (size_t)written >= cap)
        return 0;
    return (size_t)written;
}

static size_t
strip_decorations_(const char *data, size_t len, char *dest, size_t dest_cap,
                   size_t *map)
{
    if (!data || !dest || dest_cap == 0)
        return 0;

    size_t out = 0;
    size_t i   = 0;
    while (i < len) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\b') {
            if (out > 0)
                out--;
            i++;
            continue;
        }
        if (ch == '\033') {
            size_t seq_end = i + 1;
            if (seq_end < len) {
                unsigned char next = (unsigned char)data[seq_end];
                if (next == '[') {
                    seq_end++;
                    while (seq_end < len) {
                        unsigned char c = (unsigned char)data[seq_end++];
                        if (c >= '@' && c <= '~')
                            break;
                    }
                } else {
                    seq_end++;
                }
            }
            if (seq_end > len)
                seq_end = len;
            i = seq_end;
            continue;
        }
        if (out >= dest_cap)
            break;
        dest[out] = (char)ch;
        if (map)
            map[out] = i;
        out++;
        i++;
    }
    if (out < dest_cap)
        dest[out] = '\0';
    return out;
}

static int
man_match_at_(const char *data, size_t len, size_t pos, size_t *name_len,
              size_t *section_offset, size_t *section_len, size_t *token_len)
{
    if (pos >= len || !man_is_name_char_(data[pos]))
        return 0;

    size_t i = pos;
    while (i < len && man_is_name_char_(data[i])) {
        i++;
        if (i - pos > 64)
            return 0;
    }
    if (i >= len || data[i] != '(' || i == pos)
        return 0;

    size_t section_start = i + 1;
    size_t j             = section_start;
    int has_section      = 0;
    while (j < len && man_is_section_char_(data[j])) {
        has_section = 1;
        j++;
        if (j - section_start > 32)
            return 0;
    }
    if (!has_section || j >= len || data[j] != ')')
        return 0;

    if (name_len)
        *name_len = i - pos;
    if (section_offset)
        *section_offset = section_start - pos;
    if (section_len)
        *section_len = j - section_start;
    if (token_len)
        *token_len = (j - pos) + 1;
    return 1;
}

static int
parse_links_man(const char *buf, size_t len, struct link_iter *res)
{
    char *plain = malloc(len + 1);
    if (plain == NULL)
        return -1;

    size_t plain_len = strip_decorations_(buf, len, plain, len + 1, NULL);

    size_t cur = 0;
    struct link_pos pos = {0};

    while (cur < plain_len) {
        size_t name_len       = 0;
        size_t section_offset = 0;
        size_t section_len    = 0;
        size_t token_len      = 0;
        if (man_match_at_(plain, plain_len, cur, &name_len, &section_offset,
                          &section_len, &token_len)) {
            const char *name    = plain + cur;
            const char *section = plain + cur + section_offset;
            char link_buf[128];
            size_t link_len = man_format_link_(link_buf, sizeof(link_buf), name,
                                               name_len, section, section_len);
            if (link_len > 0) {
                struct link_pos start = pos;
                struct link_pos tmp   = pos;
                for (size_t k = 0; k < token_len; ++k)
                    line_advance_(plain[cur + k], &tmp);
                if (record_span_(res, link_buf, link_len, plain + cur,
                                 token_len, start, tmp, LINKS_KIND_MAN) == -1) {
                    free(plain);
                    return -1;
                }
                pos = tmp;
            }
            cur += token_len;
            continue;
        }
        line_advance_(plain[cur], &pos);
        cur++;
    }

    free(plain);
    return 0;
}

static int
parse_links_osc8(const char *buf, size_t len, struct link_iter *res)
{
    size_t i       = 0;
    struct link_pos pos = {0};

    while (i < len) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\n') {
            pos.row++;
            pos.col = 0;
            i++;
            continue;
        }
        if (ch == '\r') {
            pos.col = 0;
            i++;
            continue;
        }
        if (ch == '\033' && i + 4 < len && buf[i + 1] == ']' &&
            buf[i + 2] == '8') {
            const char *start           = buf + i;
            const char *cursor          = start + 3;
            const char *end             = buf + len;
            const char *first_semicolon = memchr(cursor, ';', end - cursor);
            if (first_semicolon == NULL)
                break;
            const char *second_semicolon =
                memchr(first_semicolon + 1, ';', end - (first_semicolon + 1));
            if (second_semicolon == NULL)
                break;
            const char *link_start = second_semicolon + 1;
            const char *link_end   = strstr(link_start, "\033\\");
            if (link_end == NULL)
                break;
            const char *text_start = link_end + 2;
            const char *text_end   = strstr(text_start, "\033]8;;\033\\");
            if (text_end == NULL)
                break;

            size_t link_len   = (size_t)(link_end - link_start);
            struct link_pos pstart = pos;
            struct link_pos tmp    = pos;

            size_t text_len = (size_t)(text_end - text_start);
            char *plain     = malloc(text_len + 1);
            if (plain == NULL)
                return -1;
            size_t plain_len =
                strip_decorations_(text_start, text_len, plain, text_len + 1,
                                   NULL);
            for (size_t ti = 0; ti < text_len; ++ti)
                line_advance_(text_start[ti], &tmp);

            if (record_span_(res, link_start, link_len, plain, plain_len,
                             pstart, tmp, LINKS_KIND_OSC8) == -1) {
                free(plain);
                return -1;
            }
            free(plain);

            pos = tmp;
            i   = (size_t)(text_end + strlen("\033]8;;\033\\") - buf);
            continue;
        }

        pos.col++;
        i++;
    }

    return 0;
}

int
parse_links(const struct offscr_view *view, enum link_kind kind,
            struct link_iter *out)
{
    if (view == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (view->data == NULL || view->len == 0)
        return 0;
    if (out->count == 0)
        out->index = 0;

    switch (kind) {
        case LINKS_KIND_OSC8:
            return parse_links_osc8(view->data, view->len, out);
        case LINKS_KIND_MAN:
            return parse_links_man(view->data, view->len, out);
        default:
            /* Not implemented yet. */
            return 0;
    }
}

void
link_iter_free(struct link_iter *res)
{
    if (res == NULL)
        return;
    for (size_t i = 0; i < res->count; ++i) {
        free(res->spans[i].link);
        free(res->spans[i].text);
    }
    free(res->spans);
    res->spans    = NULL;
    res->count    = 0;
    res->capacity = 0;
}

int
links_ok(const struct link_iter *iter)
{
    return iter != NULL && iter->index < iter->count;
}

const struct link_span *
links_get(const struct link_iter *iter, size_t idx)
{
    if (iter == NULL || idx >= iter->count)
        return NULL;
    return &iter->spans[idx];
}

static int
same_link_(const struct link_span *a, const struct link_span *b)
{
    if (a == NULL || b == NULL)
        return 0;
    if (a->link == NULL || b->link == NULL)
        return 0;
    return strcmp(a->link, b->link) == 0;
}

size_t *
links_match(const struct link_iter *iter, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (!links_ok(iter))
        return NULL;
    const struct link_span *base = links_get(iter, iter->index);
    if (base == NULL)
        return NULL;

    size_t start = iter->index;
    while (start > 0) {
        const struct link_span *prev = links_get(iter, start - 1);
        if (!same_link_(base, prev))
            break;
        start--;
    }

    size_t end = iter->index;
    while (end < iter->count) {
        const struct link_span *next = links_get(iter, end);
        if (!same_link_(base, next))
            break;
        end++;
    }

    size_t match_count = end - start;
    if (match_count == 0)
        return NULL;

    size_t *indices = malloc(sizeof(size_t) * match_count);
    if (indices == NULL)
        return NULL;
    for (size_t i = 0; i < match_count; ++i)
        indices[i] = start + i;
    if (out_len != NULL)
        *out_len = match_count;
    return indices;
}

int
links_next(struct link_iter *iter)
{
    if (!links_ok(iter))
        return 0;
    const struct link_span *current = links_get(iter, iter->index);
    size_t idx                      = iter->index + 1;
    while (idx < iter->count) {
        if (!same_link_(current, links_get(iter, idx))) {
            iter->index = idx;
            return 1;
        }
        idx++;
    }
    iter->index = iter->count;
    return 0;
}

int
links_prev(struct link_iter *iter)
{
    if (iter == NULL || iter->index == 0)
        return 0;
    const struct link_span *current = links_get(iter, iter->index);
    size_t idx                      = iter->index;
    while (idx > 0) {
        idx--;
        if (!same_link_(current, links_get(iter, idx))) {
            iter->index = idx;
            return 1;
        }
    }
    iter->index = 0;
    return 0;
}
