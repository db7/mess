#include "nav.h"
#include "pager.h"
#include "man.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

static void usage_(const char *prog);
static void print_version_(void);
static int dispatcher_handle_arguments(int argc, char *argv[]);
static int dispatcher_open_link(const char *link, char **pager_flags,
                                int pager_flag_count);
static bool is_markdown_path_(const char *path);
static bool is_manpage_path_(const char *path);
static bool is_url_(const char *target);
static bool parse_man_uri_(const char *uri, char **name_out, char **section_out);
static int run_markdown_stream_(const char *path, char **pager_flags,
                                int pager_flag_count);
static int run_man_stream_(const char *path, char **pager_flags,
                           int pager_flag_count);
static int run_man_lookup_(const char *name, const char *section,
                           char **pager_flags, int pager_flag_count);
static int launch_browser_(const char *link);
static char *strip_fragment_(const char *link);
static int run_simple_command_(char *const argv[]);
static int pager_invoke_(char *target, char **pager_flags,
                         int pager_flag_count);
static int pager_invoke_mode_(nav_mode_t mode, char *target,
                              char **pager_flags, int pager_flag_count);

static bool cli_mode_forced_;
static nav_mode_t cli_forced_mode_;

#ifndef MESS_VERSION
#define MESS_VERSION "dev"
#endif

int
main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            print_version_();
            return 0;
        }
        if (strcmp(argv[1], "--open") == 0) {
            if (argc < 3) {
                usage_(argv[0]);
                return EXIT_FAILURE;
            }
            return dispatcher_open_link(argv[2], NULL, 0);
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage_(argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "--man") == 0 || strcmp(argv[1], "-m") == 0) {
            cli_mode_forced_ = true;
            cli_forced_mode_ = NAV_MODE_MAN;
            // Shift argv so dispatcher sees the remaining args unchanged.
            argc--;
            argv++;
        }
    }

    return dispatcher_handle_arguments(argc, argv);
}

// Print the command-line usage banner.
static void
usage_(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-m] [pager-args]\n"
            "  %s --open <url>\n"
            "  %s --version\n",
            prog, prog, prog);
}

// Show the compiled version string.
static void
print_version_(void)
{
    printf("mess %s\n", MESS_VERSION);
}

static int
dispatcher_handle_arguments(int argc, char *argv[])
{
    nav_set_document_path(NULL);
    int dashdash = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            dashdash = i;
            break;
        }
    }

    int search_end                 = (dashdash == -1) ? argc : dashdash;
    int target_index               = -1;
    const char *flag_before_target = NULL;

    for (int i = 1; i < search_end; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-m") == 0 || strcmp(arg, "--man") == 0) {
            cli_mode_forced_ = true;
            cli_forced_mode_ = NAV_MODE_MAN;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            if (target_index != -1) {
                fprintf(stderr,
                        "mess: pager flags must come after '--' (saw '%s')\n",
                        arg);
                return EXIT_FAILURE;
            }
            if (!flag_before_target)
                flag_before_target = arg;
            continue;
        }
        if (target_index != -1) {
            fprintf(stderr,
                    "mess: multiple targets specified ('%s' and '%s')\n",
                    argv[target_index], arg);
            return EXIT_FAILURE;
        }
        target_index = i;
    }

    char **pager_flags   = NULL;
    int pager_flag_count = 0;
    if (dashdash != -1) {
        pager_flags      = &argv[dashdash + 1];
        pager_flag_count = argc - dashdash - 1;
    }

    if (flag_before_target) {
        fprintf(stderr,
                "mess: pager flags ('%s') must come after '--' separator\n",
                flag_before_target);
        return EXIT_FAILURE;
    }

    if (target_index == -1)
        return pager_invoke_(NULL, pager_flags, pager_flag_count);

    char *target = argv[target_index];

    if (strncmp(target, "man://", 6) == 0) {
        char *name    = NULL;
        char *section = NULL;
        if (!parse_man_uri_(target, &name, &section))
            return EXIT_FAILURE;
        nav_set_document_path(target);
        int rc = run_man_lookup_(name, section, pager_flags, pager_flag_count);
        free(name);
        free(section);
        return rc;
    }

    if (is_url_(target))
        return dispatcher_open_link(target, pager_flags, pager_flag_count);

    if (is_markdown_path_(target)) {
        nav_set_document_path(target);
        return run_markdown_stream_(target, pager_flags, pager_flag_count);
    }

    if (is_manpage_path_(target)) {
        nav_set_document_path(target);
        return run_man_stream_(target, pager_flags, pager_flag_count);
    }

    if (target && strcmp(target, "-") != 0)
        nav_set_document_path(target);
    else
        nav_set_document_path(NULL);

    return pager_invoke_(target, pager_flags, pager_flag_count);
}

static int
dispatcher_open_link(const char *link, char **pager_flags, int pager_flag_count)
{
    if (!link)
        return EXIT_FAILURE;

    nav_set_document_path(NULL);

    if (strncmp(link, "file://", 7) == 0) {
        char *path = strip_fragment_(link + 7);
        if (!path)
            return EXIT_FAILURE;

        nav_set_document_path(path);
        int rc;
        if (is_markdown_path_(path)) {
            rc = run_markdown_stream_(path, pager_flags, pager_flag_count);
        } else if (is_manpage_path_(path)) {
            rc = run_man_stream_(path, pager_flags, pager_flag_count);
        } else {
            rc = pager_invoke_(path, pager_flags, pager_flag_count);
        }
        free(path);
        return rc;
    }

    if (strncmp(link, "man://", 6) == 0) {
        char *name    = NULL;
        char *section = NULL;
        if (!parse_man_uri_(link, &name, &section))
            return EXIT_FAILURE;
        nav_set_document_path(link);
        int rc = run_man_lookup_(name, section, pager_flags, pager_flag_count);
        free(name);
        free(section);
        return rc;
    }

    if (is_url_(link))
        return launch_browser_(link);

    if (is_markdown_path_(link)) {
        nav_set_document_path(link);
        return run_markdown_stream_(link, pager_flags, pager_flag_count);
    }

    if (is_manpage_path_(link)) {
        nav_set_document_path(link);
        return run_man_stream_(link, pager_flags, pager_flag_count);
    }

    if (strcmp(link, "-") != 0)
        nav_set_document_path(link);

    return pager_invoke_((char *)link, pager_flags, pager_flag_count);
}

// Determine if the path looks like a Markdown file.
static bool
is_markdown_path_(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path)
        return false;
    char ext[32];
    size_t len = strlen(dot + 1);
    if (len >= sizeof(ext))
        return false;
    for (size_t i = 0; i < len; ++i) {
        ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    }
    ext[len]                    = '\0';
    const char *markdown_exts[] = {"md",  "markdown", "mdown", "mdwn",
                                   "mkd", "mkdn",     "mdtxt", "mdtext"
                                  };
    for (size_t i = 0; i < sizeof(markdown_exts) / sizeof(markdown_exts[0]);
         ++i) {
        if (strcmp(ext, markdown_exts[i]) == 0)
            return true;
    }
    return false;
}

static bool
is_manpage_path_(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path)
        return false;
    const char *ext = dot + 1;
    if (*ext == '\0')
        return false;
    if (!isdigit((unsigned char) * ext))
        return false;
    for (const char *p = ext + 1; *p; ++p) {
        if (!isalnum((unsigned char) * p))
            return false;
    }
    return true;
}

// Check whether the string contains a URL scheme.
static bool
is_url_(const char *target)
{
    if (!target)
        return false;
    const char *colon = strchr(target, ':');
    if (!colon || colon == target)
        return false;
    for (const char *p = target; p < colon; ++p) {
        if (!isalnum((unsigned char) * p) && *p != '+' && *p != '-' && *p != '.')
            return false;
    }
    if (colon[1] == '/' && colon[2] == '/')
        return true;
    return false;
}

static bool
parse_man_uri_(const char *uri, char **name_out, char **section_out)
{
    if (!uri || strncmp(uri, "man://", 6) != 0)
        return false;

    const char *payload = uri + 6;
    if (!*payload) {
        fprintf(stderr, "mess: invalid man URI '%s'\n", uri);
        return false;
    }

    const char *dot = strrchr(payload, '.');
    if (!dot || dot == payload || dot[1] == '\0') {
        fprintf(stderr, "mess: invalid man URI '%s'\n", uri);
        return false;
    }

    size_t name_len = (size_t)(dot - payload);
    char *name      = strndup(payload, name_len);
    if (!name) {
        perror("strndup");
        return false;
    }

    const char *section_src = dot + 1;
    for (const char *p = section_src; *p; ++p) {
        if (!isalnum((unsigned char) * p)) {
            fprintf(stderr, "mess: invalid man section in '%s'\n", uri);
            free(name);
            return false;
        }
    }

    char *section = strdup(section_src);
    if (!section) {
        perror("strdup");
        free(name);
        return false;
    }

    *name_out    = name;
    *section_out = section;
    return true;
}

// Pipe lowdown output into the pager for the provided file/args.
static int
run_markdown_stream_(const char *path, char **pager_flags, int pager_flag_count)
{
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("lowdown", "lowdown", "-tterm", "--term-no-links", path,
               (char *)NULL);

        // Soft fallback if lowdown is missing: just cat the file to the pager.
        // This preserves basic paging even without nice Markdown rendering.
        execlp("cat", "cat", path, (char *)NULL);

        // If both execs failed:
        perror("exec lowdown/cat");
        _exit(127);
    }

    close(pipefd[1]);
    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin == -1) {
        perror("dup stdin");
        close(pipefd[0]);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }

    if (dup2(pipefd[0], STDIN_FILENO) == -1) {
        perror("dup2 stdin");
        close(pipefd[0]);
        close(saved_stdin);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }
    close(pipefd[0]);

    int rc = pager_invoke_mode_(NAV_MODE_MAN, NULL, pager_flags,
                                pager_flag_count);

    if (dup2(saved_stdin, STDIN_FILENO) == -1) {
        perror("restore stdin");
    }
    close(saved_stdin);

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid lowdown");
        return EXIT_FAILURE;
    }
    if (rc == 0) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            rc = EXIT_FAILURE;
    }
    return rc;
}

static int
run_man_stream_(const char *path, char **pager_flags, int pager_flag_count)
{
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("man", "man", "-P", "cat", "-l", path, (char *)NULL);
        execlp("mandoc", "mandoc", "-T", "utf8", path, (char *)NULL);
        execlp("cat", "cat", path, (char *)NULL);
        perror("exec man/mandoc/cat");
        _exit(127);
    }

    close(pipefd[1]);
    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin == -1) {
        perror("dup stdin");
        close(pipefd[0]);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }

    if (dup2(pipefd[0], STDIN_FILENO) == -1) {
        perror("dup2 stdin");
        close(pipefd[0]);
        close(saved_stdin);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }
    close(pipefd[0]);

    int rc = pager_invoke_mode_(NAV_MODE_MAN, NULL, pager_flags,
                                pager_flag_count);

    if (dup2(saved_stdin, STDIN_FILENO) == -1)
        perror("restore stdin");
    close(saved_stdin);

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid man");
        return EXIT_FAILURE;
    }
    if (rc == 0) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            rc = EXIT_FAILURE;
    }
    return rc;
}

static int
run_man_lookup_(const char *name, const char *section, char **pager_flags,
                int pager_flag_count)
{
    if (!name || !section)
        return EXIT_FAILURE;

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("man", "man", section, name, (char *)NULL);
        execlp("man", "man", name, (char *)NULL);
        perror("exec man");
        _exit(127);
    }

    close(pipefd[1]);
    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin == -1) {
        perror("dup stdin");
        close(pipefd[0]);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }

    if (dup2(pipefd[0], STDIN_FILENO) == -1) {
        perror("dup2 stdin");
        close(pipefd[0]);
        close(saved_stdin);
        (void)waitpid(pid, NULL, 0);
        return EXIT_FAILURE;
    }
    close(pipefd[0]);

    int rc = pager_invoke_(NULL, pager_flags, pager_flag_count);

    if (dup2(saved_stdin, STDIN_FILENO) == -1)
        perror("restore stdin");
    close(saved_stdin);

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid man");
        return EXIT_FAILURE;
    }
    if (rc == 0) {
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            rc = EXIT_FAILURE;
    }
    return rc;
}

// Spawn the preferred browser for a URL.
static int
launch_browser_(const char *link)
{
    const char *browser = getenv("BROWSER");
    wordexp_t we;
    bool have_wordexp    = false;
    char **browser_words = NULL;
    size_t browser_count = 0;

    if (browser && browser[0]) {
        if (wordexp(browser, &we, WRDE_NOCMD) == 0 && we.we_wordc > 0) {
            browser_words = we.we_wordv;
            browser_count = we.we_wordc;
            have_wordexp  = true;
        }
    }

#ifdef __APPLE__
    char *fallback[] = {"open", NULL};
#else
    char *fallback[] = {"xdg-open", NULL};
#endif

    if (!browser_words) {
        browser_words = fallback;
        browser_count = 1;
    }

    char **cmd = calloc(browser_count + 2, sizeof(char *));
    if (!cmd) {
        perror("calloc browser command");
        if (have_wordexp)
            wordfree(&we);
        return EXIT_FAILURE;
    }

    size_t pos = 0;
    for (size_t i = 0; i < browser_count; ++i) {
        cmd[pos++] = browser_words[i];
    }
    cmd[pos++] = (char *)link;
    cmd[pos]   = NULL;

    int rc = run_simple_command_(cmd);

    if (have_wordexp)
        wordfree(&we);
    free(cmd);
    return rc;
}

// Return a copy of the link without any anchor fragment.
static char *
strip_fragment_(const char *link)
{
    const char *hash = strchr(link, '#');
    size_t len       = hash ? (size_t)(hash - link) : strlen(link);
    char *result     = malloc(len + 1);
    if (!result) {
        perror("malloc");
        return NULL;
    }
    memcpy(result, link, len);
    result[len] = '\0';
    return result;
}

// Fork/exec a simple argv list and propagate its exit status.
static int
run_simple_command_(char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    return EXIT_FAILURE;
}

static int
pager_invoke_(char *target, char **pager_flags, int pager_flag_count)
{
    nav_mode_t mode =
        cli_mode_forced_ ? cli_forced_mode_ : NAV_MODE_OSC8;
    return pager_invoke_mode_(mode, target, pager_flags, pager_flag_count);
}

static int
pager_invoke_mode_(nav_mode_t mode, char *target, char **pager_flags,
                   int pager_flag_count)
{
    int total   = 1 + pager_flag_count + (target ? 1 : 0);
    char **argv = calloc((size_t)total + 1, sizeof(char *));
    if (!argv) {
        perror("calloc pager args");
        return EXIT_FAILURE;
    }

    argv[0] = (char *)"mess";
    for (int i = 0; i < pager_flag_count; ++i)
        argv[1 + i] = pager_flags[i];
    if (target)
        argv[1 + pager_flag_count] = target;
    argv[total] = NULL;

    // Reason: Reuse the pager harness for both OSC8 and man highlight modes.
    int rc =
        (mode == NAV_MODE_MAN) ? man_run(total, argv) : pager_run(total, argv);
    free(argv);
    return rc;
}
