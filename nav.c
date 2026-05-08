#include "nav.h"

#include "ansi.h"
#include "links.h"
#include "log.h"
#include "styler.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

struct nav {
    int notify_rd;
    int notify_wr;
    int parse_flags;
    const char *self_cmd;
    bool show_status_link;
};

enum nav_process_code {
    NAV_PROCESS_CONTINUE = 0,
    NAV_PROCESS_STOP,
    NAV_PROCESS_REFRESH,
    NAV_PROCESS_QUIT,
};

struct nav_process_result {
    enum nav_process_code status;
};

struct nav_state {
    const struct nav *owner;
    const struct offscr_view *view;
    struct readq *rq;
    struct link_iter links;
    size_t selected_index;
    size_t next_index;
    size_t baseline_row;
    size_t last_logical_row;
    size_t screen_cols;
    int out_fd;
    bool status_visible;
    bool show_link_in_status;
    bool has_active_link;
};

static struct nav *global_nav_;

static bool env_flag_(const char *name, bool default_value);

static void cycle_link_(struct nav_state *state);
static bool move_link_spatial_(struct nav_state *state, int dx, int dy);
static bool select_link_at_(struct nav_state *state, size_t target);
static size_t current_block_end_(struct nav_state *state, size_t start);
static bool index_in_block_(size_t idx, size_t start, size_t end, size_t count);
static size_t highlight_block_(struct nav_state *state, size_t start_idx);
static void restore_active_block_(struct nav_state *state);
static bool urls_equal_(const char *a, const char *b);
static size_t status_row_(const struct offscr_view *view, size_t width);
static size_t visual_row_(const struct offscr_view *view, size_t width,
                          size_t target_row);
static size_t visual_rows_(const struct nav_state *state, size_t row);
static char *render_line_for_span_(const struct nav_state *state,
                                   const struct link_span *span);
static char *render_plain_line_(const struct nav_state *state,
                                const struct link_span *span);
static void replace_line_(struct nav_state *state, size_t row, char *content);
static void finish_repaint_(struct nav_state *state);
static void open_uri_with_mess_(const struct nav_state *state, const char *uri);
static void edit_with_editor_(const char *fallback_path, size_t line);
static bool editor_file_first_(const char *cmd);
static struct nav_process_result nav_process_input_(struct nav_state *,
                                                    struct readq *);
static void nav_draw_status_bar_(struct nav_state *state);
static void nav_restore_active_selection_(struct nav_state *state);
static void nav_status_clear_(struct nav_state *state);
static void nav_render_full_screen_(struct nav_state *state);
static void nav_show_status_message_(struct nav_state *state,
                                     const char *message);
static const char *nav_selected_link_(const struct nav_state *state);
#define NAV_NEXT_LINK_HOTKEY '\t'

static bool
env_flag_(const char *name, bool default_value)
{
    if (!name)
        return default_value;
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return default_value;
    if (strcmp(value, "0") == 0)
        return false;
    if (strcmp(value, "1") == 0)
        return true;
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "off") == 0)
        return false;
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0)
        return true;
    return default_value;
}

struct nav *
nav_create(const struct nav_opts *opts)
{
    struct nav *nav = calloc(1, sizeof(*nav));
    if (!nav)
        return NULL;
    int parse_flags = opts ? opts->parse_flags : 0;
    if (parse_flags == 0)
        parse_flags = PAGER_PARSE_OSC8;
    nav->parse_flags = parse_flags;
    nav->self_cmd =
        (opts && opts->self_cmd && opts->self_cmd[0]) ? opts->self_cmd : NULL;
    nav->show_status_link = env_flag_("MESS_STATUS_LINKS", true);
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        free(nav);
        return NULL;
    }
    nav->notify_rd = pipefd[0];
    nav->notify_wr = pipefd[1];
    global_nav_    = nav;
    return nav;
}

void
nav_destroy(struct nav *nav)
{
    if (!nav)
        return;
    if (nav->notify_rd >= 0)
        close(nav->notify_rd);
    if (nav->notify_wr >= 0)
        close(nav->notify_wr);
    if (global_nav_ == nav)
        global_nav_ = NULL;
    free(nav);
}

nav_result_t
nav_run(struct nav *nav, const struct offscr_view *view, struct readq *rq,
        int out_fd, size_t width, bool redraw)
{
    if (!nav || !view || !rq || out_fd < 0) {
        log_debug("nav: invalid parameters");
        return NAV_RESULT_ERROR;
    }

    struct nav_state state = {
        .owner               = nav,
        .view                = view,
        .rq                  = rq,
        .out_fd              = out_fd,
        .screen_cols         = width,
        .status_visible      = false,
        .show_link_in_status = nav->show_status_link,
        .has_active_link     = false,
    };

    int parse_flags = nav->parse_flags;
    if (parse_flags == 0)
        parse_flags = PAGER_PARSE_OSC8;

    if ((parse_flags & PAGER_PARSE_OSC8) &&
        parse_links(view, LINKS_KIND_OSC8, &state.links) == -1) {
        log_debug("nav: parse_links osc8 failed");
        return NAV_RESULT_ERROR;
    }
    if ((parse_flags & PAGER_PARSE_MAN) &&
        parse_links(view, LINKS_KIND_MAN, &state.links) == -1) {
        log_debug("nav: parse_links man failed");
        link_iter_free(&state.links);
        return NAV_RESULT_ERROR;
    }

    state.last_logical_row = 0;
    for (size_t i = 0; i < view->len; ++i) {
        if (view->data[i] == '\n')
            state.last_logical_row++;
    }
    state.baseline_row = status_row_(view, state.screen_cols);

    if (redraw) {
        const char reset_seq[] = "\x1b[2J\x1b[H";
        (void)write(out_fd, reset_seq, sizeof(reset_seq) - 1);
        (void)write(out_fd, view->data, view->len);
        char place[32];
        int place_len = snprintf(place, sizeof(place), "\x1b[%zu;1H",
                                 state.baseline_row + 1);
        if (place_len > 0)
            (void)write(out_fd, place, (size_t)place_len);
        nav_draw_status_bar_(&state);
    }

    nav_result_t final_result = NAV_RESULT_ERROR;
    int first                 = true;

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(rq->fd, &readfds);
        int max_fd = rq->fd;

        if (!first) {
            if (nav->notify_rd >= 0) {
                FD_SET(nav->notify_rd, &readfds);
                if (nav->notify_rd > max_fd)
                    max_fd = nav->notify_rd;
            }

            int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                log_debug("nav: select failed");
                final_result = NAV_RESULT_ERROR;
                break;
            }
        }

        if (first || FD_ISSET(rq->fd, &readfds)) {
            if (!first) {
                bool filled = readq_refill(rq);
                (void)filled;
            } else {
                first = false;
            }
            struct nav_process_result pr = nav_process_input_(&state, rq);

            if (pr.status == NAV_PROCESS_STOP) {
                nav_status_clear_(&state);
                final_result = NAV_RESULT_STOP;
                break;
            }
            if (pr.status == NAV_PROCESS_REFRESH) {
                nav_status_clear_(&state);
                const char refresh = '\f';
                (void)write(out_fd, &refresh, 1);
                final_result = NAV_RESULT_REFRESH;
                break;
            }
            if (pr.status == NAV_PROCESS_QUIT) {
                nav_status_clear_(&state);
                final_result = NAV_RESULT_QUIT;
                break;
            }
        }

        if (nav->notify_rd >= 0 && FD_ISSET(nav->notify_rd, &readfds)) {
            char ch;
            ssize_t n = read(nav->notify_rd, &ch, 1);
            if (n > 0 && ch != 0) {
                nav_status_clear_(&state);
                log_debug("nav: canceled");
                final_result = NAV_RESULT_CANCELLED;
                break;
            }
        }
    }

    nav_status_clear_(&state);
    link_iter_free(&state.links);
    return final_result;
}

void
nav_cancel(struct nav *nav)
{
    if (!nav)
        nav = global_nav_;
    if (!nav || nav->notify_wr < 0)
        return;
    char c = 1;
    (void)write(nav->notify_wr, &c, 1);
}

static struct nav_process_result
nav_process_input_(struct nav_state *state, struct readq *rq)
{
    struct nav_process_result result = {
        .status = NAV_PROCESS_CONTINUE,
    };

    while (readq_available_bytes(rq) > 0) {
        unsigned char byte = (unsigned char)rq->buffer[rq->start];

        if (byte == '\x03' || byte == '\x1b') {
            rq->start++;
            result.status = NAV_PROCESS_STOP;
            break;
        }

        if (byte == 'q' || byte == 'Q') {
            rq->start++;
            result.status = NAV_PROCESS_QUIT;
            break;
        }

        if (byte == NAV_NEXT_LINK_HOTKEY) {
            cycle_link_(state);
            rq->start++;
            continue;
        }

        if (byte == 'h' || byte == 'H') {
            move_link_spatial_(state, -1, 0);
            rq->start++;
            continue;
        }

        if (byte == 'j' || byte == 'J') {
            if (!move_link_spatial_(state, 0, +1)) {
                result.status = NAV_PROCESS_STOP;
                break;
            }
            rq->start++;
            continue;
        }

        if (byte == 'k' || byte == 'K') {
            if (!move_link_spatial_(state, 0, -1)) {
                result.status = NAV_PROCESS_STOP;
                break;
            }
            rq->start++;
            continue;
        }

        if (byte == 'l' || byte == 'L') {
            move_link_spatial_(state, +1, 0);
            rq->start++;
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            const struct link_span *span = NULL;
            if (state->links.count > 0) {
                size_t index =
                    state->has_active_link ? state->selected_index : 0;
                span = links_get(&state->links, index % state->links.count);
            }
            if (span && span->link) {
                nav_show_status_message_(state, "Opening link...");
                open_uri_with_mess_(state, span->link);
                nav_render_full_screen_(state);
            }
            rq->start++;
            continue;
        }

        if (byte == 'e' || byte == 'E') {
            const struct link_span *span = NULL;
            if (state->links.count > 0) {
                size_t index =
                    state->has_active_link ? state->selected_index : 0;
                span = links_get(&state->links, index % state->links.count);
            }
            if (span && span->link) {
                nav_show_status_message_(state, "Opening editor...");
                edit_with_editor_(span->link, span->row + 1);
                nav_render_full_screen_(state);
            }
            rq->start++;
            continue;
        }

        if (byte == '\f') {
            rq->start++;
            result.status = NAV_PROCESS_REFRESH;
            break;
        }

        if (isprint(byte))
            log_debug("input: '%c' (0x%02x)", byte, byte);
        else
            log_debug("input: 0x%02x", byte);
        result.status = NAV_PROCESS_STOP;
        break;
    }

    if (rq->start == rq->end) {
        rq->start = 0;
        rq->end   = 0;
    }

    return result;
}

static void
cycle_link_(struct nav_state *state)
{
    if (!state || state->links.count == 0)
        return;
    (void)select_link_at_(state, state->next_index % state->links.count);
}

static size_t
span_col_start_(const struct link_span *span)
{
    return span ? span->columns.start : 0;
}

static size_t
span_col_end_(const struct link_span *span)
{
    if (!span)
        return 0;
    return span->columns.end > span->columns.start ? span->columns.end :
                                                     span->columns.start + 1;
}

static size_t
span_col_center_(const struct link_span *span)
{
    size_t start = span_col_start_(span);
    size_t end   = span_col_end_(span);
    return start + (end - start) / 2;
}

static size_t
column_distance_(size_t col, const struct link_span *span)
{
    size_t start = span_col_start_(span);
    size_t end   = span_col_end_(span);
    if (col < start)
        return start - col;
    if (col >= end)
        return col - (end - 1);
    return 0;
}

static bool
spatial_target_(struct nav_state *state, int dx, int dy, size_t *target)
{
    if (!state || !target || state->links.count == 0)
        return false;

    size_t count   = state->links.count;
    size_t current = state->has_active_link ? state->selected_index % count :
                                              state->next_index % count;
    const struct link_span *cur_span = links_get(&state->links, current);
    if (!cur_span)
        return false;

    size_t block_start = current;
    size_t block_end   = current_block_end_(state, block_start);
    size_t cur_start   = span_col_start_(cur_span);
    size_t cur_end     = span_col_end_(cur_span);
    size_t cur_center  = span_col_center_(cur_span);

    bool found       = false;
    size_t best      = 0;
    size_t best_row  = (size_t)-1;
    size_t best_col  = (size_t)-1;
    size_t best_bias = (size_t)-1;

    for (size_t idx = 0; idx < count; ++idx) {
        if (index_in_block_(idx, block_start, block_end, count))
            continue;

        const struct link_span *span = links_get(&state->links, idx);
        if (!span)
            continue;

        size_t row_dist = 0;
        size_t col_dist = 0;
        size_t bias     = idx;

        if (dx < 0) {
            if (span->row == cur_span->row) {
                if (span_col_end_(span) > cur_start)
                    continue;
                row_dist = 0;
                col_dist = cur_start - span_col_end_(span);
                bias     = (size_t)0 - span_col_end_(span);
            } else if (span->row < cur_span->row) {
                row_dist = cur_span->row - span->row;
                col_dist = (size_t)0 - span_col_end_(span);
                bias     = (size_t)0 - span_col_start_(span);
            } else {
                continue;
            }
        } else if (dx > 0) {
            if (span->row == cur_span->row) {
                if (span_col_start_(span) < cur_end)
                    continue;
                row_dist = 0;
                col_dist = span_col_start_(span) - cur_end;
                bias     = span_col_start_(span);
            } else if (span->row > cur_span->row) {
                row_dist = span->row - cur_span->row;
                col_dist = span_col_start_(span);
                bias     = span_col_start_(span);
            } else {
                continue;
            }
        } else if (dy < 0) {
            if (span->row >= cur_span->row)
                continue;
            row_dist = cur_span->row - span->row;
            col_dist = column_distance_(cur_center, span);
            bias     = (size_t)-span->row;
        } else if (dy > 0) {
            if (span->row <= cur_span->row)
                continue;
            row_dist = span->row - cur_span->row;
            col_dist = column_distance_(cur_center, span);
            bias     = span->row;
        } else {
            return false;
        }

        if (!found || row_dist < best_row ||
            (row_dist == best_row && col_dist < best_col) ||
            (row_dist == best_row && col_dist == best_col &&
             bias < best_bias)) {
            found     = true;
            best      = idx;
            best_row  = row_dist;
            best_col  = col_dist;
            best_bias = bias;
        }
    }

    if (!found)
        return false;
    *target = best;
    return true;
}

static bool
move_link_spatial_(struct nav_state *state, int dx, int dy)
{
    size_t target = 0;
    if (spatial_target_(state, dx, dy, &target))
        return select_link_at_(state, target);
    return false;
}

static bool
select_link_at_(struct nav_state *state, size_t target)
{
    if (!state || state->links.count == 0)
        return false;

    size_t count        = state->links.count;
    size_t target_start = target % count;

    restore_active_block_(state);

    state->selected_index = target_start;
    state->next_index     = highlight_block_(state, target_start);
    finish_repaint_(state);
    return true;
}

static size_t
current_block_end_(struct nav_state *state, size_t start)
{
    if (!state || state->links.count == 0)
        return start;

    size_t count                  = state->links.count;
    const struct link_span *first = links_get(&state->links, start % count);
    if (!first)
        return start % count;

    size_t current = start % count;
    while (current + 1 < count) {
        size_t next                  = current + 1;
        const struct link_span *span = links_get(&state->links, next);
        if (!span || !urls_equal_(first->link, span->link))
            return current;
        current = next;
    }
    return current;
}

static bool
index_in_block_(size_t idx, size_t start, size_t end, size_t count)
{
    if (count == 0)
        return false;
    idx %= count;
    start %= count;
    end %= count;
    if (start <= end)
        return idx >= start && idx <= end;
    return idx >= start || idx <= end;
}

static size_t
highlight_block_(struct nav_state *state, size_t start_idx)
{
    if (!state)
        return start_idx;
    size_t count = state->links.count;
    if (count == 0) {
        state->has_active_link = false;
        return start_idx;
    }

    size_t idx       = start_idx % count;
    bool first_entry = true;
    bool highlighted = false;

    const struct link_span *first_span = links_get(&state->links, idx);
    const char *url                    = first_span ? first_span->link : NULL;

    while (idx < count) {
        const struct link_span *span = links_get(&state->links, idx);
        if (!span)
            break;
        if (!first_entry && !urls_equal_(url, span->link))
            break;

        char *line = render_line_for_span_(state, span);
        if (line)
            replace_line_(state, span->row, line);

        highlighted = true;
        first_entry = false;
        idx++;
    }

    state->has_active_link = highlighted;
    return idx;
}

static void
restore_active_block_(struct nav_state *state)
{
    if (!state || !state->has_active_link || state->links.count == 0)
        return;

    size_t count = state->links.count;
    size_t idx   = state->selected_index % count;
    size_t end   = current_block_end_(state, idx);

    while (true) {
        const struct link_span *span = links_get(&state->links, idx);
        char *line                   = render_plain_line_(state, span);
        if (line && span)
            replace_line_(state, span->row, line);
        if (idx == end)
            break;
        idx++;
    }

    state->has_active_link = false;
}

static bool
urls_equal_(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}

static void
advance_printable_cell_(size_t width, size_t *visual_row, size_t *col)
{
    if (!visual_row || !col)
        return;
    if (width > 0 && *col == width) {
        (*visual_row)++;
        *col = 0;
    }
    (*col)++;
}

static size_t
utf8_advance_(const char *data, size_t len, size_t idx)
{
    if (!data || idx >= len)
        return idx + 1;
    unsigned char ch = (unsigned char)data[idx];
    if (ch < 0x80)
        return idx + 1;

    size_t need = 1;
    if ((ch & 0xe0) == 0xc0)
        need = 2;
    else if ((ch & 0xf0) == 0xe0)
        need = 3;
    else if ((ch & 0xf8) == 0xf0)
        need = 4;

    if (idx + need > len)
        return idx + 1;
    for (size_t pos = idx + 1; pos < idx + need; ++pos) {
        if (((unsigned char)data[pos] & 0xc0) != 0x80)
            return idx + 1;
    }
    return idx + need;
}

static size_t
visual_row_(const struct offscr_view *view, size_t width, size_t target_row)
{
    if (!view || !view->data || target_row == 0)
        return 0;

    size_t logical_row = 0;
    size_t visual_row  = 0;
    size_t col         = 0;
    size_t idx         = 0;

    while (idx < view->len) {
        unsigned char ch = (unsigned char)view->data[idx];
        if (ch == '\n') {
            logical_row++;
            visual_row++;
            col = 0;
            idx++;
            if (logical_row == target_row)
                return visual_row;
            continue;
        }
        if (ch == '\r') {
            col = 0;
            idx++;
            continue;
        }
        if (ch == '\b') {
            if (col > 0)
                col--;
            idx++;
            continue;
        }
        if (ch == '\033') {
            idx = ansi_skip_seq(view->data, view->len, idx);
            continue;
        }
        advance_printable_cell_(width, &visual_row, &col);
        idx = utf8_advance_(view->data, view->len, idx);
    }

    return visual_row;
}

static size_t
status_row_(const struct offscr_view *view, size_t width)
{
    if (!view || !view->data)
        return 0;

    size_t visual_row = 0;
    size_t col        = 0;
    size_t idx        = 0;

    while (idx < view->len) {
        unsigned char ch = (unsigned char)view->data[idx];
        if (ch == '\n') {
            visual_row++;
            col = 0;
            idx++;
            continue;
        }
        if (ch == '\r') {
            col = 0;
            idx++;
            continue;
        }
        if (ch == '\b') {
            if (col > 0)
                col--;
            idx++;
            continue;
        }
        if (ch == '\033') {
            idx = ansi_skip_seq(view->data, view->len, idx);
            continue;
        }
        advance_printable_cell_(width, &visual_row, &col);
        idx = utf8_advance_(view->data, view->len, idx);
    }

    return visual_row;
}

static size_t
visual_rows_(const struct nav_state *state, size_t row)
{
    if (!state || !state->view)
        return 1;

    size_t start = visual_row_(state->view, state->screen_cols, row);
    size_t end   = row < state->last_logical_row ?
                       visual_row_(state->view, state->screen_cols, row + 1) :
                       state->baseline_row + 1;
    if (end <= start)
        return 1;
    return end - start;
}

static char *
render_line_for_span_(const struct nav_state *state,
                      const struct link_span *span)
{
    const char *fallback = (span && span->text) ? span->text : NULL;
    if (!state || !span)
        return fallback ? strdup(fallback) : NULL;
    if (!state->view)
        return fallback ? strdup(fallback) : NULL;

    char *plain = offscr_extract(state->view, span->row);
    if (!plain)
        return fallback ? strdup(fallback) : NULL;

    struct styler_style style = {
        .span      = span->indices,
        .enter_seq = "\x1b[7m",
        .exit_seq  = "\x1b[0m",
    };
    char *styled = styler_apply_offsets(plain, &style, 1);
    if (styled) {
        free(plain);
        return styled;
    }
    return plain;
}

static char *
render_plain_line_(const struct nav_state *state, const struct link_span *span)
{
    if (!state || !span || !state->view)
        return NULL;
    return offscr_extract(state->view, span->row);
}

static void
replace_line_(struct nav_state *state, size_t row, char *content)
{
    if (!state || !content)
        return;

    char seq[64];
    size_t screen_row = visual_row_(state->view, state->screen_cols, row);
    size_t rows       = visual_rows_(state, row);
    for (size_t visual = 0; visual < rows; ++visual) {
        int len = snprintf(seq, sizeof(seq), "\x1b[%zu;1H\x1b[2K",
                           screen_row + visual + 1);
        if (len > 0 && write(state->out_fd, seq, (size_t)len) == -1)
            log_debug("nav: write clear seq failed");
    }

    int len = snprintf(seq, sizeof(seq), "\x1b[%zu;1H", screen_row + 1);
    if (len > 0 && write(state->out_fd, seq, (size_t)len) == -1)
        log_debug("nav: write seq failed");

    size_t content_len = strlen(content);
    if (content_len > 0 && write(state->out_fd, content, content_len) == -1)
        log_debug("nav: write content failed");

    free(content);
}

static void
finish_repaint_(struct nav_state *state)
{
    if (!state)
        return;
    char restore[32];
    int restore_len = snprintf(restore, sizeof(restore), "\x1b[%zu;1H",
                               state->baseline_row + 1);
    if (restore_len > 0)
        (void)write(state->out_fd, restore, (size_t)restore_len);
    nav_draw_status_bar_(state);
}

static void
open_uri_with_mess_(const struct nav_state *state, const char *uri)
{
    if (!state || !uri || uri[0] == '\0')
        return;
    const char *binary = "mess";
    if (state->owner && state->owner->self_cmd &&
        state->owner->self_cmd[0] != '\0')
        binary = state->owner->self_cmd;

    pid_t pid = fork();
    if (pid == -1) {
        log_debug("nav: fork failed while opening uri");
        return;
    }
    if (pid == 0) {
        execlp(binary, binary, uri, (char *)NULL);
        perror(binary);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) == -1)
        log_debug("nav: waitpid failed for uri child");
}

static void
edit_with_editor_(const char *fallback_path, size_t line)
{
    const char *target = getenv("MESSFILE");
    if (!target || target[0] == '\0')
        target = fallback_path;
    if (!target || target[0] == '\0')
        return;

    const char *cmd = getenv("VISUAL");
    if (cmd == NULL || cmd[0] == '\0')
        cmd = getenv("EDITOR");
    if (cmd == NULL || cmd[0] == '\0')
        cmd = "vi";

    wordexp_t we;
    if (wordexp(cmd, &we, WRDE_NOCMD) != 0)
        return;

    char line_arg[64];
    bool have_line = line > 0;
    if (have_line)
        snprintf(line_arg, sizeof(line_arg), "+%zu", line);

    char **argv = calloc(we.we_wordc + (have_line ? 3 : 2), sizeof(char *));
    if (!argv) {
        wordfree(&we);
        return;
    }
    for (size_t i = 0; i < we.we_wordc; ++i)
        argv[i] = we.we_wordv[i];
    if (have_line && editor_file_first_(we.we_wordv[0])) {
        argv[we.we_wordc]     = (char *)target;
        argv[we.we_wordc + 1] = line_arg;
        argv[we.we_wordc + 2] = NULL;
    } else if (have_line) {
        argv[we.we_wordc]     = line_arg;
        argv[we.we_wordc + 1] = (char *)target;
        argv[we.we_wordc + 2] = NULL;
    } else {
        argv[we.we_wordc]     = (char *)target;
        argv[we.we_wordc + 1] = NULL;
    }

    pid_t pid = fork();
    if (pid == -1) {
        free(argv);
        wordfree(&we);
        log_debug("nav: fork failed while editing uri");
        return;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    free(argv);
    wordfree(&we);
    if (waitpid(pid, NULL, 0) == -1)
        log_debug("nav: waitpid failed for editor child");
}

static bool
editor_file_first_(const char *cmd)
{
    if (!cmd || !*cmd)
        return false;
    const char *base = strrchr(cmd, '/');
    base             = base ? base + 1 : cmd;
    return strcmp(base, "kak") == 0;
}

static void
nav_draw_status_bar_(struct nav_state *state)
{
    if (!state)
        return;
    static const char clear_seq[]   = "\x1b[2K";
    static const char pip_label[]   = "[mess] ";
    static const char color_start[] = "\x1b[48;5;238m\x1b[97m";
    static const char color_end[]   = "\x1b[0m";

    (void)write(state->out_fd, clear_seq, sizeof(clear_seq) - 1);

    size_t width = state->screen_cols;
    if (width == 0) {
        (void)write(state->out_fd, color_start, sizeof(color_start) - 1);
        (void)write(state->out_fd, pip_label, strlen(pip_label));
        (void)write(state->out_fd, color_end, sizeof(color_end) - 1);
        char move[32];
        int move_len = snprintf(move, sizeof(move), "\x1b[%zu;1H",
                                state->baseline_row + 1);
        if (move_len > 0)
            (void)write(state->out_fd, move, (size_t)move_len);
        state->status_visible = true;
        return;
    }

    char *bar = calloc(width, sizeof(char));
    if (!bar) {
        (void)write(state->out_fd, color_start, sizeof(color_start) - 1);
        (void)write(state->out_fd, pip_label, strlen(pip_label));
        (void)write(state->out_fd, color_end, sizeof(color_end) - 1);
        (void)write(state->out_fd, "\r", 1);
        state->status_visible = true;
        return;
    }

    memset(bar, ' ', width);
    size_t label_len = strlen(pip_label);
    size_t copy_len  = (label_len > width) ? width : label_len;
    memcpy(bar, pip_label, copy_len);

    const char *selected_link = nav_selected_link_(state);
    if (selected_link && copy_len < width) {
        static const char prefix[] = "=> ";
        size_t pos                 = copy_len;
        for (size_t i = 0; i < sizeof(prefix) - 1 && pos < width; ++i)
            bar[pos++] = prefix[i];
        if (pos < width) {
            size_t remaining = width - pos;
            size_t link_len  = strlen(selected_link);
            if (link_len <= remaining) {
                memcpy(bar + pos, selected_link, link_len);
            } else if (remaining >= 4) {
                size_t display = remaining - 3;
                memcpy(bar + pos, selected_link, display);
                memcpy(bar + pos + display, "...", 3);
            } else {
                memcpy(bar + pos, selected_link, remaining);
            }
        }
    }
    (void)write(state->out_fd, color_start, sizeof(color_start) - 1);
    (void)write(state->out_fd, bar, width);
    free(bar);
    (void)write(state->out_fd, color_end, sizeof(color_end) - 1);
    char move[64];
    size_t col   = width > 0 ? width : 1;
    int move_len = snprintf(move, sizeof(move), "\x1b[%zu;%zuH",
                            state->baseline_row + 1, col);
    if (move_len > 0)
        (void)write(state->out_fd, move, (size_t)move_len);
    state->status_visible = true;
}

static void
nav_status_clear_(struct nav_state *state)
{
    if (!state || !state->status_visible)
        return;

    nav_restore_active_selection_(state);

    if (state->baseline_row != state->last_logical_row && state->view &&
        state->view->data) {
        static const char clear_seq[] = "\x1b[2J\x1b[H";
        (void)write(state->out_fd, clear_seq, sizeof(clear_seq) - 1);
        if (state->view->len > 0)
            (void)write(state->out_fd, state->view->data, state->view->len);
        char move[32];
        int move_len = snprintf(move, sizeof(move), "\x1b[%zu;1H",
                                state->baseline_row + 1);
        if (move_len > 0)
            (void)write(state->out_fd, move, (size_t)move_len);
        state->status_visible = false;
        return;
    }

    char seq[64];
    size_t row = state->baseline_row + 1;
    int len    = snprintf(seq, sizeof(seq), "\x1b[%zu;1H\x1b[2K", row);
    if (len > 0)
        (void)write(state->out_fd, seq, (size_t)len);

    char *last_row = NULL;
    if (state->view)
        last_row = offscr_extract(state->view, state->last_logical_row);
    if (last_row) {
        size_t text_len = strlen(last_row);
        if (text_len > 0)
            (void)write(state->out_fd, last_row, text_len);
        free(last_row);
    }
    (void)write(state->out_fd, "\r", 1);
    state->status_visible = false;
}

static void
nav_restore_active_selection_(struct nav_state *state)
{
    restore_active_block_(state);
    finish_repaint_(state);
}

static void
nav_render_full_screen_(struct nav_state *state)
{
    if (!state || !state->view || !state->view->data)
        return;
    static const char clear_seq[] = "\x1b[2J\x1b[H";
    (void)write(state->out_fd, clear_seq, sizeof(clear_seq) - 1);
    if (state->view->len > 0)
        (void)write(state->out_fd, state->view->data, state->view->len);
    char move[32];
    int move_len =
        snprintf(move, sizeof(move), "\x1b[%zu;1H", state->baseline_row + 1);
    if (move_len > 0)
        (void)write(state->out_fd, move, (size_t)move_len);
    nav_draw_status_bar_(state);
}

static void
nav_show_status_message_(struct nav_state *state, const char *message)
{
    if (!state || !message)
        return;
    char seq[64];
    size_t row = state->baseline_row + 1;
    int len    = snprintf(seq, sizeof(seq), "\x1b[%zu;1H\x1b[2K", row);
    if (len > 0)
        (void)write(state->out_fd, seq, (size_t)len);
    static const char color_start[] = "\x1b[48;5;238m\x1b[97m";
    static const char color_end[]   = "\x1b[0m";
    static const char pip_label[]   = "[mess] ";
    (void)write(state->out_fd, color_start, sizeof(color_start) - 1);
    (void)write(state->out_fd, pip_label, strlen(pip_label));
    (void)write(state->out_fd, message, strlen(message));
    (void)write(state->out_fd, color_end, sizeof(color_end) - 1);
    (void)write(state->out_fd, "\r", 1);
    state->status_visible = true;
}

static const char *
nav_selected_link_(const struct nav_state *state)
{
    if (!state || !state->show_link_in_status || !state->has_active_link)
        return NULL;
    if (state->links.count == 0)
        return NULL;
    const struct link_span *span =
        links_get(&state->links, state->selected_index % state->links.count);
    if (!span || !span->link || span->link[0] == '\0')
        return NULL;
    return span->link;
}
