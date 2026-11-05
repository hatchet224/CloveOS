#ifndef SUPERVISOR_ELF_DEPENDENCY_H
#define SUPERVISOR_ELF_DEPENDENCY_H
#include <stdbool.h>
#include <stddef.h>
/* Remove only the pinned Saturn build's unused DT_NEEDED libtmenu entry.
 * No executable instructions, offsets, relocations or symbols are changed.
 * Caller authenticates the entire input and resulting ELF with separate pins. */
bool sc_elf_remove_unused_tmenu(unsigned char *bytes, size_t size);
#endif
