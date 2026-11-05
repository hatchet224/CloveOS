#ifndef CONSOLE_CATALOG_INDEX_H
#define CONSOLE_CATALOG_INDEX_H
#include <stdbool.h>
#include <sys/types.h>
/* Registration hint from a protected, build-time verified content index.
 * This does NOT authenticate the complete live ROM. The emulator must check
 * its full digest before HELLO/release, even when preflight uses this hint. */
bool sc_catalog_index_lookup(int content_fd, const char *path, off_t size,
                             const char *index_root, char digest[65]);
#endif
