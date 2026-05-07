#include "uri.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Validate markdown extension detection across case variants.
static void
test_markdown_detection(void)
{
    assert(uri_is_markdown_path("README.md"));
    assert(uri_is_markdown_path("notes.MDTXT"));
    assert(!uri_is_markdown_path("plain.txt"));
    assert(!uri_is_markdown_path("noext"));
}

// Ensure man page suffix detection recognizes common patterns.
static void
test_manpage_detection(void)
{
    assert(uri_is_manpage_path("printf.3"));
    assert(uri_is_manpage_path("/usr/share/man/man1/ls.1"));
    assert(!uri_is_manpage_path("archive.tar.gz"));
    assert(!uri_is_manpage_path("noext"));
}

// Confirm HTTP/HTTPS URLs are identified while others are rejected.
static void
test_http_detection(void)
{
    assert(uri_is_http_url("http://example.com"));
    assert(uri_is_http_url("HTTPS://secure.example"));
    assert(!uri_is_http_url("ftp://example.com"));
    assert(!uri_is_http_url("file:///tmp/foo"));
}

// Exercise parsing of the custom man:// scheme.
static void
test_man_uri_detection(void)
{
    assert(uri_is_man_uri("man://printf.3"));
    assert(!uri_is_man_uri("MAN://invalid")); // case-sensitive scheme

    uri_man_topic topic;
    assert(uri_parse_man_uri("man://socket.2", &topic));
    assert(strcmp(topic.name, "socket") == 0);
    assert(strcmp(topic.section, "2") == 0);

    assert(!uri_parse_man_uri("man://invalid", &topic));
}

// Check that file:// URIs map to canonicalised real paths.
static void
test_file_uri_conversion(void)
{
    char buf[128];
    char template[] = "uri-test-XXXXXX";
    int fd          = mkstemp(template);
    assert(fd >= 0);
    close(fd);

    char abs_path[PATH_MAX];
    assert(realpath(template, abs_path) != NULL);

    char uri[PATH_MAX + 16];
    snprintf(uri, sizeof(uri), "file://%s", abs_path);
    assert(uri_convert_file_uri(uri, buf, sizeof(buf)));
    assert(strcmp(buf, abs_path) == 0);

    char host_uri[PATH_MAX + 32];
    snprintf(host_uri, sizeof(host_uri), "file://localhost%s", abs_path);
    assert(uri_convert_file_uri(host_uri, buf, sizeof(buf)));
    assert(strcmp(buf, abs_path) == 0);

    assert(unlink(template) == 0);
    assert(!uri_convert_file_uri("file://", buf, sizeof(buf)));
    assert(!uri_convert_file_uri("http://example.com", buf, sizeof(buf)));
}

// Verify uri_parse classifies various inputs correctly.
static void
test_uri_parse(void)
{
    uri_info info;
    assert(uri_parse("https://example", &info));
    assert(info.type == URI_KIND_BROWSER);

    assert(uri_parse("man://printf.3", &info));
    assert(info.type == URI_KIND_MAN_TOPIC);
    assert(strcmp(info.man.name, "printf") == 0);
    assert(strcmp(info.man.section, "3") == 0);

    char md_template[] = "uri-md-XXXXXX.md";
    int md_fd          = mkstemps(md_template, 3);
    assert(md_fd >= 0);
    close(md_fd);
    assert(uri_parse(md_template, &info));
    assert(info.type == URI_KIND_MARKDOWN_FILE);
    unlink(md_template);

    char man_template[] = "uri-man-XXXXXX.1";
    int man_fd          = mkstemps(man_template, 2);
    assert(man_fd >= 0);
    close(man_fd);
    assert(uri_parse(man_template, &info));
    assert(info.type == URI_KIND_MAN_FILE);
    unlink(man_template);
}

int
main(void)
{
    test_markdown_detection();
    test_manpage_detection();
    test_http_detection();
    test_man_uri_detection();
    test_file_uri_conversion();
    test_uri_parse();
    puts("uri tests OK");
    return 0;
}
