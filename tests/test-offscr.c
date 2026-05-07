#include "offscr.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Helper to write the full payload to a pipe before closing it. */
static void
write_payload_(int fd, const char *data)
{
	size_t len = strlen(data);
	ssize_t wrote = write(fd, data, len);
	assert(wrote == (ssize_t)len);
}

/* Ensure capturing from a pipe collects the complete snapshot. */
static void
test_capture_reads_full_payload_(void)
{
	int pipefd[2];
	assert(pipe(pipefd) == 0);

	struct offscr_ctx *ctx = offscr_new(NULL);
	assert(ctx != NULL);

	write_payload_(pipefd[1], "hello world");
	close(pipefd[1]);

	int rc = offscr_capture(ctx, pipefd[0]);
	assert(rc == OFFSCR_CAPTURE_OK);
	struct offscr_view view = offscr_view(ctx);
	assert(view.len == strlen("hello world"));
	assert(memcmp(view.data, "hello world", view.len) == 0);
	assert(view.truncated == 0);

	close(pipefd[0]);
	offscr_free(ctx);
}

/* Confirm a timeout leaves the previous snapshot untouched. */
static void
test_capture_timeout_(void)
{
	int pipefd[2];
	assert(pipe(pipefd) == 0);

	struct offscr_opts opts = {
		.max_bytes = 1024,
		.first_byte_timeout_ms = 25,
		.next_byte_timeout_ms = 25,
	};
	struct offscr_ctx *ctx = offscr_new(&opts);
	assert(ctx != NULL);

	/* Seed the snapshot with known content. */
	write_payload_(pipefd[1], "seed");
	close(pipefd[1]);
	assert(offscr_capture(ctx, pipefd[0]) == OFFSCR_CAPTURE_OK);
	struct offscr_view seeded = offscr_view(ctx);
	assert(seeded.len == 4);
	assert(memcmp(seeded.data, "seed", 4) == 0);
	close(pipefd[0]);

	/* Re-open the pipe with no writer activity to trigger timeout. */
	assert(pipe(pipefd) == 0);

	int rc = offscr_capture(ctx, pipefd[0]);
	assert(rc == OFFSCR_CAPTURE_TIMEOUT);
	struct offscr_view view = offscr_view(ctx);
	assert(view.len == 4);
	assert(memcmp(view.data, "seed", 4) == 0);
	assert(view.truncated == 0);

	close(pipefd[0]);
	close(pipefd[1]);
	offscr_free(ctx);
}

/* Verify the capture honours max_bytes and reports truncation. */
static void
test_capture_respects_limit_(void)
{
	int pipefd[2];
	assert(pipe(pipefd) == 0);

	struct offscr_opts opts = {
		.max_bytes = 5,
		.first_byte_timeout_ms = 100,
		.next_byte_timeout_ms = 100,
		.max_lines = 0,
	};
	struct offscr_ctx *ctx = offscr_new(&opts);
	assert(ctx != NULL);

	write_payload_(pipefd[1], "exceed");
	close(pipefd[1]);

	int rc = offscr_capture(ctx, pipefd[0]);
	assert(rc == OFFSCR_CAPTURE_OK);
	struct offscr_view view = offscr_view(ctx);
	assert(view.len == 5);
	assert(memcmp(view.data, "excee", 5) == 0);
	assert(view.truncated != 0);

	offscr_reset(ctx);
	view = offscr_view(ctx);
	assert(view.len == 0);
	assert(view.truncated == 0);

	close(pipefd[0]);
	offscr_free(ctx);
}

/* Verify the capture honours a line limit and truncates accordingly. */
static void
test_capture_respects_line_limit_(void)
{
	int pipefd[2];
	assert(pipe(pipefd) == 0);

	struct offscr_opts opts = {
		.max_bytes = 0,
		.first_byte_timeout_ms = 100,
		.next_byte_timeout_ms = 100,
		.max_lines = 2,
	};
	struct offscr_ctx *ctx = offscr_new(&opts);
	assert(ctx != NULL);

	write_payload_(pipefd[1], "one\n"
				  "two\n"
				  "three\n");
	close(pipefd[1]);

	int rc = offscr_capture(ctx, pipefd[0]);
	assert(rc == OFFSCR_CAPTURE_OK);
	struct offscr_view view = offscr_view(ctx);
	assert(view.len == strlen("one\ntwo\n"));
	assert(memcmp(view.data, "one\ntwo\n", view.len) == 0);
	assert(view.truncated != 0);

	close(pipefd[0]);
	offscr_free(ctx);
}

static void
test_offscr_extract_ignores_cursor_move(void)
{
	const char sample[] = "\x1b[Afoo";
	struct offscr_view view = {
		.data = sample,
		.len  = strlen(sample),
	};
	char *line = offscr_extract(&view, 0);
	assert(line != NULL);
	assert(strcmp(line, "foo") == 0);
	free(line);
}

/* Verify drain mode consumes input without storing a view. */
static void
test_capture_drain_discards_snapshot_(void)
{
	int pipefd[2];
	assert(pipe(pipefd) == 0);

	struct offscr_opts opts = {
		.first_byte_timeout_ms = 50,
		.next_byte_timeout_ms = 50,
		.drain = true,
	};
	struct offscr_ctx *ctx = offscr_new(&opts);
	assert(ctx != NULL);

	write_payload_(pipefd[1], "drain me");
	close(pipefd[1]);

	assert(offscr_capture(ctx, pipefd[0]) == OFFSCR_CAPTURE_OK);
	struct offscr_view view = offscr_view(ctx);
	assert(view.len == 0);
	assert(view.truncated == 0);

	close(pipefd[0]);
	offscr_free(ctx);
}

int
main(void)
{
	test_capture_reads_full_payload_();
	test_capture_timeout_();
	test_capture_respects_limit_();
	test_capture_respects_line_limit_();
	test_offscr_extract_ignores_cursor_move();
	test_capture_drain_discards_snapshot_();
	puts("offscr tests OK");
	return 0;
}
