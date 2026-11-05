#ifndef SESSIOND_LIBRARY_H
#define SESSIOND_LIBRARY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sessiond/disc_playlist.h"
#define CS_LIBRARY_MAX 1024U
#define CS_METADATA_MAX 512U
struct cs_metadata {
    char path[512], name[97], image[512];
};
struct cs_metadata_list {
    struct cs_metadata games[CS_METADATA_MAX];
    size_t count;
};
/* Local scraper imports only. No network access, external entities, shell
 * launchers or executable metadata. Failed parsing leaves output untouched. */
bool cs_metadata_parse_xml(const char *text, size_t bytes, struct cs_metadata_list *out);
bool cs_metadata_parse_json(const char *text, size_t bytes, struct cs_metadata_list *out);
/* Resolve Onion-relative paths (including Emu/GB/../../Roms/GB) inside a card.
 * Legacy /mnt/SDCARD paths are rebased to the selected card. Output is relative
 * to that card and contains no dot components. Nothing may escape its root. */
bool cs_card_resolve(const char *base, const char *path, char out[512]);
struct cs_library_card {
    const char *root, *identity;
    bool require_mount;
};
struct cs_rom {
    char id[65], title[97], system[33], content[512], artwork[512];
    unsigned int card;
    bool named_by_metadata;
};
struct cs_library {
    struct cs_rom games[CS_LIBRARY_MAX];
    size_t count;
    uint64_t revision;
    unsigned int missing_cards, malformed_metadata;
    bool truncated;
};
/* Blocking, bounded storage-worker operation. At most two cards, 64 system
 * configs per card, one ROM subfolder level. Builds a fresh snapshot; absent
 * cards and malformed individual configs do not crash or erase durable history. */
bool cs_library_scan(const struct cs_library_card *cards, size_t count, struct cs_library *out);
int cs_library_open_absolute(const char *path, bool directory);
#endif
