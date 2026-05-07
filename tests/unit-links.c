// RUN: %x
#include "links.h"
#include "support/osc8_samples.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_link_offsets(void)
{
    const char sample[] =
        "\x1b]8;;https://example.com\x1b\\Link\x1b]8;;\x1b\\\n";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    assert(iter.count == 1);
    const struct link_span *span = links_get(&iter, 0);
    assert(span != NULL);
    const char *text = strstr(sample, "Link");
    assert(text != NULL);
    size_t text_offset = (size_t)(text - sample);
    assert(span->row == 0);
    assert(span->row_offset == 0);
    assert(span->indices.start == text_offset);
    assert(span->indices.end == text_offset + strlen("Link"));
    assert(span->columns.start == 0);
    assert(span->columns.end == strlen("Link"));
    link_iter_free(&iter);
}

static void
test_parse_simple_snapshot(void)
{
    struct offscr_view view = {
        .data      = OSC8_SAMPLE_TWO_LINKS_,
        .len       = strlen(OSC8_SAMPLE_TWO_LINKS_),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    assert(iter.count == 2);
    assert(strcmp(iter.spans[0].link, "https://first.example") == 0);
    assert(strcmp(iter.spans[0].text, "First") == 0);
    assert(iter.spans[0].kind == LINKS_KIND_OSC8);
    assert(strcmp(iter.spans[1].link, "https://second.example") == 0);
    assert(strcmp(iter.spans[1].text, "Second") == 0);
    assert(iter.spans[1].kind == LINKS_KIND_OSC8);
    link_iter_free(&iter);
}

static void
test_parse_positions(void)
{
    const char sample[] =
        "\x1b]8;;https://pos.example\x1b\\First\x1b]8;;\x1b\\\n"
        "Next line\x1b]8;;https://next.example\x1b\\Second\x1b]8;;\x1b\\";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    assert(iter.count == 2);

    assert(iter.spans[0].row == 0);
    assert(iter.spans[0].columns.start == 0);
    assert(iter.spans[0].columns.end == strlen("First"));
    assert(iter.spans[0].kind == LINKS_KIND_OSC8);

    assert(iter.spans[1].row == 1);
    assert(iter.spans[1].columns.start == strlen("Next line"));
    assert(iter.spans[1].columns.end == strlen("Next line") + strlen("Second"));
    assert(iter.spans[1].kind == LINKS_KIND_OSC8);
    link_iter_free(&iter);
}

static void
test_iter_match_and_next(void)
{
    const char sample[] =
        "\x1b]8;;https://dup.example\x1b\\A\x1b]8;;\x1b\\ "
        "\x1b]8;;https://dup.example\x1b\\B\x1b]8;;\x1b\\ "
        "\x1b]8;;https://other.example\x1b\\C\x1b]8;;\x1b\\";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    assert(iter.count == 3);
    assert(links_ok(&iter));

    size_t match_len = 0;
    size_t *matches  = links_match(&iter, &match_len);
    assert(matches != NULL);
    assert(match_len == 2);
    assert(matches[0] == 0);
    assert(matches[1] == 1);
    free(matches);

    assert(links_next(&iter));
    assert(iter.index == 2);
    matches = links_match(&iter, &match_len);
    assert(match_len == 1 && matches[0] == 2);
    free(matches);

    assert(!links_next(&iter));
    assert(iter.index == iter.count);

    assert(links_prev(&iter));
    assert(iter.index == 2);

    link_iter_free(&iter);
}

static void
test_parse_multiple_modes(void)
{
    const char sample[] =
        "\x1b]8;;https://example.com\x1b\\Link\x1b]8;;\x1b\\\n"
        "printf(3) call\n";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    size_t osc_count = iter.count;
    assert(parse_links(&view, LINKS_KIND_MAN, &iter) == 0);
    assert(iter.count >= osc_count);
    link_iter_free(&iter);
}

static void
test_mixed_link_order(void)
{
    const char sample[] =
        "printf(3) only\n"
        "\x1b]8;;https://example.com\x1b\\Later\x1b]8;;\x1b\\\n";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_OSC8, &iter) == 0);
    assert(parse_links(&view, LINKS_KIND_MAN, &iter) == 0);
    assert(iter.count == 2);
    assert(iter.spans[0].kind == LINKS_KIND_MAN);
    assert(iter.spans[0].row == 0);
    assert(strcmp(iter.spans[0].link, "man://printf.3") == 0);
    assert(iter.spans[1].kind == LINKS_KIND_OSC8);
    assert(iter.spans[1].row == 1);
    assert(strcmp(iter.spans[1].link, "https://example.com") == 0);
    link_iter_free(&iter);
}

static void
test_colored_man_token(void)
{
    const char sample[]     = "\x1b[1;94mwordexp(3)\x1b[0m and others";
    struct offscr_view view = {
        .data      = sample,
        .len       = strlen(sample),
        .truncated = 0,
    };
    struct link_iter iter = {0};
    assert(parse_links(&view, LINKS_KIND_MAN, &iter) == 0);
    assert(iter.count >= 1);
    const struct link_span *span = links_get(&iter, 0);
    assert(span != NULL);
    assert(strcmp(span->text, "wordexp(3)") == 0);
    assert(strcmp(span->link, "man://wordexp.3") == 0);
    assert(span->kind == LINKS_KIND_MAN);
    link_iter_free(&iter);
}

int
main(void)
{
    test_parse_simple_snapshot();
    test_parse_positions();
    test_iter_match_and_next();
    test_parse_multiple_modes();
    test_mixed_link_order();
    test_colored_man_token();
    test_link_offsets();
    puts("links tests OK");
    return 0;
}
