#ifndef READQ_H
#define READQ_H
#include <stdbool.h>
#include <unistd.h>

// Define the buffer size and refill threshold
#define READQ_SIZE 1024

// Structure to manage the buffer/queue
typedef struct {
    char buffer[READQ_SIZE + 1]; // The buffer to hold data
    size_t start;                   // Start index of unread data
    size_t end;                     // End index of unread data
    int fd;                         // File descriptor to read from
} readq;

// Initialize the readq
void init_queue(readq *queue, int fd);

// Refill the buffer from the file descriptor
// return false when EOF or empty
bool refill_queue(readq *queue);

// Get the next byte from the buffer
// Returns -1 if no more data is available and the buffer can't be refilled
int pick_byte(readq *queue);

int get_next_byte(readq *queue);

// Check if the queue is empty and cannot be refilled
bool is_empty(readq *queue);

#endif // READQ_H
