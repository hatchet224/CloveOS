#ifndef SUPERVISOR_ZIP_H
#define SUPERVISOR_ZIP_H
#include <stdbool.h>
#include <stddef.h>
/* Read one exact trusted member into memory. Never extracts filesystem paths.
 * ZIP32 only; stored/deflate, bounded output and CRC, no encryption or volumes. */
bool sc_zip_read(const unsigned char *zip, size_t size, const char *member,
                    size_t limit, unsigned char **bytes, size_t *count);
typedef bool (*sc_zip_member)(void *context, const char *name, size_t expanded);
bool sc_zip_walk(const unsigned char *zip, size_t size, sc_zip_member visit, void *context);
#endif
