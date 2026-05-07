// RUN: cc %s -o %s.bin
// RUN: env MESSPAGER=%s.bin %mess man://printf.3 | %check
// CHECK: ARGS: %s.bin
// CHECK: MESSFILE=(null)
// CHECK: FIRST:PRINTF(3)

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
    FILE *fp         = path ? fopen(path, "r") : stdin;
    char line[512];
    if (fp && fgets(line, sizeof line, fp))
        printf("FIRST:%s", line);
    else
        printf("FIRST:\n");
    if (fp && fp != stdin)
        fclose(fp);

    return 0;
}
