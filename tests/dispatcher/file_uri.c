// RUN: cc %s -o %s.bin
// RUN: echo uri > %s.data
// RUN: env PAGER=%s.bin %S/../../mess file://%s.data | %check
// CHECK: ARGS: %s.bin %s.data
// CHECK: MESSFILE=%s.data
// CHECK: FIRST:uri

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
    const char *path = (argc > 1) ? argv[1] : NULL;
    FILE *fp         = path ? fopen(path, "r") : NULL;
    char line[512];
    if (fp && fgets(line, sizeof line, fp))
        printf("FIRST:%s", line);
    else
        printf("FIRST:\n");
    if (fp)
        fclose(fp);
    return 0;
}
