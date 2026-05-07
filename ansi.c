#include "ansi.h"

struct ansi_seq
ansi_parse_seq(const char *data, size_t len, size_t idx)
{
    struct ansi_seq seq = {
        .type     = ANSI_SEQ_NONE,
        .end      = idx < len ? idx + 1 : idx,
        .final    = 0,
        .complete = false,
    };
    if (!data || idx >= len || data[idx] != '\033')
        return seq;

    if (idx + 1 >= len) {
        seq.type = ANSI_SEQ_ESC;
        seq.end  = len;
        return seq;
    }

    unsigned char next = (unsigned char)data[idx + 1];
    size_t pos         = idx + 2;

    if (next == '[') {
        seq.type = ANSI_SEQ_CSI;
        while (pos < len) {
            unsigned char term = (unsigned char)data[pos++];
            if (term >= '@' && term <= '~') {
                seq.end      = pos;
                seq.final    = term;
                seq.complete = true;
                return seq;
            }
        }
        seq.end = len;
        return seq;
    }

    if (next == ']') {
        seq.type = ANSI_SEQ_OSC;
        while (pos < len) {
            unsigned char cur = (unsigned char)data[pos++];
            if (cur == '\a') {
                seq.end      = pos;
                seq.final    = cur;
                seq.complete = true;
                return seq;
            }
            if (cur == '\033' && pos < len &&
                (unsigned char)data[pos] == '\\') {
                seq.end      = pos + 1;
                seq.final    = '\\';
                seq.complete = true;
                return seq;
            }
        }
        seq.end = len;
        return seq;
    }

    seq.type     = ANSI_SEQ_ESC;
    seq.end      = pos;
    seq.final    = next;
    seq.complete = true;
    return seq;
}

size_t
ansi_skip_seq(const char *data, size_t len, size_t idx)
{
    return ansi_parse_seq(data, len, idx).end;
}

bool
ansi_seq_is_cursor_movement(const struct ansi_seq *seq)
{
    if (!seq || seq->type != ANSI_SEQ_CSI || !seq->complete)
        return false;
    switch (seq->final) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case 'f':
            return true;
        default:
            return false;
    }
}
