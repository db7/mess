#include "nav.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void
write_pipe_(int fd, const char *data)
{
	size_t len = strlen(data);
	assert(write(fd, data, len) == (ssize_t)len);
}

static void
test_nav_session_exit_key(void)
{
	int tty_pipe[2];
	assert(pipe(tty_pipe) == 0);

	struct nav_session_args args = {
		.tty_fd = tty_pipe[0],
		.notify_fd = -1,
		.out_fd = -1,
		.timeout_ms = 500,
		.exit_key = 'q',
	};
	struct nav_session *sess = nav_session_begin(&args);
	assert(sess != NULL);

	write_pipe_(tty_pipe[1], "q");

	nav_event_t ev = nav_session_run(sess);
	assert(ev == NAV_EVENT_EXIT);

	nav_session_end(sess);
	close(tty_pipe[0]);
	close(tty_pipe[1]);
}

static void
test_nav_session_timeout(void)
{
	int tty_pipe[2];
	assert(pipe(tty_pipe) == 0);

	struct nav_session_args args = {
		.tty_fd = tty_pipe[0],
		.notify_fd = -1,
		.out_fd = -1,
		.timeout_ms = 50,
		.exit_key = 'q',
	};
	struct nav_session *sess = nav_session_begin(&args);
	assert(sess != NULL);

	nav_event_t ev = nav_session_run(sess);
	assert(ev == NAV_EVENT_TIMEOUT);

	nav_session_end(sess);
	close(tty_pipe[0]);
	close(tty_pipe[1]);
}

static void
test_nav_session_dirty_notification(void)
{
	int tty_pipe[2];
	int notify_pipe[2];
	assert(pipe(tty_pipe) == 0);
	assert(pipe(notify_pipe) == 0);

	struct nav_session_args args = {
		.tty_fd = tty_pipe[0],
		.notify_fd = notify_pipe[0],
		.out_fd = -1,
		.timeout_ms = 1000,
		.exit_key = 'q',
	};
	struct nav_session *sess = nav_session_begin(&args);
	assert(sess != NULL);

	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		close(tty_pipe[0]);
		close(tty_pipe[1]);
		close(notify_pipe[0]);
		usleep(100 * 1000);
		write_pipe_(notify_pipe[1], "x");
		_exit(0);
	}

	close(notify_pipe[1]);
	nav_event_t ev = nav_session_run(sess);
	assert(ev == NAV_EVENT_DIRTY);

	int status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status));

	nav_session_end(sess);
	close(tty_pipe[0]);
	close(tty_pipe[1]);
	close(notify_pipe[0]);
}

int
main(void)
{
	test_nav_session_exit_key();
	test_nav_session_timeout();
	test_nav_session_dirty_notification();
	puts("nav session tests OK");
	return 0;
}
