#ifndef MESS_ANSI_H
#define MESS_ANSI_H

#include <stdbool.h>
#include <stddef.h>

enum ansi_seq_type {
    ANSI_SEQ_NONE = 0,
    ANSI_SEQ_CSI,
    ANSI_SEQ_OSC,
    ANSI_SEQ_ESC,
};

struct ansi_seq {
    enum ansi_seq_type type;
    size_t end;
    unsigned char final;
    bool complete;
};

struct ansi_seq ansi_parse_seq(const char *data, size_t len, size_t idx);
size_t ansi_skip_seq(const char *data, size_t len, size_t idx);
bool ansi_seq_is_cursor_movement(const struct ansi_seq *seq);

#endif /* MESS_ANSI_H */
