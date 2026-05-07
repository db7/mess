#ifndef LOG_H
#define LOG_H

void log_init(int fd);
void log_debugf(const char *fmt, ...);
void log_debugln(const char *fmt, ...);

#endif /* LOG_H */
