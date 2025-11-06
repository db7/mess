#include "readq.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

// Initialize the queue descriptor used for PTY reads.
void
readq_init(struct readq *queue, int fd)
{
    queue->start      = 0;
    queue->end        = 0;
    queue->last_start = 0;
    queue->last_len   = 0;
    queue->fd         = fd;
}

// Refill the buffer from the file descriptor
bool
readq_refill(struct readq *queue)
{
    const ssize_t capacity = READQ_SIZE - 1;
    ssize_t nread          = queue->end - queue->start;
    assert(nread >= 0);

    if (queue->start > 0) {
        // Move unread data to the beginning of the buffer
        memmove(queue->buffer, queue->buffer + queue->start, nread);

        // Update buffer pointers
        queue->start              = 0;
        queue->end                = nread;
        queue->buffer[queue->end] = '\0';
    }

    queue->last_len = 0;

    if (nread == capacity)
        return false;

    // Read new data into the remaining space in the buffer
    nread = read(queue->fd, queue->buffer + queue->end, capacity - nread);
    if (nread <= 0)
        return false;

    queue->last_start = queue->end;
    queue->last_len   = (size_t)nread;
    queue->end += nread;
    queue->buffer[queue->end] = '\0';
    return true;
}

// Get the current byte from the buffer
// Returns -1 if no more data is available and the buffer can't be refilled
int
readq_pick_byte(struct readq *queue)
{
    if (queue->start >= queue->end) {
        // Buffer is empty, try to refill it
        if (1 || !readq_refill(queue)) {
            return -1; // No more data available
        }
    }

    // Return the next byte and advance the start index
    return queue->buffer[queue->start];
}

// Get the next byte from the buffer
// Returns -1 if no more data is available and the buffer can't be refilled
int
readq_get_next(struct readq *queue)
{
    int b = readq_pick_byte(queue);
    if (b != -1)
        queue->start++;
    return b;
}

// Check if the queue is empty and cannot be refilled
bool
readq_is_empty(struct readq *queue)
{
    // If no unread data and refill fails, the queue is empty
    return readq_pick_byte(queue) == -1;
}

const char *
readq_last_ptr(const struct readq *queue)
{
    if (queue->last_len == 0)
        return NULL;
    return queue->buffer + queue->last_start;
}

size_t
readq_last_len(const struct readq *queue)
{
    return queue->last_len;
}

size_t
readq_available_bytes(const struct readq *queue)
{
    if (!queue)
        return 0;
    return (queue->end >= queue->start) ? (queue->end - queue->start) : 0;
}
