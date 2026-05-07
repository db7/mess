#include "links.h"
#include "nav.h"
#include "log.h"
#include "offscr.h"
#include "pager.h"
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

#ifndef READQ_SIZE
#define READQ_SIZE 4096
#endif

static bool set_raw_mode_(int fd, struct termios *out_prev);
static void restore_terminal_(int fd, const struct termios *term);
static void change_terminal_size_(int fd, int rows, int cols);
static void get_terminal_size_(int fd, int *rows, int *cols);
static bool handle_tty_pass_mode_(int child_fd, struct readq *tty_q,
                                  int tty_fd);
static bool handle_child_output_(struct readq *child_q);
static bool handle_upstream_input_(int child_fd, int upstream_fd, bool *done);
static bool install_winch_(void);
static void handle_sigint_(int sig);
static void handle_winch_signal_(int sig);
static char *capture_stdin_to_temp_(void);
static void cleanup_capture_path_(char *path);
static bool append_arg_(char ***argvp, const char *arg);
static bool pager_enter_nav_mode_(int child_fd, int tty_fd);

// File descriptor used to propagate SIGWINCH notifications via a pipe.
static int winch_fd_;
// Saved terminal attributes so raw mode can be undone on exit.
static struct termios saved_term_;
// Flag indicating whether mess currently owns the terminal in raw mode.
static bool term_in_raw_;
// Counts SIGINT deliveries so we can escalate after repeated interrupts.
static volatile sig_atomic_t sigint_count_;
// PID of the forked pager child process, or -1 when inactive.
static pid_t pager_child_pid_ = -1;
static struct offscr_ctx *offscr_ctx_;
static struct nav *pager_nav_;

#define MESS_ENTER_HOTKEY '\t'

// Parse the pager command from environment and split it into argv.
static int
build_pager_command_(char ***argv_out)
{
    const char *cmd = getenv("MESSPAGER");
    if (!cmd || !cmd[0])
        cmd = getenv("PAGER");
    if (!cmd || !cmd[0])
        cmd = "less -R";

    wordexp_t we;
    if (wordexp(cmd, &we, WRDE_NOCMD) != 0) {
        fprintf(stderr, "mess: unable to parse pager command '%s'\n", cmd);
        return -1;
    }
    size_t count = we.we_wordc;
    char **argv  = calloc(count + 1, sizeof(char *));
    if (!argv) {
        wordfree(&we);
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        argv[i] = strdup(we.we_wordv[i]);
        if (!argv[i]) {
            for (size_t j = 0; j < i; ++j)
                free(argv[j]);
            free(argv);
            wordfree(&we);
            return -1;
        }
    }
    argv[count] = NULL;

    wordfree(&we);
    *argv_out = argv;
    return 0;
}

// Release the argv array built by build_pager_command_().
static void
free_pager_command_(char **argv)
{
    if (!argv)
        return;
    for (size_t i = 0; argv[i]; ++i)
        free(argv[i]);
    free(argv);
}

static int pager_run_interactive_(char **pager_cmd, bool forward_stdin);
static int pager_run_piped_(char **pager_cmd);

int
pager_run(int parse_flags)
{
    (void)parse_flags;
    // stdin stays a TTY when mess runs without upstream piping (e.g. invoked
    // directly from the shell or as a pager with the caller leaving stdin on
    // the controlling terminal), so we still need to handle that path.
    bool stdin_is_tty  = isatty(STDIN_FILENO);
    char *capture_path = NULL;
    if (!stdin_is_tty)
        capture_path = capture_stdin_to_temp_();

    char **pager_cmd = NULL;
    if (build_pager_command_(&pager_cmd) != 0)
        return -1;

    struct nav_opts nav_opts = {
        .debug_fd = STDERR_FILENO,
    };
    pager_nav_ = nav_create(&nav_opts);
    if (!pager_nav_) {
        free_pager_command_(pager_cmd);
        cleanup_capture_path_(capture_path);
        return -1;
    }

    if (capture_path) {
        if (!append_arg_(&pager_cmd, capture_path)) {
            cleanup_capture_path_(capture_path);
            free_pager_command_(pager_cmd);
            return -1;
        }
    }

    bool interactive = stdin_is_tty || (capture_path && isatty(STDOUT_FILENO));
    bool forward_stdin = (!stdin_is_tty && capture_path == NULL);
    int rc = interactive ? pager_run_interactive_(pager_cmd, forward_stdin) :
                           pager_run_piped_(pager_cmd);
    free_pager_command_(pager_cmd);
    nav_destroy(pager_nav_);
    pager_nav_ = NULL;
    cleanup_capture_path_(capture_path);
    return rc;
}
// Drive the pager with a pseudo-tty when mess runs interactively.
static int
pager_run_interactive_(char **pager_cmd, bool forward_stdin)
{
    int master_fd;
    int slave_fd = -1;
#ifdef __linux__
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("posix_openpt");
        free_pager_command_(pager_cmd);
        return -1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        free_pager_command_(pager_cmd);
        return -1;
    }
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == -1) {
        perror("openpty");
        free_pager_command_(pager_cmd);
        return -1;
    }
#endif

    winch_fd_ = master_fd;

    struct termios prev_term;
    int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1 || !set_raw_mode_(tty_fd, &prev_term)) {
        perror("open /dev/tty");
        close(master_fd);
#ifndef __linux__
        close(slave_fd);
#endif
        free_pager_command_(pager_cmd);
        return -1;
    }
    saved_term_   = prev_term;
    term_in_raw_  = true;
    sigint_count_ = 0;
    signal(SIGINT, handle_sigint_);
    install_winch_();

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        restore_terminal_(tty_fd, &saved_term_);
        close(tty_fd);
        close(master_fd);
#ifndef __linux__
        close(slave_fd);
#endif
        offscr_free(offscr_ctx_);
        offscr_ctx_ = NULL;
        free_pager_command_(pager_cmd);
        return -1;
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
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);
        close(master_fd);
        execvp(pager_cmd[0], pager_cmd);
        perror("exec pager");
        _exit(127);
    }

#ifndef __linux__
    close(slave_fd);
#endif

    offscr_ctx_ = offscr_new(NULL);
    if (offscr_ctx_ == NULL) {
        perror("offscr_new");
        restore_terminal_(tty_fd, &saved_term_);
        close(tty_fd);
        close(master_fd);
#ifndef __linux__
        close(slave_fd);
#endif
        free_pager_command_(pager_cmd);
        return -1;
    }
    struct readq child_q;
    struct readq tty_q;
    readq_init(&child_q, master_fd);
    readq_init(&tty_q, tty_fd);
    int upstream_fd    = forward_stdin ? STDIN_FILENO : -1;
    bool upstream_done = false;

    fd_set readfds;
    int max_fd = master_fd;
    if (tty_fd > max_fd)
        max_fd = tty_fd;
    if (upstream_fd > max_fd)
        max_fd = upstream_fd;
    int err    = 0;
    int status = 0;

    while (true) {
        int rv = waitpid(pid, &status, WNOHANG);
        if (rv == pid)
            break;

        FD_ZERO(&readfds);
        FD_SET(tty_fd, &readfds);
        FD_SET(master_fd, &readfds);
        if (upstream_fd != -1 && !upstream_done)
            FD_SET(upstream_fd, &readfds);
        int sel = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (sel == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            err = 1;
            break;
        }

        if (FD_ISSET(tty_fd, &readfds)) {
            if (!handle_tty_pass_mode_(master_fd, &tty_q, tty_fd)) {
                err = 1;
                break;
            }
        }

        if (FD_ISSET(master_fd, &readfds)) {
            if (!handle_child_output_(&child_q)) {
                err = 1;
                break;
            }
        }

        if (upstream_fd != -1 && !upstream_done &&
            FD_ISSET(upstream_fd, &readfds)) {
            if (!handle_upstream_input_(master_fd, upstream_fd,
                                        &upstream_done)) {
                err = 1;
                break;
            }
            if (upstream_done)
                upstream_fd = -1;
        }
    }

    if (!err && WIFEXITED(status) && WEXITSTATUS(status) != 0)
        err = 1;
    if (WIFSIGNALED(status))
        err = 1;

    if (term_in_raw_)
        restore_terminal_(tty_fd, &saved_term_);
    close(tty_fd);
    close(master_fd);
    offscr_free(offscr_ctx_);
    offscr_ctx_ = NULL;
    return err ? -1 : 0;
}

// Execute the pager when mess is fed through a pipeline.
static int
pager_run_piped_(char **pager_cmd)
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execvp(pager_cmd[0], pager_cmd);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

// Switch the tty to raw mode while remembering the previous settings.
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

// Restore canonical terminal settings once raw mode is no longer needed.
static void
restore_terminal_(int fd, const struct termios *term)
{
    if (term)
        tcsetattr(fd, TCSAFLUSH, term);
    term_in_raw_ = false;
}

// Force the child pseudo-tty to adopt the provided dimensions.
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

// Query the current terminal size, falling back to 80x24 when unknown.
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

// Install a SIGWINCH handler that resizes the child PTY.
static bool
install_winch_(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch_signal_;
    return sigaction(SIGWINCH, &sa, NULL) != -1;
}

// Count SIGINT deliveries so a second interrupt terminates immediately.
static void
handle_sigint_(int sig)
{
    (void)sig;
    sigint_count_++;
    if (sigint_count_ >= 2)
        exit(1);
    nav_cancel(pager_nav_);
}

// React to SIGWINCH by relaying the new terminal size downstream.
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

// Process keyboard input from the controlling terminal.
static bool
handle_tty_pass_mode_(int child_fd, struct readq *tty_queue, int tty_fd)
{
    printf("hi\n");
    if (!readq_refill(tty_queue))
        return true;

    while (readq_available_bytes(tty_queue) > 0) {
        int ch = readq_get_next(tty_queue);
        if (ch < 0)
            break;
        if (ch == MESS_ENTER_HOTKEY) {
            if (!pager_enter_nav_mode_(child_fd, tty_fd))
                return false;
            return true;
        }
        char out = (char)ch;
        if (write(child_fd, &out, 1) == -1) {
            perror("write to child");
            return false;
        }
    }

    tty_queue->start = tty_queue->end = 0;
    return true;
}

// Consume bytes produced by the child process and forward to stdout.
static bool
handle_child_output_(struct readq *bq)
{
    if (!readq_refill(bq))
        return true;

    size_t len = readq_available_bytes(bq);
    if (len > 0) {
        const char *data = bq->buffer + bq->start;
        if (write(STDOUT_FILENO, data, len) == -1) {
            perror("write stdout");
            return false;
        }
    }
    bq->start = bq->end = 0;
    return true;
}

// Forward data from the upstream fd into the child PTY, tracking EOF.
static bool
handle_upstream_input_(int child_fd, int upstream_fd, bool *done)
{
    char buffer[READQ_SIZE];
    ssize_t n = read(upstream_fd, buffer, sizeof(buffer));
    if (n > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w =
                write(child_fd, buffer + written, (size_t)(n - written));
            if (w == -1) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror("write upstream");
                return false;
            }
            written += w;
        }
        return true;
    }
    if (n == 0) {
        const char eot = 4;
        (void)write(child_fd, &eot, 1);
        if (done)
            *done = true;
        return true;
    }
    if (errno == EINTR || errno == EAGAIN)
        return true;
    perror("read upstream");
    return false;
}

static bool
pager_enter_nav_mode_(int child_fd, int tty_fd)
{
    if (!pager_nav_) {
        fprintf(stderr, "navigation unavailable\n");
        return false;
    }
    if (offscr_ctx_ == NULL)
        return false;

    log_debugln("nav: entering mode");
    const char ctrl_l = '\f';
    if (write(child_fd, &ctrl_l, 1) == -1) {
        perror("write Ctrl-L");
        return false;
    }

    if (offscr_capture(offscr_ctx_, child_fd) < 0) {
        perror("offscr_capture");
        return false;
    }

    log_debugln("nav: captured view");
    struct offscr_view view = offscr_view(offscr_ctx_);
    struct readq input;
    readq_init(&input, tty_fd);

    log_debugln("nav: running nav_run");
    nav_result_t rc = nav_run(pager_nav_, &view, &input, tty_fd);
    switch (rc) {
    case NAV_RESULT_STOP:
        log_debugln("nav: stop");
        break;
    case NAV_RESULT_REFRESH:
        log_debugln("nav: refresh");
        break;
    case NAV_RESULT_CANCELLED:
        log_debugln("nav: cancelled");
        break;
    default:
        log_debugln("nav: error");
        break;
    }
    sigint_count_   = 0;
    return rc != NAV_RESULT_ERROR;
}

// Persist stdin to a temporary file so the pager can re-open it later.
static char *
capture_stdin_to_temp_(void)
{
    static const char tpl[] = "/tmp/mess-input-XXXXXX";
    char path[sizeof(tpl)];
    memcpy(path, tpl, sizeof(tpl));

    int fd = mkstemp(path);
    if (fd == -1)
        return NULL;

    char buffer[8192];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd, buffer + written, (size_t)(n - written));
            if (w == -1) {
                if (errno == EINTR)
                    continue;
                goto fail;
            }
            written += w;
        }
    }
    if (n == -1)
        goto fail;
    if (lseek(fd, 0, SEEK_SET) == -1)
        goto fail;
    close(fd);
    return strdup(path);
fail:
    close(fd);
    unlink(path);
    return NULL;
}

// Remove and free a temporary capture path created by capture_stdin_to_temp_().
static void
cleanup_capture_path_(char *path)
{
    if (!path)
        return;
    unlink(path);
    free(path);
}

// Append a string to a NULL-terminated argv array.
static bool
append_arg_(char ***argvp, const char *arg)
{
    if (!argvp || !*argvp || !arg)
        return false;
    size_t count = 0;
    while ((*argvp)[count])
        count++;
    char **newv = realloc(*argvp, (count + 2) * sizeof(char *));
    if (!newv)
        return false;
    newv[count] = strdup(arg);
    if (!newv[count])
        return false;
    newv[count + 1] = NULL;
    *argvp          = newv;
    return true;
}
