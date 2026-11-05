#ifndef SUPERVISOR_CONFIG_H
#define SUPERVISOR_CONFIG_H
#include "sessiond/json.h"
#include "supervisor/wire.h"
#define SC_CATALOG_MAX 1024U
struct sc_game
{
    struct cl_entry entry;
    struct cs_profile profile;
    char content[512], core[512], config[512];
};
struct sc_config
{
    char daemon[512], ui[512], retroarch[512], base_profile[512], bridge_launch[512];
    char catalog_path[512], socket[108], system_path[512], poweroff[512], level_helper[512];
    char menu_input_name[256];
    char card_helper[512];
    char gameswitcher[512];
    char library_secondary[512], library_secondary_identity[129];
    unsigned int menu_code;
    struct cs_profile base;
    struct sc_ui_init ui_config;
    struct sc_game games[SC_CATALOG_MAX];
    size_t game_count;
};
bool sc_config_load(const char *path, struct sc_config *config);
int sc_catalog_refresh(void);
const struct sc_game *sc_game_find(const struct sc_config *config, const char game[65]);
int sc_open_absolute(const char *path, bool directory);
bool sc_read_json(const char *path, struct cs_json *json);
bool sc_hash_path(const char *path, const char expected[65], uint64_t limit);
/* Presentation checks never authorize execution. START uses authenticate=true
 * to verify registered content and hash core/executable before spawning. The
 * endpoint independently hashes the complete ROM before HELLO/release. */
bool sc_retroarch_hash(const char *path, const char expected[65]);
bool sc_game_files_check(const struct sc_game *game, const char *retroarch, bool authenticate);
bool sc_identity_file(const char *path, const struct cs_identity *expected);
bool sc_sync_path(const char *path);
bool sc_games_unmounted(const struct sc_config *config);
#endif
