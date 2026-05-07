#include "offscr.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct offscr_ctx {
    struct offscr_opts opts;
    char *buf;
    size_t len;
    size_t cap;
    int truncated;
    size_t lines;
};

/* Default snapshot budget when no max_bytes is configured. */
static const size_t default_soft_limit_ = 64 * 1024;
/* Default timeout (ms) used when the caller does not override it. */
static const unsigned int default_timeout_ms_ = 250;

/*
 * Reason: Ensure the internal buffer is large enough for the next write,
 * growing exponentially to amortise allocation cost.
 */
static int
ensure_capacity_(struct offscr_ctx *ctx, size_t needed)
{
    if (needed <= ctx->cap)
        return 0;

    size_t limit = ctx->opts.max_bytes ? ctx->opts.max_bytes : SIZE_MAX;
    size_t next  = ctx->cap ? ctx->cap : 4096;
    while (next < needed) {
        if (next > limit / 2) {
            next = limit;
            break;
        }
        next *= 2;
        if (next > limit)
            next = limit;
    }

    if (next < needed)
        return -1;

    char *nbuf = realloc(ctx->buf, next);
    if (nbuf == NULL)
        return -1;

    ctx->buf = nbuf;
    ctx->cap = next;
    return 0;
}

/*
 * Reason: Clamp the number of bytes copied based on the configured limit.
 */
static size_t
allowed_copy_size_(const struct offscr_ctx *ctx, size_t incoming)
{
    if (ctx->opts.max_bytes == 0)
        return incoming;
    if (ctx->len >= ctx->opts.max_bytes)
        return 0;
    size_t remaining = ctx->opts.max_bytes - ctx->len;
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
            ctx->len       = 0;
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
            if (ensure_capacity_(ctx, ctx->len + copy_len) != 0)
                return -1;
            memcpy(ctx->buf + ctx->len, chunk, copy_len);
            ctx->len += copy_len;
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
    ctx->len       = 0;
    ctx->truncated = 0;
    ctx->lines     = 0;
}

struct offscr_view
offscr_view(const struct offscr_ctx *ctx)
{
    struct offscr_view view = {0};
    if (ctx == NULL)
        return view;
    view.data      = ctx->buf;
    view.len       = ctx->len;
    view.truncated = ctx->truncated;
    return view;
}

void
offscr_free(struct offscr_ctx *ctx)
{
    if (ctx == NULL)
        return;
    free(ctx->buf);
    free(ctx);
}
