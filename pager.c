#include "nav.h"
#include "offscr.h"
#include "pager.h"
#include "readq.h"

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
static bool handle_tty_pass_mode_(int child_fd, struct readq *tty_q, int tty_fd);
static bool handle_child_output_(struct readq *child_q, bool *produced);
static bool handle_upstream_input_(int child_fd, int upstream_fd, bool *done);
static bool install_winch_(void);
static void handle_sigint_(int sig);
static void handle_winch_signal_(int sig);
static char *capture_stdin_to_temp_(void);
static void cleanup_capture_path_(char *path);
static bool append_arg_(char ***argvp, const char *arg);
static void configure_snapshot_timeouts_(void);
static bool pager_enter_nav_mode_(int child_fd, int tty_fd);
static void nav_notify_dirty_(void);
static void drain_fd_(int fd);
// Initial timeout (ms) to wait before capturing the first pager snapshot.
static double snapshot_timeout_initial_ms_ = 2000.0;
// Exponential decay factor applied to subsequent snapshot timeouts.
static double snapshot_timeout_decay_      = 0.5;
// Current timeout (ms) in effect for the next snapshot attempt.
static double snapshot_timeout_current_ms_;
// Tracks whether the previous iteration was still collecting snapshot data.
static bool snapshot_collecting_prev_;

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
static int nav_notify_pipe_[2] = {-1, -1};
static struct nav_session *active_nav_session_;
static volatile sig_atomic_t nav_exit_requested_;

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
    // stdin stays a TTY when mess runs without upstream piping (e.g. invoked
    // directly from the shell or as a pager with the caller leaving stdin on
    // the controlling terminal), so we still need to handle that path.
    bool stdin_is_tty  = isatty(STDIN_FILENO);
    char *capture_path = NULL;
    if (!stdin_is_tty)
        capture_path = capture_stdin_to_temp_();

    nav_reset();
    configure_snapshot_timeouts_();
    if (parse_flags & PAGER_PARSE_MAN)
        nav_set_mode(NAV_MODE_MAN);
    else
        nav_set_mode(NAV_MODE_OSC8);
    nav_set_document_path(NULL);

    char **pager_cmd = NULL;
    if (build_pager_command_(&pager_cmd) != 0)
        return -1;

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
    cleanup_capture_path_(capture_path);
    return rc;
}
// Prime snapshot timeout values from environment overrides.
static void
configure_snapshot_timeouts_(void)
{
    const char *env = getenv("MESS_SNAPSHOT_TIMEOUT_MS");
    if (env && *env) {
        char *end = NULL;
        double val = strtod(env, &end);
        if (end != env && val > 0.0)
            snapshot_timeout_initial_ms_ = val;
    }

    env = getenv("MESS_SNAPSHOT_TIMEOUT_DECAY");
    if (env && *env) {
        char *end = NULL;
        double val = strtod(env, &end);
        if (end != env && val > 0.0) {
            if (val >= 1.0)
                val = 0.99;
            snapshot_timeout_decay_ = val;
        }
    }

    snapshot_timeout_current_ms_ = 0.0;
    snapshot_collecting_prev_    = false;
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
        if (nav_notify_pipe_[0] != -1)
            close(nav_notify_pipe_[0]);
        if (nav_notify_pipe_[1] != -1)
            close(nav_notify_pipe_[1]);
        nav_notify_pipe_[0] = nav_notify_pipe_[1] = -1;
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
        if (nav_notify_pipe_[0] != -1)
            close(nav_notify_pipe_[0]);
        if (nav_notify_pipe_[1] != -1)
            close(nav_notify_pipe_[1]);
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

    nav_set_status_fd(tty_fd);
    nav_set_input_fd(tty_fd);
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
    if (pipe(nav_notify_pipe_) == -1) {
        perror("pipe");
        offscr_free(offscr_ctx_);
        offscr_ctx_ = NULL;
        restore_terminal_(tty_fd, &saved_term_);
        close(tty_fd);
        close(master_fd);
#ifndef __linux__
        close(slave_fd);
#endif
        free_pager_command_(pager_cmd);
        return -1;
    }
    fcntl(nav_notify_pipe_[0], F_SETFL,
          fcntl(nav_notify_pipe_[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(nav_notify_pipe_[1], F_SETFL,
          fcntl(nav_notify_pipe_[1], F_GETFL, 0) | O_NONBLOCK);

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

        bool collecting = nav_snapshot_collecting();
        if (collecting && !snapshot_collecting_prev_) {
            snapshot_timeout_current_ms_ = snapshot_timeout_initial_ms_;
            snapshot_collecting_prev_    = true;
        } else if (!collecting) {
            snapshot_timeout_current_ms_ = 0.0;
            snapshot_collecting_prev_    = false;
        }

        struct timeval timeout;
        struct timeval *timeout_ptr = NULL;
        if (collecting && snapshot_timeout_current_ms_ > 0.0) {
            double ms = snapshot_timeout_current_ms_;
            if (ms < 1.0)
                ms = 1.0;
            timeout.tv_sec  = (time_t)(ms / 1000.0);
            double rem_ms   = ms - ((double)timeout.tv_sec * 1000.0);
            long usec       = (long)(rem_ms * 1000.0);
            if (timeout.tv_sec == 0 && usec <= 0)
                usec = 1;
            timeout.tv_usec = (suseconds_t)usec;
            timeout_ptr     = &timeout;
        }

        FD_ZERO(&readfds);
        FD_SET(tty_fd, &readfds);
        FD_SET(master_fd, &readfds);
        if (upstream_fd != -1 && !upstream_done)
            FD_SET(upstream_fd, &readfds);
        int sel = select(max_fd + 1, &readfds, NULL, NULL, timeout_ptr);
        if (sel == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            err = 1;
            break;
        }
        if (sel == 0) {
            if (nav_snapshot_collecting()) {
                if (nav_snapshot_finish() && nav_snapshot_ready())
                    nav_emit_snapshot();
            }
            snapshot_timeout_current_ms_ = 0.0;
            snapshot_collecting_prev_    = false;
            continue;
        }

        if (FD_ISSET(tty_fd, &readfds)) {
            if (!handle_tty_pass_mode_(master_fd, &tty_q, tty_fd)) {
                err = 1;
                break;
            }
        }

        bool child_produced = false;
        if (FD_ISSET(master_fd, &readfds)) {
            if (!handle_child_output_(&child_q, &child_produced)) {
                err = 1;
                break;
            }
        }

        if (child_produced && nav_snapshot_collecting()) {
            snapshot_timeout_current_ms_ *= snapshot_timeout_decay_;
            if (snapshot_timeout_current_ms_ < 1.0)
                snapshot_timeout_current_ms_ = 1.0;
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

    nav_clear_status();
    nav_set_input_fd(-1);
    nav_set_status_fd(-1);
    if (term_in_raw_)
        restore_terminal_(tty_fd, &saved_term_);
    close(tty_fd);
    close(master_fd);
    if (active_nav_session_ != NULL) {
        nav_session_end(active_nav_session_);
        active_nav_session_ = NULL;
    }
    if (nav_notify_pipe_[0] != -1)
        close(nav_notify_pipe_[0]);
    if (nav_notify_pipe_[1] != -1)
        close(nav_notify_pipe_[1]);
    nav_notify_pipe_[0] = nav_notify_pipe_[1] = -1;
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
    nav_exit_requested_ = 1;
    nav_notify_dirty_();
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
    nav_exit_requested_ = 1;
    nav_notify_dirty_();
}

// Process keyboard input from the controlling terminal.
static bool
handle_tty_pass_mode_(int child_fd, struct readq *tty_queue, int tty_fd)
{
    if (!readq_refill(tty_queue))
        return true;

    while (tty_queue->start < tty_queue->end) {
        char ch = tty_queue->buffer[tty_queue->start++];
        if (ch == MESS_ENTER_HOTKEY) {
            tty_queue->start = tty_queue->end = 0;
            if (!pager_enter_nav_mode_(child_fd, tty_fd))
                return false;
            return true;
        }
        if (write(child_fd, &ch, 1) == -1) {
            perror("write to child");
            return false;
        }
    }

    tty_queue->start = tty_queue->end = 0;
    return true;
}

// Consume bytes produced by the child process and push them through nav.
static bool
handle_child_output_(struct readq *bq, bool *produced)
{
    if (produced)
        *produced = false;
    if (!readq_refill(bq))
        return true;

    ssize_t nread = nav_process_output(bq);
    if (nread > 0) {
        if (produced)
            *produced = true;
        bool freeze_output = nav_mode_active() && nav_freeze_enabled();
        if (freeze_output) {
            // Buffer the redraw frame while navigation is frozen; a later
            // timeout will flush the snapshot to the terminal.
            if (nav_snapshot_collecting())
                nav_append_snapshot(bq->buffer, (size_t)nread);
            if (!nav_snapshot_collecting() && nav_snapshot_ready())
                nav_emit_snapshot();
            nav_set_output_dirty(nav_snapshot_collecting());
        } else {
            char render_buf[READQ_SIZE * 3];
            size_t render_len = nav_render_highlighted(
                bq->buffer, (size_t)nread, render_buf, sizeof(render_buf));
            const char *data = render_len ? render_buf : bq->buffer;
            size_t len       = render_len ? render_len : (size_t)nread;
            nav_set_output_dirty(false);
            if (write(STDOUT_FILENO, data, len) == -1) {
                perror("write stdout");
                return false;
            }
        }
        nav_render_status();
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

static void
drain_fd_(int fd)
{
    if (fd < 0)
        return;
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
}

static void
nav_notify_dirty_(void)
{
    if (active_nav_session_ == NULL)
        return;
    if (nav_notify_pipe_[1] < 0)
        return;
    char c = 'x';
    ssize_t rc = write(nav_notify_pipe_[1], &c, 1);
    (void)rc;
}

static bool
pager_enter_nav_mode_(int child_fd, int tty_fd)
{
    if (offscr_ctx_ == NULL)
        return false;

    const char ctrl_l = '\f';
    if (write(child_fd, &ctrl_l, 1) == -1) {
        perror("write Ctrl-L");
        return false;
    }

    int cap_rc = offscr_capture(offscr_ctx_, child_fd);
    if (cap_rc < 0) {
        perror("offscr_capture");
        return false;
    }

    struct nav_session_args args = {
        .tty_fd = tty_fd,
        .notify_fd = nav_notify_pipe_[0],
        .out_fd = STDOUT_FILENO,
        .timeout_ms = 250,
        .exit_key = '\x1b',
    };

    drain_fd_(nav_notify_pipe_[0]);
    active_nav_session_ = nav_session_begin(&args);
    if (active_nav_session_ == NULL)
        return false;

    bool running = true;
    nav_exit_requested_ = 0;
    while (running) {
        nav_event_t ev = nav_session_run(active_nav_session_);
        switch (ev) {
        case NAV_EVENT_EXIT:
            running = false;
            break;
        case NAV_EVENT_DIRTY:
            if (offscr_capture(offscr_ctx_, child_fd) < 0)
                running = false;
            break;
        case NAV_EVENT_TIMEOUT:
            break;
        case NAV_EVENT_ERROR:
        default:
            running = false;
            break;
        }
        if (nav_exit_requested_) {
            nav_exit_requested_ = 0;
            running = false;
        }
    }

    nav_session_end(active_nav_session_);
    active_nav_session_ = NULL;
    drain_fd_(nav_notify_pipe_[0]);
    nav_exit_requested_ = 0;
    sigint_count_ = 0;
    return true;
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
