#ifndef LOG_H
#define LOG_H

void log_init(int fd);
void log_debug(const char *fmt, ...);
void log_warn(const char *fmt, ...);

#endif /* LOG_H */
