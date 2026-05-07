/*
 * Simple growable string buffer used throughout mess.
 */
#ifndef MESS_STRBUF_H
#define MESS_STRBUF_H

#include <stddef.h>

struct strbuf {
    char *data;
    size_t len;
    size_t cap;
};

#define STRBUF_INIT \
    {               \
        NULL, 0, 0  \
    }

void strbuf_init(struct strbuf *, size_t initial_cap);
void strbuf_reset(struct strbuf *);
void strbuf_free(struct strbuf *);

int strbuf_reserve(struct strbuf *, size_t extra);
int strbuf_append(struct strbuf *, const char *data, size_t len);
int strbuf_push(struct strbuf *, char ch);
int strbuf_append_str(struct strbuf *, const char *str);

char *strbuf_detach(struct strbuf *);
const char *strbuf_data(const struct strbuf *);
size_t strbuf_len(const struct strbuf *);

#endif /* MESS_STRBUF_H */
