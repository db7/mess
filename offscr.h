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

#include <stddef.h>

/*
 * Result codes returned by offscr_capture().
 * A zero result means a snapshot was captured; a positive value indicates
 * a recoverable timeout; negative values map to standard errno failures.
 */
enum offscr_status { OFFSCR_CAPTURE_OK = 0, OFFSCR_CAPTURE_TIMEOUT = 1 };

/*
 * Tunables for an off-screen capture context.
 * max_bytes limits how much data is stored per snapshot (0 = unlimited),
 * capture_timeout_ms defines how long poll() waits for initial bytes
 * before reporting a timeout (0 = block indefinitely), and max_lines
 * optionally caps the number of newline-terminated lines copied (0 = no cap).
 */
struct offscr_opts {
    size_t max_bytes;
    unsigned int capture_timeout_ms;
    size_t max_lines;
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
 * Passing NULL applies sensible defaults (64KiB buffer, 250ms timeout).
 * Returns NULL on allocation failure.
 */
struct offscr_ctx *offscr_new(const struct offscr_opts *);

/*
 * Capture a snapshot from the child PTY identified by child_fd.
 * On success the buffered view is replaced with fresh bytes and
 * OFFSCR_CAPTURE_OK is returned. If no data becomes available before
 * the timeout expires, OFFSCR_CAPTURE_TIMEOUT is returned and the
 * previous snapshot is left untouched. On error, -1 is returned and
 * errno conveys the failure reason.
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
 * Release storage associated with the context, invalidating any extant views.
 */
void offscr_free(struct offscr_ctx *);

#endif /* MESS_OFFSCR_H */
