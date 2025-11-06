#include "nav.h"
#include "readq.h"
#include "support/osc8_samples.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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

int
main(void)
{
    test_parses_simple_sequence_();
    test_parsing_survives_split_sequences_();
    test_malformed_sequence_does_not_allocate_();
    test_ring_buffer_replaces_oldest_();
    puts("parser tests OK");
    return 0;
}
