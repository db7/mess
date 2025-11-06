#include "nav.h"
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
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wordexp.h>
#ifdef __linux__
#include <pty.h>
#else
#include <util.h> // for openpty(), forkpty()
#endif

#define BUFFER_SIZE           4096
#define STATUS_HEIGHT_RESERVE 0

static bool set_raw_mode_(int fd, struct termios *out_prev);
static void change_terminal_size_(int fd, int rows, int cols);
static void get_terminal_size_(int fd, int *rows, int *cols);
static void run_child_(int fd, int argc, char *argv[]);
static int run_parent_(int child_fd, pid_t pid);
static void restore_terminal_fd_(int fd);
static void restore_terminal_atexit_(void);
static void handle_winch_(int sig);
static void apply_winch_(pid_t child_pid);
static bool handle_tty_input_(int child_fd, struct readq *tty_queue,
                              pid_t child_pid);
static bool handle_child_output_(struct readq *bq);
static void debug_output_bytes_(const char *tag, const void *buf, size_t len);

static int winch_fd_;
static struct termios prev_term_;
static bool term_in_raw_mode_;
static bool restore_registered_;
static pager_termios_hook_fn termios_hook_;
static pid_t child_pid_                    = -1;
static int restore_fd_at_exit_             = -1;
static volatile int keepRunning_           = 1;
static volatile sig_atomic_t sigint_count_ = 0;
static int debug_output_fd_                = -2;

void
pager_set_termios_hook(pager_termios_hook_fn hook)
{
    termios_hook_ = hook;
}

// Configure the controlling terminal in raw mode for the pager session.
static bool
set_raw_mode_(int fd, struct termios *out_prev)
{
    struct termios term;
    memset(&term, 0, sizeof(term));

    // Get the current terminal settings
    if (tcgetattr(fd, &term) == -1) {
        perror("tcgetattr");
        // Bubble the failure to the caller so it can unwind cleanly (closing
        // fds, restoring the terminal, reporting an error) instead of exiting
        // from deep inside the helper.
        return false;
    }
    if (out_prev)
        *out_prev = term;
    if (termios_hook_ && out_prev) {
        // Allow embedders to observe or adjust the canonical settings before
        // raw mode toggles them. This is used by tests to restore the tty.
        termios_hook_(out_prev, 'S');
    }

    // Save the original settings (for restoring later)
    // struct termios orig_term = term;

    // Set the terminal to raw mode
    // term.c_lflag &=
    //     ~(ICANON | ECHO | ECHOE | ISIG); // Disable canonical mode, echo,
    //     etc.

    term.c_lflag &=
        ~(ICANON | ECHO | ECHOE); // Disable canonical mode, echo, etc.
    term.c_iflag &= ~(IXON | IXOFF | IXANY); // Disable flow control
    term.c_oflag &= ~OPOST;                  // Disable output processing
    term.c_cflag |= CS8;                     // 8-bit characters
    term.c_cc[VMIN]  = 1; // Minimum number of characters to read
    term.c_cc[VTIME] = 0; // No timeout for input

    // Apply the new terminal settings
    if (tcsetattr(fd, TCSANOW, &term) == -1) {
        perror("tcsetattr");
        return false;
    }
    return true;
}

// Resize the PTY to the requested row/column dimensions.
static void
change_terminal_size_(int fd, int rows, int cols)
{
    struct winsize ws;
    ws.ws_row    = rows; // Set the number of rows (height)
    ws.ws_col    = cols; // Set the number of columns (width)
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    // Use ioctl() to set the window size for the slave terminal
    if (ioctl(fd, TIOCSWINSZ, &ws) == -1) {
        perror("ioctl TIOCSWINSZ");
        // Without a valid window size the child PTY is unusable, so abort so
        // the pager doesn’t continue in a broken terminal state.
        exit(EXIT_FAILURE);
    }
}

// Query the current window size for a PTY file descriptor.
static void
get_terminal_size_(int fd, int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        // Treat inability to read the terminal geometry as fatal; downstream
        // callers rely on accurate dimensions to size the PTY correctly.
        exit(EXIT_FAILURE);
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
}


// Track SIGINT invocations to trigger graceful shutdown.
static void
handle_sigint_(int dummy)
{
    (void)dummy;
    sigint_count_++;
    if (keepRunning_ == 1) {
        keepRunning_ = 0;
    }
}

int
pager_run(int argc, char *argv[])
{
    int master_fd;
    int slave_fd = -1;
    pid_t pid;

    keepRunning_ = 1;

    // Open a new pseudo-terminal pair (master and slave). The parent watches
    // the master fd to capture the child's output and forward keystrokes, while
    // the child treats the slave fd as its controlling terminal. The kernel
    // pipes data both ways: child stdout/stderr writes travel child → slave →
    // kernel → master → parent, and keystrokes the parent forwards head back
    // parent → master → kernel → slave → child (arriving on the child's stdin).
#ifdef __linux__
    // For Linux interface, the slave will be created in the child process.
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        perror("erro creating PTY");
        // Without a PTY pair the pager cannot run, so abort immediately.
        exit(EXIT_FAILURE);
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("ioctl TIOCGWINSZ");
        // The pager requires a usable controlling terminal; bail out if we
        // cannot read its size before spawning the child.
        exit(EXIT_FAILURE);
    }
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == -1) {
        perror("openpty");
        // No PTY means no child pager; terminate with an error.
        exit(EXIT_FAILURE);
    }
#endif

    // Save master_fd to properly handle SIGWINCH later.
    winch_fd_ = master_fd;

    // Install a SIGINT handler before forking. The handler is inherited by the
    // child, but the child execs immediately, so its handler gets replaced by
    // the pager's defaults. Keeping the handler in the parent lets Ctrl-C first
    // trigger a graceful shutdown and only escalate if pressed again. There is
    // a narrow window between fork and exec where the child would still run
    // this handler if SIGINT arrived, but the worst case is a redundant
    // graceful shutdown—acceptable for an abort signal. If we ever needed to
    // avoid that, we could move the installation to the parent branch after
    // fork, but that would create a window where the parent remains on the
    // default handler and would exit immediately on Ctrl-C. Installing early is
    // the least-bad trade.
    (void)signal(SIGINT, handle_sigint_);

    pid = fork();
    if (pid == -1) {
        perror("fork");
        // Without a child process we cannot proxy the pager; treat as fatal.
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        // Child process closes the master_fd as it's not needed proceeds with
        // the slave_fd only. In the particular case of Linux, the slave_fd
        // still have to be created.
#ifdef __linux__
        slave_fd = open(ptsname(master_fd), O_RDWR);
#endif
        if (slave_fd < 0) {
            perror("failed to open slave PTY");
            // If we cannot access the slave PTY the child cannot communicate
            // with the parent, so terminate the child immediately.
            exit(EXIT_FAILURE);
        }
        close(master_fd);
        run_child_(slave_fd, argc, argv);
        perror("run_child"); // Only reached if execvp fails
        exit(EXIT_FAILURE);
    }

#ifndef __linux__
    close(slave_fd);
#endif

    return run_parent_(master_fd, pid);
}

// Exec the underlying pager in the child side of the PTY.
static void
run_child_(int fd, int argc, char *argv[])
{
    // Create a new session to become the controlling process
    if (setsid() == -1) {
        perror("setsid");
        return;
    }
    if (ioctl(fd, TIOCSCTTY, 0) < 0) {
        perror("Failed to set controlling terminal");
        return;
    }
    keepRunning_ = 123;

    // In the child process, execute the 'less' command with arguments from argv
    if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
        perror("dup2");
        return;
    }

    // if (isatty(STDIN_FILENO)) {
    //   if (dup2(fd, STDIN_FILENO) == -1) {
    //     perror("dup2 stdin");
    //     return;
    //   }
    // }

    const char *pager_env = getenv("PAGER");
    wordexp_t we;
    bool have_wordexp  = false;
    char **pager_words = NULL;
    size_t pager_count = 0;

    if (pager_env && pager_env[0]) {
        if (wordexp(pager_env, &we, WRDE_NOCMD) == 0 && we.we_wordc > 0) {
            pager_words  = we.we_wordv;
            pager_count  = we.we_wordc;
            have_wordexp = true;
        }
    }

    char *fallback[] = {"less", NULL};
    if (!pager_words) {
        pager_words = fallback;
        pager_count = 1;
    }

    bool add_raw = false;
    if (pager_count > 0 && strcmp(pager_words[0], "less") == 0) {
        add_raw = true;
        for (size_t i = 1; i < pager_count; ++i) {
            if (strcmp(pager_words[i], "-r") == 0 ||
                strcmp(pager_words[i], "-R") == 0) {
                add_raw = false;
                break;
            }
        }
    }

    size_t total_args = pager_count + (add_raw ? 1 : 0) + (size_t)(argc - 1);
    char **cmd        = calloc(total_args + 1, sizeof(char *));
    if (!cmd) {
        perror("calloc pager command");
        if (have_wordexp)
            wordfree(&we);
        _exit(1);
    }

    size_t pos = 0;
    for (size_t i = 0; i < pager_count; ++i) {
        cmd[pos++] = pager_words[i];
    }
    if (add_raw) {
        cmd[pos++] = "-r";
    }
    for (int j = 1; j < argc; ++j) {
        cmd[pos++] = argv[j];
    }
    cmd[pos] = NULL;

    execvp(cmd[0], cmd);
    perror("execvp");
    if (have_wordexp)
        wordfree(&we);
    free(cmd);
}

// Restore canonical terminal settings when the pager exits.
static void
restore_terminal_fd_(int fd)
{
    if (!term_in_raw_mode_ || fd < 0)
        return;
    if (tcsetattr(fd, TCSAFLUSH, &prev_term_) == 0) {
        term_in_raw_mode_ = false;
        if (termios_hook_) {
            struct termios current;
            memset(&current, 0, sizeof(current));
            if (tcgetattr(fd, &current) == 0)
                termios_hook_(&current, 'R');
            else
                termios_hook_(&prev_term_, 'R');
        }
        if (fd == restore_fd_at_exit_)
            restore_fd_at_exit_ = -1;
    }
}

static void
restore_terminal_atexit_(void)
{
    restore_terminal_fd_(restore_fd_at_exit_);
}

// Receive SIGWINCH and propagate the resize to child and status.
static void
handle_winch_(int sig)
{
    (void)sig;
    apply_winch_(child_pid_);
}

// Apply the latest terminal dimensions and notify the child.
static void
apply_winch_(pid_t child_pid)
{
    if (winch_fd_ <= 0)
        return;

    int rows = 0;
    int cols = 0;
    get_terminal_size_(STDOUT_FILENO, &rows, &cols);
    int child_rows = rows;
    if (STATUS_HEIGHT_RESERVE > 0 && rows > STATUS_HEIGHT_RESERVE)
        child_rows = rows - STATUS_HEIGHT_RESERVE;
    change_terminal_size_(winch_fd_, child_rows, cols);
    if (child_pid > 0)
        (void)kill(child_pid, SIGWINCH);
}

static bool
handle_tty_input_(int child_fd, struct readq *tty_queue, pid_t child_pid)
{
    if (!readq_refill(tty_queue))
        return true;

    size_t available = readq_available_bytes(tty_queue);
    if (available == 0)
        return true;

    char *buf           = tty_queue->buffer + tty_queue->start;
    ssize_t forward_len = (ssize_t)available;
    bool forward =
        nav_process_input(buf, &forward_len, sizeof(tty_queue->buffer));

    if (nav_consume_refresh_request()) {
        if (child_pid > 0) {
            if (kill(child_pid, SIGWINCH) == -1 && errno != ESRCH) {
                perror("kill SIGWINCH");
                return false;
            }
        }
        const char refresh = '\f';
        if (write(child_fd, &refresh, 1) == -1) {
            perror("write refresh to child_fd");
            return false;
        }
    }

    if (forward && forward_len > 0) {
        if (write(child_fd, buf, (size_t)forward_len) == -1) {
            perror("write text to child_fd");
            return false;
        }
    }

    tty_queue->start = 0;
    tty_queue->end   = 0;
    return true;
}

static bool
handle_child_output_(struct readq *bq)
{
    if (!readq_refill(bq))
        return true;

    ssize_t nread = nav_process_output(bq);
    if (nread > 0) {
        char render_buf[READQ_SIZE * 3];
        size_t render_len = nav_render_highlighted(
                                bq->buffer, (size_t)nread, render_buf, sizeof(render_buf));
        const char *write_data = render_len > 0 ? render_buf : bq->buffer;
        size_t write_len       = render_len > 0 ? render_len : (size_t)nread;
        debug_output_bytes_("child-chunk", write_data, write_len);
        if (write(STDOUT_FILENO, write_data, write_len) == -1) {
            perror("processing output");
            return false;
        }
        if (nav_selected_index() >= 0)
            nav_render_status();
    }

    return true;
}

static int
run_parent_(int child_fd, pid_t pid)
{
    int err    = 0;
    int tty_fd = -1;

    // Execution continues in the parent. We copy the PTY master fd into the
    // global `winch_fd_` before the child closes its copy so the SIGWINCH
    // handler can always resize the PTY master associated with the running
    // pager; descriptor tables are per-process, so the child's close does not
    // invalidate the parent's handle. The child inherits the same global state,
    // but it never dereferences `winch_fd_`: it runs the child branch, execs
    // the pager, and leaves resize handling to the parent side of the PTY.
    // Signal dispositions are inherited across fork, so we wait to install the
    // SIGWINCH handler until we're in the parent branch; the child would
    // inherit any earlier handler anyway, and this ordering makes it explicit
    // that only the parent should listen for window-size changes.
    child_pid_ = pid;

    // Up to this point we touched only the new PTY; now we open the controlling
    // terminal (`/dev/tty`). Unlike the PTY master, this fd lets us talk
    // directly to the user's real terminal for interactive input handling. The
    // data path is user → keyboard → OS → `/dev/tty` → parent; when forwarding,
    // we write back through the PTY (parent → master → kernel → slave → child),
    // and the child's stdout makes the reverse trip to reach the screen.
    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1) {
        perror("open /dev/tty");
        err = 1;
        goto cleanup;
    }

    // The navigation subsystem writes its status line and prompts through this
    // fd so they bypass the child process and render straight on the terminal,
    // letting us overlay navigation hints without altering the child’s stdout.
    nav_set_status_fd(tty_fd);
    if (!set_raw_mode_(tty_fd, &prev_term_)) {
        err = 1;
        goto cleanup;
    }

    term_in_raw_mode_   = true;
    restore_fd_at_exit_ = tty_fd;
    if (!restore_registered_) {
        atexit(restore_terminal_atexit_);
        restore_registered_ = true;
    }

    apply_winch_(pid);

    struct sigaction sa;
    sa.sa_handler = handle_winch_;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        perror("sigaction");
        err = 1;
        goto cleanup;
    }

    fd_set read_fds;
    int max_fd = child_fd > tty_fd ? child_fd : tty_fd;

    struct readq child_queue;
    struct readq tty_queue;
    readq_init(&child_queue, child_fd);
    readq_init(&tty_queue, tty_fd);
    // Reset navigation state so each pager invocation starts from a clean slate
    // and can trigger any initial redraw it needs.
    nav_reset();
    nav_set_input_fd(tty_fd);

    // Reset the soft-vs-hard interrupt counter; `handle_sigint_()` escalates to
    // a hard stop after the user presses Ctrl-C twice.
    sigint_count_ = 0;

    int child_status       = 0;
    bool have_child_status = false;

    do {
        int status;
        int options = keepRunning_ ? WNOHANG : 0;
        if (pid == waitpid(pid, &status, options)) {
            child_status      = status;
            have_child_status = true;
            break;
        }

        // Use select() to multiplex between user keystrokes (read on `tty_fd`)
        // and the child process output stream (read on
        // `child_fd`).
        FD_ZERO(&read_fds);
        FD_SET(tty_fd, &read_fds);
        FD_SET(child_fd, &read_fds);

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
            if (!keepRunning_)
                continue;
            perror("select");
            err = 1;
            goto cleanup;
        }

        if (FD_ISSET(tty_fd, &read_fds)) {
            if (!handle_tty_input_(child_fd, &tty_queue, pid)) {
                err = 1;
                goto cleanup;
            }
        }

        if (FD_ISSET(child_fd, &read_fds)) {
            if (!handle_child_output_(&child_queue)) {
                err = 1;
                goto cleanup;
            }
        }
    } while (keepRunning_ == 1);

    if (sigint_count_ >= 2)
        err = 1;
    if (!err && have_child_status) {
        if (WIFEXITED(child_status)) {
            if (WEXITSTATUS(child_status) != 0)
                err = 1;
        } else if (WIFSIGNALED(child_status)) {
            err = 1;
        }
    }

cleanup:
    nav_clear_status();
    restore_terminal_fd_(tty_fd);
    close(child_fd);
    if (tty_fd != -1)
        close(tty_fd);
    child_pid_ = -1;
    nav_set_input_fd(-1);
    nav_set_status_fd(-1);
    if (sigint_count_ >= 2)
        err = 1;
    return err ? EXIT_FAILURE : 0;
}

static void
debug_output_bytes_(const char *tag, const void *buf, size_t len)
{
    if (debug_output_fd_ == -1)
        return;
    if (debug_output_fd_ == -2) {
        const char *path = getenv("MESS_DEBUG_OUTPUT");
        if (!path || !path[0]) {
            debug_output_fd_ = -1;
            return;
        }
        debug_output_fd_ = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (debug_output_fd_ == -1)
            return;
    }
    if (debug_output_fd_ >= 0) {
        if (!tag)
            tag = "chunk";
        dprintf(debug_output_fd_, "%s len=%zu\n", tag, len);
        size_t limit = len < 256 ? len : 256;
        for (size_t i = 0; i < limit; ++i) {
            unsigned char ch = ((const unsigned char *)buf)[i];
            if (ch >= 0x20 && ch <= 0x7e)
                dprintf(debug_output_fd_, "%c", ch);
            else if (ch == '\n')
                dprintf(debug_output_fd_, "\\n");
            else if (ch == '\r')
                dprintf(debug_output_fd_, "\\r");
            else
                dprintf(debug_output_fd_, "\\x%02x", ch);
        }
        if (limit < len)
            dprintf(debug_output_fd_, "...(+%zu bytes)", len - limit);
        dprintf(debug_output_fd_, "\n");
    }
}
