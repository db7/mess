#include "nav.h"
#include "readq.h"
#include "support/osc8_samples.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char README_HEAD_SAMPLE_[] =
    "mess -- more or less with links\n"
    "===============================\n"
    "\n"
    "`mess` a wrapper program that adds link navigation support to pagers, eg,\n"
    "`less`. It watches the output of the underlying pager for OSC8 sequences\n"
    "and lets you tab through the links.\n"
    "\n"
    "`mess` relies on external tools such as `less`, `lynx`, and `lowdown`.\n"
    "\n"
    "When the input is a man page (e.g. a `foo.1` file, `man://` link, or the\n"
    "output of `man -P cat`), `mess` scans the rendered text for references\n"
    "such as `printf(3)` or `socket(2)` and exposes them as navigation targets.\n";

static void
feed_(struct readq *queue, const char *data)
{
    size_t len = strlen(data);
    size_t unread = queue->end - queue->start;
    memmove(queue->buffer, queue->buffer + queue->start, unread);
    queue->start = 0;
    queue->end = unread;
    assert(unread + len < READQ_SIZE);
    memcpy(queue->buffer + queue->end, data, len);
    queue->end += len;
    queue->buffer[queue->end] = '\0';
}

static void
test_parses_simple_sequence_(void)
{
    nav_reset();
    struct readq queue;
    readq_init(&queue, -1);

    feed_(&queue, OSC8_SAMPLE_SIMPLE_);
    nav_process_output(&queue);

    assert(nav_link_count() == 1);
    assert(strcmp(nav_link_at(0), "https://example.com") == 0);
    assert(strcmp(nav_text_at(0), "Example") == 0);
}

static void
test_parsing_survives_split_sequences_(void)
{
    nav_reset();
    struct readq queue;
    readq_init(&queue, -1);

    feed_(&queue, OSC8_SAMPLE_SPLIT_PART1_);
    nav_process_output(&queue);
    assert(nav_link_count() == 0);

    feed_(&queue, OSC8_SAMPLE_SPLIT_PART2_);
    nav_process_output(&queue);
    assert(nav_link_count() == 1);
    assert(strcmp(nav_link_at(0), "https://split.test") == 0);
    assert(strcmp(nav_text_at(0), "Split Link") == 0);
}

static void
test_malformed_sequence_does_not_allocate_(void)
{
    nav_reset();
    struct readq queue;
    readq_init(&queue, -1);

    feed_(&queue, OSC8_SAMPLE_MALFORMED_);
    nav_process_output(&queue);

    assert(nav_link_count() == 0);
}

static void
test_man_parser_handles_backspaces_(void)
{
    nav_reset();
    nav_set_mode(NAV_MODE_MAN);
    struct readq queue;
    readq_init(&queue, -1);
    const char sample[] = "p\bp"
                          "r\br"
                          "i\bi"
                          "n\bn"
                          "t\bt"
                          "f\bf(3) and more";
    feed_(&queue, sample);
    nav_process_output(&queue);
    assert(nav_link_count() == 1);
    assert(strcmp(nav_link_at(0), "man://printf.3") == 0);
    assert(strcmp(nav_text_at(0), "printf(3)") == 0);
    nav_set_mode(NAV_MODE_OSC8);
}

static void
test_ring_buffer_replaces_oldest_(void)
{
    nav_reset();
    struct readq queue;
    readq_init(&queue, -1);

    char fragment[256];
    for (int i = 0; i < MAX_URLS + 3; ++i) {
        snprintf(fragment, sizeof(fragment),
                 "\x1b]8;;https://ring.test/%d\x1b\\Link %d\x1b]8;;\x1b\\", i, i);
        feed_(&queue, fragment);
        nav_process_output(&queue);
        queue.start = queue.end = 0;
    }

    assert(nav_link_count() == MAX_URLS);
    assert(strcmp(nav_link_at(0), "https://ring.test/3") == 0);
    assert(strcmp(nav_text_at(0), "Link 3") == 0);
    assert(strcmp(nav_link_at(MAX_URLS - 1), "https://ring.test/12") == 0);
}

static void
test_readme_head_parses_man_links_(void)
{
    nav_reset();
    nav_set_mode(NAV_MODE_MAN);
    struct readq queue;
    readq_init(&queue, -1);

    feed_(&queue, README_HEAD_SAMPLE_);
    ssize_t chunk = nav_process_output(&queue);
    assert(chunk > 0);
    assert(nav_link_count() >= 2);

    bool printf_found = false;
    bool socket_found = false;
    for (int i = 0; i < nav_link_count(); ++i) {
        const char *link = nav_link_at(i);
        if (!link)
            continue;
        if (strstr(link, "printf.3"))
            printf_found = true;
        if (strstr(link, "socket.2"))
            socket_found = true;
    }
    assert(printf_found && socket_found);

    char render_buf[READQ_SIZE * 3];
    size_t rendered =
        nav_render_highlighted(queue.buffer, (size_t)chunk, render_buf,
                               sizeof(render_buf));
    assert(rendered > 0);
    assert(strstr(render_buf, "printf(3)") != NULL);
    assert(strstr(render_buf, "socket(2)") != NULL);
    assert(strstr(render_buf, "94m") == NULL);

    nav_set_mode(NAV_MODE_OSC8);
}

int
main(void)
{
    test_parses_simple_sequence_();
    test_parsing_survives_split_sequences_();
    test_malformed_sequence_does_not_allocate_();
    test_man_parser_handles_backspaces_();
    test_ring_buffer_replaces_oldest_();
    test_readme_head_parses_man_links_();
    puts("parser tests OK");
    return 0;
}
