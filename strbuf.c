#include "strbuf.h"

#include <stdlib.h>
#include <string.h>

static size_t
next_capacity(size_t current, size_t required)
{
    size_t cap = current ? current : 16;
    while (cap < required) {
        size_t next = cap * 2;
        if (next < cap) {
            cap = required;
            break;
        }
        cap = next;
    }
    return cap < required ? required : cap;
}

void
strbuf_init(struct strbuf *sb, size_t initial_cap)
{
    if (sb == NULL)
        return;
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    if (initial_cap == 0)
        return;
    sb->data = malloc(initial_cap);
    if (sb->data == NULL)
        return;
    sb->cap     = initial_cap;
    sb->data[0] = '\0';
}

void
strbuf_reset(struct strbuf *sb)
{
    if (sb == NULL)
        return;
    sb->len = 0;
    if (sb->data != NULL)
        sb->data[0] = '\0';
}

void
strbuf_free(struct strbuf *sb)
{
    if (sb == NULL)
        return;
    free(sb->data);
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
}

int
strbuf_reserve(struct strbuf *sb, size_t extra)
{
    if (sb == NULL)
        return -1;
    if (extra == 0) {
        if (sb->data == NULL && sb->cap == 0) {
            sb->data = malloc(1);
            if (sb->data == NULL)
                return -1;
            sb->cap     = 1;
            sb->data[0] = '\0';
        }
        return 0;
    }
    if (sb->data == NULL && sb->cap == 0) {
        size_t cap = next_capacity(0, extra + 1);
        sb->data   = malloc(cap);
        if (sb->data == NULL)
            return -1;
        sb->cap     = cap;
        sb->len     = 0;
        sb->data[0] = '\0';
        return 0;
    }
    if (sb->len > SIZE_MAX - extra - 1)
        return -1;
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap)
        return 0;
    size_t cap = next_capacity(sb->cap, needed);
    char *buf  = realloc(sb->data, cap);
    if (buf == NULL)
        return -1;
    sb->data = buf;
    sb->cap  = cap;
    return 0;
}

int
strbuf_append(struct strbuf *sb, const char *data, size_t len)
{
    if (sb == NULL)
        return -1;
    if (len == 0 || data == NULL)
        return len == 0 ? 0 : -1;
    if (strbuf_reserve(sb, len) != 0)
        return -1;
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return 0;
}

int
strbuf_push(struct strbuf *sb, char ch)
{
    if (sb == NULL)
        return -1;
    if (strbuf_reserve(sb, 1) != 0)
        return -1;
    sb->data[sb->len++] = ch;
    sb->data[sb->len]   = '\0';
    return 0;
}

int
strbuf_append_str(struct strbuf *sb, const char *str)
{
    if (str == NULL)
        return 0;
    return strbuf_append(sb, str, strlen(str));
}

char *
strbuf_detach(struct strbuf *sb)
{
    if (sb == NULL)
        return NULL;
    char *data = sb->data;
    sb->data   = NULL;
    sb->len    = 0;
    sb->cap    = 0;
    return data;
}

const char *
strbuf_data(const struct strbuf *sb)
{
    if (sb == NULL)
        return NULL;
    return sb->data;
}

size_t
strbuf_len(const struct strbuf *sb)
{
    if (sb == NULL)
        return 0;
    return sb->len;
}
