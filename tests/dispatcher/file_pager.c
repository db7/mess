// AGENT: DONT CHANGE THIS FILE!
// RUN: cc %s -o %s.bin
// RUN: printf sample > %s.data
// RUN: env PAGER=%s.bin %mess < %s.data | %check
// CHECK: ARGS: %s.bin
// CHECK: MESSFILE=(null)
// CHECK: FIRST:sample

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    printf("ARGS:");
    for (int i = 0; i < argc; ++i)
        printf(" %s", argv[i]);
    printf("\n");
    const char *messfile = getenv("MESSFILE");
    printf("MESSFILE=%s\n", messfile ? messfile : "(null)");
    char line[512];
    if (fgets(line, sizeof line, stdin))
        printf("FIRST:%s", line);
    else
        printf("FIRST:\n");
    return 0;
}
