/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "supervisor/config.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned int core_hashes;
static uint64_t core_bytes;
static bool counted_hash(const char *path, const char expected[65], uint64_t limit);
#define sc_hash_path counted_hash
#define SC_CATALOG_LIBRARY
#ifdef BOOT_BASELINE
#include "../src/catalog-baseline.c"
#else
#include "../src/catalog.c"
#endif
#undef sc_hash_path
static bool counted_hash(const char *path, const char expected[65], uint64_t limit)
{
    if (strstr(path, "/system/lib/libretro/") == path) {
        struct stat info;
        assert(stat(path, &info) == 0 && info.st_size >= 0);
        ++core_hashes;
        core_bytes += (uint64_t)info.st_size;
    }
    return sc_hash_path(path, expected, limit);
}
int main(int argc, char **argv)
{
    struct timespec before, after;
    assert(argc == 2 && clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(sc_catalog_refresh() == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    struct sc_config *config = calloc(1U, sizeof(*config));
    assert(config != NULL && sc_config_load("/run/console/config/supervisor.json", config));
    assert(config->game_count == 14U);
    for (size_t i = 0U; i < config->game_count; ++i)
        printf("BOOT_ENTRY %s %s %s %s\n", config->games[i].entry.system,
               config->games[i].entry.identity.game, config->games[i].entry.identity.core,
               config->games[i].content);
    free(config);
#ifdef BOOT_BASELINE
    assert(core_hashes >= 8U);
#else
    assert(core_hashes == 0U && core_bytes == 0U);
#endif
    const int64_t elapsed = ((int64_t)after.tv_sec - (int64_t)before.tv_sec) * 1000 +
        ((int64_t)after.tv_nsec - (int64_t)before.tv_nsec) / 1000000;
    printf("BOOT_CATALOG_RESULT variant=%s elapsed_ms=%" PRId64 " core_hashes=%u core_bytes=%" PRIu64 "\n",
           argv[1], elapsed, core_hashes, core_bytes);
    return 0;
}
