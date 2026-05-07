#include "links.h"

#include "ansi.h"
#include "strbuf.h"

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
    size_t cap = res->capacity == 0 ? 4 : res->capacity;
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

static int
compare_spans_(const void *lhs, const void *rhs)
{
    const struct link_span *a = lhs;
    const struct link_span *b = rhs;
    if (!a || !b)
        return 0;
    if (a->row < b->row)
        return -1;
    if (a->row > b->row)
        return 1;
    if (a->columns.start < b->columns.start)
        return -1;
    if (a->columns.start > b->columns.start)
        return 1;
    if (a->row_offset < b->row_offset)
        return -1;
    if (a->row_offset > b->row_offset)
        return 1;
    if (a->indices.start < b->indices.start)
        return -1;
    if (a->indices.start > b->indices.start)
        return 1;
    return 0;
}

static void
sort_spans_(struct link_iter *iter)
{
    if (iter == NULL || iter->count < 2)
        return;
    qsort(iter->spans, iter->count, sizeof(*iter->spans), compare_spans_);
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
             const char *text_begin, size_t text_len, size_t row,
             size_t row_offset, struct range cols, struct range indices,
             enum link_kind kind)
{
    if (ensure_capacity_(res, res->count + 1) == -1)
        return -1;

    struct link_span *span = &res->spans[res->count];
    span->link             = dup_range_(link_begin, link_len);
    if (span->link == NULL)
        return -1;
    span->text = dup_range_(text_begin, text_len);
    if (span->text == NULL) {
        free(span->link);
        return -1;
    }
    span->text_len   = text_len;
    span->row        = row;
    span->row_offset = row_offset;
    span->columns    = cols;
    span->indices    = indices;
    span->kind       = kind;
    res->count++;
    return 0;
}

struct cursor_pos {
    size_t row;
    size_t col;
    size_t row_offset;
};

struct osc8_seq {
    const char *uri;
    size_t uri_len;
    size_t end;
};

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

static int
man_name_all_upper_(const char *name, size_t len)
{
    int saw_alpha = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (isalpha(ch)) {
            saw_alpha = 1;
            if (!isupper(ch))
                return 0;
        }
    }
    return saw_alpha;
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

static int
strip_decorations_(const char *data, size_t len, struct strbuf *dest,
                   size_t *map, size_t *out_len)
{
    if (!data || dest == NULL)
        return -1;
    strbuf_reset(dest);
    if (strbuf_reserve(dest, 0) != 0)
        return -1;

    size_t out = 0;
    size_t i   = 0;
    while (i < len) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\b') {
            if (out > 0) {
                out--;
                dest->len       = out;
                dest->data[out] = '\0';
            }
            i++;
            continue;
        }
        if (ch == '\033') {
            i = ansi_skip_seq(data, len, i);
            continue;
        }
        if (strbuf_push(dest, (char)ch) != 0)
            return -1;
        out = dest->len;
        if (map)
            map[out - 1] = i;
        i++;
    }
    if (map)
        map[out] = i;
    if (out_len)
        *out_len = out;
    return 0;
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
build_row_offsets_(const char *buf, size_t len, size_t **offsets_out,
                   size_t *count_out)
{
    if (!buf || !offsets_out || !count_out)
        return -1;
    size_t cap      = 16;
    size_t count    = 0;
    size_t *offsets = malloc(cap * sizeof(*offsets));
    if (offsets == NULL)
        return -1;
    offsets[count++] = 0;
    for (size_t idx = 0; idx < len; ++idx) {
        if (buf[idx] == '\n') {
            if (count == cap) {
                cap *= 2;
                size_t *tmp = realloc(offsets, cap * sizeof(*tmp));
                if (tmp == NULL) {
                    free(offsets);
                    return -1;
                }
                offsets = tmp;
            }
            offsets[count++] = idx + 1;
        }
    }
    *offsets_out = offsets;
    *count_out   = count;
    return 0;
}

static size_t
row_offset_for_(const size_t *offsets, size_t count, size_t row)
{
    if (offsets == NULL || count == 0)
        return 0;
    if (row < count)
        return offsets[row];
    return offsets[count - 1];
}

static void
cursor_advance_byte_(struct cursor_pos *pos, unsigned char ch, size_t next_idx)
{
    if (!pos)
        return;
    if (ch == '\n') {
        pos->row++;
        pos->col        = 0;
        pos->row_offset = next_idx;
    } else if (ch == '\r') {
        pos->col = 0;
    } else if (ch == '\b') {
        if (pos->col > 0)
            pos->col--;
    } else {
        pos->col++;
    }
}

static int
parse_osc8_seq_(const char *buf, size_t len, size_t idx, struct osc8_seq *out)
{
    if (!buf || !out || idx >= len || buf[idx] != '\033')
        return 0;

    struct ansi_seq seq = ansi_parse_seq(buf, len, idx);
    if (seq.type != ANSI_SEQ_OSC || !seq.complete)
        return 0;

    size_t payload_start = idx + 2;
    if (payload_start >= seq.end || buf[payload_start] != '8')
        return 0;

    const char *payload = buf + payload_start + 1;
    const char *end     = buf + seq.end;
    if (end >= buf + 2 && end[-1] == '\\' && end[-2] == '\033')
        end -= 2;
    else if (end > payload && end[-1] == '\a')
        end -= 1;

    const char *first_semicolon = memchr(payload, ';', (size_t)(end - payload));
    if (!first_semicolon)
        return 0;
    const char *second_semicolon =
        memchr(first_semicolon + 1, ';', (size_t)(end - (first_semicolon + 1)));
    if (!second_semicolon)
        return 0;

    out->uri     = second_semicolon + 1;
    out->uri_len = (size_t)(end - out->uri);
    out->end     = seq.end;
    return 1;
}

static int
parse_links_man(const char *buf, size_t len, const size_t *row_offsets,
                size_t row_count, struct link_iter *res)
{
    struct strbuf plain = STRBUF_INIT;
    size_t *map         = malloc((len + 1) * sizeof(*map));
    if (map == NULL) {
        strbuf_free(&plain);
        return -1;
    }

    size_t plain_len = 0;
    if (strip_decorations_(buf, len, &plain, map, &plain_len) == -1) {
        strbuf_free(&plain);
        free(map);
        return -1;
    }
    const char *plain_data = strbuf_data(&plain);
    if (plain_data == NULL) {
        strbuf_free(&plain);
        free(map);
        return -1;
    }
    size_t cur = 0;
    size_t row = 0;
    size_t col = 0;

    while (cur < plain_len) {
        size_t name_len       = 0;
        size_t section_offset = 0;
        size_t section_len    = 0;
        size_t token_len      = 0;
        if (man_match_at_(plain_data, plain_len, cur, &name_len,
                          &section_offset, &section_len, &token_len)) {
            const char *name    = plain_data + cur;
            const char *section = plain_data + cur + section_offset;
            if (man_name_all_upper_(name, name_len)) {
                col += token_len;
                cur += token_len;
                continue;
            }
            char link_buf[128];
            size_t link_len = man_format_link_(link_buf, sizeof(link_buf), name,
                                               name_len, section, section_len);
            if (link_len > 0) {
                size_t abs_start = map[cur];
                size_t abs_end =
                    map[cur + token_len < plain_len ? cur + token_len :
                                                      plain_len];
                size_t row_offset =
                    row_offset_for_(row_offsets, row_count, row);
                struct range columns = {
                    .start = col,
                    .end   = col + token_len,
                };
                struct range indices = {
                    .start = abs_start - row_offset,
                    .end   = abs_end - row_offset,
                };
                if (record_span_(res, link_buf, link_len, plain_data + cur,
                                 token_len, row, row_offset, columns, indices,
                                 LINKS_KIND_MAN) == -1) {
                    free(map);
                    strbuf_free(&plain);
                    return -1;
                }
            }
            col += token_len;
            cur += token_len;
            continue;
        }

        char ch = plain_data[cur];
        if (ch == '\n') {
            row++;
            col = 0;
        } else if (ch == '\r') {
            col = 0;
        } else {
            col++;
        }
        cur++;
    }

    free(map);
    strbuf_free(&plain);
    return 0;
}

static int
parse_links_osc8(const char *buf, size_t len, struct link_iter *res)
{
    struct cursor_pos pos = {0};
    size_t i             = 0;

    while (i < len) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\033') {
            struct osc8_seq open = {0};
            if (!parse_osc8_seq_(buf, len, i, &open)) {
                i = ansi_skip_seq(buf, len, i);
                continue;
            }
            if (open.uri_len == 0) {
                i = open.end;
                continue;
            }

            size_t text_start = open.end;
            struct cursor_pos text_pos = pos;
            size_t scan = text_start;
            struct osc8_seq close = {0};
            bool found_close = false;
            while (scan < len) {
                unsigned char text_ch = (unsigned char)buf[scan];
                if (text_ch == '\033' &&
                    parse_osc8_seq_(buf, len, scan, &close) &&
                    close.uri_len == 0) {
                    found_close = true;
                    break;
                }
                if (text_ch == '\033') {
                    scan = ansi_skip_seq(buf, len, scan);
                    continue;
                }
                cursor_advance_byte_(&text_pos, text_ch, scan + 1);
                scan++;
            }
            if (!found_close)
                break;

            struct strbuf plain = STRBUF_INIT;
            size_t plain_len    = 0;
            if (strip_decorations_(buf + text_start, scan - text_start, &plain,
                                   NULL, &plain_len) == -1) {
                strbuf_free(&plain);
                return -1;
            }
            const char *plain_data = strbuf_data(&plain);
            if (plain_data == NULL) {
                strbuf_free(&plain);
                return -1;
            }
            struct range columns = {
                .start = pos.col,
                .end   = pos.col + plain_len,
            };
            struct range indices = {
                .start = text_start - pos.row_offset,
                .end   = scan - pos.row_offset,
            };
            if (record_span_(res, open.uri, open.uri_len, plain_data, plain_len,
                             pos.row, pos.row_offset, columns, indices,
                             LINKS_KIND_OSC8) == -1) {
                strbuf_free(&plain);
                return -1;
            }
            strbuf_free(&plain);
            pos = text_pos;
            i   = close.end;
            continue;
        }
        cursor_advance_byte_(&pos, ch, i + 1);
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

    int rc = 0;
    switch (kind) {
        case LINKS_KIND_OSC8:
            rc = parse_links_osc8(view->data, view->len, out);
            break;
        case LINKS_KIND_MAN: {
            size_t *row_offsets = NULL;
            size_t row_count    = 0;
            if (build_row_offsets_(view->data, view->len, &row_offsets,
                                   &row_count) == -1)
                return -1;
            rc = parse_links_man(view->data, view->len, row_offsets, row_count,
                                 out);
            free(row_offsets);
            break;
        }
        default:
            rc = 0;
            break;
    }

    if (rc == 0)
        sort_spans_(out);

    return rc;
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
