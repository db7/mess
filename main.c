#include "nav.h"
#include "readq.h"
#include <assert.h>
#include <fcntl.h>
#include <regex.h>
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
#ifdef __linux__
#include <pty.h>
#else
#include <util.h> // for openpty(), forkpty()
#endif

#define BUFFER_SIZE 4096

// Function declarations
void set_raw_mode(int fd);

// Global file descriptor for the slave PTY
int winch_fd;

// Function to set the terminal to raw mode
struct termios prev_term;
static bool set_term;
void set_raw_mode(int fd) {
  struct termios term;

  // Get the current terminal settings
  if (tcgetattr(fd, &term) == -1) {
    perror("tcgetattr");
    exit(EXIT_FAILURE);
  }
  prev_term = term;
  set_term = true;

  // Save the original settings (for restoring later)
  // struct termios orig_term = term;

  // Set the terminal to raw mode
  // term.c_lflag &=
  //     ~(ICANON | ECHO | ECHOE | ISIG); // Disable canonical mode, echo, etc.

  term.c_lflag &=
      ~(ICANON | ECHO | ECHOE);            // Disable canonical mode, echo, etc.
  term.c_iflag &= ~(IXON | IXOFF | IXANY); // Disable flow control
  term.c_oflag &= ~OPOST;                  // Disable output processing
  term.c_cflag |= CS8;                     // 8-bit characters
  term.c_cc[VMIN] = 1;  // Minimum number of characters to read
  term.c_cc[VTIME] = 0; // No timeout for input

  // Apply the new terminal settings
  if (tcsetattr(fd, TCSANOW, &term) == -1) {
    perror("tcsetattr");
    exit(EXIT_FAILURE);
  }
}

// Function to change the terminal size (simulate smaller height)
void change_terminal_size(int fd, int rows, int cols) {
  struct winsize ws;
  ws.ws_row = rows; // Set the number of rows (height)
  ws.ws_col = cols; // Set the number of columns (width)
  ws.ws_xpixel = 0;
  ws.ws_ypixel = 0;

  // Use ioctl() to set the window size for the slave terminal
  if (ioctl(fd, TIOCSWINSZ, &ws) == -1) {
    perror("ioctl TIOCSWINSZ");
    exit(EXIT_FAILURE);
  }
}

// Function to get the current terminal size
void get_terminal_size(int fd, int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == -1) {
    perror("ioctl TIOCGWINSZ");
    exit(EXIT_FAILURE);
  }
  *rows = ws.ws_row;
  *cols = ws.ws_col;
}

// Signal handler for SIGWINCH
void _handle_winch(int parent, int child) {
  int parent_rows, parent_cols;

  // Get the parent terminal size
  get_terminal_size(parent, &parent_rows, &parent_cols);

  const int margin = 0;
  int child_rows = parent_rows > margin ? parent_rows - margin : parent_rows;

  // Change the child terminal size to 1 row shorter
  change_terminal_size(child, child_rows, parent_cols);
}

// Signal handler for SIGWINCH
void handle_winch(int sig) {
  (void)sig;
  _handle_winch(STDOUT_FILENO, winch_fd);
}

static volatile int keepRunning = 1;

void handle_sigint(int dummy) {
  (void)dummy;
  printf("ctrl-c caught: %d\n\r", keepRunning);
  if (keepRunning == 1) {
    keepRunning = 0;
  } else
    exit(1);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void run_child(int fd, int argc, char *argv[]);

int main(int argc, char *argv[]) {
  int master_fd, slave_fd;
  pid_t pid;
  char buffer[BUFFER_SIZE]; // Buffer to read input
  ssize_t nread;

#ifndef __linux__
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
    perror("ioctl TIOCGWINSZ");
    exit(EXIT_FAILURE);
  }
  // Open a new pseudo-terminal pair (master and slave)
  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == -1) {
    perror("openpty");
    exit(1);
  }
#else
                                          // linux crap
  master_fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
    perror("erro creating PTY");
    exit(EXIT_FAILURE);
  }
#endif

  winch_fd = master_fd;

  (void)signal(SIGINT, handle_sigint);

  // Fork the process
  pid = fork();
  if (pid == -1) {
    perror("fork");
    exit(1);
  }

  if (pid == 0) {
#ifdef __linux__
    slave_fd = open(ptsname(master_fd), O_RDWR);
#endif

    if (slave_fd < 0) {
      perror("failed to open slave PTY");
      exit(EXIT_FAILURE);
    }
    // Close the master_fd as it's not needed in the child process
    close(master_fd);
    run_child(slave_fd, argc, argv);
    perror("run_child"); // Only reached if execvp fails
    exit(1);
  }
  // In the parent process, we will read from the master file descriptor and
  // modify the output


#ifndef __linux__
  // Close the slave_fd, as it is already handled by the child
  close(slave_fd);
#endif

  int tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd == -1) {
      perror("open /dev/tty");
      goto error;
    }

  set_raw_mode(tty_fd);


#if 0
  // Set up the SIGWINCH signal handler to adjust terminal sizes
  _handle_winch(STDOUT_FILENO, winch_fd);
  struct sigaction sa;
  sa.sa_handler = handle_winch;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGWINCH, &sa, NULL) == -1) {
    perror("sigaction");
    goto error;
  }
#endif

  // Use select() to handle input and output concurrently
  fd_set read_fds;
  int max_fd = master_fd > tty_fd ? master_fd : tty_fd;

 //int flags = fcntl(master_fd, F_GETFL, 0);
 // fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

  readq bq;
  init_queue(&bq, master_fd);

  // int status;
  // waitpid(pid, &status,0);
  //     goto end;

  do {
    // Check if child process terminated
    int status;
    int options = keepRunning ? WNOHANG : 0;
    if (pid == waitpid(pid, &status, options)) {
      // handle status
      // if (WIFEXITED(status)) {
      //  goto error;
      //}
      break;
    }

    // Wait for input from either stdin or the PTY master side
    FD_ZERO(&read_fds);
    FD_SET(tty_fd, &read_fds); // Watch for input on stdin
    FD_SET(master_fd, &read_fds);    // Watch for output from the master side

    if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
      if (!keepRunning)
        continue;
      perror("select");
      goto error;
    }

    // Handle input from stdin (keyboard input)
    if (FD_ISSET(tty_fd, &read_fds)) {
      nread = read(tty_fd, buffer, sizeof(buffer));
      if (nread > 0 && process_input(buffer, nread)) {
        if (write(master_fd, buffer, nread) == -1) {
          perror("write text to master_fd");
          goto error;
        }
      }
    }

    // Handle output from the master side (output from less)
    if (FD_ISSET(master_fd, &read_fds)) {
      if (refill_queue(&bq)) {
        nread = process_output(&bq);
        if (nread > 0 && write(STDOUT_FILENO, bq.buffer, nread) == -1) {
          perror("processing output");
          goto error;
        }
      }
    }
  } while (keepRunning == 1);
  int err = 0;
  goto end;
error:
  err = 1;
end:
  // Close master PTY and tty_fd after use
  (void)tcsetattr(tty_fd, TCSAFLUSH, &prev_term);
  close(master_fd);
  close(tty_fd);
  print_links();
  return err ? EXIT_FAILURE : 0;
}

void run_child(int fd, int argc, char *argv[]) {

#if 1
  // Create a new session to become the controlling process
  if (setsid() == -1) {
    perror("setsid");
    return;
  }
  if (ioctl(fd, TIOCSCTTY, 0) < 0) {
    perror("Failed to set controlling terminal");
    return;
  }
#endif
  keepRunning = 123;

  // In the child process, execute the 'less' command with arguments from argv
  if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
    perror("dup2");
    return;
  }

  //if (isatty(STDIN_FILENO)) {
  //  if (dup2(fd, STDIN_FILENO) == -1) {
  //    perror("dup2 stdin");
  //    return;
  //  }
  //}

  // Add -R to the arguments passed to less
  char *less_args[argc + 2];
  int i =0;
    less_args[i++] = "less";
    less_args[i++] = "-r";

  for (int j = 1; j < argc; i++, j++) {
    less_args[i] = argv[j];
  }
  less_args[i] = NULL;

  // for (int i = 0; i < argc + 10; i++)
  //   printf("%s\n", less_args[i]);

  //// Execute the 'less' command with arguments passed to the program
  execvp(less_args[0], less_args);
  perror("execvp"); // Only reached if execvp fails
}
