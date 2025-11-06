/*******************************************************************************
 * navigation interface
 ******************************************************************************/
#ifndef NAV_H
#define NAV_H
#include "readq.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// maximum number of URLS to be kept in memory
#define MAX_URLS 10

typedef int (*nav_launcher_fn)(const char *link, const char *text);
typedef int (*nav_editor_fn)(const char *path);

// Extract USC8 links from pager output, keeping URLs stored in internal state.
ssize_t nav_process_output(struct readq *bq);

// Look for navigation commands in the input string. May mutate `c` to pack the
// bytes that should be forwarded and updates `nread` with the count to pass
// through. Returns whether anything should be forwarded to the pager.
bool nav_process_input(char *c, ssize_t *nread, size_t capacity);

// Copy pager output into a destination buffer, applying highlight to the
// selected link text if present. Returns the number of bytes written, or 0 on
// failure.
size_t nav_render_highlighted(const char *input, size_t len, char *dest,
                              size_t dest_cap);

// Print all links (debugging)
void nav_print_links(void);

// Reset navigation state (used in tests and on startup)
void nav_reset(void);

// Set a custom launcher for opening links.
void nav_set_launcher(nav_launcher_fn launcher);

// Set a custom launcher for opening the editor.
void nav_set_editor_launcher(nav_editor_fn launcher);

// Record the current document path so launchers know the working file.
void nav_set_document_path(const char *path);

// Retrieve the document path registered with the navigation layer.
const char *nav_document_path(void);

// Configure which file descriptor should receive status-line updates.
void nav_set_status_fd(int fd);

// Configure which file descriptor should be used for peeking additional input
// bytes when escape sequences arrive split across reads.
void nav_set_input_fd(int fd);

// Clear any status messages shown through the pager.
void nav_clear_status(void);

// Request the pager to redraw after an external command finishes.
void nav_request_refresh(void);

// Re-render the status line for the current selection (or clear it).
void nav_render_status(void);

// Helpers for inspecting navigation state (primarily for tests)

// Return the number of tracked links.
int nav_link_count(void);

// Return the URL stored at the given index.
const char *nav_link_at(int idx);

// Return the display text associated with the link at index.
const char *nav_text_at(int idx);

// Return the currently selected link index, or -1 if nothing selected.
int nav_selected_index(void);
// Return whether the navigation status bar is currently visible.
bool nav_status_visible(void);
// Return true if the pager should receive a redraw request (Ctrl-L equivalent).
bool nav_consume_refresh_request(void);

#endif
