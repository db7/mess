// RUN: cat ../data/sample1.md | %b | %check
//
// CHECK: [osc8] [0] (3,75)-(3,90)
// CHECK-SAME: [osc8] [0] {{.*}} "OSC8"
// CHECK-SAME: [osc8] [0] {{.*}} https://gist.github.com/egmontkob/
//
// CHECK: [osc8] [1] (4,4)-(4,24)
// CHECK-SAME: [osc8] [1] {{.*}} "sequences"
// CHECK-SAME: [osc8] [1] {{.*}} https://gist.github.com/egmontkob/
//
// CHECK: [osc8] [2] (6,70)-(6,85) "lynx" -> https://lynx.browser.org/
//
// CHECK: [osc8] [3] (6,91)-(6,109) "lowdown"
// CHECK-SAME: [osc8] [3] {{.*}} https://kristaps.bsd.lv/lowdown/
//
// CHECK: [man] [4] (8,34)-(8,44)
// CHECK-SAME: [man] [4] {{.*}} "wordexp(3)" -> man://wordexp.3

#include "log.h"
#include "nav.h"
#include "readq.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct nav *global_nav;

static int configure_tty_raw(int fd, struct termios *saved);
static void restore_tty(int fd, const struct termios *saved);
static void handle_sigint(int sig);

int
main(void)
{
    struct offscr_opts opts = {
        .max_bytes             = 0,
        .first_byte_timeout_ms = 0,
        .next_byte_timeout_ms  = 0,
        .max_lines             = 30,
    };
    int status = EXIT_FAILURE;
    struct termios saved_termios;
    bool raw_configured = false;

    struct offscr_ctx *ctx = offscr_new(&opts);
    if (ctx == NULL) {
        perror("offscr_new");
        goto out;
    }

    // int fd = open("input.txt", O_RDONLY);
    int fd = STDIN_FILENO;

    if (offscr_capture(ctx, fd) < 0) {
        perror("offscr_capture");
        goto out;
    }
    if (fd != STDIN_FILENO)
        close(fd);

    struct offscr_view view = offscr_view(ctx);
    int tty                 = open("/dev/tty", O_RDONLY);
    if (isatty(tty)) {
        if (configure_tty_raw(tty, &saved_termios) == 0)
            raw_configured = true;
        else
            perror("configure_tty_raw");
    }

    log_init(STDERR_FILENO);

    struct nav_opts nav_opts = {
        .debug_fd = STDERR_FILENO,
    };
    global_nav = nav_create(&nav_opts);
    if (!global_nav) {
        fprintf(stderr, "nav_create failed\n");
        goto out;
    }

    struct readq input_queue;
    readq_init(&input_queue, tty);

    struct sigaction sa = {
        .sa_handler = handle_sigint,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    nav_result_t nav_rc =
        nav_run(global_nav, &view, &input_queue, STDOUT_FILENO, 0, true);
    const char *final_msg = NULL;
    switch (nav_rc) {
        case NAV_RESULT_STOP:
            final_msg = "nav: stop";
            status    = EXIT_SUCCESS;
            break;
        case NAV_RESULT_REFRESH:
            final_msg = "nav: refresh";
            status    = EXIT_SUCCESS;
            break;
        case NAV_RESULT_CANCELLED:
            final_msg = "nav: cancelled";
            status    = EXIT_SUCCESS;
            break;
        case NAV_RESULT_QUIT:
            final_msg = "nav: quit";
            status    = EXIT_SUCCESS;
            break;
        default:
            final_msg = "nav: error";
            status    = EXIT_FAILURE;
            break;
    }
    if (final_msg)
        log_debug("%s", final_msg);

out:
    if (raw_configured) {
        restore_tty(tty, &saved_termios);
        close(tty);
    }
    nav_destroy(global_nav);
    global_nav = NULL;
    if (ctx)
        offscr_free(ctx);
    return status;
}

static int
configure_tty_raw(int fd, struct termios *saved)
{
    struct termios raw;
    if (tcgetattr(fd, saved) < 0)
        return -1;
    raw = *saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw) < 0)
        return -1;
    return 0;
}

static void
restore_tty(int fd, const struct termios *saved)
{
    if (saved)
        (void)tcsetattr(fd, TCSANOW, saved);
}

static void
handle_sigint(int sig)
{
    (void)sig;
    nav_cancel(global_nav);
}
