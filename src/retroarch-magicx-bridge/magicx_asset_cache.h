/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MAGICX_ASSET_CACHE_H
#define MAGICX_ASSET_CACHE_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#define MAGICX_ASSET_SLOTS 16U
struct magicx_asset_entry {
    char path[4096], digest[65];
    struct stat fingerprint;
    int pin;
    bool used;
};
struct magicx_asset_cache {
    struct magicx_asset_entry entries[MAGICX_ASSET_SLOTS];
    uint64_t bytes_hashed;
    bool initialized, pin_fat_session;
};
/* Active ROM/BIOS assets must remain immutable for this emulator process.
 * A first-use full hash is retained only while a held descriptor and path
 * still identify the same file. A changed fingerprint fails the session. */
void magicx_asset_cache_init(struct magicx_asset_cache *cache, bool pin_fat_session);
void magicx_asset_cache_close(struct magicx_asset_cache *cache);
bool magicx_asset_digest(struct magicx_asset_cache *cache, int fd, const char *path,
                        uint64_t limit, char digest[65]);
/* Provisioned large discs use a read-only, independently full-hashed index.
 * Sixteen distributed 64KiB samples replace a whole-disc read at each launch.
 * This detects ordinary replacement/damage, not every possible changed byte;
 * unindexed content retains the complete hash path. State artifacts always
 * retain full verification. */
bool magicx_content_digest(struct magicx_asset_cache *cache, int fd, const char *path,
                          uint64_t limit, char digest[65]);
#endif
