#include "uri.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

bool
uri_is_markdown_path(const char *path)
{
    if (!path)
        return false;
    static const char *exts[] = {"md",  "markdown", "mdown", "mdwn",
                                 "mkd", "mkdn",     "mdtxt", "mdtext"};
    const char *dot           = strrchr(path, '.');
    if (!dot || dot == path)
        return false;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i) {
        if (strcasecmp(dot + 1, exts[i]) == 0)
            return true;
    }
    return false;
}

bool
uri_is_manpage_path(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path)
        return false;
    const char *ext = dot + 1;
    if (!*ext)
        return false;
    if (!isdigit((unsigned char)*ext))
        return false;
    for (const char *p = ext + 1; *p; ++p) {
        if (!isalnum((unsigned char)*p))
            return false;
    }
    return true;
}

bool
uri_is_http_url(const char *target)
{
    if (!target)
        return false;
    if (strncasecmp(target, "http://", 7) == 0)
        return true;
    if (strncasecmp(target, "https://", 8) == 0)
        return true;
    return false;
}

bool
uri_is_man_uri(const char *target)
{
    return target && strncmp(target, "man://", 6) == 0;
}

bool
uri_convert_file_uri(const char *uri, char *out, size_t out_sz)
{
    if (!uri || !out || out_sz == 0)
        return false;
    if (strncmp(uri, "file://", 7) != 0)
        return false;
    const char *rest = uri + 7;
    const char *path = NULL;
    if (rest[0] == '/') {
        path = rest;
    } else {
        const char *slash = strchr(rest, '/');
        if (!slash || slash[0] == '\0')
            return false;
        path = slash;
    }
    if (!path || *path == '\0')
        return false;

    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        perror(path);
        return false;
    }

    struct stat st;
    if (stat(resolved, &st) == -1) {
        perror(resolved);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : ENOENT;
        return false;
    }

    int written = snprintf(out, out_sz, "%s", resolved);
    if (written < 0 || (size_t)written >= out_sz)
        return false;
    return true;
}

bool
uri_canonicalize_raw_path(const char *path, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0)
        return false;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return false;
    struct stat st;
    if (stat(resolved, &st) == -1)
        return false;
    if (!S_ISREG(st.st_mode)) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : ENOENT;
        return false;
    }
    int written = snprintf(out, out_sz, "%s", resolved);
    if (written < 0 || (size_t)written >= out_sz)
        return false;
    return true;
}

bool
uri_parse_man_uri(const char *uri, uri_man_topic *topic)
{
    if (!topic)
        return false;
    topic->name[0]    = '\0';
    topic->section[0] = '\0';
    if (!uri_is_man_uri(uri))
        return false;

    const char *payload = uri + strlen("man://");
    const char *dot     = strrchr(payload, '.');
    if (!dot || dot == payload || dot[1] == '\0')
        return false;

    size_t name_len    = (size_t)(dot - payload);
    size_t section_len = strlen(dot + 1);
    if (name_len >= sizeof(topic->name) ||
        section_len >= sizeof(topic->section))
        return false;

    memcpy(topic->name, payload, name_len);
    topic->name[name_len] = '\0';
    memcpy(topic->section, dot + 1, section_len + 1);
    return true;
}

bool
uri_parse(const char *target, uri_info *info)
{
    if (!target || !*target || !info)
        return false;
    memset(info, 0, sizeof(*info));
    info->raw  = target;
    info->type = URI_KIND_FILE;

    if (uri_is_man_uri(target)) {
        if (!uri_parse_man_uri(target, &info->man))
            return false;
        info->type = URI_KIND_MAN_TOPIC;
        return true;
    }

    if (uri_is_http_url(target)) {
        info->type = URI_KIND_BROWSER;
        return true;
    }

    if (uri_convert_file_uri(target, info->path, sizeof(info->path))) {
        // already classified via extension below
    } else {
        if (!uri_canonicalize_raw_path(target, info->path, sizeof(info->path)))
            return false;
    }

    if (uri_is_markdown_path(info->path)) {
        info->type = URI_KIND_MARKDOWN_FILE;
    } else if (uri_is_manpage_path(info->path)) {
        info->type = URI_KIND_MAN_FILE;
    } else {
        info->type = URI_KIND_FILE;
    }
    return true;
}

bool
uri_is_path(const uri_info *info)
{
    if (!info)
        return false;
    switch (info->type) {
        case URI_KIND_MARKDOWN_FILE:
        case URI_KIND_MAN_FILE:
        case URI_KIND_FILE:
            return true;
        default:
            return false;
    }
}
