// RUN: cc %s -o %s.bin
// RUN: env BROWSER=%s.bin %S/../../mess https://dispatcher.example | %check
// CHECK: ARGS: %s.bin https://dispatcher.example
// CHECK: MESSFILE=(null)

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
    return 0;
}
