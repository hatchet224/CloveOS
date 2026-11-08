#include "supervisor/disk.h"
#include "launcher/preferences.h"
#include "sessiond/library.h"
#include "supervisor/diagnostics.h"
#include "supervisor/tools_backend.h"
#include "supervisor/battery_monitor.h"
#include "supervisor/core_choice.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Private queue metadata must not reuse fields in the UI request. In
 * particular, SCRAPE_MISSING.revision is its next-game cursor. */
struct queued_message {
    struct sc_message message; /* first member: existing queue/free ownership */
    uint64_t cancel_epoch;
};
static uint64_t queued_cancel_epoch(const struct sc_message *request)
{ return ((const struct queued_message *)request)->cancel_epoch; }

static enum cs_store_status reader(const struct sc_config *c, struct cs_store *store)
{
    struct cs_store_config config = {0};
    config.card_path = c->base.card_path;
    config.card_uuid = c->base.card_uuid;
    config.max_state_bytes = c->base.max_state_bytes;
    config.qualified_fat_syncfs = c->base.qualified_fat_syncfs;
    return cs_store_open_reader(store, &config);
}
static void error(struct sc_disk_result *r, const char *text)
{
    uint64_t token = r->message.token;
    sc_message_init(&r->message, SC_UI_ERROR, token);
    (void)snprintf(r->message.data.text, sizeof(r->message.data.text), "%s", text);
}
static bool telemetry_text(const char *path, char text[32])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    ssize_t n;
    if (fd < 0) return false;
    do
    {
        n = read(fd, text, 31U);
    } while (n < 0 && errno == EINTR);
    if (close(fd) != 0 || n <= 0 || n >= 31) return false;
    text[n] = '\0';
    text[strcspn(text, "\r\n")] = '\0';
    return true;
}
static int telemetry(const char *path, int maximum)
{
    char text[32];
    uint64_t value;
    return telemetry_text(path, text) && cs_decimal(text, &value) && value <= (uint64_t)maximum
        ? (int)value : -1;
}
static bool battery_charging(const char *status_path, int usb_online)
{
    char status[32];
    /* USB ONLINE describes the supply, not the direction of battery current.
     * Never show the charging bolt for an unknown or discharging battery. */
    return usb_online == 1 && telemetry_text(status_path, status) && strcmp(status, "Charging") == 0;
}
static const char *alias_core(const char *system, uint64_t *limit)
{
    static const struct
    {
        const char *system, *core;
        uint64_t mib;
    } systems[] = {{"GB", "gambatte", 8U},       {"GBC", "gambatte", 8U},
                   {"GBA", "mgba", 32U},         {"FC", "fceumm", 32U},
                   {"SFC", "snes9x2005_plus", 16U}, {"MD", "picodrive", 16U},
                   {"MS", "picodrive", 4U}, {"GG", "picodrive", 4U},
                   {"PCE", "mednafen_pce_fast", 4095U}, {"PICO", "fake08", 8U},
                   {"ARCADE", "fbneo", 512U}, {"FBNEO", "fbneo", 512U}, {"NEOGEO", "fbneo", 512U},
                   {"MAME2003PLUS", "mame2003_plus", 512U}, {"ATARI", "stella2014", 1U}};
    for (size_t i = 0U; i < sizeof(systems) / sizeof(systems[0]); ++i)
        if (strcmp(system, systems[i].system) == 0)
        {
            *limit = systems[i].mib * 1024U * 1024U;
            return systems[i].core;
        }
    return NULL;
}
/* Alias qualification is restricted to already enrolled, qualified core families.
 * Hash once, bound both the file and total scan work, then reuse catalog identity. */
static bool alias_content(const struct cs_rom *rom, char digest[65], uint64_t *budget)
{
    int fd;
    struct stat st, after;
    unsigned char buffer[32768];
    uint64_t used = 0U;
    struct cs_sha256 hash;
    bool ok = true;
    uint64_t limit = 0U;
    if (alias_core(rom->system, &limit) == NULL || *budget >= UINT64_C(128) * 1024U * 1024U)
    {
        return false;
    }
    fd = sc_open_absolute(rom->content, false);
    if (fd < 0)
    {
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1 ||
        (uint64_t)st.st_size > limit ||
        (uint64_t)st.st_size > UINT64_C(128) * 1024U * 1024U - *budget)
    {
        (void)close(fd);
        return false;
    }
    cs_sha256_init(&hash);
    for (;;)
    {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0)
        {
            ok = false;
            break;
        }
        if (n == 0)
        {
            break;
        }
        used += (uint64_t)n;
        if (used > limit)
        {
            ok = false;
            break;
        }
        cs_sha256_update(&hash, buffer, (size_t)n);
    }
    *budget += used;
    if (fstat(fd, &after) != 0 || st.st_dev != after.st_dev || st.st_ino != after.st_ino ||
        st.st_size != after.st_size || used != (uint64_t)st.st_size ||
        st.st_mtim.tv_sec != after.st_mtim.tv_sec || st.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    {
        ok = false;
    }
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (ok)
    {
        cs_sha256_final(&hash, digest);
    }
    return ok;
}
static const char *rom_relative(const struct sc_config *c, const char *content)
{
    size_t n = strlen(c->base.card_path);
    if (strncmp(content, c->base.card_path, n) == 0 && strncmp(content + n, "/Roms/", 6U) == 0)
        return content + n + 6U;
    if (c->library_secondary[0] != '\0')
    {
        n = strlen(c->library_secondary);
        if (strncmp(content, c->library_secondary, n) == 0 &&
            strncmp(content + n, "/Roms/", 6U) == 0)
            return content + n + 6U;
    }
    return NULL;
}
static void decorate_checkpoint(struct cs_store *store, struct cl_entry *entry)
{
    struct cs_record record;
    enum cs_store_status status;
    int n;
    entry->screenshot_path[0] = '\0';
    if (entry->latest.generation == 0U)
        return;
    status = cs_store_read_checkpoint(store, &entry->identity, entry->latest.generation,
                                      entry->latest.manifest, &record);
    entry->state_status =
        status == CS_STORE_OK
            ? CL_READY
            : (status == CS_STORE_MEDIA || status == CS_STORE_EMPTY
                   ? CL_MISSING
                   : (status == CS_STORE_INCOMPATIBLE ? CL_INCOMPATIBLE : CL_DAMAGED));
    if (status != CS_STORE_OK || record.schema != 2U)
        return;
    n = snprintf(entry->screenshot_path, sizeof(entry->screenshot_path),
                 "%s/.console/sessions/%s/g-%020" PRIu64 "/screenshot.png", store->card_path,
                 record.identity.game, record.generation);
    if (n < 0 || (size_t)n >= sizeof(entry->screenshot_path))
        entry->screenshot_path[0] = '\0';
}
struct library_cache {
    struct cl_entry *entries;
    size_t count;
    uint64_t revision, enabled, mount_id;
    dev_t device;
    ino_t inode;
    bool valid, truncated;
    unsigned int malformed_metadata;
    uint64_t battery_try_ms;
    enum cl_battery_power battery_power;
};
static int favorite_title_order(const void *left,const void *right)
{
    const struct cl_entry *a=left,*b=right;
    int comparison=strcasecmp(a->title,b->title);
    return comparison!=0?comparison:strcmp(a->identity.game,b->identity.game);
}
static void library_action(struct cl_entry *entry, enum cl_library_action action, const char *label)
{
    memset(entry, 0, sizeof(*entry));
    entry->library_action = action;
    (void)snprintf(entry->title, sizeof(entry->title), "%s", label);
    /* The anchor is for navigation only. This row never enters launch preflight. */
    (void)snprintf(entry->identity.game, sizeof(entry->identity.game), "%064x", (unsigned int)action);
}
static bool library_snapshot(const struct sc_config *c, struct cl_history *out,
                             const struct sp_package_selection *selection,
                             const struct cl_history *query, struct library_cache *cache,
                             const struct cs_store *store)
{
    struct cs_library *library = calloc(1U, sizeof(*library));
    struct cl_entry *entries = calloc(CL_LIBRARY_UI_BOUND, sizeof(*entries));
    size_t entry_count = 0U;
    uint64_t stride = query->library_page_size == 0U ? CL_HISTORY_MAX : query->library_page_size;
    struct cs_library_card cards[2] = {{c->base.card_path, c->base.card_uuid, true},
                                       {c->library_secondary, c->library_secondary_identity, true}};
    size_t count = c->library_secondary[0] == '\0' ? 1U : 2U, i, j;
    uint64_t alias_budget = 0U;
    if (library == NULL || entries == NULL || query->count > CL_HISTORY_MAX ||
        (unsigned int)query->library_mode > CL_LIBRARY_FAVORITES ||
        strnlen(query->library_system, sizeof(query->library_system)) ==
            sizeof(query->library_system) ||
        strnlen(query->library_anchor, sizeof(query->library_anchor)) ==
            sizeof(query->library_anchor) ||
        strnlen(query->library_keyword,sizeof(query->library_keyword))==sizeof(query->library_keyword) ||
        query->library_offset > CL_LIBRARY_UI_BOUND || stride > CL_HISTORY_MAX)
    {
        free(library);
        free(entries);
        return false;
    }
    if (query->library_mode == CL_LIBRARY_FAVORITES)
        for (i = 0U; i < query->count; ++i)
            if (strnlen(query->entries[i].identity.game, 65U) != 64U ||
                !cs_hex_valid(query->entries[i].identity.game, 64U))
            {
                free(library);
                free(entries);
                return false;
            }
    bool cached = cache->valid && cache->entries != NULL && c->library_secondary[0] == '\0' &&
                  query->revision != UINT64_MAX && cache->enabled == selection->enabled &&
                  cache->device == store->device && cache->inode == store->inode &&
                  cache->mount_id == store->mount_id;
    memset(out, 0, sizeof(*out));
    if (cached) {
        memcpy(entries, cache->entries, cache->count * sizeof(*entries));
        entry_count = cache->count;
        library->revision = cache->revision;
        library->truncated = cache->truncated;
        library->malformed_metadata = cache->malformed_metadata;
        goto paginate;
    }
    if (!cs_library_scan(cards, count, library))
    {
        free(library);
        free(entries);
        return false;
    }
    /* Discovery and deduplication cover the whole bounded card scan. The IPC
     * frame contains only the explicitly requested 100-row window. */
    for (i = 0U; i < library->count && entry_count < CS_LIBRARY_MAX; ++i)
    {
        const struct cs_rom *rom = &library->games[i];
        struct cl_entry *entry = &entries[entry_count];
        const struct sc_game *registered = NULL;
        if (!sc_tools_system_visible(selection, rom->system))
            continue;
        for (j = 0U; j < c->game_count; ++j)
        {
            if (strcmp(c->games[j].content, rom->content) == 0)
            {
                registered = &c->games[j];
                break;
            }
        }
        if (registered == NULL)
        {
            char digest[65];
            if (alias_content(rom, digest, &alias_budget))
            {
                char core_path[128];
                uint64_t limit;
                const char *core = alias_core(rom->system, &limit);
                (void)snprintf(core_path, sizeof(core_path), "/system/lib/libretro/%s_libretro.so",
                               core);
                for (j = 0U; j < c->game_count; ++j)
                {
                    if (strcmp(c->games[j].core, core_path) == 0 &&
                        strcmp(c->games[j].profile.identity.content, digest) == 0)
                    {
                        registered = &c->games[j];
                        break;
                    }
                }
            }
        }
        if (registered != NULL)
        {
            bool duplicate = false;
            for (j = 0U; j < entry_count; ++j)
            {
                if (strcmp(entries[j].identity.game, registered->entry.identity.game) == 0)
                {
                    duplicate = true;
                }
            }
            if (duplicate)
            {
                continue;
            }
            *entry = registered->entry;
            if (rom->named_by_metadata) strcpy(entry->title, rom->title);
        }
        else
        {
            /* All-zero qualified fields mean UNENROLLED, never a measured hash.
             * The presentation key cannot pass sc_game_find/select_launch. */
            char zero[65];
            memset(zero, '0', 64U);
            zero[64] = '\0';
            strcpy(entry->identity.game, rom->id);
            strcpy(entry->identity.content, zero);
            strcpy(entry->identity.core, zero);
            strcpy(entry->identity.config, zero);
            strcpy(entry->identity.bios, zero);
            strcpy(entry->identity.retroarch, zero);
            strcpy(entry->identity.disc, zero);
            strcpy(entry->identity.policy, zero);
            entry->game_status = CL_INCOMPATIBLE;
        }
        ++entry_count;
        entry->discovered = true;
        entry->launch_registered = registered != NULL;
        if (registered == NULL)
        {
            strcpy(entry->title, rom->title);
            strcpy(entry->system, rom->system);
            strcpy(entry->content_path, rom->content);
        }
        strcpy(entry->artwork_path, rom->artwork);
        (void)snprintf(entry->context, sizeof(entry->context), "TF%u%s", rom->card + 1U,
                       registered == NULL ? " - CORE / PROFILE SETUP NEEDED" : "");
    }
    /* The separately validated catalog is authoritative for execution. Do not
     * lose a qualified flat game merely because raw presentation discovery hit
     * its smaller window. Never expose hidden managed-disc children here.
     * All arrays retain their existing capacities; unregistered rows yield to
     * authenticated catalog entries when the presentation array is full. */
    for (i = 0U; i < c->game_count; ++i) {
        const struct sc_game *game = &c->games[i];
        size_t root = strlen(c->base.card_path), slot;
        const char *folder, *file;
        if (!sc_tools_system_visible(selection, game->entry.system) ||
            strncmp(game->content, c->base.card_path, root) != 0 ||
            strncmp(game->content + root, "/Roms/", 6U) != 0) continue;
        folder = game->content + root + 6U;file = strchr(folder, '/');
        if (file == NULL || file[1] == '.' || strchr(file + 1U, '/') != NULL) continue;
        for (slot = 0U; slot < entry_count; ++slot)
            if (strcmp(entries[slot].identity.game, game->entry.identity.game) == 0) break;
        if (slot < entry_count || !sc_game_files_check(game, c->retroarch, false)) continue;
        if (entry_count < CS_LIBRARY_MAX) slot = entry_count++;
        else {
            for (slot = entry_count; slot > 0U; --slot)
                if (!entries[slot - 1U].launch_registered) break;
            if (slot == 0U) continue;
            --slot;
        }
        entries[slot] = game->entry;
        entries[slot].discovered = true;entries[slot].launch_registered = true;
        (void)snprintf(entries[slot].context, sizeof(entries[slot].context), "TF1");
    }
    if (c->library_secondary[0] == '\0') {
        if (cache->entries == NULL) cache->entries = calloc(CS_LIBRARY_MAX, sizeof(*entries));
        if (cache->entries != NULL) {
            memcpy(cache->entries, entries, entry_count * sizeof(*entries));
            cache->count = entry_count; cache->revision = library->revision;
            cache->truncated = library->truncated;
            cache->malformed_metadata = library->malformed_metadata;
            cache->enabled = selection->enabled; cache->device = store->device;
            cache->inode = store->inode; cache->mount_id = store->mount_id;
            cache->valid = true;
        }
    }
paginate:
    out->revision = library->revision;
    out->app_features_known = true;
    out->app_features = sc_tools_app_features(selection);
    if (query->library_expert && query->library_mode != CL_LIBRARY_FAVORITES) {
        size_t kept = 0U;
        for (i = 0U; i < entry_count; ++i)
            if (entries[i].launch_registered && sc_tools_system_expert(entries[i].system))
                entries[kept++] = entries[i];
        entry_count = kept;
    }
    if (query->library_mode != CL_LIBRARY_FAVORITES)
        entry_count = cl_group_discs(entries, entry_count, query->library_anchor);
    if (query->library_mode == CL_LIBRARY_SYSTEMS)
    {
        size_t counts[CS_LIBRARY_MAX] = {0}, kept = 0U;
        for (i = 0U; i < entry_count; ++i)
        {
            for (j = 0U; j < kept; ++j)
                if (strcmp(entries[j].system, entries[i].system) == 0)
                    break;
            if (j == kept)
            {
                entries[kept++] = entries[i];
            }
            ++counts[j];
        }
        entry_count = kept;
        for (i = 0U; i < entry_count; ++i)
        {
            strcpy(entries[i].title, entries[i].system);
            entries[i].artwork_path[0] = '\0';
            (void)snprintf(entries[i].context, sizeof(entries[i].context), "%zu games", counts[i]);
        }
        if (!query->library_expert && query->library_search_tile && (out->app_features & CL_APP_SEARCH) != 0U)
            library_action(&entries[entry_count++], CL_LIBRARY_SEARCH_RESULTS, "Search");
        if (!query->library_expert && (out->app_features & CL_APP_PORTS) != 0U)
            library_action(&entries[entry_count++],CL_LIBRARY_PORTS,"Ports");
    }
    else
    {
        size_t kept = 0U;
        for (i = 0U; i < entry_count; ++i)
        {
            bool match = query->library_mode == CL_LIBRARY_GAMES &&
                         (query->library_system[0] == '\0' ||
                          strcmp(query->library_system, entries[i].system) == 0);
            if (query->library_mode == CL_LIBRARY_FAVORITES)
                for (j = 0U; j < query->count; ++j)
                {
                    if (strcmp(query->entries[j].identity.game, entries[i].identity.game) == 0)
                        match = true;
                }
            if (match && cl_title_matches(entries[i].title,query->library_keyword))
                entries[kept++] = entries[i];
        }
        entry_count = kept;
        if (query->library_mode == CL_LIBRARY_GAMES && query->library_search_results &&
            (out->app_features & CL_APP_SEARCH) != 0U) {
            library_action(&entries[entry_count++], CL_LIBRARY_CLEAR_SEARCH, "~Clear search");
        } else if (query->library_mode == CL_LIBRARY_GAMES && query->library_system[0] != '\0' &&
                   (out->app_features & CL_APP_LIST_SHORTCUTS) != 0U) {
            char filter[96];
            if (query->library_keyword[0] != '\0') {
                library_action(&entries[entry_count++], CL_LIBRARY_CLEAR_FILTER, "~Clear filter");
                (void)snprintf(filter, sizeof(filter), "~Filter: %s", query->library_keyword);
            } else strcpy(filter, "~Filter");
            library_action(&entries[entry_count++], CL_LIBRARY_FILTER, filter);
            library_action(&entries[entry_count++], CL_LIBRARY_REFRESH, "~Refresh roms");
        }
    }
    if(query->library_mode==CL_LIBRARY_FAVORITES &&
       (cl_preferences_settings(store)&CL_FAVORITES_SORTED)!=0U)
        qsort(entries,entry_count,sizeof(*entries),favorite_title_order);
    out->library_mode = query->library_mode;
    out->library_expert = query->library_expert && query->library_mode != CL_LIBRARY_FAVORITES;
    out->library_search_tile = query->library_search_tile;
    out->library_hide_portmaster = query->library_hide_portmaster;
    out->library_search_results = query->library_search_results;
    strcpy(out->library_system, query->library_system);
    strcpy(out->library_keyword, query->library_keyword);
    out->library_query_id = query->library_query_id;
    out->library_page_size = stride;
    out->library_total = entry_count;
    out->library_truncated = library->truncated;
    out->library_offset = query->library_offset;
    if (query->library_anchor[0] != '\0')
        for (i = 0U; i < entry_count; ++i)
            if (strcmp(query->library_anchor, entries[i].identity.game) == 0)
            {
                out->library_offset = (i / stride) * stride;
                break;
            }
    if (entry_count == 0U)
        out->library_offset = 0U;
    else if (out->library_offset >= entry_count)
        out->library_offset = ((entry_count - 1U) / stride) * stride;
    out->count = entry_count - (size_t)out->library_offset;
    if (out->count > stride)
        out->count = (size_t)stride;
    if (out->count != 0U)
        memcpy(out->entries, entries + (size_t)out->library_offset,
               out->count * sizeof(out->entries[0]));
    out->library_more = out->library_offset + out->count < entry_count;
    if (library->truncated)
        fprintf(
            stderr,
            "library: scan bound %u reached; additional ROMs require narrowing the card library\n",
            CS_LIBRARY_MAX);
    if (library->malformed_metadata != 0U)
    {
        fprintf(stderr, "library: ignored %u malformed metadata/config files\n",
                library->malformed_metadata);
    }
    free(library);
    free(entries);
    return true;
}
static void job(const struct sc_config *c, struct sc_tools_backend *tools,
                const struct sc_message *request, struct sc_disk_result *r,
                struct library_cache *cache)
{
    struct cs_store store;
    struct cl_history *history;
    enum cs_store_status pointers[2];
    enum cl_history_result result;
    r->message.token = request->token;
    if (request->type == SC_UI_STATUS)
    {
        int usb=telemetry("/sys/class/power_supply/axp2202-usb/online",1);
        sc_message_init(&r->message, SC_UI_STATUS, request->token);
        r->external_supply=usb!=0; /* Unknown input power cannot authorize shutdown. */
        r->message.data.telemetry.battery =
            telemetry("/sys/class/power_supply/axp2202-battery/capacity", 100);
        r->message.data.telemetry.usb = battery_charging(
            "/sys/class/power_supply/axp2202-battery/status",
            usb);
        r->message.data.telemetry.brightness = (uint8_t)(1 + telemetry("/run/console/brightness-level", 10));
        r->message.data.telemetry.volume = (uint8_t)(1 + telemetry("/run/console/volume-level", 20));
        cs_store_init(&store);
        uint64_t now_ms;
        int battery = r->message.data.telemetry.battery;
        enum cl_battery_power power = usb < 0 ? CL_POWER_UNKNOWN : usb == 0 ? CL_ON_BATTERY :
            r->message.data.telemetry.usb ? CL_CHARGING : CL_EXTERNAL_POWER;
        bool record = request->data.telemetry.record_history && battery >= 0 && sc_now(&now_ms) &&
            (cache->battery_try_ms == 0U || now_ms - cache->battery_try_ms >= 300000U ||
             (cache->battery_power != power && now_ms - cache->battery_try_ms >= 15000U));
        if (record) {
            cache->battery_try_ms = now_ms;
            cache->battery_power = power;
        }
        if ((request->data.telemetry.busy || record) && reader(c,&store)==CS_STORE_OK) {
            if (request->data.telemetry.busy) {
                r->settings=cl_preferences_runtime(&store, &r->shortcuts);
                r->settings_loaded=true;
            }
            time_t wall = record ? time(NULL) : 0;
            if (wall >= 1577836800 && (uint64_t)wall <= UINT32_MAX)
                (void)sc_battery_history_record(&store, (uint32_t)wall, battery, power);
        }
        cs_store_close(&store);
        r->ok = true;
        return;
    }
    cs_store_init(&store);
    if (reader(c, &store) != CS_STORE_OK)
    {
        if (request->type == SC_UI_LIBRARY)
        {
            error(r, "Game card unavailable. Previous library and checkpoints retained.");
            cs_store_close(&store);
            return;
        }
        if (request->type == SC_UI_TOOL)
        {
            sc_message_init(&r->message, SC_UI_TOOLS, request->token);
            r->message.data.tools.kind = cl_tool_request_kind(&request->data.tool_request);
            r->message.data.tools.error = true;
            strcpy(r->message.data.tools.status,
                   "Game card unavailable; settings and activity were retained.");
            r->ok = true;
        }
        else if (request->type == SC_DIAGNOSTICS)
        {
            sc_message_init(&r->message, SC_DIAGNOSTICS, request->token);
        }
        else if (request->type == SC_UI_INPUT_VERIFIED)
        {
            r->verification_write = true;
            sc_message_init(&r->message, SC_UI_INPUT_READY, request->token);
            r->message.data.result = CS_RESULT_FAILED;
        }
        else
        {
            error(r, "Game card/store unavailable. Previous list and checkpoints are retained.");
        }
        cs_store_close(&store);
        return;
    }
    /* Browsing never authorizes LOAD. Avoid hashing every state artifact for
     * each row, history pointer and manual slot. The daemon uses its separate
     * writable store and fully verifies the selected generation before restore. */
    store.presentation_only = true;
    if (request->type == SC_UI_TOOL)
    {
        sc_message_init(&r->message, SC_UI_TOOLS, request->token);
        sc_tools_request(tools, c, &store, &request->data.tool_request, &r->message.data.tools);
        r->ok = true;
        cs_store_close(&store);
        return;
    }
    if (request->type == SC_UI_LIBRARY)
    {
        struct sp_package_selection selection;
        sc_message_init(&r->message, SC_UI_LIBRARY, request->token);
        r->ok = sc_tools_packages(tools, &store, &selection) &&
                library_snapshot(c, &r->message.data.history, &selection, &request->data.history,
                                 cache, &store);
        if (!r->ok)
            error(r, "Library/package selection could not be verified. Previous list retained.");
        cs_store_close(&store);
        return;
    }
    if (request->type == SC_DIAGNOSTICS)
    {
        r->ok = sc_diagnostics_save(&store);
        sc_message_init(&r->message, SC_DIAGNOSTICS, request->token);
        cs_store_close(&store);
        return;
    }
    if(request->type==SC_REMOVE || request->type==SC_DELETE_STATES){
        r->native_maintenance=true;r->message=*request;
        r->ok=cs_hex_valid(request->data.text,64U) && sc_native_maintenance(c,request->data.text,request->type==SC_DELETE_STATES);
        cs_store_close(&store);return;
    }
    if (request->type == SC_UI_INPUT_VERIFIED)
    {
        enum cs_store_status saved = cl_input_record_save(&store, &request->data.verification);
        int save_errno = saved == CS_STORE_OK ? 0 : errno;
        r->verification_write = true;
        r->verification = request->data.verification;
        r->ok = saved == CS_STORE_OK && cl_input_runtime_publish(&r->verification);
        if (!r->ok)
            fprintf(stderr, "controller persistence: record_status=%u save_errno=%d "
                            "runtime_errno=%d; launch remains withheld\n",
                    (unsigned int)saved, save_errno, saved == CS_STORE_OK ? errno : 0);
        sc_message_init(&r->message, SC_UI_INPUT_READY, request->token);
        r->message.data.result = r->ok ? CS_RESULT_COMPLETED : CS_RESULT_FAILED;
        cs_store_close(&store);
        return;
    }
    bool automatic_resume = false;
    if (request->type == SC_QUERY)
    {
        r->settings=cl_preferences_runtime(&store, &r->shortcuts);
        r->settings_loaded=true;
        automatic_resume=(r->settings&CL_DISABLE_AUTO_RESUME)==0U;
        enum cs_store_status loaded =
            cl_input_record_load(&store, &c->ui_config.input, &r->verification);
        r->verification_loaded =
            loaded == CS_STORE_OK && cl_input_runtime_publish(&r->verification);
        fprintf(stderr, "controller verification: %s (record status=%u)\n",
                r->verification_loaded ? "restored exact GUID/baseline" : "physical check required",
                (unsigned int)loaded);
        (void)sc_tools_selected_icons(c, &store, r->selected_icons, r->icon_theme);
        if (!sc_tools_selected_theme(c, &store, r->selected_theme))
            fprintf(stderr, "theme selection unavailable; configured default retained\n");
    }
    history = calloc(1U, sizeof(*history));
    if (history == NULL)
    {
        error(r, "Catalog allocation failed.");
        cs_store_close(&store);
        return;
    }
    result = cl_history_read_catalog(&store, history, pointers);
    if (result != CL_HISTORY_OK && result != CL_HISTORY_EMPTY)
    {
        if (request->type == SC_QUERY)
        {
            if (automatic_resume &&
                cs_store_read_resume(&store, false, &r->resume) == CS_STORE_OK && !r->resume.menu)
            {
                r->automatic_candidate = true; /* secondary history cannot veto a valid resume */
            }
            error(r, "History failed validation; committed resume is validated independently.");
            goto done;
        }
        if (request->type == SC_UI_HISTORY)
        {
            error(r, "History failed validation. It was not replaced with an empty list.");
            goto done;
        }
        /* Library/state inspection does not replace corrupt recents with empty history. */
    }
    if (request->type == SC_UI_HISTORY || request->type == SC_QUERY)
    {
        sc_message_init(&r->message, SC_UI_HISTORY, request->token);
        sc_native_history(c,history);
        r->message.data.history = *history;
        const char *paths[CL_HISTORY_MAX] = {0};
        uint64_t totals[CL_HISTORY_MAX] = {0};
        for (size_t i = 0U; i < r->message.data.history.count; ++i)
        {
            struct cl_entry *entry = &r->message.data.history.entries[i];
            const struct sc_game *game = sc_game_find(c, entry->identity.game);
            if (game != NULL && cs_identity_equal(&game->profile.identity, &entry->identity))
            {
                strcpy(entry->content_path, game->content);
                strcpy(entry->artwork_path, game->entry.artwork_path);
                entry->launch_registered = true;
                paths[i] = rom_relative(c, game->content);
            }
            decorate_checkpoint(&store, entry);
        }
        if (r->message.data.history.count != 0U)
        {
            enum ca_result activity =
                ca_totals_read(&store, paths, r->message.data.history.count, totals);
            if (activity != CA_OK && activity != CA_EMPTY)
                fprintf(stderr, "Recent activity overlay unavailable: %s\n",
                        ca_result_name(activity));
            for (size_t i = 0U; i < r->message.data.history.count; ++i)
                r->message.data.history.entries[i].total_play_seconds = totals[i];
        }
        if (automatic_resume && pointers[0] == CS_STORE_OK &&
            cs_store_read_resume(&store, false, &r->resume) == CS_STORE_OK && !r->resume.menu)
        {
            r->automatic_candidate = true;
        }
        if(automatic_resume && sc_native_current(c,&r->native_resume) && !r->native_resume.menu)r->native_candidate=true;
        r->ok = true;
    }
    else if (request->type == SC_UI_PROBE)
    {
        if (request->token == UINT64_MAX) {
            /* The launcher drains preference writes before handing off. Use
             * this preflight snapshot even when the last UI poll was stale. */
            r->settings = cl_preferences_runtime(&store, &r->shortcuts);
            r->settings_loaded = true;
        }
        struct sc_game effective;
        const struct sc_game *game = sc_core_resolve(c,request->data.probe.entry.identity.game,&effective) ? &effective : NULL;
        struct cl_entry entry = request->data.probe.entry;
        struct cl_state_snapshot *states;
        size_t i;
        sc_message_init(&r->message, SC_UI_STATES, request->token);
        states = &r->message.data.states;
        states->identity = entry.identity;
        states->qualified_identity = entry.identity;
        if (game != NULL) {
            struct cl_entry legacy;
            if(sc_native_legacy_entry(&store,game,&legacy))entry=legacy;
            else entry.identity=game->profile.identity;
        }
        states->history_revision = request->data.probe.revision;
        states->game_status = CL_MISSING;
        states->latest_status = CL_UNKNOWN;
        states->previous_status = CL_UNKNOWN;
        if(game==NULL && entry.native_state){
            struct clove_native_record native;
            if(sc_native_by_key(c,entry.identity.game,&native)){
                states->game_status=CL_READY;states->native_available=true;states->latest_status=CL_MISSING;
                sc_native_states(&native,states);
                r->ok=true;goto done;
            }
        }
        for (i = 0U; i < history->count; ++i)
        {
            if (cs_identity_equal(&history->entries[i].identity, &entry.identity))
            {
                entry.latest = history->entries[i].latest;
                entry.previous = history->entries[i].previous;
                break;
            }
        }
        for (i = 0U; i < 2U; ++i)
        {
            struct cs_record record;
            if (cs_store_read_resume(&store, i == 1U, &record) == CS_STORE_OK &&
                cs_identity_equal(&record.identity, &entry.identity))
            {
                struct cl_reference ref;
                ref.generation = record.generation;
                memcpy(ref.manifest, record.manifest, 65U);
                if (ref.generation > entry.latest.generation)
                {
                    entry.previous = entry.latest;
                    entry.latest = ref;
                }
                else if (ref.generation < entry.latest.generation &&
                         ref.generation > entry.previous.generation)
                {
                    entry.previous = ref;
                }
            }
        }
        states->latest = entry.latest;
        states->previous = entry.previous;
        if (game != NULL && sc_tools_game_enabled(tools, &store, game))
        {
            states->qualified_identity = entry.identity;
            if (request->token == UINT64_MAX) {
                /* Unknown content can still require a complete ISO read here.
                 * Persist the attempt BEFORE that work: a later queued log
                 * snapshot cannot run if storage stalls inside the hash. */
                fprintf(stderr, "launch preflight begin: system=%s content=%s\n",
                        game->entry.system, game->content);
                if (!sc_diagnostics_save(&store))
                    fprintf(stderr, "launch preflight: diagnostic snapshot unavailable\n");
            }
            if (sc_game_files_check(game, c->retroarch, request->token == UINT64_MAX))
            {
                states->game_status = CL_READY;
                states->native_available = true;
                /* This supervisor-only preflight authenticates launch files.
                 * The chosen immutable checkpoint is validated by the daemon
                 * and LOAD; unrelated manual slots cannot authorize launch. */
                if (request->token == UINT64_MAX) { r->ok = true; goto done; }
                states->latest_status =
                    cl_history_probe(&store, &entry, &entry.identity, &entry.latest);
                states->previous_status =
                    cl_history_probe(&store, &entry, &entry.identity, &entry.previous);
                states->manual_count = CS_MANUAL_SLOTS;
                for (i = 0U; i < CS_MANUAL_SLOTS; ++i)
                {
                    struct cs_record slot;
                    enum cs_store_status saved =
                        cs_store_slot_read(&store, &entry.identity, (unsigned int)i, &slot);
                    states->manual[i].slot = (unsigned int)i;
                    (void)snprintf(states->manual[i].label, sizeof(states->manual[i].label),
                                   "Slot %02u", (unsigned int)i);
                    states->manual[i].status =
                        saved == CS_STORE_OK
                            ? CL_READY
                            : (saved == CS_STORE_EMPTY || saved == CS_STORE_MEDIA
                                   ? CL_MISSING
                                   : (saved == CS_STORE_INCOMPATIBLE ? CL_INCOMPATIBLE
                                                                     : CL_DAMAGED));
                    if (saved == CS_STORE_OK)
                    {
                        states->manual[i].reference.generation = slot.generation;
                        strcpy(states->manual[i].reference.manifest, slot.manifest);
                    }
                }
                struct clove_native_record native;
                if(sc_native_game(c,game,&native))sc_native_states(&native,states);
                (void)snprintf(states->compatibility, sizeof(states->compatibility),
                               "Registered launch files present. START verifies full content/core; "
                               "Native settings remain editable; legacy state artifacts are verified on selection.");
            }
            else
            {
                (void)snprintf(states->compatibility, sizeof(states->compatibility),
                               "Content/core/emulator is missing, changed, or unreadable. No "
                               "launch is authorized.");
            }
        }
        else
        {
            (void)snprintf(states->compatibility, sizeof(states->compatibility),
                           "Game/core is unregistered, disabled, or its package selection failed "
                           "validation. Saved data is retained.");
        }
        r->ok = true;
    }
    else
    {
        error(r, "Unknown catalog request.");
    }
done:
    free(history);
    cs_store_close(&store);
}
static void publish(struct sc_disk *d, struct sc_disk_result *r)
{
    uint64_t one = 1U;
    (void)pthread_mutex_lock(&d->mutex);
    /* Never discard an accepted mutation's acknowledgement. The epoll owner
     * drains results even after UI exit; only this worker waits for capacity. */
    while (d->result_count == 4U)
        (void)pthread_cond_wait(&d->condition, &d->mutex);
    d->results[(d->result_head + d->result_count) % 4U] = r;
    ++d->result_count;
    (void)pthread_mutex_unlock(&d->mutex);
    while (write(d->event_fd, &one, sizeof(one)) < 0 && errno == EINTR)
    {
    }
}
static enum ca_result activity_update(const struct sc_config *c, struct ca_activity **activity,
                                      const struct sc_activity_update *update)
{
    const struct sc_game *registered;
    struct sc_game effective;
    struct ca_game game = {0};
    const char *relative = NULL;
    struct cs_store store;
    enum ca_result result;
    if (!update->active)
        return *activity == NULL ? CA_OK : ca_finish(*activity, update->mono_ms, update->wall_sec);
    if (!update->running)
        return *activity == NULL ? CA_OK : ca_hold(*activity, update->mono_ms, update->wall_sec);
    registered = sc_core_resolve(c,update->entry.identity.game,&effective) ? &effective : NULL;
    if (registered == NULL ||
        !cs_identity_equal(&registered->profile.identity, &update->entry.identity) ||
        update->session == 0U)
        return CA_STATE;
    relative = rom_relative(c, registered->content);
    if (relative == NULL || strlen(relative) >= sizeof(game.relative_path))
        return CA_STATE;
    strcpy(game.relative_path, relative);
    strcpy(game.name, registered->entry.title);
    strcpy(game.image_path, registered->entry.artwork_path);
    /* Onion stores the full ROM cache DB name in rom.type. This is metadata,
     * never an executable path or a filesystem creation request. */
    {
        const char *slash = strchr(relative, '/');
        int folder = slash == NULL ? (int)strlen(relative) : (int)(slash - relative);
        int n = snprintf(game.system, sizeof(game.system), "%.*s%.*s/%.*s_cache6.db",
                         (int)(relative - registered->content), registered->content, folder,
                         relative, folder, relative);
        if (n < 0 || (size_t)n >= sizeof(game.system))
            return CA_LIMIT;
    }
    if (*activity == NULL)
    {
        cs_store_init(&store);
        if (reader(c, &store) != CS_STORE_OK)
        {
            cs_store_close(&store);
            return CA_MEDIA;
        }
        result = ca_open(&store, activity);
        cs_store_close(&store);
        if (result != CA_OK)
            return result;
    }
    return ca_start(*activity, &game, update->session, update->mono_ms, update->wall_sec);
}
static bool online_cancelled(void *context)
{
    struct sc_disk *d = context; bool result;
    (void)pthread_mutex_lock(&d->mutex);
    result = d->closing || d->network_paused || d->online_cancel_epoch != d->online_job_epoch;
    (void)pthread_mutex_unlock(&d->mutex); return result;
}
/* Called with the queue mutex held. Activity timestamps retain their original
 * order, but periodic database flushes need not delay a foreground request. */
static bool foreground_waiting(const struct sc_disk *d)
{
    for (size_t i = 0U; i < d->job_count; ++i) {
        unsigned int type = d->jobs[(d->job_head + i) % SC_DISK_JOBS]->type;
        if (type != SC_UI_STATUS && type != SC_DIAGNOSTICS) return true;
    }
    return false;
}
static void *thread_main(void *context)
{
    struct sc_disk *d = context;
    struct sc_tools_backend tools = {0};
    struct library_cache cache = {0};
    tools.cancel.cancelled = online_cancelled;
    tools.cancel.context = d;
    struct ca_activity *activity = NULL;
    for (;;)
    {
        struct sc_message *request = NULL;
        struct sc_activity_update update = {0};
        struct sc_disk_result *r;
        bool closing, activity_job = false, allow_offline;
        (void)pthread_mutex_lock(&d->mutex);
        while (d->job_count == 0U && d->activity_count == 0U && !d->verification_pending &&
               !d->closing)
        {
            (void)pthread_cond_wait(&d->condition, &d->mutex);
        }
        closing =
            d->closing && !d->verification_pending && d->activity_count == 0U && d->job_count == 0U;
        allow_offline = d->allow_offline;
        if (d->verification_pending)
        {
            request = calloc(1U, sizeof(*request));
            if (request != NULL)
            {
                sc_message_init(request, SC_UI_INPUT_VERIFIED, d->verification_token);
                request->data.verification = d->verification;
                d->verification_pending = false;
            }
        }
        else if (d->activity_count != 0U && !foreground_waiting(d))
        {
            activity_job = true;
            update = d->activity_jobs[d->activity_head];
            d->activity_head = (d->activity_head + 1U) % 32U;
            --d->activity_count;
        }
        else if (!closing)
        {
            size_t chosen = 0U;
            for (size_t i = 0U; i < d->job_count; ++i) {
                unsigned int type = d->jobs[(d->job_head + i) % SC_DISK_JOBS]->type;
                if (type != SC_UI_STATUS && type != SC_DIAGNOSTICS) { chosen = i; break; }
            }
            request = d->jobs[(d->job_head + chosen) % SC_DISK_JOBS];
            for (size_t i = chosen; i + 1U < d->job_count; ++i)
                d->jobs[(d->job_head + i) % SC_DISK_JOBS] = d->jobs[(d->job_head + i + 1U) % SC_DISK_JOBS];
            --d->job_count;
        }
        (void)pthread_mutex_unlock(&d->mutex);
        bool catalog_partial = false;
        uint64_t catalog_partial_token = 0U;
        r = calloc(1U, sizeof(*r));
        if (r == NULL || (!closing && !activity_job && request == NULL))
        {
            /* No completion fabricated. Parent remains alive and cannot unmount. */
            free(request);
            free(r);
            ca_close(activity);
            free(cache.entries);
            return NULL;
        }
        if (activity_job)
        {
            r->activity_write = true;
            sc_message_init(&r->message, SC_ACTIVITY_TICK, 0U);
            r->activity_result = activity_update(d->config, &activity, &update);
            r->ok = r->activity_result == CA_OK;
            if (!r->ok)
            {
                fprintf(stderr, "Activity Tracker: %s (session=%" PRIu64 ", running=%u)\n",
                        ca_result_name(r->activity_result), update.session,
                        (unsigned int)update.running);
                publish(d, r);
            }
            else
                free(r); /* Successful telemetry never crowds out UI mutation acknowledgements. */
            continue;
        }
        if (closing)
        {
            struct cs_store store;
            cs_store_init(&store);
            r->final = true;
            if (activity != NULL)
            {
                uint64_t now;
                time_t wall = time(NULL);
                enum ca_result result = (!sc_now(&now) || wall < 0)
                                            ? CA_CLOCK
                                            : ca_finish(activity, now, (int64_t)wall);
                if (result != CA_OK)
                    fprintf(stderr,
                            "Activity Tracker final flush failed: %s; checkpoint barrier remains "
                            "independent\n",
                            ca_result_name(result));
                ca_close(activity);
                activity = NULL;
            }
            r->ok = reader(d->config, &store) == CS_STORE_OK;
            if (r->ok)
            {
                if (!sc_diagnostics_save(&store))
                {
                    fprintf(stderr, "supervisor: final diagnostic snapshot failed; volume barrier "
                                    "still required\n");
                }
                r->ok = cs_store_sync(&store) == CS_STORE_OK;
            }
            else if (allow_offline && sc_games_unmounted(d->config))
            {
                r->ok = true;
            }
            cs_store_close(&store);
            if (r->ok)
            {
                r->ok = sc_sync_path(d->config->system_path);
            }
            publish(d, r);
            free(cache.entries);
            return NULL;
        }
        if (request->type==SC_UI_LIBRARY && request->data.history.revision==UINT64_MAX) {
            r->replacement=calloc(1U,sizeof(*r->replacement));
            int scan_result=r->replacement!=NULL ? sc_catalog_refresh() : SC_CATALOG_FAILED;
            bool refreshed=r->replacement!=NULL &&
                (scan_result==SC_CATALOG_OK || scan_result==SC_CATALOG_PARTIAL) &&
                sc_config_load("/run/console/config/supervisor.json",r->replacement);
            if(refreshed){
                (void)pthread_mutex_lock(&d->mutex);d->catalog_waiting=true;d->catalog_accepted=false;(void)pthread_mutex_unlock(&d->mutex);
                publish(d,r);
                (void)pthread_mutex_lock(&d->mutex);
                while(d->catalog_waiting)(void)pthread_cond_wait(&d->condition,&d->mutex);
                refreshed=d->catalog_accepted;(void)pthread_mutex_unlock(&d->mutex);
                r=calloc(1U,sizeof(*r));
                if(r==NULL){free(request);ca_close(activity);free(cache.entries);return NULL;}
            }else{free(r->replacement);r->replacement=NULL;}
            if(!refreshed){
                sc_message_init(&r->message,SC_UI_ERROR,request->token);
                strcpy(r->message.data.text,"ROM refresh failed. The previous list is retained.");
                free(request);publish(d,r);continue;
            }
            catalog_partial = scan_result == SC_CATALOG_PARTIAL;
            catalog_partial_token = request->token;
            free(cache.entries);memset(&cache,0,sizeof(cache));
        }
        if (request->type == SC_UI_TOOL && request->data.tool_request.operation >= CL_TOOL_LOAD_SCRAPER &&
            request->data.tool_request.operation <= CL_TOOL_CANCEL_ONLINE)
            d->online_job_epoch = queued_cancel_epoch(request);
        {
            uint64_t started_at = 0U, completed_at;
            bool measured = sc_now(&started_at);
            job(d->config, &tools, request, r, &cache);
            if (measured && sc_now(&completed_at) && completed_at >= started_at &&
                completed_at - started_at >= 250U)
                fprintf(stderr, "storage job: type=%u preflight=%u elapsed_ms=%llu\n",
                        request->type, request->token == UINT64_MAX,
                        (unsigned long long)(completed_at - started_at));
        }
        free(request);
        publish(d, r);
        /* Queue the existing Information/error presentation AFTER the valid
         * replacement and library page, so Refresh cannot imply completeness.
         * No second catalog or new menu protocol is introduced. */
        if (catalog_partial) {
            r = calloc(1U, sizeof(*r));
            if (r != NULL) {
                sc_message_init(&r->message, SC_UI_ERROR, catalog_partial_token);
                strcpy(r->message.data.text, "ROM list refreshed partially. Scanned games are usable. Limits: 1024 games, 4096 ROMs/folder, 32GiB new hashes; ASCII filenames required.");
                publish(d, r);
            }
        }
    }
}
bool sc_disk_activity(struct sc_disk *d, const struct sc_activity_update *update)
{
    size_t index;
    if (d == NULL || update == NULL || update->wall_sec < 0 ||
        (update->running &&
         (!update->active || update->session == 0U || !cs_identity_valid(&update->entry.identity))))
        return false;
    (void)pthread_mutex_lock(&d->mutex);
    if (d->closing)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        return false;
    }
    if (d->activity_count != 0U)
    {
        struct sc_activity_update *last =
            &d->activity_jobs[(d->activity_head + d->activity_count - 1U) % 32U];
        if (last->active == update->active && last->running == update->running &&
            last->session == update->session &&
            cs_identity_equal(&last->entry.identity, &update->entry.identity) &&
            last->mono_ms <= update->mono_ms)
        {
            /* Preserve the first active frontier; coalesce only later ticks so
             * a fast producer cannot erase start/pause/resume accounting. */
            if (d->activity_count >= 2U)
            {
                const struct sc_activity_update *previous =
                    &d->activity_jobs[(d->activity_head + d->activity_count - 2U) % 32U];
                if (previous->active == last->active && previous->running == last->running &&
                    previous->session == last->session &&
                    cs_identity_equal(&previous->entry.identity, &last->entry.identity))
                {
                    *last = *update;
                    (void)pthread_mutex_unlock(&d->mutex);
                    return true;
                }
            }
        }
    }
    if (d->activity_count == 32U)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        return false;
    }
    index = (d->activity_head + d->activity_count) % 32U;
    d->activity_jobs[index] = *update;
    ++d->activity_count;
    (void)pthread_cond_signal(&d->condition);
    (void)pthread_mutex_unlock(&d->mutex);
    return true;
}
bool sc_disk_start(struct sc_disk *d, const struct sc_config *config)
{
    memset(d, 0, sizeof(*d));
    d->event_fd = -1;
    d->config = config;
    if (pthread_mutex_init(&d->mutex, NULL) != 0)
    {
        return false;
    }
    if (pthread_cond_init(&d->condition, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&d->mutex);
        return false;
    }
    d->event_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (d->event_fd < 0 || pthread_create(&d->thread, NULL, thread_main, d) != 0)
    {
        if (d->event_fd >= 0)
        {
            (void)close(d->event_fd);
        }
        (void)pthread_cond_destroy(&d->condition);
        (void)pthread_mutex_destroy(&d->mutex);
        return false;
    }
    d->started = true;
    return true;
}
bool sc_disk_verify(struct sc_disk *d, const struct cl_input_verification *verification,
                    uint64_t token)
{
    if (!cl_input_verification_valid(&d->config->ui_config.input, verification))
    {
        return false;
    }
    (void)pthread_mutex_lock(&d->mutex);
    if (d->closing || d->verification_pending)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        return false;
    }
    d->verification = *verification;
    d->verification_token = token;
    d->verification_pending = true;
    (void)pthread_cond_signal(&d->condition);
    (void)pthread_mutex_unlock(&d->mutex);
    return true;
}
void sc_disk_pause_network(struct sc_disk *d, bool paused)
{
    if (!d->started) return;
    (void)pthread_mutex_lock(&d->mutex);
    if (paused && !d->network_paused) ++d->online_cancel_epoch;
    d->network_paused = paused;
    (void)pthread_mutex_unlock(&d->mutex);
}

bool sc_disk_submit(struct sc_disk *d, const struct sc_message *request)
{
    struct queued_message *queued = malloc(sizeof(*queued));
    struct sc_message *copy = queued != NULL ? &queued->message : NULL;
    size_t i;
    if (copy == NULL)
    {
        return false;
    }
    *copy = *request;
    (void)pthread_mutex_lock(&d->mutex);
    queued->cancel_epoch = d->online_cancel_epoch;
    if (d->closing)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        free(copy);
        return false;
    }
    if (request->type == SC_UI_TOOL && request->data.tool_request.operation == CL_TOOL_CANCEL_ONLINE) {
        ++d->online_cancel_epoch;
        (void)pthread_mutex_unlock(&d->mutex); free(copy); return true;
    }
    if (request->type == SC_UI_PROBE || request->type == SC_UI_LIBRARY ||
        request->type == SC_UI_STATUS || request->type == SC_DIAGNOSTICS)
    {
        for (i = 0U; i < d->job_count; ++i)
        {
            size_t index = (d->job_head + i) % SC_DISK_JOBS;
            if (d->jobs[index]->type == request->type &&
                (d->jobs[index]->token >> 32) == (request->token >> 32) &&
                (request->type != SC_UI_LIBRARY ||
                 (d->jobs[index]->data.history.library_mode == request->data.history.library_mode &&
                  (d->jobs[index]->data.history.revision == UINT64_MAX) ==
                  (request->data.history.revision == UINT64_MAX))))
            {
                free(d->jobs[index]);
                d->jobs[index] = copy;
                (void)pthread_mutex_unlock(&d->mutex);
                return true;
            }
        }
    }
    if ((request->type == SC_UI_STATUS || request->type == SC_DIAGNOSTICS) && d->job_count >= 2U) {
        (void)pthread_mutex_unlock(&d->mutex); free(copy); return false;
    }
    if (d->job_count == SC_DISK_JOBS)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        free(copy);
        return false;
    }
    d->jobs[(d->job_head + d->job_count) % SC_DISK_JOBS] = copy;
    ++d->job_count;
    (void)pthread_cond_signal(&d->condition);
    (void)pthread_mutex_unlock(&d->mutex);
    return true;
}
bool sc_disk_result(struct sc_disk *d, struct sc_disk_result *r)
{
    struct sc_disk_result *stored;
    (void)pthread_mutex_lock(&d->mutex);
    if (d->result_count == 0U)
    {
        (void)pthread_mutex_unlock(&d->mutex);
        return false;
    }
    stored = d->results[d->result_head];
    d->result_head = (d->result_head + 1U) % 4U;
    --d->result_count;
    (void)pthread_cond_signal(&d->condition);
    (void)pthread_mutex_unlock(&d->mutex);
    *r = *stored;
    free(stored);
    return true;
}
void sc_disk_accept_catalog(struct sc_disk *d,bool accepted)
{
    (void)pthread_mutex_lock(&d->mutex);
    d->catalog_accepted=accepted;d->catalog_waiting=false;
    (void)pthread_cond_broadcast(&d->condition);(void)pthread_mutex_unlock(&d->mutex);
}
void sc_disk_finish(struct sc_disk *d)
{
    size_t kept = 0U;
    struct sc_message *mutations[SC_DISK_JOBS];
    (void)pthread_mutex_lock(&d->mutex);
    d->closing = true;
    d->catalog_accepted=false;d->catalog_waiting=false;
    while (d->job_count != 0U)
    {
        struct sc_message *request = d->jobs[d->job_head];
        if (request->type == SC_UI_TOOL &&
            (request->data.tool_request.operation == CL_TOOL_APPLY_PACKAGES ||
             request->data.tool_request.operation == CL_TOOL_ROLLBACK_PACKAGES ||
             request->data.tool_request.operation == CL_TOOL_APPLY_THEME ||
             request->data.tool_request.operation == CL_TOOL_APPLY_ICON_PACK ||
             request->data.tool_request.operation == CL_TOOL_APPLY_ICON ||
             request->data.tool_request.operation == CL_TOOL_RESET_CONFIG ||
             request->data.tool_request.operation == CL_TOOL_REGENERATE_HISTORY ||
             request->data.tool_request.operation == CL_TOOL_CONNECT_NETWORK ||
             request->data.tool_request.operation == CL_TOOL_FORGET_NETWORK ||
             request->data.tool_request.operation == CL_TOOL_SAVE_ONLINE_SETTING ||
             cl_tool_maintenance_file_job(request->data.tool_request.operation) ||
             request->data.tool_request.operation == CL_TOOL_SET_CORE || request->data.tool_request.operation==CL_TOOL_SET_CLOCK || request->data.tool_request.operation==CL_TOOL_SET_TIMEZONE))
            mutations[kept++] = request;
        else
            free(request);
        d->job_head = (d->job_head + 1U) % SC_DISK_JOBS;
        --d->job_count;
    }
    d->job_head = 0U;
    d->job_count = kept;
    for (size_t i = 0U; i < kept; ++i)
        d->jobs[i] = mutations[i];
    (void)pthread_cond_signal(&d->condition);
    (void)pthread_mutex_unlock(&d->mutex);
}
void sc_disk_offline_permission(struct sc_disk *d)
{
    (void)pthread_mutex_lock(&d->mutex);
    d->allow_offline = true;
    (void)pthread_mutex_unlock(&d->mutex);
}
void sc_disk_join(struct sc_disk *d)
{
    size_t i;
    if (!d->started)
    {
        return;
    }
    (void)pthread_join(d->thread, NULL);
    for (i = 0U; i < d->result_count; ++i)
    {
        struct sc_disk_result *result=d->results[(d->result_head+i)%4U];
        free(result->replacement);free(result);
    }
    (void)close(d->event_fd);
    (void)pthread_cond_destroy(&d->condition);
    (void)pthread_mutex_destroy(&d->mutex);
    d->started = false;
}
