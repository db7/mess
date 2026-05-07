#ifndef defs_h_INCLUDED
#define defs_h_INCLUDED

#include <stddef.h>

#ifdef MESS_TESTING
#define MESS_STATIC
#else
#define MESS_STATIC static
#endif
/*
 * Half-open range helper. start marks the first column/byte included in the
 * range and end points one past the final element.
 */
struct range {
    size_t start;
    size_t end;
};

#define unreachable() __builtin_unreachable()

#endif // defs_h_INCLUDED
