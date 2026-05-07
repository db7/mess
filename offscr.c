#include "offscr.h"

#include "ansi.h"
#include "log.h"
#include "strbuf.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct offscr_ctx {
    unsigned int first_byte_timeout_ms;
    unsigned int next_byte_timeout_ms;
    bool drain;
    struct strbuf buf;
};

static const size_t default_soft_limit_ = 64 * 1024;
static const unsigned int default_first_timeout_ms_ = 1000;
static const unsigned int default_next_timeout_ms_  = 250;

static int poll_timeout_value_(unsigned int timeout_ms);

static size_t
allowed_copy_size_(const struct offscr_ctx *ctx, size_t incoming)
{
    size_t current = strbuf_len(&ctx->buf);
    if (current >= default_soft_limit_)
        return 0;
    size_t remaining = default_soft_limit_ - current;
    return incoming > remaining ? remaining : incoming;
}

static size_t
strip_cursor_movements_(char *chunk, size_t len)
{
    size_t in  = 0;
    size_t out = 0;
    while (in < len) {
        if (chunk[in] == '\033' && in + 1 < len && chunk[in + 1] == '[') {
            struct ansi_seq seq = ansi_parse_seq(chunk, len, in);
            if (!seq.complete) {
                while (in < len)
                    chunk[out++] = chunk[in++];
                break;
            }
            if (ansi_seq_is_cursor_movement(&seq)) {
                in = seq.end;
                continue;
            }
            while (in < seq.end)
                chunk[out++] = chunk[in++];
            continue;
        }
        chunk[out++] = chunk[in++];
    }
    return out;
}

struct offscr_ctx *
offscr_new(const struct offscr_opts *opts)
{
    struct offscr_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    unsigned int first_wait = default_first_timeout_ms_;
    unsigned int next_wait  = default_next_timeout_ms_;
    bool drain              = false;
    if (opts) {
        if (opts->first_byte_timeout_ms != 0)
            first_wait = opts->first_byte_timeout_ms;
        if (opts->next_byte_timeout_ms != 0)
            next_wait = opts->next_byte_timeout_ms;
        drain = opts->drain;
    }
    ctx->first_byte_timeout_ms = first_wait;
    ctx->next_byte_timeout_ms  = next_wait;
    ctx->drain                 = drain;
    strbuf_init(&ctx->buf, 0);

    return ctx;
}

int
offscr_capture(struct offscr_ctx *ctx, int fd)
{
    if (ctx == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfd = {
        .fd     = fd,
        .events = POLLIN | POLLHUP | POLLERR,
    };
    const int first_timeout =
        poll_timeout_value_(ctx->first_byte_timeout_ms);
    const int next_timeout =
        poll_timeout_value_(ctx->next_byte_timeout_ms);
    const bool draining = ctx->drain;
    int timeout         = first_timeout;

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

        ssize_t nread = read(fd, chunk, sizeof(chunk));
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

        size_t filtered = strip_cursor_movements_(chunk, (size_t)nread);
        if (filtered == 0) {
            timeout = saw_data ? next_timeout : first_timeout;
            continue;
        }
        nread = (ssize_t)filtered;

        if (!saw_data) {
            strbuf_reset(&ctx->buf);
            saw_data       = 1;
            timeout        = next_timeout;
        }

        size_t copy_len = allowed_copy_size_(ctx, (size_t)nread);

        if (copy_len > 0 && !draining) {
            if (strbuf_append(&ctx->buf, chunk, copy_len) != 0)
                return -1;
        }

        if ((size_t)nread > copy_len)
            return OFFSCR_CAPTURE_OK;

        timeout = next_timeout;
    }
}

struct offscr_view
offscr_view(const struct offscr_ctx *ctx)
{
    struct offscr_view view = {0};
    if (ctx == NULL)
        return view;
    view.data      = strbuf_data(&ctx->buf);
    view.len       = strbuf_len(&ctx->buf);
    return view;
}

char *
offscr_extract(const struct offscr_view *view, size_t target_row)
{
    if (view == NULL || view->data == NULL)
        return NULL;

    size_t row         = 0;
    size_t idx         = 0;
    struct strbuf line = STRBUF_INIT;
    if (strbuf_reserve(&line, 0) != 0)
        return NULL;

    int warned_cursor = 0;

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
            if (row == target_row) {
                if (idx + 1 < view->len && view->data[idx + 1] == '\n') {
                    idx++;
                    continue;
                }
                strbuf_reset(&line);
            }
            idx++;
            continue;
        }
        if (ch == '\033') {
            size_t seq_start = idx;
            struct ansi_seq seq = ansi_parse_seq(view->data, view->len, idx);
            if (row == target_row) {
                if (ansi_seq_is_cursor_movement(&seq)) {
                    if (!warned_cursor) {
                        log_warn("offscr: ignoring cursor movement on row %zu",
                                 target_row);
                        warned_cursor = 1;
                    }
                    idx = seq.end;
                    continue;
                }
                if (strbuf_append(&line, view->data + seq_start,
                                  seq.end - seq_start) != 0) {
                    strbuf_free(&line);
                    return NULL;
                }
            }
            idx = seq.end;
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

    if (row < target_row) {
        strbuf_free(&line);
        return NULL;
    }
    return strbuf_detach(&line);
}

void
offscr_forget_first_row(struct offscr_ctx *ctx)
{
    if (ctx == NULL || ctx->buf.data == NULL || ctx->buf.len == 0)
        return;

    size_t len   = ctx->buf.len;
    char *data   = ctx->buf.data;
    size_t start = 0;
    while (start < len) {
        char ch = data[start++];
        if (ch == '\n')
            break;
        if (ch == '\r') {
            if (start < len && data[start] == '\n') {
                start++;
                break;
            }
            continue;
        }
    }

    if (start >= len) {
        strbuf_reset(&ctx->buf);
        return;
    }

    size_t remaining = len - start;
    memmove(data, data + start, remaining);
    ctx->buf.len    = remaining;
    data[remaining] = '\0';
}

void
offscr_free(struct offscr_ctx *ctx)
{
    if (ctx == NULL)
        return;
    strbuf_free(&ctx->buf);
    free(ctx);
}

static int
poll_timeout_value_(unsigned int timeout_ms)
{
    return timeout_ms == 0 ? -1 : (int)timeout_ms;
}
