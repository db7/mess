#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "pager.h"
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
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <pty.h>
#else
#include <util.h>
#endif

struct termios_record {
    char stage;
    struct termios term;
};

// Capture writes performed by the pager's termios hook during tests.
static int hook_fd_ = -1;

// Record terminal attributes emitted during pager execution.
static void
termios_hook_(const struct termios *term, char stage)
{
    if (hook_fd_ == -1)
        return;
    struct termios_record rec = {stage, *term};
    size_t offset = 0;
    const char *ptr = (const char *)&rec;
    while (offset < sizeof(rec)) {
        ssize_t written = write(hook_fd_, ptr + offset, sizeof(rec) - offset);
        if (written <= 0) {
            _exit(2);
        }
        offset += (size_t)written;
    }
}

// Drain any remaining bytes from the PTY to avoid blocking.
static void
drain_output_(int fd)
{
    char buf[256];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        break;
    }
}

// Wait for the pager child to exit while streaming output to the drain.
static void
wait_for_exit_(pid_t pid, int master_fd, int *status_out)
{
    for (;;) {
        pid_t res = waitpid(pid, status_out, WNOHANG);
        if (res == -1) {
            perror("waitpid");
            exit(1);
        }
        if (res == pid)
            break;
        drain_output_(master_fd);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    drain_output_(master_fd);
}

typedef void (*input_fn)(pid_t child_pid, int master_fd);

// Send a quit command to the pager after a short delay.
static void
send_quit_(pid_t child_pid, int master_fd)
{
    (void)child_pid;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    if (write(master_fd, "q", 1) == -1) {
        perror("write q");
        exit(1);
    }
}

// Simulate Ctrl-C followed by quit to test signal handling.
static void
send_ctrl_c_then_quit_(pid_t child_pid, int master_fd)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    if (kill(child_pid, SIGINT) == -1) {
        perror("kill SIGINT");
        exit(1);
    }
    nanosleep(&ts, NULL);
    if (write(master_fd, "q", 1) == -1) {
        perror("write q");
        exit(1);
    }
}

// Deliver two Ctrl-C signals to force the pager to abort.
static void
send_double_ctrl_c_(pid_t child_pid, int master_fd)
{
    (void)master_fd;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    if (kill(child_pid, SIGINT) == -1) {
        perror("kill SIGINT");
        exit(1);
    }
    nanosleep(&ts, NULL);
    if (kill(child_pid, SIGINT) == -1) {
        perror("kill SIGINT");
        exit(1);
    }
}

// Close a file descriptor helper that resets the stored handle.
static void
close_fd_(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

// Execute a single integration test scenario against the pager.
static void
run_case_(const char *label, input_fn fn, int expected_exit)
{
    int master_fd = -1;
    int slave_fd = -1;
    struct winsize win;
    win.ws_row = 24;
    win.ws_col = 80;
    win.ws_xpixel = 0;
    win.ws_ypixel = 0;
    int flags;
    int status = 0;
    int pipe_fds[2];
    struct termios_record rec;
    struct termios saved = {0};
    struct termios restored = {0};
    bool saw_saved = false;
    bool saw_restored = false;

    if (pipe(pipe_fds) == -1) {
        perror("pipe");
        exit(1);
    }

    if (openpty(&master_fd, &slave_fd, NULL, NULL, &win) == -1) {
        perror("openpty");
        exit(1);
    }

    hook_fd_ = pipe_fds[1];
    pager_set_termios_hook(termios_hook_);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        if (setsid() == -1) {
            perror("setsid");
            _exit(1);
        }
        if (ioctl(slave_fd, TIOCSCTTY, 0) == -1) {
            perror("TIOCSCTTY");
            _exit(1);
        }
        if (dup2(slave_fd, STDIN_FILENO) == -1 ||
            dup2(slave_fd, STDOUT_FILENO) == -1 ||
            dup2(slave_fd, STDERR_FILENO) == -1) {
            perror("dup2 slave");
            _exit(1);
        }
        close_fd_(&slave_fd);
        close_fd_(&master_fd);
        int rc = pager_run(1, (char *[]) {
            "mess", NULL
        });
        _exit(rc);
    }

    close(pipe_fds[1]);
    hook_fd_ = -1;
    close_fd_(&slave_fd);

    flags = fcntl(master_fd, F_GETFL);
    if (flags == -1 || fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl master");
        exit(1);
    }

    fn(pid, master_fd);

    wait_for_exit_(pid, master_fd, &status);

    ssize_t total = 0;
    while ((total = read(pipe_fds[0], &rec, sizeof(rec))) > 0) {
        if (total != sizeof(rec)) {
            fprintf(stderr, "%s: partial hook record\n", label);
            exit(1);
        }
        if (rec.stage == 'S') {
            saved = rec.term;
            saw_saved = true;
        } else if (rec.stage == 'R') {
            restored = rec.term;
            saw_restored = true;
        }
    }
    close_fd_(&pipe_fds[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit) {
        fprintf(stderr, "%s: unexpected exit status %d\n", label,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        exit(1);
    }

    if (!saw_saved || !saw_restored) {
        fprintf(stderr, "%s: missing termios hook data\n", label);
        exit(1);
    }

    if (memcmp(&saved, &restored, sizeof(struct termios)) != 0) {
        fprintf(stderr, "%s: terminal settings not restored\n", label);
        exit(1);
    }

    close_fd_(&master_fd);
    fprintf(stderr, "%s: ok\n", label);
}

int
main(void)
{
    run_case_("quit", send_quit_, 0);
    run_case_("ctrl-c then quit", send_ctrl_c_then_quit_, 0);
    run_case_("double ctrl-c", send_double_ctrl_c_, 1);
    return 0;
}
