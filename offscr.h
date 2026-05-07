/*
 * Off-screen snapshot helper for mess.
 * ------------------------------------
 *
 * The offscr module collects child PTY output long enough to assemble a
 * screen snapshot that other components (e.g., navigation) can analyse without
 * managing file descriptors directly.
 */
#ifndef MESS_OFFSCR_H
#define MESS_OFFSCR_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Result codes returned by offscr_capture().
 * A zero result means a snapshot was captured; a positive value indicates
 * a recoverable timeout; negative values map to standard errno failures.
 */
enum offscr_status { OFFSCR_CAPTURE_OK = 0, OFFSCR_CAPTURE_TIMEOUT = 1 };

/*
 * Tunables for an off-screen capture context.
 * max_bytes limits how much data is stored per snapshot (0 = unlimited).
 * first_byte_timeout_ms caps how long capture waits for the very first byte
 * of a snapshot (0 = block indefinitely, otherwise default to 1000ms).
 * next_byte_timeout_ms controls the inter-byte wait once data is flowing
 * (0 = block indefinitely, otherwise default to 250ms). max_lines optionally
 * caps the number of newline-terminated lines copied (0 = no cap), and drain
 * toggles whether capture buffers the stream (false) or simply drains it
 * without retaining a snapshot (true).
 */
struct offscr_opts {
    size_t max_bytes;
    unsigned int first_byte_timeout_ms;
    unsigned int next_byte_timeout_ms;
    size_t max_lines;
    bool drain;
};

/*
 * Immutable view of the most recent snapshot.
 * data points to the captured buffer, len reports the number of bytes
 * stored, and truncated signals whether max_bytes clipped the capture.
 */
struct offscr_view {
    const char *data;
    size_t len;
    int truncated;
};

struct offscr_ctx;

/*
 * Allocate a snapshot context configured with the supplied options.
 * Passing NULL applies sensible defaults (64KiB buffer, 1s first-byte timeout,
 * 250ms inter-byte timeout).
 * Returns NULL on allocation failure.
 */
struct offscr_ctx *offscr_new(const struct offscr_opts *);

/*
 * Capture a snapshot from the child PTY identified by child_fd.
 * On success the buffered view is replaced with fresh bytes and
 * OFFSCR_CAPTURE_OK is returned. If no data becomes available before
 * the initial timeout (or the inter-byte timeout once data is flowing),
 * OFFSCR_CAPTURE_TIMEOUT is returned and the previous snapshot is left
 * untouched. When drain mode is enabled the data is consumed without being
 * stored. On error, -1 is returned and errno conveys the failure reason.
 */
int offscr_capture(struct offscr_ctx *, int child_fd);

/*
 * Reset the buffered snapshot without altering configuration.
 * After calling this, offscr_view() reports an empty snapshot.
 */
void offscr_reset(struct offscr_ctx *);

/*
 * Return a lightweight view describing the most recent capture.
 * The pointer remains valid until the next capture, reset, or free.
 */
struct offscr_view offscr_view(const struct offscr_ctx *);

/*
 * Return a string containing the visible characters of `row` from `view`.
 * Escapes such as CSI/OSC are preserved; cursor-movement sequences are dropped
 * and logged so callers know the snapshot may omit overwritten cells.
 */
char *offscr_extract(const struct offscr_view *, size_t row);

/*
 * Convenience helper to log up to max_rows from view via log_debug().
 * Passing max_rows = 0 dumps until offscr_extract() returns NULL.
 */
void offscr_dump(const struct offscr_view *, size_t max_rows);

/*
 * Drop the first row from the snapshot stored in ctx, if any.
 */
void offscr_forget_first_row(struct offscr_ctx *);

/*
 * Release storage associated with the context, invalidating any extant views.
 */
void offscr_free(struct offscr_ctx *);

#endif /* MESS_OFFSCR_H */
