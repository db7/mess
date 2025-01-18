#ifndef NAV_H
#define NAV_H
/*******************************************************************************
 * navigation interface
 ******************************************************************************/
#include "readq.h"
#include <stdbool.h>
#include <sys/types.h>

// maximum number of URLS to be kept in memory
#define MAX_URLS 10

// Extract USC8 links from pager output, keeping URLs stored in internal state.
ssize_t process_output(readq *bq);

// Look for navigation commands in the input string. Returns whether the string
// should be forwarded to the pager.
bool process_input(char *c, ssize_t nread);

// Print all links (debugging)
void print_links(void);
#endif
