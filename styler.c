#include "styler.h"
#include "strbuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static char *
strip_styles_slice_(const char *line, size_t len, size_t *out_len)
{
    if (line == NULL)
        return NULL;
    char *plain = malloc(len + 1);
    if (plain == NULL)
        return NULL;
    size_t out = 0;
    size_t idx = 0;
    while (idx < len) {
        unsigned char ch = (unsigned char)line[idx];
        if (ch == '\x1b') {
            idx++;
            if (idx >= len)
                break;
            unsigned char next = (unsigned char)line[idx];
            idx++;
            if (next == '[') {
                while (idx < len) {
                    unsigned char term = (unsigned char)line[idx++];
                    if (term >= '@' && term <= '~')
                        break;
                }
            } else if (next == ']') {
                while (idx < len) {
                    unsigned char cur = (unsigned char)line[idx++];
                    if (cur == '\a')
                        break;
                    if (cur == '\x1b' && idx < len &&
                        (unsigned char)line[idx] == '\\') {
                        idx++;
                        break;
                    }
                }
            }
            continue;
        }
        plain[out++] = (char)ch;
        idx++;
    }
    plain[out] = '\0';
    if (out_len)
        *out_len = out;
    return plain;
}

char *
styler_strip(const char *line, size_t *out_len)
{
    if (line == NULL)
        return NULL;
    size_t len = strlen(line);
    return strip_styles_slice_(line, len, out_len);
}

static int
style_cmp_(const void *a, const void *b)
{
    const struct styler_style *sa = a;
    const struct styler_style *sb = b;
    if (sa->span.start < sb->span.start)
        return -1;
    if (sa->span.start > sb->span.start)
        return 1;
    if (sa->span.end < sb->span.end)
        return -1;
    if (sa->span.end > sb->span.end)
        return 1;
    return 0;
}

char *
styler_apply_offsets(const char *line, const struct styler_style *styles,
                     size_t style_count)
{
    if (line == NULL)
        return NULL;
    if (styles == NULL || style_count == 0)
        return strdup(line);

    size_t line_len = strlen(line);
    struct styler_style *ordered =
        malloc(style_count * sizeof(*ordered));
    if (ordered == NULL)
        return NULL;
    memcpy(ordered, styles, style_count * sizeof(*ordered));
    qsort(ordered, style_count, sizeof(*ordered), style_cmp_);

    struct strbuf out = STRBUF_INIT;
    size_t cursor = 0;

    for (size_t i = 0; i < style_count; ++i) {
        size_t start = ordered[i].span.start;
        size_t end   = ordered[i].span.end;
        if (start > line_len)
            start = line_len;
        if (end > line_len)
            end = line_len;
        if (end < start)
            end = start;
        if (start < cursor) {
            errno = EINVAL;
            strbuf_free(&out);
            free(ordered);
            return NULL;
        }

        if (strbuf_append(&out, line + cursor, start - cursor) != 0) {
            strbuf_free(&out);
            free(ordered);
            return NULL;
        }

        size_t segment_len = end - start;
        char *clean =
            strip_styles_slice_(line + start, segment_len, NULL);
        if (clean == NULL) {
            strbuf_free(&out);
            free(ordered);
            return NULL;
        }

        const char *enter = ordered[i].enter_seq;
        const char *exit  = ordered[i].exit_seq;
        size_t enter_len  = enter ? strlen(enter) : 0;
        size_t exit_len   = exit ? strlen(exit) : 0;
        size_t clean_len  = strlen(clean);

        int ok = 1;
        ok &= strbuf_append(&out, enter, enter_len) == 0;
        ok &= strbuf_append(&out, clean, clean_len) == 0;
        ok &= strbuf_append(&out, exit, exit_len) == 0;
        free(clean);
        if (!ok) {
            strbuf_free(&out);
            free(ordered);
            return NULL;
        }

        cursor = end;
    }

    if (strbuf_append(&out, line + cursor, line_len - cursor) != 0) {
        strbuf_free(&out);
        free(ordered);
        return NULL;
    }

    free(ordered);
    char *result = strbuf_detach(&out);
    strbuf_free(&out);
    return result;
}
