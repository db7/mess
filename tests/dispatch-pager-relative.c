// clang-format off
// RUN: env MESS_PAGER=%(realpath %x) MESS_MDRENDER=cat %mess %S/data/nested/page.md | %check
// CHECK: ARGS: {{.*}}/dispatch-pager-relative.bin
// CHECK: MESSFILE={{.*}}/data/nested/page.md
// CHECK: CWD:{{.*}}/data/nested
// CHECK: FIRST:{{ *}}nested page
// clang-format on

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    printf("ARGS:");
    for (int i = 0; i < argc; ++i)
        printf(" %s", argv[i]);
    printf("\n");
    const char *messfile = getenv("MESSFILE");
    printf("MESSFILE=%s\n", messfile ? messfile : "(null)");
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        printf("CWD:%s\n", cwd);
    else
        printf("CWD:\n");
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
