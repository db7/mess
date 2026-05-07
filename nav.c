#include "nav.h"

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
#include <unistd.h>

struct nav {
    int notify_rd;
    int notify_wr;
};

enum nav_action_type {
    NAV_ACTION_REPLACE_LINE = 0,
    NAV_ACTION_OPEN_URI,
    NAV_ACTION_EDIT_URI,
};

struct nav_action {
    enum nav_action_type type;
    size_t row;
    char *content;
    char *uri;
    bool is_skip;
    struct nav_action *next;
};

enum nav_process_code {
    NAV_PROCESS_CONTINUE = 0,
    NAV_PROCESS_STOP,
    NAV_PROCESS_REFRESH,
};

struct nav_process_result {
    enum nav_process_code status;
    struct nav_action *actions;
};

struct nav_action_list {
    struct nav_action *head;
    struct nav_action *tail;
};

struct nav_state {
    const struct offscr_view *view;
    struct readq *rq;
    struct link_iter prev;
    struct link_iter cur;
    size_t baseline_row;
    int out_fd;
};

static struct nav *global_nav_;

static void cycle_link_(struct nav_state *state, int direction,
                        struct nav_action_list *actions);
static size_t step_index_(size_t idx, size_t count, int direction);
static size_t find_block_start_(struct nav_state *state, size_t idx);
static size_t collect_block_(struct nav_state *state, size_t start_idx,
                             struct nav_action_list *actions);
static void append_plain_history_forward_(struct nav_state *state,
                                          size_t target,
                                          struct nav_action_list *actions);
static void append_plain_history_backward_(struct nav_state *state,
                                           size_t target,
                                           struct nav_action_list *actions);
static bool urls_equal_(const char *a, const char *b);
static char *render_line_for_span_(const struct nav_state *state,
                                   const struct link_span *span);
static char *render_plain_line_(const struct nav_state *state,
                                const struct link_span *span);
static void append_replace_line_action_(struct nav_action_list *actions,
                                        size_t row, char *line,
                                        bool is_skip);
static void append_uri_action_(struct nav_action_list *actions,
                               enum nav_action_type type, const char *uri);
static void apply_actions_(const struct nav_state *state,
                           const struct nav_action *actions);
static void nav_log_actions_(const struct nav_state *state,
                             const struct nav_action *actions);
static void nav_free_actions(struct nav_action *);
static struct nav_process_result nav_process_input_(struct nav_state *,
                                                    struct readq *);

struct nav *
nav_create(const struct nav_opts *opts)
{
    struct nav *nav = calloc(1, sizeof(*nav));
    if (!nav)
        return NULL;
    log_init(opts ? opts->debug_fd : -1);
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
        int out_fd)
{
    if (!nav || !view || !rq || out_fd < 0) {
        log_debugln("nav: invalid parameters");
        return NAV_RESULT_ERROR;
    }

    struct nav_state state = {
        .view   = view,
        .rq     = rq,
        .out_fd = out_fd,
    };

    if (parse_links(view, LINKS_KIND_OSC8, &state.prev) == -1) {
        log_debugln("nav: parse_links failed");
        return NAV_RESULT_ERROR;
    }
    state.cur = state.prev;

    state.baseline_row = 0;
    for (size_t i = 0; i < view->len; ++i) {
        if (view->data[i] == '\n')
            state.baseline_row++;
    }

    const char reset_seq[] = "\x1b[2J\x1b[H";
    (void)write(out_fd, reset_seq, sizeof(reset_seq) - 1);
    (void)write(out_fd, view->data, view->len);

    nav_result_t final_result = NAV_RESULT_ERROR;

    while (1) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        pfds[nfds].fd      = rq->fd;
        pfds[nfds].events  = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
        if (nav->notify_rd >= 0) {
            pfds[nfds].fd      = nav->notify_rd;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int ready = poll(pfds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            log_debugln("nav: poll failed");
            final_result = NAV_RESULT_ERROR;
            break;
        }

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_debugln("nav: tty error");
            final_result = NAV_RESULT_ERROR;
            break;
        }

        if (pfds[0].revents & POLLIN) {
            bool filled = readq_refill(rq);
            (void)filled;
            struct nav_process_result pr = nav_process_input_(&state, rq);
            apply_actions_(&state, pr.actions);
            nav_log_actions_(&state, pr.actions);
            nav_free_actions(pr.actions);

            if (pr.status == NAV_PROCESS_STOP) {
                final_result = NAV_RESULT_STOP;
                break;
            }
            if (pr.status == NAV_PROCESS_REFRESH) {
                const char refresh = '\f';
                (void)write(out_fd, &refresh, 1);
                final_result = NAV_RESULT_REFRESH;
                break;
            }
        }

        if (nav->notify_rd >= 0 && nfds > 1 &&
            (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            char ch;
            ssize_t n = read(nav->notify_rd, &ch, 1);
            if (n > 0 && ch != 0) {
                log_debugln("nav: cancelled");
                final_result = NAV_RESULT_CANCELLED;
                break;
            }
        }
    }

    link_iter_free(&state.prev);
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
        .status  = NAV_PROCESS_CONTINUE,
        .actions = NULL,
    };

    struct nav_action_list actions = {0};

    while (readq_available_bytes(rq) > 0) {
        unsigned char byte = (unsigned char)rq->buffer[rq->start];

        if (byte == '\x03' || byte == '\x1b') {
            rq->start++;
            result.status = NAV_PROCESS_STOP;
            break;
        }

        if (byte == ' ') {
            cycle_link_(state, +1, &actions);
            rq->start++;
            continue;
        }

        if (byte == '\x7f' || byte == '\b') {
            cycle_link_(state, -1, &actions);
            rq->start++;
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            const struct link_span *span = NULL;
            if (state->prev.count > 0)
                span = links_get(&state->prev,
                                 state->prev.index % state->prev.count);
            if (span && span->link)
                append_uri_action_(&actions, NAV_ACTION_OPEN_URI, span->link);
            rq->start++;
            continue;
        }

        if (byte == 'e' || byte == 'E') {
            const struct link_span *span = NULL;
            if (state->prev.count > 0)
                span = links_get(&state->prev,
                                 state->prev.index % state->prev.count);
            if (span && span->link)
                append_uri_action_(&actions, NAV_ACTION_EDIT_URI, span->link);
            rq->start++;
            continue;
        }

        if (byte == '\f') {
            rq->start++;
            result.status = NAV_PROCESS_REFRESH;
            break;
        }

        if (isprint(byte))
            log_debugln("input: '%c' (0x%02x)", byte, byte);
        else
            log_debugln("input: 0x%02x", byte);
        rq->start++;
    }

    if (rq->start == rq->end) {
        rq->start = 0;
        rq->end   = 0;
    }

    result.actions = actions.head;
    return result;
}

static void
cycle_link_(struct nav_state *state, int direction,
            struct nav_action_list *actions)
{
    if (!state || !actions || state->cur.count == 0)
        return;

    size_t count = state->cur.count;
    int dir      = (direction >= 0) ? 1 : -1;
    size_t idx   = state->cur.index % count;

    idx = find_block_start_(state, idx);

    if (dir >= 0)
        append_plain_history_forward_(state, idx, actions);
    else
        append_plain_history_backward_(state, idx, actions);

    size_t block_end = collect_block_(state, idx, actions);

    if (dir >= 0)
        state->cur.index = block_end;
    else
        state->cur.index = idx;
}

static size_t
step_index_(size_t idx, size_t count, int direction)
{
    if (count == 0)
        return 0;
    if (direction >= 0)
        return (idx + 1) % count;
    return (idx == 0) ? (count - 1) : (idx - 1);
}

static size_t
find_block_start_(struct nav_state *state, size_t idx)
{
    size_t count = state->cur.count;
    if (count == 0)
        return idx;

    const struct link_span *span = links_get(&state->cur, idx % count);
    if (!span)
        return idx % count;
    const char *url = span->link;
    size_t current  = idx % count;

    while (true) {
        size_t prev = step_index_(current, count, -1);
        if (prev == current)
            break;
        const struct link_span *prev_span = links_get(&state->cur, prev);
        if (!prev_span || !urls_equal_(url, prev_span->link))
            break;
        current = prev;
        if (current == idx)
            break;
    }
    return current;
}

static size_t
collect_block_(struct nav_state *state, size_t start_idx,
               struct nav_action_list *actions)
{
    size_t count = state->cur.count;
    if (count == 0)
        return start_idx;

    size_t idx       = start_idx % count;
    size_t processed = 0;
    bool first_entry = true;

    const struct link_span *first_span = links_get(&state->cur, idx);
    const char *url = first_span ? first_span->link : NULL;

    while (processed < count) {
        const struct link_span *span = links_get(&state->cur, idx);
        if (!span)
            break;
        if (!first_entry && !urls_equal_(url, span->link))
            break;

        char *line = render_line_for_span_(state, span);
        if (line)
            append_replace_line_action_(actions, span->row, line,
                                        !first_entry);

        processed++;
        first_entry = false;
        idx         = step_index_(idx, count, +1);
        if (idx == start_idx)
            break;
    }

    return idx;
}

static void
append_plain_history_forward_(struct nav_state *state, size_t target,
                              struct nav_action_list *actions)
{
    if (!state || !actions || state->prev.count == 0)
        return;

    size_t count = state->prev.count;
    size_t idx   = state->prev.index % count;
    size_t dest  = target % count;

    while (idx != dest) {
        const struct link_span *span = links_get(&state->prev, idx);
        char *line = render_plain_line_(state, span);
        if (line && span)
            append_replace_line_action_(actions, span->row, line, false);
        idx = step_index_(idx, count, +1);
    }

    state->prev.index = dest;
}

static void
append_plain_history_backward_(struct nav_state *state, size_t target,
                               struct nav_action_list *actions)
{
    if (!state || !actions || state->prev.count == 0)
        return;

    size_t count = state->prev.count;
    size_t idx   = state->prev.index % count;
    size_t dest  = target % count;

    while (idx != dest) {
        idx = step_index_(idx, count, -1);
        const struct link_span *span = links_get(&state->prev, idx);
        char *line = render_plain_line_(state, span);
        if (line && span)
            append_replace_line_action_(actions, span->row, line, false);
    }

    state->prev.index = dest;
}

static bool
urls_equal_(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
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
render_plain_line_(const struct nav_state *state,
                   const struct link_span *span)
{
    if (!state || !span || !state->view)
        return NULL;
    return offscr_extract(state->view, span->row);
}

static void
append_replace_line_action_(struct nav_action_list *actions, size_t row,
                            char *line, bool is_skip)
{
    if (!actions || !line)
        return;

    struct nav_action *action = calloc(1, sizeof(*action));
    if (!action) {
        free(line);
        return;
    }

    action->type    = NAV_ACTION_REPLACE_LINE;
    action->row     = row;
    action->content = line;
    action->is_skip = is_skip;

    if (!actions->head)
        actions->head = action;
    else
        actions->tail->next = action;
    actions->tail = action;
}

static void
append_uri_action_(struct nav_action_list *actions,
                   enum nav_action_type type, const char *uri)
{
    if (!actions || !uri)
        return;
    struct nav_action *action = calloc(1, sizeof(*action));
    if (!action)
        return;
    action->type = type;
    action->uri  = strdup(uri);
    if (!action->uri) {
        free(action);
        return;
    }
    if (!actions->head)
        actions->head = action;
    else
        actions->tail->next = action;
    actions->tail = action;
}

static void
apply_actions_(const struct nav_state *state,
               const struct nav_action *actions)
{
    if (!state)
        return;
    for (const struct nav_action *action = actions; action;
         action                         = action->next) {
        if (action->type != NAV_ACTION_REPLACE_LINE || !action->content)
            continue;
        char seq[64];
        int len = snprintf(seq, sizeof(seq), "\x1b[%zu;1H\x1b[2K",
                           action->row + 1);
        if (len > 0) {
            if (write(state->out_fd, seq, (size_t)len) == -1)
                log_debugln("nav: write seq failed");
        }
        size_t l = strlen(action->content);
        if (l > 0) {
            if (write(state->out_fd, action->content, l) == -1)
                log_debugln("nav: write content failed");
        }
    }
    char restore[32];
    int restore_len = snprintf(restore, sizeof(restore), "\x1b[%zu;1H",
                               state->baseline_row + 1);
    if (restore_len > 0)
        (void)write(state->out_fd, restore, (size_t)restore_len);
}

static void
nav_log_actions_(const struct nav_state *state,
                 const struct nav_action *actions)
{
    (void)state;

    for (const struct nav_action *action = actions; action;
         action                         = action->next) {
        if (action->type == NAV_ACTION_REPLACE_LINE && action->content) {
            const char *label = action->is_skip ? "skip" : "link";
            log_debugln("%s row %zu: %s", label, action->row + 1,
                        action->content);
        } else if ((action->type == NAV_ACTION_OPEN_URI ||
                    action->type == NAV_ACTION_EDIT_URI) &&
                   action->uri) {
            const char *verb =
                action->type == NAV_ACTION_OPEN_URI ? "open" : "edit";
            log_debugln("%s %s", verb, action->uri);
        }
    }
}

static void
nav_free_actions(struct nav_action *actions)
{
    while (actions) {
        struct nav_action *next = actions->next;
        free(actions->content);
        free(actions->uri);
        free(actions);
        actions = next;
    }
}
