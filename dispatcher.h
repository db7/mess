#ifndef MESS_DISPATCHER_H
#define MESS_DISPATCHER_H

void dispatcher_set_self_path(const char *path);
int dispatcher_open(const char *target);
int dispatcher_run(const char *target);

#endif
