#ifndef READQ_H
#define READQ_H

#include <stdbool.h>
#include <stddef.h>

#ifndef READQ_SIZE
#define READQ_SIZE 4096
#endif

struct readq {
    char buffer[READQ_SIZE + 1];
    size_t start;
    size_t end;
    size_t last_start;
    size_t last_len;
    int fd;
};

void readq_init(struct readq *queue, int fd);
bool readq_refill(struct readq *queue);
int readq_pick_byte(struct readq *queue);
int readq_get_next(struct readq *queue);
bool readq_is_empty(struct readq *queue);
const char *readq_last_read_ptr(const struct readq *queue);
size_t readq_last_read_len(const struct readq *queue);
size_t readq_available_bytes(const struct readq *queue);

#endif /* READQ_H */
