#include "offscr.h"
#include "strbuf.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct offscr_ctx {
    struct offscr_opts opts;
    struct strbuf buf;
    int truncated;
    size_t lines;
};

/* Default snapshot budget when no max_bytes is configured. */
static const size_t default_soft_limit_ = 64 * 1024;

/* Default timeout (ms) used when the caller does not override it. */
static const unsigned int default_timeout_ms_ = 250;

/*
 * Reason: Clamp the number of bytes copied based on the configured limit.
 */
static size_t
allowed_copy_size_(const struct offscr_ctx *ctx, size_t incoming)
{
    if (ctx->opts.max_bytes == 0)
        return incoming;
    size_t current = strbuf_len(&ctx->buf);
    if (current >= ctx->opts.max_bytes)
        return 0;
    size_t remaining = ctx->opts.max_bytes - current;
    return incoming > remaining ? remaining : incoming;
}

struct offscr_ctx *
offscr_new(const struct offscr_opts *opts)
{
    struct offscr_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    size_t max_bytes     = default_soft_limit_;
    unsigned int timeout = default_timeout_ms_;
    size_t max_lines     = 0;

    if (opts != NULL) {
        if (opts->max_bytes != 0)
            max_bytes = opts->max_bytes;
        if (opts->capture_timeout_ms != 0)
            timeout = opts->capture_timeout_ms;
        max_lines = opts->max_lines;
    }

    ctx->opts.max_bytes          = max_bytes;
    ctx->opts.capture_timeout_ms = timeout;
    ctx->opts.max_lines          = max_lines;
    ctx->lines                   = 0;
    strbuf_init(&ctx->buf, 0);

    return ctx;
}

int
offscr_capture(struct offscr_ctx *ctx, int child_fd)
{
    if (ctx == NULL || child_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfd = {
        .fd     = child_fd,
        .events = POLLIN | POLLHUP | POLLERR,
    };
    int timeout = (int)ctx->opts.capture_timeout_ms;

    int saw_data = 0;
    char chunk[4096];

    for (;;) {
        int prc = poll(&pfd, 1, timeout);
        if (prc == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (prc == 0) {
            return saw_data ? OFFSCR_CAPTURE_OK : OFFSCR_CAPTURE_TIMEOUT;
        }

        ssize_t nread = read(child_fd, chunk, sizeof(chunk));
        if (nread == 0) {
            return saw_data ? OFFSCR_CAPTURE_OK : OFFSCR_CAPTURE_TIMEOUT;
        }
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return saw_data ? OFFSCR_CAPTURE_OK : OFFSCR_CAPTURE_TIMEOUT;
            }
            return -1;
        }

        if (!saw_data) {
            strbuf_reset(&ctx->buf);
            ctx->truncated = 0;
            ctx->lines     = 0;
        }
        saw_data = 1;

        size_t copy_len = allowed_copy_size_(ctx, (size_t)nread);
        size_t lines    = ctx->lines;
        size_t limit    = ctx->opts.max_lines;

        if (limit > 0) {
            if (lines >= limit) {
                ctx->truncated = 1;
                return OFFSCR_CAPTURE_OK;
            }
            size_t i = 0;
            for (; i < copy_len; ++i) {
                if (chunk[i] == '\n') {
                    lines++;
                    if (lines >= limit) {
                        ++i;
                        break;
                    }
                }
            }
            copy_len = i;
        }

        if (copy_len > 0) {
            if (strbuf_append(&ctx->buf, chunk, copy_len) != 0)
                return -1;
        }
        ctx->lines = lines;

        if ((size_t)nread > copy_len)
            ctx->truncated = 1;
        if (limit > 0 && ctx->lines >= limit)
            ctx->truncated = 1;

        if (ctx->truncated)
            return OFFSCR_CAPTURE_OK;

        timeout = (int)ctx->opts.capture_timeout_ms;
    }
}

void
offscr_reset(struct offscr_ctx *ctx)
{
    if (ctx == NULL)
        return;
    strbuf_reset(&ctx->buf);
    ctx->truncated = 0;
    ctx->lines     = 0;
}

struct offscr_view
offscr_view(const struct offscr_ctx *ctx)
{
    struct offscr_view view = {0};
    if (ctx == NULL)
        return view;
    view.data      = strbuf_data(&ctx->buf);
    view.len       = strbuf_len(&ctx->buf);
    view.truncated = ctx->truncated;
    return view;
}

static int
is_csi_cursor_movement_(unsigned char terminator)
{
    switch (terminator) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case 'f':
            return 1;
        default:
            return 0;
    }
}

char *
offscr_extract(const struct offscr_view *view, size_t target_row)
{
    if (view == NULL || view->data == NULL)
        return NULL;

    size_t row = 0;
    size_t idx = 0;
    struct strbuf line = STRBUF_INIT;
    if (strbuf_reserve(&line, 0) != 0)
        return NULL;
    int seen_cursor = 0;

    while (idx < view->len) {
        unsigned char ch = (unsigned char)view->data[idx];
        if (ch == '\b') {
            if (row == target_row && line.len > 0) {
                line.len--;
                line.data[line.len] = '\0';
            }
            idx++;
            continue;
        }
        if (ch == '\n') {
            if (row == target_row)
                break;
            row++;
            if (row > target_row)
                break;
            idx++;
            continue;
        }
        if (ch == '\r') {
            if (row == target_row)
                strbuf_reset(&line);
            idx++;
            continue;
        }
        if (ch == '\033') {
            size_t seq_start = idx;
            idx++;
            if (idx >= view->len)
                break;
            unsigned char next = (unsigned char)view->data[idx];
            size_t seq_end     = idx + 1;
            int cursor         = 0;
            if (next == '[') {
                while (seq_end < view->len) {
                    unsigned char term = (unsigned char)view->data[seq_end++];
                    if (term >= '@' && term <= '~') {
                        if (is_csi_cursor_movement_(term))
                            cursor = 1;
                        break;
                    }
                }
                if (seq_end > view->len)
                    seq_end = view->len;
            } else if (next == ']') {
                while (seq_end < view->len) {
                    unsigned char cur = (unsigned char)view->data[seq_end++];
                    if (cur == '\a')
                        break;
                    if (cur == '\033' && seq_end < view->len &&
                        (unsigned char)view->data[seq_end] == '\\') {
                        seq_end++;
                        break;
                    }
                }
            } else {
                seq_end = idx + 1;
            }
            if (row == target_row && !cursor) {
                if (strbuf_append(&line, view->data + seq_start,
                                  seq_end - seq_start) != 0) {
                    strbuf_free(&line);
                    return NULL;
                }
            } else if (row == target_row && cursor) {
                seen_cursor = 1;
                break;
            }
            idx = seq_end;
            continue;
        }
        if (row == target_row) {
            if (strbuf_push(&line, (char)ch) != 0) {
                strbuf_free(&line);
                return NULL;
            }
        }
        idx++;
    }

    if (row < target_row || seen_cursor) {
        strbuf_free(&line);
        return NULL;
    }
    return strbuf_detach(&line);
}

void
offscr_free(struct offscr_ctx *ctx)
{
    if (ctx == NULL)
        return;
    strbuf_free(&ctx->buf);
    free(ctx);
}
