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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main(int argc, char *argv[]) {
   if (argc != 2) {
     fprintf(stderr, "Usage: %s <url>\n", argv[0]);
     exit(1);
   }
   const char *lowdown_str= "bash -c 'clear && lowdown -tterm --term-no-links %s | lless'";
   const char *lynx_str = "lynx %s";
   const char *cmd_str = NULL;

    char cmd[1024];
    const char *link = argv[1];
    if ((strncmp(link, "file://", 7) == 0)) {
      link = strstr(link + 7, "/");
      cmd_str = lowdown_str;
    } else if ((strncmp(link, "https://", 8) == 0)) {
      cmd_str = lynx_str;
    } else {
      cmd_str = lowdown_str;
    }
      sprintf(cmd, cmd_str, link);
   return system(cmd);
}
