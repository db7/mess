#include "readq.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

// Initialize the readq
void init_queue(readq *queue, int fd) {
  queue->start = 0;
  queue->end = 0;
  queue->fd = fd;
}

// Refill the buffer from the file descriptor
bool refill_queue(readq *queue) {
  const ssize_t capacity = READQ_SIZE - 1;
  ssize_t nread = queue->end - queue->start;
  assert(nread >= 0);

  if (queue->start > 0) {
    // Move unread data to the beginning of the buffer
    memmove(queue->buffer, queue->buffer + queue->start, nread);

    // Update buffer pointers
    queue->start = 0;
    queue->end = nread;
    queue->buffer[queue->end] = '\0';
  }

  if (nread == capacity)
    return false;

  // Read new data into the remaining space in the buffer
  nread = read(queue->fd, queue->buffer + queue->end, capacity - nread);
  if (nread <= 0)
    return false;

  queue->end += nread;
  queue->buffer[queue->end] = '\0';
  return true;
}

// Get the current byte from the buffer
// Returns -1 if no more data is available and the buffer can't be refilled
int pick_byte(readq *queue) {
  if (queue->start >= queue->end) {
    // Buffer is empty, try to refill it
    if (1 || !refill_queue(queue)) {
      return -1; // No more data available
    }
  }

  // Return the next byte and advance the start index
  return queue->buffer[queue->start];
}

// Get the next byte from the buffer
// Returns -1 if no more data is available and the buffer can't be refilled
int get_next_byte(readq *queue) {
  int b = pick_byte(queue);
  if (b != -1)
    queue->start++;
  return b;
}

// Check if the queue is empty and cannot be refilled
bool is_empty(readq *queue) {
  // If no unread data and refill fails, the queue is empty
  return pick_byte(queue) == -1;
}
