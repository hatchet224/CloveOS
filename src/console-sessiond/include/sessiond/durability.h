#ifndef SESSIOND_DURABILITY_H
#define SESSIOND_DURABILITY_H
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
/* FAT directory fsync may succeed without flushing the allocation table.
 * A qualified FAT publication therefore always includes a volume barrier. */
static inline int cs_flush_directory(int fd, bool fat, bool qualified)
{
    int result;
    do { result = fsync(fd); } while (result != 0 && errno == EINTR);
    if (!fat) return result;
    if (!qualified || (result != 0 && errno != EINVAL && errno != EOPNOTSUPP)) return -1;
    do { result = syncfs(fd); } while (result != 0 && errno == EINTR);
    return result;
}
#endif
