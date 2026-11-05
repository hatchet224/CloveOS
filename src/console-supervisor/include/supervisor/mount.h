#ifndef SUPERVISOR_MOUNT_H
#define SUPERVISOR_MOUNT_H
#include <stdbool.h>
bool sc_mount_check(const char *path, const char *device, const char *uuid, const char *type,
                    bool read_only);
bool sc_runtime_check(void);
#endif
