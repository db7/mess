#ifndef MESS_DISPATCHER_H
#define MESS_DISPATCHER_H

#include <stdbool.h>

void dispatcher_set_self_path(const char *path);
int dispatcher_run(const char *target);
bool dispatcher_command_is_self(const char *cmd);
const char *dispatcher_self_path(void);

#endif
