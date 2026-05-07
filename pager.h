#ifndef MESS_PAGER_H
#define MESS_PAGER_H

enum pager_parse_mode {
    PAGER_PARSE_NONE = 0,
    PAGER_PARSE_OSC8 = 1 << 0,
    PAGER_PARSE_MAN  = 1 << 1,
};

int pager_run(int parse_flags);

#endif
