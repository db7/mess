#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int log_fd_ = -1;

void
log_init(int fd)
{
    log_fd_ = fd;
}

static void
log_vprint_(const char *prefix, const char *fmt, va_list ap, int add_newline)
{
    if (log_fd_ < 0 || !fmt)
        return;
    char buf[1024];
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (written <= 0)
        return;
    size_t len = (size_t)written;
    if ((size_t)written >= sizeof(buf))
        len = sizeof(buf) - 1;
    if (prefix) {
        (void)write(log_fd_, prefix, strlen(prefix));
    }
    (void)write(log_fd_, buf, len);
    if (add_newline)
        (void)write(log_fd_, "\n", 1);
}

void
log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprint_(NULL, fmt, ap, 1);
    va_end(ap);
}

void
log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprint_("WARN: ", fmt, ap, 1);
    va_end(ap);
}
