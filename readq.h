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

void readq_init(struct readq *q, int fd);
bool readq_refill(struct readq *q);
int readq_pick_byte(struct readq *q);
int readq_get_next(struct readq *q);
bool readq_is_empty(struct readq *q);
const char *readq_last_ptr(const struct readq *q);
size_t readq_last_len(const struct readq *q);
size_t readq_available_bytes(const struct readq *);
static inline const char *
readq_data(const struct readq *q)
{
    return q->buffer + q->start;
}
static inline size_t
readq_len(const struct readq *q)
{
    return readq_available_bytes(q);
}
static inline void
readq_clear(struct readq *q)
{
    q->start = q->end = 0;
}

#endif /* READQ_H */
