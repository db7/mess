#ifndef MESS_URI_H
#define MESS_URI_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define URI_MAN_NAME_MAX    64
#define URI_MAN_SECTION_MAX 16

typedef struct {
    char name[URI_MAN_NAME_MAX];
    char section[URI_MAN_SECTION_MAX];
} uri_man_topic;

typedef enum {
    URI_KIND_BROWSER = 0,
    URI_KIND_MAN_TOPIC,
    URI_KIND_MARKDOWN_FILE,
    URI_KIND_MAN_FILE,
    URI_KIND_FILE
} uri_kind;

typedef struct {
    uri_kind type;
    const char *raw;
    char path[PATH_MAX];
    uri_man_topic man;
} uri_info;

bool uri_is_markdown_path(const char *path);
bool uri_is_manpage_path(const char *path);
bool uri_is_http_url(const char *target);
bool uri_is_man_uri(const char *target);
bool uri_convert_file_uri(const char *uri, char *out, size_t out_sz);
bool uri_parse_man_uri(const char *uri, uri_man_topic *topic);
bool uri_canonicalize_raw_path(const char *path, char *out, size_t out_sz);
bool uri_is_path(const uri_info *info);
bool uri_parse(const char *target, uri_info *info);

#endif
