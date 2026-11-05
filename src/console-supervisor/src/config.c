#include "supervisor/config.h"
#include "sessiond/disc_playlist.h"
#include "sessiond/retroarch_compat.h"
#include "supervisor/catalog_index.h"
#include "sessiond/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

int sc_open_absolute(const char *path, bool directory) {
    const char *p;
    int fd;
    if (path == NULL || path[0] != '/' || strlen(path) < 2U || strlen(path) > 4095U ||
        path[strlen(path) - 1U] == '/') {
        return -1;
    }
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    p = path + 1;
    while (fd >= 0 && *p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash == NULL ? strlen(p) : (size_t)(slash - p);
        char name[256];
        int next;
        if (n == 0U || n >= sizeof(name) || (n == 1U && *p == '.') ||
            (n == 2U && p[0] == '.' && p[1] == '.')) {
            (void)close(fd);
            return -1;
        }
        memcpy(name, p, n);
        name[n] = '\0';
        next = openat(fd, name,
                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                          ((slash != NULL || directory) ? O_DIRECTORY : 0));
        (void)close(fd);
        fd = next;
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
    }
    return fd;
}
bool sc_read_json(const char *path, struct cs_json *json) {
    char data[CS_JSON_MAX + 1U];
    struct stat st;
    size_t used = 0U;
    bool ok = true;
    int fd = sc_open_absolute(path, false);
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0022U) != 0U || st.st_size < 1 || st.st_size > (off_t)CS_JSON_MAX) {
        ok = false;
    }
    while (ok && used < sizeof(data)) {
        ssize_t n = read(fd, data + used, sizeof(data) - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    return ok && used <= CS_JSON_MAX && cs_json_parse(json, data, used);
}
static bool text(const struct cs_json *j, const char *key, char *out, size_t capacity, bool path) {
    const char *s = cs_json_string(j, key);
    if (s == NULL || strlen(s) >= capacity || (path && s[0] != '/')) {
        return false;
    }
    strcpy(out, s);
    return true;
}
static bool same_board(const struct cs_profile *a, const struct cs_profile *b) {
    return strcmp(a->card_path, b->card_path) == 0 && strcmp(a->card_uuid, b->card_uuid) == 0 &&
           strcmp(a->bridge, b->bridge) == 0 && strcmp(a->input_name, b->input_name) == 0 &&
           strcmp(a->input_path, b->input_path) == 0 && a->bridge_uid == b->bridge_uid &&
           a->key_code == b->key_code && a->max_state_bytes == b->max_state_bytes &&
           a->qualified_fat_syncfs == b->qualified_fat_syncfs;
}
static bool optional_keys(const struct cs_json *j, const char *const *required, size_t count,
                          const char *const *optional, size_t optional_count) {
    size_t i, k;
    if (j->count < count || j->count > count + optional_count)
        return false;
    for (i = 0U; i < count; ++i) {
        bool found = false;
        for (k = 0U; k < j->count; ++k)
            if (strcmp(j->fields[k].key, required[i]) == 0)
                found = true;
        if (!found)
            return false;
    }
    for (i = 0U; i < j->count; ++i) {
        bool known = false;
        for (k = 0U; k < count; ++k)
            if (strcmp(j->fields[i].key, required[k]) == 0)
                known = true;
        for (k = 0U; k < optional_count; ++k)
            if (strcmp(j->fields[i].key, optional[k]) == 0)
                known = true;
        if (!known)
            return false;
    }
    return true;
}
static bool ui_load(const char *path, struct sc_ui_init *ui) {
    struct cs_json j;
    uint64_t value;
    size_t i;
    static const char *const buttons[] = {"up",   "down",        "left",     "right", "a",
                                          "b",    "x",           "y",        "l1",    "r1",
                                          "menu", "volume_down", "volume_up", "start", "select"};
    static const char *const keys[] = {"guid",
                                       "hat",
                                       "up",
                                       "down",
                                       "left",
                                       "right",
                                       "a",
                                       "b",
                                       "x",
                                       "y",
                                       "l1",
                                       "r1",
                                       "menu",
                                       "volume_down",
                                       "volume_up",
                                       "display",
                                       "rotation",
                                       "portrait_rotation",
                                       "software_fallback",
                                       "input_verified"};
    static const char *const optional[] = {"theme_path", "theme_overrides", "start", "select"};
    if (!sc_read_json(path, &j) || !optional_keys(&j, keys, 20U, optional, 4U) ||
        !text(&j, "guid", ui->input.guid, 33U, false) || !cs_hex_valid(ui->input.guid, 32U)) {
        return false;
    }
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        bool present = false;
        for (size_t field = 0U; field < j.count; ++field)
            if (strcmp(j.fields[field].key, buttons[i]) == 0) present = true;
        if (i >= CL_START && !present) { ui->input.button[i] = -1; continue; }
        if (!cs_json_u64(&j, buttons[i], &value) || (value > 255U && value != 65535U)) {
            return false;
        }
        ui->input.button[i] = value == 65535U ? -1 : (int)value;
    }
    if (!cs_json_u64(&j, "hat", &value) || (value > 255U && value != 65535U)) {
        return false;
    }
    ui->input.hat = value == 65535U ? -1 : (int)value;
    if (!cs_json_u64(&j, "display", &value) || value > 15U) {
        return false;
    }
    ui->display = (int)value;
    if (!cs_json_u64(&j, "rotation", &value) ||
        (value != 0U && value != 90U && value != 180U && value != 270U && value != 999U)) {
        return false;
    }
    ui->rotation = value == 999U ? -1 : (int)value;
    if (!cs_json_u64(&j, "portrait_rotation", &value) || (value != 90U && value != 270U)) {
        return false;
    }
    ui->portrait_rotation = (int)value;
    (void)snprintf(ui->theme_path, sizeof(ui->theme_path),
                   "/system/share/console/themes/Clove");
    if (cs_json_string(&j, "theme_path") != NULL &&
        !text(&j, "theme_path", ui->theme_path, sizeof(ui->theme_path), true))
        return false;
    if (strcmp(ui->theme_path, "/system/share/console/themes/OnionMoss") == 0)
        strcpy(ui->theme_path, "/system/share/console/themes/Clove");
    if (cs_json_string(&j, "theme_overrides") != NULL &&
        !text(&j, "theme_overrides", ui->theme_overrides, sizeof(ui->theme_overrides), true))
        return false;
    return cs_json_bool(&j, "software_fallback", &ui->software_fallback) &&
           cs_json_bool(&j, "input_verified", &ui->input_verified);
}
static bool catalog_load(struct sc_config *c) {
    int fd = sc_open_absolute(c->catalog_path, true);
    DIR *directory;
    struct dirent *entry;
    struct stat st;
    bool ok = true;
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022U) != 0U) {
        (void)close(fd);
        return false;
    }
    directory = fdopendir(fd);
    if (directory == NULL) {
        (void)close(fd);
        return false;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct cs_json j;
        struct sc_game *game;
        char path[1024], profile[512];
        size_t length = strlen(entry->d_name), i;
        static const char *const keys[] = {"title",     "system",      "context", "content_path",
                                           "core_path", "config_path", "profile"};
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (length < 6U || strcmp(entry->d_name + length - 5U, ".json") != 0 ||
            c->game_count == SC_CATALOG_MAX) {
            ok = false;
            break;
        }
        (void)snprintf(path, sizeof(path), "%s/%s", c->catalog_path, entry->d_name);
        game = &c->games[c->game_count];
        if (!sc_read_json(path, &j) || !cs_json_keys(&j, keys, 7U) ||
            !text(&j, "title", game->entry.title, sizeof(game->entry.title), false) ||
            game->entry.title[0] == '\0' ||
            !text(&j, "system", game->entry.system, sizeof(game->entry.system), false) ||
            !text(&j, "context", game->entry.context, sizeof(game->entry.context), false) ||
            !text(&j, "content_path", game->content, sizeof(game->content), true) ||
            !text(&j, "core_path", game->core, sizeof(game->core), true) ||
            !text(&j, "config_path", game->config, sizeof(game->config), true) ||
            !text(&j, "profile", profile, sizeof(profile), true) || !sc_read_json(profile, &j) ||
            !cs_profile_load(profile, &game->profile) || !same_board(&c->base, &game->profile)) {
            ok = false;
            break;
        }
        game->entry.identity = game->profile.identity;
        game->entry.launch_registered = true;
        (void)snprintf(game->entry.content_path, sizeof(game->entry.content_path), "%s",
                       game->content);
        for (i = 0U; i < c->game_count; ++i) {
            if (strcmp(c->games[i].entry.identity.game, game->entry.identity.game) == 0) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        ++c->game_count;
    }
    if (closedir(directory) != 0) {
        ok = false;
    }
    return ok;
}
bool sc_config_load(const char *path, struct sc_config *c) {
    struct cs_json j, launch;
    char ui_path[512];
    uint64_t code, uid, maximum;
    bool fat;
    static const char *const keys[] = {"daemon",       "ui",
                                       "retroarch",    "base_profile",
                                       "ui_profile",   "bridge_launch",
                                       "catalog",      "control_socket",
                                       "system_path",  "poweroff",
                                       "level_helper", "menu_input_name",
                                       "menu_code",    "card_helper"};
    memset(c, 0, sizeof(*c));
    static const char *const optional[] = {"library_secondary", "library_secondary_identity",
                                           "gameswitcher"};
    if (!sc_read_json(path, &j) || !optional_keys(&j, keys, 14U, optional, 3U) ||
        !text(&j, "daemon", c->daemon, sizeof(c->daemon), true) ||
        !text(&j, "ui", c->ui, sizeof(c->ui), true) ||
        !text(&j, "retroarch", c->retroarch, sizeof(c->retroarch), true) ||
        !text(&j, "base_profile", c->base_profile, sizeof(c->base_profile), true) ||
        !text(&j, "ui_profile", ui_path, sizeof(ui_path), true) ||
        !text(&j, "bridge_launch", c->bridge_launch, sizeof(c->bridge_launch), true) ||
        !text(&j, "catalog", c->catalog_path, sizeof(c->catalog_path), true) ||
        !text(&j, "control_socket", c->socket, sizeof(c->socket), true) ||
        !text(&j, "system_path", c->system_path, sizeof(c->system_path), true) ||
        !text(&j, "poweroff", c->poweroff, sizeof(c->poweroff), true) ||
        !text(&j, "level_helper", c->level_helper, sizeof(c->level_helper), false) ||
        (c->level_helper[0] != '\0' && c->level_helper[0] != '/') ||
        !text(&j, "menu_input_name", c->menu_input_name, sizeof(c->menu_input_name), false) ||
        !text(&j, "card_helper", c->card_helper, sizeof(c->card_helper), true) ||
        !cs_json_u64(&j, "menu_code", &code) || code > 767U ||
        !sc_read_json(c->base_profile, &launch) || !cs_profile_load(c->base_profile, &c->base) ||
        c->base.bridge_uid != geteuid() || !ui_load(ui_path, &c->ui_config) ||
        !sc_read_json(c->bridge_launch, &launch)) {
        return false;
    }
    c->menu_code = (unsigned int)code;
    (void)snprintf(c->gameswitcher, sizeof(c->gameswitcher), "%s", c->ui);
    if (cs_json_string(&j, "gameswitcher") != NULL &&
        !text(&j, "gameswitcher", c->gameswitcher, sizeof(c->gameswitcher), true))
        return false;
    if (cs_json_string(&j, "library_secondary") != NULL &&
        !text(&j, "library_secondary", c->library_secondary, sizeof(c->library_secondary), false))
        return false;
    if (c->library_secondary[0] != '\0' && c->library_secondary[0] != '/')
        return false;
    if (cs_json_string(&j, "library_secondary_identity") != NULL &&
        !text(&j, "library_secondary_identity", c->library_secondary_identity,
              sizeof(c->library_secondary_identity), false))
        return false;
    if (c->library_secondary[0] != '\0' && c->library_secondary_identity[0] == '\0')
        return false;
    /* The endpoint description is pinned alongside the game profiles. */
    if (cs_json_string(&launch, "socket") == NULL ||
        strcmp(cs_json_string(&launch, "socket"), c->base.bridge) != 0 ||
        cs_json_string(&launch, "card_path") == NULL ||
        strcmp(cs_json_string(&launch, "card_path"), c->base.card_path) != 0 ||
        cs_json_string(&launch, "card_uuid") == NULL ||
        strcmp(cs_json_string(&launch, "card_uuid"), c->base.card_uuid) != 0 ||
        !cs_json_u64(&launch, "daemon_uid", &uid) || uid != (uint64_t)geteuid() ||
        !cs_json_u64(&launch, "max_state_bytes", &maximum) || maximum != c->base.max_state_bytes ||
        !cs_json_bool(&launch, "qualified_fat_syncfs", &fat) ||
        fat != c->base.qualified_fat_syncfs) {
        return false;
    }
    {
        int fd = sc_open_absolute(c->system_path, true);
        struct statvfs fs;
        bool safe = fd >= 0 && fstatvfs(fd, &fs) == 0 && (fs.f_flag & ST_RDONLY) != 0U;
        if (fd >= 0 && close(fd) != 0) {
            safe = false;
        }
        if (!safe || strncmp(c->socket, "/run/console/", 13U) != 0 ||
            strncmp(c->base.bridge, "/run/console/", 13U) != 0) {
            return false;
        }
    }
    return catalog_load(c);
}
const struct sc_game *sc_game_find(const struct sc_config *c, const char game[65]) {
    size_t i;
    if (strnlen(game, 65U) != 64U) {
        return NULL;
    }
    for (i = 0U; i < c->game_count; ++i) {
        if (strcmp(c->games[i].entry.identity.game, game) == 0) {
            return &c->games[i];
        }
    }
    return NULL;
}
static bool hash_file(const char *path, uint64_t limit, char digest[65]) {
    int fd = sc_open_absolute(path, false);
    struct stat before, after;
    struct cs_sha256 hash;
    unsigned char buffer[16384];
    uint64_t total = 0U;
    bool ok = true;
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > limit) {
        ok = false;
    }
    cs_sha256_init(&hash);
    while (ok) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        total += (uint64_t)n;
        if (total > limit) {
            ok = false;
            break;
        }
        cs_sha256_update(&hash, buffer, (size_t)n);
    }
    if (ok && (fstat(fd, &after) != 0 || before.st_size != after.st_size ||
               total != (uint64_t)before.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
               before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    cs_sha256_final(&hash, digest);
    return ok;
}
bool sc_hash_path(const char *path, const char expected[65], uint64_t limit) {
    char digest[65];
    return hash_file(path, limit, digest) && strcmp(digest, expected) == 0;
}
bool sc_retroarch_hash(const char *path, const char expected[65]) {
    char digest[65];
    if (!hash_file(path, UINT64_C(256) * 1024U * 1024U, digest)) return false;
    if (strcmp(path, "/system/bin/retroarch") == 0)
        cs_retroarch_checkpoint_hash(sc_open_absolute(CS_RETROARCH_COMPAT_PATH, false), digest);
    return strcmp(digest, expected) == 0;
}
static bool file_present(const char *path, uint64_t limit) {
    int fd = sc_open_absolute(path, false);
    struct stat st;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              (uint64_t)st.st_size <= limit;
    if (fd >= 0 && close(fd) != 0) ok = false;
    return ok;
}
bool sc_game_files_check(const struct sc_game *game, const char *retroarch, bool authenticate) {
    const uint64_t content_limit = UINT64_C(0xffffffff);
    const uint64_t core_limit = UINT64_C(1024) * 1024U * 1024U;
    const uint64_t ra_limit = UINT64_C(256) * 1024U * 1024U;
    if (game == NULL || retroarch == NULL) return false;
    if (!authenticate) {
        /* Selecting a row must not read a multi-gigabyte ISO, hash a core, or
         * monopolize the storage worker. Missing indexes are normal for games
         * added after flashing. Full validation remains in START below. */
        return file_present(game->content, content_limit) && file_present(game->core, core_limit) &&
               file_present(retroarch, ra_limit);
    }
    if (cs_disc_playlist_path(game->content)) {
        char digest[65];
        unsigned int discs;
        return strcmp(game->entry.system, "PS") == 0 &&
               strcmp(game->core, "/system/lib/libretro/pcsx_rearmed_libretro.so") == 0 &&
               cs_disc_ps_playlist_digest(game->content, digest, &discs) &&
               strcmp(digest, game->profile.identity.content) == 0 &&
               sc_hash_path(game->core, game->profile.identity.core, core_limit) &&
               sc_retroarch_hash(retroarch, game->profile.identity.retroarch);
    }
    bool indexed = false;
    int fd = sc_open_absolute(game->content, false);
    if (fd >= 0) {
        struct stat st; char digest[65];
        indexed = fstat(fd, &st) == 0 &&
                  sc_catalog_index_lookup(fd, game->content, st.st_size,
                                          "/etc/console/content-index", digest) &&
                  strcmp(digest, game->profile.identity.content) == 0;
        if (close(fd) != 0) indexed = false;
    }
    /* A protected registration index avoids a duplicate whole-disc read here.
     * The endpoint still hashes the complete loaded ROM before it can publish
     * its identity, complete HELLO, or release any game input. */
    return (indexed || sc_hash_path(game->content, game->profile.identity.content, content_limit)) &&
           sc_hash_path(game->core, game->profile.identity.core, core_limit) &&
           sc_retroarch_hash(retroarch, game->profile.identity.retroarch);
}
bool sc_identity_file(const char *path, const struct cs_identity *expected) {
    struct cs_json j;
    const char *const keys[] = {"game", "content",   "core", "config",
                                "bios", "retroarch", "disc", "policy"};
    const char *const values[] = {expected->game,   expected->content, expected->core,
                                  expected->config, expected->bios,    expected->retroarch,
                                  expected->disc,   expected->policy};
    size_t i;
    if (!sc_read_json(path, &j) || !cs_json_keys(&j, keys, 8U)) {
        return false;
    }
    for (i = 0U; i < 8U; ++i) {
        const char *value = cs_json_string(&j, keys[i]);
        if (value == NULL || strcmp(value, values[i]) != 0) {
            fprintf(stderr, "launch identity mismatch: field=%s expected=%s observed=%s\n",
                    keys[i], values[i], value == NULL ? "(missing)" : value);
            return false;
        }
    }
    return true;
}
bool sc_sync_path(const char *path) {
    int fd = sc_open_absolute(path, true), result;
    bool ok;
    if (fd < 0) {
        return false;
    }
    do {
        result = syncfs(fd);
    } while (result != 0 && errno == EINTR);
    ok = result == 0;
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}
bool sc_games_unmounted(const struct sc_config *config) {
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char line[8192];
    bool absent = true;
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *save = NULL, *token;
        unsigned int field;
        if (strchr(line, '\n') == NULL) {
            absent = false;
            break;
        }
        token = strtok_r(line, " ", &save);
        for (field = 0U; field < 4U && token != NULL; ++field) {
            token = strtok_r(NULL, " ", &save);
        }
        if (token != NULL) {
            char decoded[512];
            size_t i = 0U, j = 0U;
            while (token[i] != '\0' && j + 1U < sizeof(decoded)) {
                if (token[i] == '\\') {
                    if (strlen(token + i) < 4U || token[i + 1U] < '0' || token[i + 1U] > '7' ||
                        token[i + 2U] < '0' || token[i + 2U] > '7' || token[i + 3U] < '0' ||
                        token[i + 3U] > '7') {
                        absent = false;
                        break;
                    }
                    decoded[j++] = (char)((token[i + 1U] - '0') * 64 + (token[i + 2U] - '0') * 8 +
                                          token[i + 3U] - '0');
                    i += 4U;
                } else {
                    decoded[j++] = token[i++];
                }
            }
            decoded[j] = '\0';
            if (token[i] != '\0') {
                absent = false;
                break;
            }
            if (strcmp(decoded, config->base.card_path) == 0) {
                absent = false;
                break;
            }
        }
    }
    if (ferror(file) != 0) {
        absent = false;
    }
    if (fclose(file) != 0) {
        absent = false;
    }
    return absent;
}
