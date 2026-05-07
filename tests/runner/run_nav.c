#include "nav.h"
#include "offscr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
print_snapshot_(const struct offscr_view *view)
{
    if (view->len == 0)
        return;
    if (write(STDOUT_FILENO, view->data, view->len) == -1)
        perror("write snapshot");
}

struct key_buffer {
    char *data;
    size_t len;
    size_t cap;
};

static int
key_buffer_append_(struct key_buffer *buf, char ch)
{
    if (buf->len == buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 16;
        char *tmp      = realloc(buf->data, new_cap);
        if (tmp == NULL)
            return -1;
        buf->data = tmp;
        buf->cap  = new_cap;
    }
    buf->data[buf->len++] = ch;
    return 0;
}

static int
append_token_(struct key_buffer *buf, const char *tok, size_t len)
{
    char lower[32];
    if (len >= sizeof(lower))
        return -1;
    for (size_t i = 0; i < len; ++i)
        lower[i] = (char)tolower((unsigned char)tok[i]);
    lower[len] = '\0';

    if (strcmp(lower, "tab") == 0)
        return key_buffer_append_(buf, '\t');
    if (strcmp(lower, "ret") == 0 || strcmp(lower, "enter") == 0 ||
        strcmp(lower, "return") == 0 || strcmp(lower, "cr") == 0)
        return key_buffer_append_(buf, '\r');
    if (strcmp(lower, "lf") == 0)
        return key_buffer_append_(buf, '\n');
    if (strcmp(lower, "esc") == 0 || strcmp(lower, "escape") == 0)
        return key_buffer_append_(buf, '\x1b');
    if (strcmp(lower, "space") == 0)
        return key_buffer_append_(buf, ' ');
    if (strcmp(lower, "backspace") == 0 || strcmp(lower, "bs") == 0)
        return key_buffer_append_(buf, '\b');
    if (strcmp(lower, "del") == 0 || strcmp(lower, "delete") == 0)
        return key_buffer_append_(buf, '\x7f');
    if ((lower[0] == '0' && lower[1] == 'x' && len > 2) ||
        (lower[0] == 'x' && len > 1)) {
        const char *hex = lower[0] == 'x' ? lower + 1 : lower + 2;
        char *end        = NULL;
        long value       = strtol(hex, &end, 16);
        if (end == hex || *end != '\0' || value < 0 || value > 0xff)
            return -1;
        return key_buffer_append_(buf, (char)value);
    }
    return -1;
}

static int
parse_sequence_(const char *spec, struct key_buffer *buf)
{
    while (*spec) {
        if (*spec == '\\') {
            spec++;
            if (*spec == '\0')
                return -1;
            char escaped = *spec++;
            switch (escaped) {
                case 't':
                    if (key_buffer_append_(buf, '\t') == -1)
                        return -1;
                    break;
                case 'r':
                    if (key_buffer_append_(buf, '\r') == -1)
                        return -1;
                    break;
                case 'n':
                    if (key_buffer_append_(buf, '\n') == -1)
                        return -1;
                    break;
                case 'e':
                case 'E':
                    if (key_buffer_append_(buf, '\x1b') == -1)
                        return -1;
                    break;
                case '\\':
                    if (key_buffer_append_(buf, '\\') == -1)
                        return -1;
                    break;
                case '\'':
                    if (key_buffer_append_(buf, '\'') == -1)
                        return -1;
                    break;
                case '\"':
                    if (key_buffer_append_(buf, '\"') == -1)
                        return -1;
                    break;
                case 'x': {
                    if (*spec == '\0')
                        return -1;
                    char hexbuf[3] = {0};
                    size_t idx     = 0;
                    while (idx < 2 && *spec && isxdigit((unsigned char)*spec))
                        hexbuf[idx++] = *spec++;
                    if (idx == 0)
                        return -1;
                    char *end    = NULL;
                    long result  = strtol(hexbuf, &end, 16);
                    if (end == hexbuf)
                        return -1;
                    if (key_buffer_append_(buf, (char)result) == -1)
                        return -1;
                    break;
                }
                case '0': {
                    char octbuf[4] = {0};
                    size_t idx     = 0;
                    while (idx < 3 && *spec >= '0' && *spec <= '7')
                        octbuf[idx++] = *spec++;
                    if (idx == 0)
                        return -1;
                    char *end   = NULL;
                    long value  = strtol(octbuf, &end, 8);
                    if (end == octbuf)
                        return -1;
                    if (key_buffer_append_(buf, (char)value) == -1)
                        return -1;
                    break;
                }
                default:
                    if (key_buffer_append_(buf, escaped) == -1)
                        return -1;
                    break;
            }
            continue;
        }
        if (*spec == '<') {
            const char *end = strchr(spec, '>');
            if (end == NULL || end == spec + 1)
                return -1;
            if (append_token_(buf, spec + 1, (size_t)(end - spec - 1)) == -1)
                return -1;
            spec = end + 1;
            continue;
        }
        if (key_buffer_append_(buf, *spec++) == -1)
            return -1;
    }
    return 0;
}

static void
usage_(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-k sequence]\n"
            "  sequence accepts tokens like <tab>, <ret>, <esc>, <space>\n"
            "  escapes: \\\\t, \\\\r, \\\\n, \\\\e, \\\\xNN, \\\\0NNN\n",
            prog);
}

int
main(int argc, char **argv)
{
    const char *key_spec = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "hk:")) != -1) {
        switch (opt) {
            case 'k':
                key_spec = optarg;
                break;
            case 'h':
                usage_(argv[0]);
                return 0;
            default:
                usage_(argv[0]);
                return 1;
        }
    }

    int input_pipe[2] = {-1, -1};
    struct key_buffer keys = {0};

    int tty_fd = -1;
    if (key_spec != NULL) {
        if (parse_sequence_(key_spec, &keys) == -1) {
            fprintf(stderr, "invalid key sequence: %s\n", key_spec);
            free(keys.data);
            return 1;
        }
        if (keys.len == 0 || keys.data[keys.len - 1] != '\x1b') {
            if (key_buffer_append_(&keys, '\x1b') == -1) {
                free(keys.data);
                return 1;
            }
        }
        if (pipe(input_pipe) == -1) {
            perror("pipe");
            free(keys.data);
            return 1;
        }
        ssize_t written = write(input_pipe[1], keys.data, keys.len);
        if (written != (ssize_t)keys.len) {
            perror("write pipe");
            close(input_pipe[0]);
            close(input_pipe[1]);
            free(keys.data);
            return 1;
        }
        close(input_pipe[1]);
        tty_fd = input_pipe[0];
    } else {
        tty_fd = open("/dev/tty", O_RDONLY);
        if (tty_fd == -1) {
            perror("open /dev/tty");
            free(keys.data);
            return 1;
        }
    }

    nav_reset();
    nav_set_mode(NAV_MODE_OSC8);
    struct offscr_opts opts = {
        .max_bytes          = 0,
        .capture_timeout_ms = 0,
        .max_lines          = 10,
    };
    struct offscr_ctx *ctx = offscr_new(&opts);
    if (ctx == NULL) {
        perror("offscr_new");
        return 1;
    }

    int rc = offscr_capture(ctx, STDIN_FILENO);
    if (rc < 0) {
        perror("offscr_capture");
        offscr_free(ctx);
        if (key_spec != NULL)
            close(tty_fd);
        free(keys.data);
        return 1;
    }

    struct offscr_view view = offscr_view(ctx);
    print_snapshot_(&view);

    nav_set_input_fd(tty_fd);

    struct nav_session_args args = {
        .tty_fd     = tty_fd,
        .notify_fd  = -1,
        .out_fd     = STDOUT_FILENO,
        .timeout_ms = 0,
        .exit_key   = 'q',
    };

    struct nav_session *sess = nav_session_begin(&args);
    if (sess == NULL) {
        perror("nav_session_begin");
        close(tty_fd);
        offscr_free(ctx);
        return 1;
    }

    if (key_spec == NULL) {
        printf("\n[nav_runner] press 'q' (or ESC) to exit navigation session\n");
        fflush(stdout);
    }

    for (;;) {
        nav_event_t ev = nav_session_run(sess);
        if (ev == NAV_EVENT_EXIT)
            break;
        if (ev == NAV_EVENT_ERROR) {
            fprintf(stderr, "nav_session_run: error\n");
            break;
        }
    }

    nav_session_end(sess);
    close(tty_fd);
    nav_set_input_fd(-1);
    offscr_free(ctx);
    free(keys.data);
    return 0;
}
