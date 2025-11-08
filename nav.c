// Reason: Bring in navigation interfaces and queue utilities shared across
// components.
#include "nav.h"
// Reason: Read queue integration is required for parsing OSC 8 sequences from
// output streams.
#include "readq.h"

// Reason: Standard headers provide runtime checks, string handling, terminal
// control, and spawning helpers.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

// Reason: Store per-link metadata so we can highlight and launch captured OSC 8
// hyperlinks later.
typedef struct {
    char *link;
    char *text;
    int row;
    int col;
    size_t len;
} LinkEntry;

// Reason: Maintain a circular buffer of previously discovered links for
// navigation.
static LinkEntry urls_[MAX_URLS];
// Reason: Track how many slots in the ring buffer currently contain valid
// links.
static int url_count_;
// Reason: Keep the head index so older links can be evicted when the ring
// wraps.
static int url_head_;
// Reason: Cache the logical selection index; -1 means navigation mode is
// inactive.
static int selected_idx_ = -1;
// Reason: Remember which launcher should open links when activated.
static nav_launcher_fn current_launcher_;
// Reason: Track whether the UI needs to trigger a redraw of the pager content.
static bool needs_refresh_;
// Reason: Allow callers to plug in a custom document editor while defaulting to
// launch_with_editor_.
static nav_editor_fn editor_launcher_;
// Reason: Remember the active document so navigation can open it in an editor.
static char *document_path_;
// Reason: Represent the possible states for the status/overlay line.
typedef enum { STATUS_NONE = 0, STATUS_LINK, STATUS_HELP } status_mode_t;

// Reason: File descriptor used to render status lines inside the terminal UI.
static int status_fd_ = -1;
// Reason: File descriptor that supplies raw keyboard input for handling
// multi-byte sequences.
static int input_fd_ = -1;
// Reason: Track which line (if any) should be displayed at the bottom of the
// terminal.
static status_mode_t status_mode_;
// Reason: Remember to hide the status bar on the next keypress after it
// appears.
static bool status_pending_hide_;
// Reason: Track which navigation backend should parse output/highlight links.
static nav_mode_t nav_mode_ = NAV_MODE_OSC8;
// Reason: Flag to force flushing of buffered bytes when the child stream ends.
static bool force_drain_;
// Reason: Boundaries for the man-page token parser; tokens longer than these
// will be ignored so buffering stays simple.
#define MAN_NAME_MAX    64
#define MAN_SECTION_MAX 16
#define MAN_TAIL_MAX    (MAN_NAME_MAX + MAN_SECTION_MAX + 4)

// Reason: Forward declarations allow helper routines to be referenced before
// their definitions.
static bool handle_key_input_(char key);
static LinkEntry extract_osc8_(const char *input);
static void free_pair_(LinkEntry *pair);
static int logical_to_physical_(int idx);
static int launch_with_mess_(const char *link, const char *text);
static int launch_with_editor_(const char *path);
static void free_document_path_(void);
static bool cycle_link_selection_(int direction);
static void leave_navigation_mode_(bool request_refresh);
static void debug_status_bytes_(const char *tag, const void *buf, size_t len);
static bool links_equal_(const char *a, const char *b);
static const char *link_for_logical_(int idx);
static int group_start_index_(int idx);
static int group_end_index_(int idx);
static int next_group_start_(int idx, int direction);
static void show_help_overlay_(void);
static size_t build_help_overlay_(char *dest, size_t cap, size_t width);
static ssize_t nav_process_output_osc_(struct readq *bq, bool drain);
static ssize_t nav_process_output_man_(struct readq *bq, bool drain);
static size_t nav_render_highlighted_osc_(const char *input, size_t len,
                                          char *dest, size_t dest_cap);
static size_t nav_render_highlighted_man_(const char *input, size_t len,
                                          char *dest, size_t dest_cap);
static bool nav_pair_matches_selection_(const char *link, size_t link_len,
                                        const char *text, size_t text_len);
static void nav_store_entry_(char *link, char *text, size_t text_len);
static bool nav_record_link_copy_(const char *link, size_t link_len,
                                  const char *text, size_t text_len);
static void nav_scan_man_chunk_(const char *data, size_t len);
static size_t man_suffix_to_keep_(const char *data, size_t len, bool drain);
static bool man_match_at_(const char *data, size_t len, size_t pos,
                          size_t *name_len, size_t *section_offset,
                          size_t *section_len, size_t *token_len);
static size_t man_format_link_(char *dest, size_t cap, const char *name,
                               size_t name_len, const char *section,
                               size_t section_len);
static bool man_is_name_char_(char ch);
static bool man_is_section_char_(char ch);
// Reason: Track the length and timeout for detecting Shift-Tab escape
// sequences.
#define SHIFT_TAB_SEQ_LEN         3
#define SHIFT_TAB_POLL_TIMEOUT_MS 10

// Reason: Helper to opportunistically pull more bytes for multi-byte escape
// detection.
static ssize_t read_more_input_(char *dest, size_t max);

bool
nav_process_input(char *c, ssize_t *nread, size_t capacity)
{
    // Reason: Without readable bytes there is nothing to process.
    if (!nread || *nread <= 0)
        return false;

    // Reason: Keep local indices so we can mutate the buffer without losing the
    // original counts.
    ssize_t len     = *nread;
    ssize_t out_idx = 0;
    size_t idx      = 0;
    // Reason: Remember whether any navigation action occurred that triggers a
    // status repaint.
    bool did_render = false;

    // Reason: Scan the incoming bytes sequentially so stateful escape handling
    // works.
    while (idx < (size_t)len) {
        // Reason: Inspect each byte individually to decide whether it is
        // navigation input.
        char ch = c[idx];

        if (selected_idx_ != -1 && status_pending_hide_) {
            if (status_mode_ != STATUS_NONE) {
                nav_clear_status();
                did_render = true;
            } else {
                status_pending_hide_ = false;
            }
        }

        if (ch == '\x1b') {
            // Reason: Collect enough bytes to detect a full reverse-tab escape
            // sequence.
            size_t available = (size_t)(len - (ssize_t)idx);
            // Reason: Pull more bytes into the buffer until the Shift-Tab
            // signature can be matched.
            while (available < SHIFT_TAB_SEQ_LEN) {
                if ((size_t)len >= capacity)
                    break;
                size_t space = capacity - (size_t)len;
                // Reason: Never read more bytes than the caller allocated in
                // the buffer.
                if (space == 0)
                    break;
                size_t needed = SHIFT_TAB_SEQ_LEN - available;
                // Reason: Request just enough bytes to satisfy the escape
                // sequence length.
                if (needed > space)
                    needed = space;
                // Reason: Pull additional bytes directly into the
                // caller-provided buffer.
                ssize_t more = read_more_input_(c + len, needed);
                if (more <= 0)
                    // Reason: Exit the expansion loop if the input stream has
                    // nothing ready.
                    break;
                len += more;
                // Reason: Expand both total length and the amount available for
                // inspection.
                available += (size_t)more;
            }

            if ((size_t)(len - (ssize_t)idx) >= SHIFT_TAB_SEQ_LEN &&
                c[idx + 1] == '[' && c[idx + 2] == 'Z') {
                // Reason: Shift-Tab cycles the selection backwards through
                // discovered links.
                cycle_link_selection_(-1);
                did_render = true;
                idx += SHIFT_TAB_SEQ_LEN;
                continue;
            }
        }

        if (handle_key_input_(ch)) {
            // Reason: Navigation keys are consumed internally and trigger a
            // re-render.
            did_render = true;
        } else {
            if (selected_idx_ == -1) {
                // Reason: When navigation mode is inactive, pass the byte
                // through unchanged.
                c[out_idx++] = ch;
            } else {
                // Reason: While navigating, unmapped keys trigger a contextual
                // help overlay instead of escaping.
                show_help_overlay_();
                did_render = true;
            }
        }
        // Reason: Advance to the next input byte after handling the current
        // one.
        idx++;
    }

    // Reason: Only emit the status line once after processing the entire batch.
    if (did_render)
        nav_render_status();

    *nread = out_idx;
    // Reason: Signal to callers whether any bytes should be processed
    // downstream.
    return out_idx > 0;
}

static ssize_t
read_more_input_(char *dest, size_t max)
{
    // Reason: Bail if there is no source FD or nowhere to store extra bytes.
    if (input_fd_ < 0 || max == 0)
        return 0;

    struct pollfd pfd;
    pfd.fd     = input_fd_;
    pfd.events = POLLIN;
    // Reason: Poll with a short timeout so we do not block the UI waiting for
    // more bytes.
    int ready = poll(&pfd, 1, SHIFT_TAB_POLL_TIMEOUT_MS);
    if (ready <= 0 || !(pfd.revents & POLLIN))
        return 0;

    // Reason: Attempt to read any additional bytes that complete the escape
    // sequence.
    ssize_t got = read(input_fd_, dest, max);
    if (got < 0) {
        // Reason: Treat transient lack of data as no-op; other errors just
        // abort the read.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return 0;
    }
    // Reason: Report back how many bytes were appended so the caller can adjust
    // lengths.
    return got;
}

// Interpret a single keypress for navigation controls.
static bool
handle_key_input_(char key)
{
    // Reason: 'v' edits the current document, exiting mess mode when active.
    if ((key == 'v' || key == 'V')) {
        if (selected_idx_ != -1) {
            if (document_path_ && editor_launcher_)
                editor_launcher_(document_path_);
            leave_navigation_mode_(true);
            return true;
        }
        if (document_path_ && editor_launcher_) {
            editor_launcher_(document_path_);
            nav_request_refresh();
            return true;
        }
        return false;
    }

    if (key == '\t') {
        // Reason: Tab advances to the next hyperlink in logical order.
        bool moved = cycle_link_selection_(+1);
        return moved;
    } else if (key == '\n' && selected_idx_ != -1) {
        // Reason: Enter activates the highlighted link through the configured
        // launcher.
        int physical = logical_to_physical_(selected_idx_);
        if (physical >= 0 && current_launcher_) {
            current_launcher_(urls_[physical].link, urls_[physical].text);
            nav_request_refresh();
            leave_navigation_mode_(true);
            // nav_render_status();
        }
    } else if (key == '\e') {
        if (selected_idx_ == -1)
            return false;
        // Reason: Escape abandons navigation mode and restores normal input
        // handling.
        leave_navigation_mode_(true);
        return true;
    } else if (key == 's' || key == 'S') {
        if (selected_idx_ == -1)
            return false;
        status_mode_         = STATUS_LINK;
        status_pending_hide_ = true;
        return true;
    } else {
        return false;
    }
    return true;
}

// Reason: Leverage inverse-video ANSI codes to show which link is active.
#define HIGHLIGHT_ON  "\x1b[7m"
#define HIGHLIGHT_OFF "\x1b[27m"

static bool
links_equal_(const char *a, const char *b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp(a, b) == 0;
}

static const char *
link_for_logical_(int idx)
{
    int physical = logical_to_physical_(idx);
    if (physical < 0)
        return NULL;
    return urls_[physical].link;
}

static int
group_start_index_(int idx)
{
    if (idx < 0)
        return -1;
    if (idx >= url_count_)
        idx = url_count_ - 1;
    const char *link = link_for_logical_(idx);
    if (!link)
        return idx;
    while (idx > 0) {
        const char *prev = link_for_logical_(idx - 1);
        if (!links_equal_(prev, link))
            break;
        idx--;
    }
    return idx;
}

static int
group_end_index_(int idx)
{
    if (idx < 0)
        return -1;
    if (idx >= url_count_)
        idx = url_count_ - 1;
    const char *link = link_for_logical_(idx);
    if (!link)
        return idx;
    while (idx + 1 < url_count_) {
        const char *next = link_for_logical_(idx + 1);
        if (!links_equal_(next, link))
            break;
        idx++;
    }
    return idx;
}

static int
next_group_start_(int idx, int direction)
{
    if (url_count_ == 0)
        return -1;
    if (direction > 0) {
        if (idx < 0 || idx >= url_count_)
            return group_start_index_(0);
        int end       = group_end_index_(idx);
        int candidate = end + 1;
        if (candidate >= url_count_)
            candidate = 0;
        return group_start_index_(candidate);
    } else if (direction < 0) {
        if (idx < 0 || idx >= url_count_)
            return group_start_index_(url_count_ - 1);
        int start     = group_start_index_(idx);
        int candidate = start - 1;
        if (candidate < 0)
            candidate = url_count_ - 1;
        return group_start_index_(candidate);
    }
    return group_start_index_(idx);
}

static bool
cycle_link_selection_(int direction)
{
    if (url_count_ == 0) {
        // Reason: Without any stored URLs there is nothing to select or render.
        selected_idx_ = -1;
        nav_clear_status();
        return false;
    }

    if (selected_idx_ < 0 || selected_idx_ >= url_count_) {
        // Reason: Wrap from an invalid selection to the edge element requested
        // by the direction.
        selected_idx_ = (direction > 0) ? 0 : (url_count_ - 1);
        selected_idx_ = group_start_index_(selected_idx_);
    } else {
        selected_idx_ = next_group_start_(selected_idx_, direction);
    }
    if (status_mode_ != STATUS_NONE)
        nav_clear_status();
    // Reason: Any selection change should mark the UI for refresh.
    nav_request_refresh();
    return true;
}


static const char *
find_subsequence_(const char *haystack, size_t hay_len, const char *needle,
                  size_t needle_len)
{
    if (needle_len == 0 || hay_len < needle_len)
        return NULL;
    // Reason: Manually scan because OSC 8 payloads are not guaranteed to be
    // null-terminated.
    for (size_t i = 0; i <= hay_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

ssize_t
nav_process_output(struct readq *bq)
{
    bool drain = force_drain_;
    force_drain_ = false;
    if (!bq)
        return 0;
    // Reason: Route to the active backend so OSC8 parsing and man scanning can
    // coexist while sharing the rest of the navigation infrastructure.
    switch (nav_mode_) {
    case NAV_MODE_MAN:
        return nav_process_output_man_(bq, drain);
    case NAV_MODE_OSC8:
    default:
        return nav_process_output_osc_(bq, drain);
    }
}

static ssize_t
nav_process_output_osc_(struct readq *bq, bool drain)
{
    // Reason: Define the OSC 8 sequences we need to detect link start and end
    // boundaries.
    const char *osc8_start = "\e]8";
    const char *osc8_end   = "\e]8;;\e\\";

    // Reason: Consume as much queued output as possible so link extraction
    // keeps pace with the stream.
    while (bq->start != bq->end) {
        // Reason: Validate that the ring buffer exposes a sensible span before
        // parsing.
        assert(bq->start < bq->end);
        // Reason: Track the available bytes so we can detect partial escapes at
        // the tail.
        const size_t left = bq->end - bq->start;
        const char *str   = (const char *)bq->buffer + bq->start;
        const char *end   = (const char *)bq->buffer + bq->end;

        // Reason: Search for the next OSC 8 opener within the buffered slice.
        const char *start = strstr(str, osc8_start);
        if (start == NULL) {
            if (left >= 2 && strncmp(end - 2, "\e]", 2) == 0)
                // Reason: Preserve the start of a potential OSC sequence for
                // the next read.
                bq->start = bq->end - 2;
            else if (left >= 1 && strncmp(end - 1, "\e", 1) == 0)
                // Reason: Retain a solitary ESC byte that may begin a sequence
                // after more data arrives.
                bq->start = bq->end - 1;
            else
                // Reason: No escape prefix at the end, so consume the entire
                // buffer chunk.
                bq->start = bq->end;
            // Reason: Leave potential partial introducers in the buffer so they
            // can be completed later.
            break;
        }

        // Reason: Find the matching close marker to ensure the sequence is
        // complete.
        end = strstr(start, osc8_end);
        if (end == NULL) {
            // Reason: Leave partial sequences in the buffer so the next chunk
            // can finish them.
            bq->start += start - str;
            break;
        }

        // Reason: Decode the OSC 8 block into link/text components stored in
        // the ring buffer.
        LinkEntry pair = extract_osc8_(start);
        // Reason: Only cache entries that contain both the URL and its display
        // text.
        if (pair.link && pair.text) {
            nav_store_entry_(pair.link, pair.text, pair.len);
            pair.link = NULL;
            pair.text = NULL;
        }
        // Reason: Discard ownership if storage moved elsewhere or parsing
        // failed.
        free_pair_(&pair);

        // Reason: Advance beyond the processed OSC 8 block so the loop
        // continues with fresh data.
        bq->start += end + strlen(osc8_end) - str;
    }
    // Reason: Sanity-check that the queue indices never invert.
    assert(bq->start <= bq->end);
    // Reason: Let the caller know how far into the buffer the parser advanced.
    if (drain)
        bq->start = bq->end;
    return bq->start;
}

static ssize_t
nav_process_output_man_(struct readq *bq, bool drain)
{
    if (!bq)
        return 0;

    size_t total = bq->end - bq->start;
    if (total == 0)
        return 0;

    const char *data = bq->buffer + bq->start;
    size_t tail_keep = man_suffix_to_keep_(data, total, drain);
    if (tail_keep > total)
        tail_keep = total;
    size_t usable = total - tail_keep;

    if (usable > 0)
        // Reason: Only scan the portion guaranteed to contain whole tokens so
        // partial matches at the boundary are preserved for the next refill.
        nav_scan_man_chunk_(data, usable);

    bq->start += usable;
    if (drain)
        bq->start = bq->end;
    return (ssize_t)usable;
}

size_t
nav_render_highlighted(const char *input, size_t len, char *dest,
                       size_t dest_cap)
{
    switch (nav_mode_) {
    case NAV_MODE_MAN:
        return nav_render_highlighted_man_(input, len, dest, dest_cap);
    case NAV_MODE_OSC8:
    default:
        return nav_render_highlighted_osc_(input, len, dest, dest_cap);
    }
}

static size_t
nav_render_highlighted_osc_(const char *input, size_t len, char *dest,
                            size_t dest_cap)
{
    // Reason: Short helpers for delimiting OSC 8 links and their closing
    // sequences.
    static const char osc8_link_end[] = "\e\\";
    static const char osc8_close[]    = "\e]8;;\e\\";
    // Reason: Cache string lengths to avoid repeated strlen calls during
    // rendering.
    const size_t link_term_len     = strlen(osc8_link_end);
    const size_t closing_len       = strlen(osc8_close);
    const size_t highlight_on_len  = strlen(HIGHLIGHT_ON);
    const size_t highlight_off_len = strlen(HIGHLIGHT_OFF);

    // Reason: Maintain independent cursors for input scanning and destination
    // writes.
    size_t pos = 0;
    size_t out = 0;

    // Reason: Iterate until all bytes are rewritten with optional highlighting.
    while (pos < len) {
        const char *esc = memchr(input + pos, '\x1b', len - pos);
        if (!esc) {
            size_t chunk = len - pos;
            if (out + chunk > dest_cap)
                return 0;
            memcpy(dest + out, input + pos, chunk);
            out += chunk;
            break;
        }

        size_t literal_len = (size_t)(esc - (input + pos));
        if (literal_len > 0) {
            if (out + literal_len > dest_cap)
                return 0;
            memcpy(dest + out, input + pos, literal_len);
            out += literal_len;
            pos += literal_len;
        }

        if (pos >= len)
            break;

        size_t remaining = len - pos;
        if (remaining < 3 || input[pos + 1] != ']' || input[pos + 2] != '8') {
            if (out + 1 > dest_cap)
                return 0;
            dest[out++] = input[pos++];
            continue;
        }

        const char *block_start = input + pos;
        const char *first_semicolon =
            memchr(block_start + 3, ';', (input + len) - (block_start + 3));
        if (!first_semicolon) {
            size_t chunk = len - pos;
            if (out + chunk > dest_cap)
                return 0;
            memcpy(dest + out, block_start, chunk);
            out += chunk;
            break;
        }

        const char *second_semicolon = memchr(
                                           first_semicolon + 1, ';', (input + len) - (first_semicolon + 1));
        if (!second_semicolon) {
            size_t chunk = len - pos;
            if (out + chunk > dest_cap)
                return 0;
            memcpy(dest + out, block_start, chunk);
            out += chunk;
            break;
        }

        const char *link_start = second_semicolon + 1;
        const char *link_end =
            find_subsequence_(link_start, (input + len) - link_start,
                              osc8_link_end, link_term_len);
        if (!link_end) {
            size_t chunk = len - pos;
            if (out + chunk > dest_cap)
                return 0;
            memcpy(dest + out, block_start, chunk);
            out += chunk;
            break;
        }

        const char *text_start = link_end + link_term_len;
        const char *text_end = find_subsequence_(
                                   text_start, (input + len) - text_start, osc8_close, closing_len);
        if (!text_end) {
            size_t chunk = len - pos;
            if (out + chunk > dest_cap)
                return 0;
            memcpy(dest + out, block_start, chunk);
            out += chunk;
            break;
        }

        size_t prefix_len = (size_t)(text_start - block_start);
        if (out + prefix_len > dest_cap)
            return 0;
        memcpy(dest + out, block_start, prefix_len);
        out += prefix_len;

        size_t link_len = (size_t)(link_end - link_start);
        size_t text_len = (size_t)(text_end - text_start);
        bool highlight  =
            nav_pair_matches_selection_(link_start, link_len, text_start, text_len);

        if (highlight) {
            if (out + highlight_on_len + text_len + highlight_off_len >
                dest_cap)
                return 0;
            memcpy(dest + out, HIGHLIGHT_ON, highlight_on_len);
            out += highlight_on_len;
            memcpy(dest + out, text_start, text_len);
            out += text_len;
            memcpy(dest + out, HIGHLIGHT_OFF, highlight_off_len);
            out += highlight_off_len;
        } else {
            if (out + text_len > dest_cap)
                return 0;
            memcpy(dest + out, text_start, text_len);
            out += text_len;
        }

        if (out + closing_len > dest_cap)
            return 0;
        memcpy(dest + out, text_end, closing_len);
        out += closing_len;

        pos = (size_t)(text_end - input) + closing_len;
    }

    return out;
}

static size_t
nav_render_highlighted_man_(const char *input, size_t len, char *dest,
                            size_t dest_cap)
{
    const size_t highlight_on_len  = strlen(HIGHLIGHT_ON);
    const size_t highlight_off_len = strlen(HIGHLIGHT_OFF);
    size_t pos                     = 0;
    size_t out                     = 0;

    // Reason: Scan plain text for `name(section)` tokens and wrap the selected
    // one in inverse-video sequences much like the OSC8 path does.
    while (pos < len) {
        size_t name_len = 0;
        size_t section_offset;
        size_t section_len = 0;
        size_t token_len   = 0;
        if (!man_match_at_(
                input, len, pos, &name_len, &section_offset, &section_len,
                &token_len)) {
            if (out + 1 > dest_cap)
                return 0;
            dest[out++] = input[pos++];
            continue;
        }

        const char *name    = input + pos;
        const char *section = name + section_offset;
        char link_buf[MAN_NAME_MAX + MAN_SECTION_MAX + 16];
        size_t link_len =
            man_format_link_(link_buf, sizeof(link_buf), name, name_len,
                             section, section_len);
        bool highlight = link_len > 0 &&
                         nav_pair_matches_selection_(link_buf, link_len, name,
                                                     token_len);

        size_t needed = token_len + (highlight
                                         ? (highlight_on_len + highlight_off_len)
                                         : 0);
        if (out + needed > dest_cap)
            return 0;

        if (highlight) {
            memcpy(dest + out, HIGHLIGHT_ON, highlight_on_len);
            out += highlight_on_len;
        }
        memcpy(dest + out, name, token_len);
        out += token_len;
        if (highlight) {
            memcpy(dest + out, HIGHLIGHT_OFF, highlight_off_len);
            out += highlight_off_len;
        }

        pos += token_len;
    }

    return out;
}

static bool
man_is_name_char_(char ch)
{
    // Reason: Historical manpage references allow alnum as well as '_'/'-' for
    // section names like `pthread_mutex_init`.
    return isalnum((unsigned char)ch) || ch == '_' || ch == '-';
}

static bool
man_is_section_char_(char ch)
{
    // Reason: Sections commonly include dots/subsections, so permit a slightly
    // wider range while still rejecting whitespace.
    return isalnum((unsigned char)ch) || ch == '.' || ch == '_';
}

static bool
man_match_at_(const char *data, size_t len, size_t pos, size_t *name_len,
              size_t *section_offset, size_t *section_len, size_t *token_len)
{
    if (!data || pos >= len)
        return false;
    // Reason: Require anchors like foo(3) so we know how to build a man://
    // target; partial tokens are ignored to avoid false positives.

    size_t i = pos;
    if (!man_is_name_char_(data[i]))
        return false;

    while (i < len && man_is_name_char_(data[i])) {
        if (i - pos >= MAN_NAME_MAX)
            return false;
        i++;
    }
    if (i >= len || data[i] != '(' || i == pos)
        return false;

    size_t section_start = i + 1;
    size_t j             = section_start;
    bool has_section     = false;
    while (j < len && man_is_section_char_(data[j])) {
        if (j - section_start >= MAN_SECTION_MAX)
            return false;
        has_section = true;
        j++;
    }
    if (!has_section || j >= len || data[j] != ')')
        return false;

    if (name_len)
        *name_len = i - pos;
    if (section_offset)
        *section_offset = section_start - pos;
    if (section_len)
        *section_len = j - section_start;
    if (token_len)
        *token_len = (j - pos) + 1;
    return true;
}

static size_t
man_format_link_(char *dest, size_t cap, const char *name, size_t name_len,
                 const char *section, size_t section_len)
{
    if (!dest || cap == 0)
        return 0;
    // Reason: Emit the canonical scheme `man://name.section` so downstream
    // launchers can reuse the dispatcher path.
    int written = snprintf(dest, cap, "man://%.*s.%.*s", (int)name_len, name,
                           (int)section_len, section);
    if (written <= 0)
        return 0;
    if ((size_t)written >= cap)
        return 0;
    return (size_t)written;
}

static void
nav_scan_man_chunk_(const char *data, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        size_t name_len = 0;
        size_t section_offset;
        size_t section_len = 0;
        size_t token_len   = 0;
        if (!man_match_at_(data, len, pos, &name_len, &section_offset,
                           &section_len, &token_len)) {
            pos++;
            continue;
        }

        const char *name    = data + pos;
        const char *section = name + section_offset;
        char link_buf[MAN_NAME_MAX + MAN_SECTION_MAX + 16];
        size_t link_len =
            man_format_link_(link_buf, sizeof(link_buf), name, name_len,
                             section, section_len);
        if (link_len > 0)
            // Reason: Copy the discovered token into navigation storage so it
            // behaves like a standard OSC link entry.
            nav_record_link_copy_(link_buf, link_len, name, token_len);
        pos += token_len;
    }
}

static size_t
man_suffix_to_keep_(const char *data, size_t len, bool drain)
{
    if (drain || len == 0)
        return 0;
    // Reason: Keep a short tail so tokens split across reads can be reassembled
    // without rescanning the entire buffer.
    size_t limit = len < MAN_TAIL_MAX ? len : MAN_TAIL_MAX;
    size_t keep  = 0;
    while (keep < limit) {
        char ch = data[len - keep - 1];
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ')
            break;
        keep++;
    }
    return keep;
}

// Decode an OSC 8 sequence into a link/text pair.
static LinkEntry
extract_osc8_(const char *input)
{
    const char *osc8_start    = "\e]8;";
    const char *osc8_end      = "\e]8;;\e\\";
    const char *osc8_link_end = "\e\\";
    // Reason: Seed default values so callers can safely free partially parsed
    // entries.
    LinkEntry result = {NULL, NULL, -1, -1, 0};

    // Reason: Locate the OSC 8 introducer before attempting to parse link
    // payloads.
    const char *link_start = strstr(input, osc8_start);
    if (link_start == NULL)
        return result;

    // Reason: Skip the parameter section preceding the actual link target.
    link_start = strchr(link_start + strlen(osc8_start), ';');
    if (link_start == NULL)
        return result;
    link_start++;

    // Reason: The link terminator separates the URL from the display text.
    const char *link_end = strstr(link_start, osc8_link_end);
    if (link_end == NULL)
        return result;

    // Extract the link
    size_t link_length = (size_t)(link_end - link_start);
    result.link        = malloc(link_length + 1);
    if (!result.link) {
        perror("malloc failed");
        // OSC-8 parsing relies on capturing the link verbatim; treat OOM as
        // fatal so callers never receive partial structures.
        exit(EXIT_FAILURE);
    }
    // Reason: Copy the URL into owned storage because the source buffer is
    // transient.
    memcpy(result.link, link_start, link_length);
    result.link[link_length] = '\0';

    // Reason: The text payload follows immediately after the link closing
    // sequence.
    const char *text_start = link_end + strlen(osc8_link_end);
    const char *text_end   = strstr(text_start, osc8_end);
    if (text_end == NULL)
        // Reason: Abort when the closing sequence is missing to avoid reading
        // past the buffer.
        return result;

    // Extract the text
    size_t text_length = (size_t)(text_end - text_start);
    result.text        = malloc(text_length + 1);
    if (!result.text) {
        perror("malloc failed");
        free(result.link);
        // Same rationale—abort on allocation failure to avoid leaking
        // inconsistent navigation state to the rest of the system.
        exit(EXIT_FAILURE);
    }
    // Reason: Own a copy of the display text for later rendering and
    // highlighting.
    memcpy(result.text, text_start, text_length);
    result.text[text_length] = '\0';
    // Reason: Store the length so highlight comparisons can run quickly.
    result.len = text_length;

    // Reason: Hand back the populated structure; NULL fields indicate parsing
    // issues to callers.
    return result;
}

void
nav_print_links(void)
{
    // Reason: Emit a human-readable listing of stored hyperlinks for debugging.
    for (int i = 0; i < url_count_; i++) {
        // Reason: Translate loop indices to their backing storage entries.
        int idx = logical_to_physical_(i);
        // Reason: Only print complete entries so output stays clean.
        if (idx >= 0 && urls_[idx].link && urls_[idx].text) {
            // Reason: Use a Gemini-style arrow to show link text and target
            // when printing.
            printf("=> %s: %s\n", urls_[idx].text, urls_[idx].link);
        }
    }
}

void
nav_reset(void)
{
    // Reason: Exit navigation mode without forcing a status redraw.
    leave_navigation_mode_(false);
    // Reason: Release all dynamic link data before clearing the ring buffer
    // counters.
    for (int i = 0; i < MAX_URLS; ++i) {
        free_pair_(&urls_[i]);
    }
    // Reason: Reinitialize state so future captures start from a clean slate.
    url_count_           = 0;
    url_head_            = 0;
    selected_idx_        = -1;
    current_launcher_    = launch_with_mess_;
    editor_launcher_     = launch_with_editor_;
    needs_refresh_       = false;
    status_mode_         = STATUS_NONE;
    status_pending_hide_ = false;
    input_fd_            = -1;
    force_drain_         = false;
}

void
nav_set_mode(nav_mode_t mode)
{
    // Reason: Allow callers to flip parsers per-invocation without reinitializing
    // the rest of the navigation subsystem.
    nav_mode_ = mode;
}

nav_mode_t
nav_current_mode(void)
{
    return nav_mode_;
}

void
nav_request_drain(void)
{
    // Reason: Signal the parser to treat the next chunk as end-of-stream so it
    // flushes any partial tokens instead of leaving them buffered indefinitely.
    force_drain_ = true;
}

static void
nav_store_entry_(char *link, char *text, size_t text_len)
{
    if (!link || !text)
        return;

    int slot;
    if (url_count_ < MAX_URLS) {
        slot = (url_head_ + url_count_) % MAX_URLS;
        url_count_++;
    } else {
        slot      = url_head_;
        url_head_ = (url_head_ + 1) % MAX_URLS;
    }

    free_pair_(&urls_[slot]);
    urls_[slot].link = link;
    urls_[slot].text = text;
    urls_[slot].row  = -1;
    urls_[slot].col  = -1;
    urls_[slot].len  = text_len;
    if (current_launcher_ == NULL)
        current_launcher_ = launch_with_mess_;
}

static bool
nav_record_link_copy_(const char *link, size_t link_len, const char *text,
                      size_t text_len)
{
    if (!link || !text || link_len == 0 || text_len == 0)
        return false;
    char *link_copy = strndup(link, link_len);
    char *text_copy = strndup(text, text_len);
    if (!link_copy || !text_copy) {
        free(link_copy);
        free(text_copy);
        return false;
    }
    nav_store_entry_(link_copy, text_copy, text_len);
    return true;
}

static bool
nav_pair_matches_selection_(const char *link, size_t link_len,
                            const char *text, size_t text_len)
{
    if (selected_idx_ < 0 || url_count_ <= 0 || !link || !text)
        return false;

    int group_start = group_start_index_(selected_idx_);
    int group_end   = group_end_index_(selected_idx_);
    for (int i = group_start; i <= group_end; ++i) {
        int physical = logical_to_physical_(i);
        if (physical < 0)
            continue;
        const char *group_link = urls_[physical].link;
        const char *group_text = urls_[physical].text;
        if (!group_link || !group_text)
            continue;
        size_t group_link_len = strlen(group_link);
        size_t group_text_len = strlen(group_text);
        if (group_link_len == link_len && group_text_len == text_len &&
            strncmp(group_link, link, link_len) == 0 &&
            strncmp(group_text, text, text_len) == 0)
            return true;
    }
    return false;
}

void
nav_set_launcher(nav_launcher_fn launcher)
{
    // Reason: Swap in a custom launcher while falling back to the default
    // helper. Reason: Persist the caller's choice so subsequent activations use
    // it.
    current_launcher_ = launcher ? launcher : launch_with_mess_;
}

void
nav_set_editor_launcher(nav_editor_fn launcher)
{
    // Reason: Allow callers to override the editor command at runtime.
    // Reason: Cache the chosen editor so navigation mode can spawn it later.
    editor_launcher_ = launcher ? launcher : launch_with_editor_;
}

void
nav_set_document_path(const char *path)
{
    // Reason: Replace any existing document association before storing the new
    // one.
    free_document_path_();
    if (path && path[0]) {
        // Reason: Only duplicate non-empty paths so empty strings behave like
        // clearing the document.
        document_path_ = strdup(path);
        if (!document_path_) {
            perror("strdup document path");
            // The rest of the code assumes the document path is either valid or
            // NULL; abort instead of leaving callers with a dangling pointer.
            // Reason: Terminate immediately to prevent inconsistent navigation
            // state.
            exit(EXIT_FAILURE);
        }
    }
}

const char *
nav_document_path(void)
{
    // Reason: Expose the current document so the UI can display or use it
    // elsewhere. Reason: Return the stored path pointer directly; callers treat
    // NULL as unset.
    return document_path_;
}

void
nav_request_refresh(void)
{
    // Reason: Flag that a redraw is needed; the caller will later consume this
    // state.
    needs_refresh_ = true;
}

static void
write_status_line_(const char *text, bool is_help)
{
    // Reason: Skip rendering when no status fd is configured.
    if (status_fd_ < 0)
        return;

    struct winsize ws = {0};
    size_t width      = 0;
    int row           = 0;
    // Reason: Query terminal size so we can position and clamp the status line.
    if (ioctl(status_fd_, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0)
            width = (size_t)ws.ws_col;
        if (ws.ws_row > 0)
            row = ws.ws_row;
    }

    char movebuf[32];
    int movelen = snprintf(movebuf, sizeof(movebuf), "\033[%d;1H", row);

    // static const char save_seq[]  = "\x1b[s";
    static const char clear_seq[] = "\r\x1b[K";
    // static const char restore_seq[] = "\x1b[u";

    //(void)write(status_fd_, save_seq, sizeof(save_seq) - 1);
    // debug_status_bytes_("status-save", save_seq, sizeof(save_seq) - 1);
    if (movelen > 0) {
        // Reason: Emit the cursor move only when snprintf produced a valid
        // sequence.
        (void)write(status_fd_, movebuf, (size_t)movelen);
        debug_status_bytes_("status-move", movebuf, (size_t)movelen);
    } else {
        debug_status_bytes_("status-move", NULL, 0);
    }
    // Reason: Clear the current line to ensure the new status text replaces any
    // leftovers.
    (void)write(status_fd_, clear_seq, sizeof(clear_seq) - 1);
    debug_status_bytes_("status-clear", clear_seq, sizeof(clear_seq) - 1);

    char rendered[256];
    const char *out_text = text ? text : "";
    size_t len           = 0;

    if (is_help) {
        len      = build_help_overlay_(rendered, sizeof(rendered), width);
        out_text = rendered;
    } else {
        len = strlen(out_text);
    }

    // Reason: Clamp the text length to the measured terminal width to avoid
    // wrapping.
    if (width > 0 && len > width)
        len = width;
    if (len > 0) {
        // Reason: Output the truncated status text to the terminal line.
        (void)write(status_fd_, out_text, len);
        debug_status_bytes_("status-text", out_text, len);
    } else {
        // Reason: Still log that no bytes were written to help with debugging
        // empty statuses.
        debug_status_bytes_("status-text", NULL, 0);
    }

    //(void)write(status_fd_, restore_seq, sizeof(restore_seq) - 1);
    // debug_status_bytes_("status-restore", restore_seq,
    // sizeof(restore_seq) - 1);
}

void
nav_clear_status(void)
{
    // Reason: An empty string removes any previously displayed status line.
    status_mode_         = STATUS_NONE;
    status_pending_hide_ = false;
    write_status_line_("", false);
}

void
nav_render_status(void)
{
    if (status_mode_ == STATUS_NONE)
        return;
    if (status_mode_ == STATUS_HELP) {
        write_status_line_(NULL, true);
        return;
    }
    if (status_mode_ == STATUS_LINK) {
        // Reason: Convert the logical selection into a concrete link before
        // rendering.
        int physical = logical_to_physical_(selected_idx_);
        if (physical < 0) {
            // Reason: No active link means the status bar should be blank.
            nav_clear_status();
            return;
        }
        const char *link =
            urls_[physical].link ? urls_[physical].link : "(null)";
        // Reason: Prefix the link with => to match Gemini link conventions.
        char buffer[1024];
        // Reason: Compose the status line in a temporary buffer before writing
        // it out.
        int len = snprintf(buffer, sizeof(buffer), "=> %s", link);
        if (len <= 0)
            // Reason: Skip updates when snprintf fails to produce output.
            return;
        // Reason: Emit the formatted status string to the configured terminal
        // row.
        write_status_line_(buffer, false);
    }
}

static void
show_help_overlay_(void)
{
    status_mode_         = STATUS_HELP;
    status_pending_hide_ = true;
}

static size_t
build_help_overlay_(char *dest, size_t cap, size_t width)
{
    if (!dest || cap == 0)
        return 0;

    const char *segments[] = {"Tab next", "S-Tab prev", "Enter open",
                              "v edit",   "s link",     "Esc exit"
                             };
    const size_t seg_count = sizeof(segments) / sizeof(segments[0]);
    const char *separator  = "  ";
    size_t sep_len         = strlen(separator);

    size_t max_chars = (width > 0 && width + 1 < cap) ? width : (cap - 1);
    if (max_chars == 0)
        max_chars = cap - 1;

    size_t used       = 0;
    bool appended_any = false;
    bool truncated    = false;

    for (size_t i = 0; i < seg_count; ++i) {
        size_t seg_len = strlen(segments[i]);
        size_t needed  = seg_len + (appended_any ? sep_len : 0);
        if (used + needed > max_chars) {
            truncated = true;
            break;
        }
        if (appended_any) {
            memcpy(dest + used, separator, sep_len);
            used += sep_len;
        }
        memcpy(dest + used, segments[i], seg_len);
        used += seg_len;
        appended_any = true;
    }

    if (!appended_any) {
        // Reason: If the terminal width is extremely narrow, fall back to a
        // slice of the first entry.
        const char *first = segments[0];
        size_t copy       = strlen(first);
        if (copy > max_chars)
            copy = max_chars;
        memcpy(dest, first, copy);
        used = copy;
    } else if (truncated && used + 4 <= max_chars) {
        // Reason: Indicate that not all shortcuts fit on the line.
        const char ellipsis[] = " ...";
        size_t ellipsis_len   = strlen(ellipsis);
        if (used + ellipsis_len <= max_chars) {
            memcpy(dest + used, ellipsis, ellipsis_len);
            used += ellipsis_len;
        }
    }

    dest[used] = '\0';
    return used;
}


int
nav_link_count(void)
{
    // Reason: Callers need to know how many logical entries are available.
    return url_count_;
}

const char *
nav_link_at(int idx)
{
    int physical = logical_to_physical_(idx);
    // Reason: Invalid requests should signal absence instead of dereferencing
    // out-of-range slots.
    if (physical < 0)
        return NULL;
    // Reason: Return the raw URL for the requested slot.
    return urls_[physical].link;
}

const char *
nav_text_at(int idx)
{
    int physical = logical_to_physical_(idx);
    // Reason: Gracefully handle callers asking for non-existent entries.
    if (physical < 0)
        return NULL;
    // Reason: Expose the display text so UIs can render link labels.
    return urls_[physical].text;
}

int
nav_selected_index(void)
{
    // Reason: The caller may need to highlight or inspect the current
    // selection.
    return selected_idx_;
}

bool
nav_status_visible(void)
{
    return status_mode_ != STATUS_NONE;
}

// Free any dynamic fields on the supplied LinkEntry.
static void
free_pair_(LinkEntry *pair)
{
    if (!pair)
        return;
    // Reason: Each entry duplicates its link/text strings; release them
    // individually.
    free(pair->link);
    free(pair->text);
    pair->link = NULL;
    pair->text = NULL;
    // Reason: Reset metadata so stale coordinates are never reused
    // accidentally.
    pair->row = -1;
    pair->col = -1;
    pair->len = 0;
}

// Map a logical selection index into the ring buffer slot.
static int
logical_to_physical_(int idx)
{
    // Reason: Reject indexes outside the logical range before computing the
    // ring offset.
    if (idx < 0 || idx >= url_count_)
        return -1;
    // Reason: Ring buffer arithmetic maps logical slots to storage indexes.
    return (url_head_ + idx) % MAX_URLS;
}

// Shell out to mess --open for the given link.
static int
launch_with_mess_(const char *link, const char *text)
{
    (void)text;
    // Reason: Abort if no URL is provided to avoid invoking mess with an empty
    // target.
    if (!link)
        return -1;
    // Reason: Build the command line so the external tool can open the URL.
    char cmd[1024];
    // Reason: Compose the shell command and detect truncation via snprintf's
    // return value.
    int written = snprintf(cmd, sizeof(cmd), "mess --open %s", link);
    if (written < 0 || written >= (int)sizeof(cmd))
        // Reason: Refuse to execute if the command string could not be fully
        // constructed.
        return -1;
    // Reason: Use system for consistency with other launchers and capture its
    // exit status.
    int rc = system(cmd);
    // Reason: Launching a link likely changes terminal output, so schedule a
    // redraw.
    nav_request_refresh();
    // Reason: Propagate the launcher's exit status so callers can react to
    // failures.
    return rc;
}

bool
nav_consume_refresh_request(void)
{
    // Reason: Capture and clear the pending refresh flag so callers act once
    // per request.
    bool requested = needs_refresh_;
    needs_refresh_ = false;
    // Reason: Tell the caller whether this invocation consumed a pending
    // refresh.
    return requested;
}

void
nav_set_status_fd(int fd)
{
    // Reason: Allow the caller to designate where status updates should be
    // written. Reason: Store the descriptor so subsequent status writes target
    // the correct TTY.
    status_fd_ = fd;
}

void
nav_set_input_fd(int fd)
{
    // Reason: Capture the input stream so reverse-tab reads can pull more
    // bytes. Reason: Record the descriptor used when fetching additional
    // escape-sequence bytes.
    input_fd_ = fd;
}

static void
leave_navigation_mode_(bool request_refresh)
{
    // Reason: Ignore requests when no selection is active.
    if (selected_idx_ == -1)
        return;
    // Reason: Clearing the selection disables link-specific key handling.
    selected_idx_ = -1;
    if (request_refresh)
        // Reason: Some callers want to trigger a status update immediately
        // after exit.
        nav_request_refresh();
    // Reason: Ensure no stale status remains after leaving navigation mode.
    nav_clear_status();
}

static void
debug_status_bytes_(const char *tag, const void *buf, size_t len)
{
    // Reason: Lazy-open the debug file descriptor only if tracing is requested.
    static int debug_fd = -2;
    // Reason: A value of -1 indicates tracing was disabled after
    // initialization.
    if (debug_fd == -1)
        return;
    if (debug_fd == -2) {
        // Reason: Resolve the debug file lazily so normal runs avoid filesystem
        // work.
        const char *path = getenv("MESS_DEBUG_STATUS");
        if (!path || !path[0]) {
            debug_fd = -1;
            return;
        }
        debug_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (debug_fd == -1)
            return;
    }
    if (debug_fd >= 0) {
        // Reason: Emit hex dumps that help diagnose terminal control sequences.
        if (tag && tag[0])
            dprintf(debug_fd, "%s:", tag);
        else
            dprintf(debug_fd, "status:");
        for (size_t i = 0; i < len; ++i) {
            unsigned char ch = ((const unsigned char *)buf)[i];
            dprintf(debug_fd, " %02x", ch);
        }
        dprintf(debug_fd, "\n");
    }
}

// Spawn the configured editor on the active document path.
static int
launch_with_editor_(const char *path)
{
    // Reason: Editing only makes sense when a concrete document path was
    // configured.
    if (!path || !path[0])
        return -1;

    // Reason: Honor VISUAL or EDITOR environment overrides before falling back
    // to vi.
    const char *editor = getenv("VISUAL");
    if (!editor || !editor[0])
        editor = getenv("EDITOR");
    if (!editor || !editor[0])
        editor = "vi";

    wordexp_t we;
    bool have_wordexp   = false;
    char **editor_words = NULL;
    size_t editor_count = 0;
    if (wordexp(editor, &we, WRDE_NOCMD) == 0 && we.we_wordc > 0) {
        // Reason: Expand shell words so quoted editor commands resolve
        // correctly.
        editor_words = we.we_wordv;
        editor_count = we.we_wordc;
        have_wordexp = true;
    } else {
        // Reason: Fallback to treating the editor string as a single argv if
        // expansion fails.
        editor_words = (char **)&editor;
        editor_count = 1;
    }

    // Reason: Prepare an argv array large enough for the editor words, path,
    // and NULL sentinel.
    char **cmd = calloc(editor_count + 2, sizeof(char *));
    if (!cmd) {
        perror("calloc editor command");
        // Reason: Release any partially built argument list before returning an
        // error.
        if (have_wordexp)
            wordfree(&we);
        return -1;
    }

    size_t pos = 0;
    for (size_t i = 0; i < editor_count; ++i) {
        // Reason: Copy each word into the argv array that execvp will consume.
        cmd[pos++] = editor_words[i];
    }
    // Reason: Append the document path so the editor opens the current file.
    cmd[pos++] = (char *)path;
    // Reason: Null-terminate argv as required by execvp.
    cmd[pos] = NULL;

    // Reason: Fork a subprocess so the editor can run without blocking signal
    // handling in the parent.
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        // Reason: Free the allocated argv array when the fork cannot proceed.
        free(cmd);
        // Reason: Undo wordexp allocations when setup fails after expansion.
        if (have_wordexp)
            wordfree(&we);
        return -1;
    }

    if (pid == 0) {
        // Reason: Child replaces itself with the editor; reaching here means
        // exec failed.
        execvp(cmd[0], cmd);
        perror("execvp");
        _exit(127);
    }

    // Reason: Wait for the editor to exit so navigation knows when to refresh.
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        free(cmd);
        // Reason: Free the word expansion results when the editor invocation
        // does not complete.
        if (have_wordexp)
            wordfree(&we);
        return -1;
    }

    // Reason: Clean up the temporary argv array allocated for the editor
    // process.
    free(cmd);
    if (have_wordexp)
        // Reason: Release memory allocated by wordexp once it is no longer
        // needed.
        wordfree(&we);

    if (WIFEXITED(status))
        // Reason: Propagate the editor's exit status to the caller for
        // success/failure checks.
        return WEXITSTATUS(status);
    // Reason: Treat abnormal terminations as failure so callers know the edit
    // did not complete.
    return -1;
}

// Release the stored document path string.
static void
free_document_path_(void)
{
    // Reason: Avoid leaking the duplicated document path between navigation
    // sessions.
    free(document_path_);
    // Reason: Reset the pointer so callers never observe stale memory.
    document_path_ = NULL;
}
