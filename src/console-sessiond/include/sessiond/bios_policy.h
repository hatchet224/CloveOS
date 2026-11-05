#ifndef SESSIOND_BIOS_POLICY_H
#define SESSIOND_BIOS_POLICY_H
#include <stdbool.h>
#include <string.h>
/* Flycast runtime NVRAM and VMUs are writable saves, not BIOS firmware.
 * Exact relative names only. Callers still require regular files; no prefix,
 * extension, directory, symlink, BIOS template, or unknown file is exempt. */
static inline bool cs_bios_runtime_save(const char *relative) {
    if (strcmp(relative, "dc/dc_nvmem.bin") == 0) return true;
    return strlen(relative) == 18U && strncmp(relative, "dc/vmu_save_", 12U) == 0 &&
           relative[12] >= 'A' && relative[12] <= 'D' &&
           (relative[13] == '1' || relative[13] == '2') &&
           strcmp(relative + 14, ".bin") == 0;
}
/* MAME may create these directories at runtime even when no resources exist.
 * The inventory still verifies the directories and hashes every resource. */
static inline bool cs_bios_optional_container(const char *relative) {
    return strcmp(relative, "mame2003-plus/") == 0 ||
           strcmp(relative, "mame2003-plus/artwork/") == 0 ||
           strcmp(relative, "mame2003-plus/samples/") == 0;
}
#define CS_BIOS_HASH_PREFIX "magicx-bios-v2"
#endif
