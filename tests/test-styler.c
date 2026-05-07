#include "styler.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_styler_basic_wrap(void)
{
	const char sample[] = "abcdef";
	struct styler_style style = {
		.span =
			{
				.start = 1,
				.end   = 4,
			},
		.enter_seq = "\x1b[7m",
		.exit_seq  = "\x1b[0m",
	};
	char *line = styler_apply_offsets(sample, &style, 1);
	assert(line != NULL);
	assert(strcmp(line, "a\x1b[7mbcd\x1b[0mef") == 0);
	free(line);
}

static void
test_styler_reapply_after_sgr(void)
{
	const char sample[] = "\x1b[1mfoo\x1b[0m";
	struct styler_style style = {
		.span =
			{
				.start = 0,
				.end   = strlen(sample),
			},
		.enter_seq = "\x1b[7m",
		.exit_seq  = "\x1b[0m",
	};
	char *line = styler_apply_offsets(sample, &style, 1);
	assert(line != NULL);
	assert(strcmp(line, "\x1b[7mfoo\x1b[0m") == 0);
	free(line);
}

static void
test_styler_strip_visible(void)
{
	const char sample[] = "\x1b[32mhello\x1b[0m world";
	char *plain = styler_strip(sample, NULL);
	assert(plain != NULL);
	assert(strcmp(plain, "hello world") == 0);
	free(plain);
}

static void
test_styler_multiple(void)
{
	const char sample[] = "link1 link2 link3";
	struct styler_style styles[2] = {
		{
			.span =
				{
					.start = 0,
					.end   = 5,
				},
			.enter_seq = "\x1b[31m",
			.exit_seq  = "\x1b[0m",
		},
		{
			.span =
				{
					.start = 12,
					.end   = 17,
				},
			.enter_seq = "\x1b[34m",
			.exit_seq  = "\x1b[0m",
		},
	};
	char *line = styler_apply_offsets(sample, styles, 2);
	assert(line != NULL);
	assert(strcmp(line, "\x1b[31mlink1\x1b[0m link2 \x1b[34mlink3\x1b[0m") == 0);
	free(line);
}

static void
test_styler_overlap_fails(void)
{
	const char sample[] = "abcdef";
	struct styler_style styles[2] = {
		{
			.span =
				{
					.start = 0,
					.end   = 3,
				},
			.enter_seq = "\x1b[31m",
			.exit_seq  = "\x1b[0m",
		},
		{
			.span =
				{
					.start = 2,
					.end   = 5,
				},
			.enter_seq = "\x1b[34m",
			.exit_seq  = "\x1b[0m",
		},
	};
	errno = 0;
	char *line = styler_apply_offsets(sample, styles, 2);
	assert(line == NULL);
	assert(errno == EINVAL);
}

int
main(void)
{
	test_styler_basic_wrap();
	test_styler_reapply_after_sgr();
	test_styler_strip_visible();
	test_styler_multiple();
	test_styler_overlap_fails();
	puts("styler tests OK");
	return 0;
}
