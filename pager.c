#include "pager.h"

#include "defs.h"
#include "dispatcher.h"
#include "log.h"
#include "nav.h"
#include "offscr.h"
#include "readq.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wordexp.h>
#ifdef __linux__
#include <pty.h>
#else
#include <util.h>
#endif

#define MESS_ENTER_HOTKEY '\t'

static bool set_raw_mode_(int fd, struct termios *out_prev);
static void change_terminal_size_(int fd, int rows, int cols);
static void get_terminal_size_(int fd, int *rows, int *cols);
static bool handle_tty_(int child_fd, struct readq *tty_q);
static bool install_winch_(void);
static void handle_sigint_(int sig);
static void handle_winch_signal_(int sig);
static void exec_subpager_(void);
static bool enter_nav_(int child_fd, struct readq *tty_q);
int parent_run_(pid_t child_pid, int master_fd, int tty_fd, int parse_flags);

static int winch_fd_;
static volatile sig_atomic_t sigint_count_;
static pid_t pager_child_pid_ = -1;
static bool pager_quit_requested_;
static struct nav *pager_nav_;

int
pager_run(int parse_flags)
{
    if (!isatty(STDIN_FILENO) && !isatty(STDOUT_FILENO))
        exec_subpager_();

    int master_fd = -1;
    int slave_fd  = -1;
    int tty_fd    = -1;
    int rc        = -1;
    struct termios prev_term;
    bool raw_mode_set = false;

#ifdef __linux__
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("posix_openpt");
        return -1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        goto out;
    }
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == -1) {
        perror("openpty");
        goto out;
    }
#endif

    winch_fd_ = master_fd;

    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1 || !set_raw_mode_(tty_fd, &prev_term)) {
        perror("open /dev/tty");
        goto out;
    }
    raw_mode_set = true;

    sigint_count_ = 0;
    signal(SIGINT, handle_sigint_);
    install_winch_();

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        goto out;
    }

    if (pid == 0) {
#ifdef __linux__
        slave_fd = open(ptsname(master_fd), O_RDWR);
        if (slave_fd < 0)
            _exit(1);
#endif
        if (setsid() == -1)
            _exit(1);
        if (ioctl(slave_fd, TIOCSCTTY, 0) == -1)
            _exit(1);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);
        close(master_fd);
        exec_subpager_();
        unreachable();
    }

#ifndef __linux__
    close(slave_fd);
    slave_fd = -1;
#endif

    pager_child_pid_ = pid;
    rc               = parent_run_(pid, master_fd, tty_fd, parse_flags);
out:
    pager_child_pid_ = -1;
    if (tty_fd != -1 && raw_mode_set)
        tcsetattr(tty_fd, TCSAFLUSH, &prev_term);
    if (tty_fd != -1)
        close(tty_fd);
    if (master_fd != -1)
        close(master_fd);
#ifndef __linux__
    if (slave_fd != -1)
        close(slave_fd);
#endif
    return rc;
}

int
parent_run_(pid_t child_pid, int master_fd, int tty_fd, int parse_flags)
{
    struct nav_opts nav_opts = {
        .parse_flags = parse_flags,
        .self_cmd    = dispatcher_self_path(),
    };
    pager_nav_ = nav_create(&nav_opts);
    if (!pager_nav_) {
        perror("nav_create");
        return -1;
    }

    pager_quit_requested_ = false;

    struct readq child_q;
    struct readq tty_q;
    readq_init(&child_q, master_fd);
    readq_init(&tty_q, tty_fd);

    fd_set readfds;
    int max_fd = master_fd;
    if (tty_fd > max_fd)
        max_fd = tty_fd;

    int err    = -1;
    int status = 0;

    while (true) {
        int rv = waitpid(child_pid, &status, WNOHANG);
        if (rv == child_pid)
            break;

        FD_ZERO(&readfds);
        FD_SET(tty_fd, &readfds);
        FD_SET(master_fd, &readfds);

        int sel = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (sel == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            goto out;
        }

        if (FD_ISSET(tty_fd, &readfds)) {
            if (!handle_tty_(master_fd, &tty_q))
                goto out;
        }

        if (FD_ISSET(master_fd, &readfds)) {
            if (readq_refill(&child_q) &&
                write(STDOUT_FILENO, readq_data(&child_q),
                      readq_len(&child_q)) == -1) {
                perror("write stdout");
                goto out;
            }
            readq_clear(&child_q);
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        fprintf(stderr, "mess: pager exited with status %d\n",
                WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        fprintf(stderr, "mess: pager terminated by signal %d\n",
                WTERMSIG(status));
    else
        err = 0;

out:
    nav_destroy(pager_nav_);
    return err;
}

static bool
handle_tty_(int child_fd, struct readq *tty_q)
{
    if (pager_quit_requested_)
        return true;

    if (!readq_refill(tty_q))
        return true;

    bool trigger_nav = false;
    while (readq_len(tty_q) > 0) {
        int ch = readq_pick_byte(tty_q);
        if (ch < 0)
            break;
        if (ch == MESS_ENTER_HOTKEY) {
            trigger_nav = true;
            break;
        }
        char out = (char)readq_get_next(tty_q);
        if (write(child_fd, &out, 1) == -1) {
            perror("write to child");
            return false;
        }
    }

    if (trigger_nav) {
        if (!enter_nav_(child_fd, tty_q))
            return false;
    }

    readq_clear(tty_q);
    return true;
}

static int
drain_and_clear_fd_(int fd)
{
    struct offscr_ctx *ctx = offscr_new(&(struct offscr_opts){
        .drain                 = true,
        .first_byte_timeout_ms = 50,
        .next_byte_timeout_ms  = 20,
    });
    if (ctx == 0) {
        perror("offscr_new");
        return -1;
    }
    offscr_capture(ctx, fd);
    offscr_free(ctx);
    const char ctrl_l = '\f';
    if (write(fd, &ctrl_l, 1) == -1) {
        perror("write Ctrl-L");
        return -1;
    }
    return 0;
}

static bool
enter_nav_(int child_fd, struct readq *tty_q)
{
    if (!pager_nav_) {
        fprintf(stderr, "navigation unavailable\n");
        return false;
    }
    log_debug("nav: entering mode");
    if (drain_and_clear_fd_(child_fd) == -1)
        return -1;

    struct offscr_ctx *offscr_ctx_ = offscr_new(&(struct offscr_opts){
        .first_byte_timeout_ms = 1000,
        .next_byte_timeout_ms  = 200,
    });
    if (offscr_ctx_ == NULL) {
        perror("offscr_new");
        return false;
    }

    struct offscr_view view = {0};
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (offscr_capture(offscr_ctx_, child_fd) < 0) {
            perror("offscr_capture");
            offscr_free(offscr_ctx_);
            return false;
        }
        offscr_forget_first_row(offscr_ctx_);
        view = offscr_view(offscr_ctx_);
        if (view.len > 0)
            break;
        usleep(50000);
    }
    if (!view.data || view.len == 0) {
        offscr_free(offscr_ctx_);
        return false;
    }

    int tty_fd = readq_fd(tty_q);
    if (fcntl(tty_fd, F_GETFL) == -1) {
        log_debug("nav: invalid tty_fd (%d errno=%d)", tty_fd, errno);
        offscr_free(offscr_ctx_);
        return false;
    }
    int rows = 0;
    int cols = 0;
    get_terminal_size_(STDOUT_FILENO, &rows, &cols);
    size_t width = cols > 0 ? (size_t)cols : 0;

    log_debug("nav: running nav_run");
    nav_result_t rc =
        nav_run(pager_nav_, &view, tty_q, STDOUT_FILENO, width, true);
    switch (rc) {
        case NAV_RESULT_STOP:
            log_debug("nav: stop");
            if (readq_len(tty_q) > 0) {
                if (write(child_fd, readq_data(tty_q), readq_len(tty_q)) == -1)
                    perror("write pending input");
            }
            break;
        case NAV_RESULT_REFRESH:
            log_debug("nav: refresh");
            break;
        case NAV_RESULT_CANCELLED:
            log_debug("nav: canceled");
            break;
        case NAV_RESULT_QUIT: {
            log_debug("nav: quit");
            pager_quit_requested_ = true;
            const char quit       = 'q';
            if (write(child_fd, &quit, 1) == -1)
                perror("write quit");
            break;
        }
        default:
            log_debug("nav: error");
            break;
    }
    sigint_count_ = 0;
    offscr_free(offscr_ctx_);
    return rc != NAV_RESULT_ERROR;
}

static void
exec_subpager_(void)
{
    const char *cmd = getenv("MESSPAGER");
    if (cmd == NULL || cmd[0] == '\0') {
        const char *pager_env = getenv("PAGER");
        if (pager_env && pager_env[0] &&
            !dispatcher_command_is_self(pager_env)) {
            cmd = pager_env;
        }
    }
    if (cmd == NULL || cmd[0] == '\0')
        cmd = "less -R";

    wordexp_t we;
    if (wordexp(cmd, &we, WRDE_NOCMD) != 0) {
        fprintf(stderr, "mess: unable to parse pager command '%s'\n", cmd);
        _exit(127);
    }
    if (we.we_wordc == 0 || we.we_wordv == NULL || we.we_wordv[0] == NULL) {
        wordfree(&we);
        _exit(127);
    }

    execvp(we.we_wordv[0], we.we_wordv);
    perror(we.we_wordv[0]);
    wordfree(&we);
    _exit(127);
}

static bool
set_raw_mode_(int fd, struct termios *out_prev)
{
    struct termios term;
    if (tcgetattr(fd, &term) == -1)
        return false;
    if (out_prev)
        *out_prev = term;
    term.c_lflag &= ~(ICANON | ECHO | ECHOE);
    term.c_iflag &= ~(IXON | IXOFF | IXANY);
    term.c_oflag &= ~OPOST;
    term.c_cflag |= CS8;
    term.c_cc[VMIN]  = 1;
    term.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &term) != -1;
}

static void
change_terminal_size_(int fd, int rows, int cols)
{
    struct winsize ws;
    ws.ws_row    = rows;
    ws.ws_col    = cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    ioctl(fd, TIOCSWINSZ, &ws);
}

static void
get_terminal_size_(int fd, int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == -1) {
        *rows = 24;
        *cols = 80;
        return;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
}

static bool
install_winch_(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch_signal_;
    return sigaction(SIGWINCH, &sa, NULL) != -1;
}

static void
handle_sigint_(int sig)
{
    (void)sig;
    sigint_count_++;
    if (sigint_count_ >= 2)
        exit(1);
    nav_cancel(pager_nav_);
}

static void
handle_winch_signal_(int sig)
{
    (void)sig;
    int rows = 0;
    int cols = 0;
    get_terminal_size_(STDOUT_FILENO, &rows, &cols);
    if (winch_fd_ > 0)
        change_terminal_size_(winch_fd_, rows, cols);
    if (pager_child_pid_ > 0)
        (void)kill(pager_child_pid_, SIGWINCH);
}
