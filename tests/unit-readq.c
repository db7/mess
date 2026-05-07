// RUN: %x
#include "readq.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Helper to write the full buffer to the pipe in one go.
static void
write_all_(int fd, const char *data)
{
    size_t len      = strlen(data);
    ssize_t written = write(fd, data, len);
    assert(written == (ssize_t)len);
}

// Confirm that readq_refill keeps unread bytes intact between fills.
static void
test_refill_preserves_unread_bytes_(void)
{
    int fds[2];
    assert(pipe(fds) == 0);

    struct readq queue;
    readq_init(&queue, fds[0]);

    write_all_(fds[1], "hello");
    assert(readq_refill(&queue));
    assert(queue.start == 0);
    assert(queue.end == 5);
    assert(strncmp(queue.buffer, "hello", 5) == 0);
    assert(readq_last_len(&queue) == 5);
    const char *chunk = readq_last_ptr(&queue);
    assert(chunk != NULL && strncmp(chunk, "hello", 5) == 0);

    queue.start = 2; // leave "llo" unread
    write_all_(fds[1], " world");

    assert(readq_refill(&queue));
    assert(queue.start == 0);
    assert(queue.end == 9);
    assert(strcmp(queue.buffer, "llo world") == 0);
    assert(readq_last_len(&queue) == 6);
    chunk = readq_last_ptr(&queue);
    assert(chunk != NULL && strncmp(chunk, " world", 6) == 0);

    close(fds[1]);
    queue.start = queue.end;
    assert(!readq_refill(&queue));
    assert(readq_last_len(&queue) == 0);
    assert(readq_last_ptr(&queue) == NULL);

    close(fds[0]);
}

int
main(void)
{
    test_refill_preserves_unread_bytes_();
    puts("readq tests OK");
    return 0;
}
