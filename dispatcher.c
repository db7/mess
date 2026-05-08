#include "dispatcher.h"

#include "uri.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>

static int set_file_env_(const char *path);
static int run_man_file_(const char *path);
static int run_man_topic_(const char *name, const char *section);
static int run_md_(const char *path);
static int run_file_(const char *path);
static int run_uri_(const char *link);
static int run_simple_(char *const argv[]);
static int run_stdin_(char *const argv[], int fd);
static int build_cmd_(const char *env, const char *fallback, wordexp_t *we);
static int wait_child_(pid_t pid);
static const char *self_(void);
static const char *browser_(void);
static const char *md_(void);
bool dispatcher_command_is_self(const char *cmd);
static void set_manpager_(void);
static char *quote_sh_(const char *text);
static void set_uri_env_(const char *uri);
static bool find_self_(const char *name);

static char self_path_[PATH_MAX];
static bool have_self_path_;

int
dispatcher_run(const char *target)
{
    if (!target || !target[0]) {
        fprintf(stderr, "mess: no target provided\n");
        errno = EINVAL;
        return -1;
    }

    uri_info info;
    if (!uri_parse(target, &info)) {
        perror(target);
        return -1;
    }

    if (uri_is_path(&info)) {
        if (set_file_env_(info.path) != 0)
            return -1;
    } else {
        set_uri_env_(info.raw);
    }

    switch (info.type) {
        case URI_KIND_BROWSER:
            return run_uri_(info.raw);
        case URI_KIND_MAN_TOPIC:
            return run_man_topic_(info.man.name, info.man.section);
        case URI_KIND_MARKDOWN_FILE:
            return run_md_(info.path);
        case URI_KIND_MAN_FILE:
            return run_man_file_(info.path);
        case URI_KIND_FILE:
        default:
            return run_file_(info.path);
    }
}

void
dispatcher_set_self_path(const char *path)
{
    have_self_path_ = false;
    self_path_[0]   = '\0';

    if (!path || !path[0])
        return;

    char resolved[PATH_MAX];
    if (strchr(path, '/')) {
        if (realpath(path, resolved)) {
            snprintf(self_path_, sizeof(self_path_), "%s", resolved);
            have_self_path_ = true;
        }
        return;
    }

    (void)find_self_(path);
}

static bool
find_self_(const char *name)
{
    const char *path_env = getenv("PATH");
    if (!name || !*name || !path_env)
        return false;

    const char *entry = path_env;
    while (true) {
        const char *colon = strchr(entry, ':');
        size_t dir_len    = colon ? (size_t)(colon - entry) : strlen(entry);
        const char *dir   = dir_len == 0 ? "." : entry;
        size_t actual_len = dir_len == 0 ? 1 : dir_len;

        char candidate[PATH_MAX];
        int written = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                               (int)actual_len, dir, name);
        if (written > 0 && (size_t)written < sizeof(candidate) &&
            access(candidate, X_OK) == 0) {
            char resolved[PATH_MAX];
            if (realpath(candidate, resolved)) {
                snprintf(self_path_, sizeof(self_path_), "%s", resolved);
                have_self_path_ = true;
                return true;
            }
        }

        if (!colon)
            break;
        entry = colon + 1;
    }

    return false;
}

static int
set_file_env_(const char *path)
{
    if (setenv("MESSFILE", path, 1) == -1) {
        perror("setenv MESSFILE");
        return -1;
    }
    return 0;
}

static void
set_uri_env_(const char *uri)
{
    if (!uri || !uri[0])
        return;
    if (setenv("MESS_URI", uri, 1) == -1)
        perror("setenv MESS_URI");
}

static const char *
self_(void)
{
    return have_self_path_ ? self_path_ : "mess";
}

static const char *
browser_(void)
{
#ifdef __APPLE__
    return "open";
#else
    return "xdg-open";
#endif
}

static const char *
md_(void)
{
    return "mdcat";
}

bool
dispatcher_command_is_self(const char *cmd)
{
    if (!cmd)
        return false;
    while (isspace((unsigned char)*cmd))
        cmd++;
    if (!*cmd)
        return false;
    size_t len = strcspn(cmd, " \t");
    if (memchr(cmd, '/', len)) {
        char first[PATH_MAX];
        if (len >= sizeof(first))
            return false;
        memcpy(first, cmd, len);
        first[len] = '\0';
        char resolved[PATH_MAX];
        if (have_self_path_ && realpath(first, resolved) &&
            strcmp(resolved, self_path_) == 0)
            return true;
    }
    if (have_self_path_ && strncmp(cmd, self_path_, len) == 0 &&
        self_path_[len] == '\0')
        return true;
    return (len == 4 && strncmp(cmd, "mess", 4) == 0);
}

const char *
dispatcher_self_path(void)
{
    if (have_self_path_)
        return self_path_;
    return NULL;
}

static int
build_cmd_(const char *env, const char *fallback, wordexp_t *we)
{
    const char *candidates[3] = {NULL, NULL, NULL};
    if (env && *env)
        candidates[0] = env;
    candidates[1] = fallback;
    if (fallback && strcmp(fallback, "mdcat") == 0)
        candidates[2] = "lowdown -tterm --term-no-links --term-width=100";
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *cmd = candidates[i];
        if (!cmd || !*cmd)
            continue;
        int rc = wordexp(cmd, we, WRDE_NOCMD);
        if (rc == 0)
            return 0;
        fprintf(stderr, "mess: unable to parse command '%s'\n", cmd);
    }
    errno = EINVAL;
    return -1;
}

static int
run_md_(const char *path)
{
    const char *renderer_env = getenv("MESS_MDRENDER");
    wordexp_t render_we;
    if (build_cmd_(renderer_env, md_(), &render_we) != 0)
        return -1;
    char **renderer_argv = calloc(render_we.we_wordc + 2, sizeof(char *));
    if (!renderer_argv) {
        wordfree(&render_we);
        return -1;
    }
    for (size_t i = 0; i < render_we.we_wordc; ++i)
        renderer_argv[i] = render_we.we_wordv[i];
    renderer_argv[render_we.we_wordc]     = (char *)path;
    renderer_argv[render_we.we_wordc + 1] = NULL;

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(renderer_argv);
        wordfree(&render_we);
        return -1;
    }

    pid_t renderer = fork();
    if (renderer == -1) {
        perror("fork");
        free(renderer_argv);
        wordfree(&render_we);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (renderer == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(renderer_argv[0], renderer_argv);
        execlp("cat", "cat", path, (char *)NULL);
        perror("markdown renderer");
        _exit(127);
    }

    pid_t pager = fork();
    if (pager == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        free(renderer_argv);
        wordfree(&render_we);
        wait_child_(renderer);
        return -1;
    }
    if (pager == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        const char *binary = self_();
        execlp(binary, binary, "-m", "-o", (char *)NULL);
        perror("pager");
        _exit(127);
    }

    close(pipefd[0]);
    close(pipefd[1]);
    free(renderer_argv);
    wordfree(&render_we);
    int rc1 = wait_child_(renderer);
    int rc2 = wait_child_(pager);
    return (rc1 == 0) ? rc2 : rc1;
}

static int
run_file_(const char *path)
{
    int rc = 0;
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror(path);
        rc = -1;
    } else {
        char *const argv[] = {
            (char *)self_(),
            (char *)"-m",
            (char *)"-o",
            NULL,
        };
        rc = run_stdin_(argv, fd);
    }
    return rc;
}

static int
run_man_file_(const char *path)
{
    set_manpager_();
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||       \
    defined(__OpenBSD__) || defined(__DragonFly__)
    char *const argv[] = {(char *)"man", (char *)path, NULL};
#else
    char *const argv[] = {(char *)"man", (char *)"-l", (char *)path, NULL};
#endif
    return run_simple_(argv);
}

static int
run_man_topic_(const char *name, const char *section)
{
    if (!name || !section)
        return -1;
    set_manpager_();
    char *const argv[] = {(char *)"man", (char *)section, (char *)name, NULL};
    return run_simple_(argv);
}

static int
run_uri_(const char *link)
{
    const char *browser_env = getenv("BROWSER");
    wordexp_t we;
    if (build_cmd_(browser_env, browser_(), &we) != 0)
        return -1;
    char **argv = calloc(we.we_wordc + 2, sizeof(char *));
    if (!argv) {
        wordfree(&we);
        return -1;
    }
    for (size_t i = 0; i < we.we_wordc; ++i)
        argv[i] = we.we_wordv[i];
    argv[we.we_wordc]     = (char *)link;
    argv[we.we_wordc + 1] = NULL;
    int rc                = run_simple_(argv);
    free(argv);
    wordfree(&we);
    return rc;
}

static int
run_simple_(char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    return wait_child_(pid);
}

static int
run_stdin_(char *const argv[], int fd)
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(fd);
        return -1;
    }
    if (pid == 0) {
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2 stdin");
            _exit(1);
        }
        close(fd);
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    close(fd);
    return wait_child_(pid);
}

static int
wait_child_(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static void
set_manpager_(void)
{
    if (!have_self_path_)
        return;

    char *quoted = quote_sh_(self_path_);
    if (!quoted)
        return;

    char buffer[PATH_MAX * 2];
    int len = snprintf(buffer, sizeof(buffer), "%s -m", quoted);
    free(quoted);
    if (len <= 0 || (size_t)len >= sizeof(buffer))
        return;

    if (setenv("MANPAGER", buffer, 1) == -1)
        perror("setenv MANPAGER");
}

static char *
quote_sh_(const char *text)
{
    if (!text)
        return NULL;
    size_t len = 2; // surrounding quotes
    for (const char *p = text; *p; ++p) {
        if (*p == '\'')
            len += 4;
        else
            len += 1;
    }
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    char *w = out;
    *w++    = '\'';
    for (const char *p = text; *p; ++p) {
        if (*p == '\'') {
            memcpy(w, "'\\''", 4);
            w += 4;
        } else {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w   = '\0';
    return out;
}
