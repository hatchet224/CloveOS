#include "launcher/i18n.h"
#include "sessiond/durability.h"
#include "supervisor/tools_backend.h"
#include "supervisor/battery_monitor.h"
#include "supervisor/network.h"
#include "supervisor/online.h"
#include "supervisor/core_choice.h"
#include "supervisor/glo_scripts.h"
#include "supervisor/clock.h"
#include "supervisor/maintenance.h"
#include "supervisor/zip.h"
#include "console_activity/activity.h"
#include "launcher/icons.h"
#include "lodepng.h"
#include "sessiond/document.h"
#include "sessiond/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void text_copy(char *out, size_t cap, const char *text)
{
    size_t n = strlen(text);
    if (n >= cap)
        n = cap - 1U;
    while (n > 0U && !cs_utf8_valid(text, n))
        --n;
    memcpy(out, text, n);
    out[n] = '\0';
}
static bool registry(struct sc_tools_backend *b)
{
    int root;
    if (b->registry_loaded)
        return true;
    root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0)
        return false;
    b->registry_loaded =
        sp_package_registry_describe(root, "etc/console/BINARY_MANIFEST.json", &b->registry);
    if (close(root) != 0)
        b->registry_loaded = false;
    return b->registry_loaded;
}
static bool selection_ok(enum sp_package_status r)
{
    return r == SP_PACKAGE_OK || r == SP_PACKAGE_EMPTY || r == SP_PACKAGE_RECOVERED;
}
bool sc_tools_packages(struct sc_tools_backend *b, const struct cs_store *store,
                       struct sp_package_selection *selection)
{
    return registry(b) && selection_ok(sp_package_load(store, &b->registry, selection));
}
static bool contains_system(const char *list, const char *system)
{
    size_t size = strlen(system);
    while (*list != '\0')
    {
        const char *end = strchr(list, '|');
        size_t n = end == NULL ? strlen(list) : (size_t)(end - list);
        if (n == size && memcmp(list, system, n) == 0)
            return true;
        if (end == NULL)
            break;
        list = end + 1;
    }
    return false;
}
bool sc_tools_system_visible(const struct sp_package_selection *s, const char *system)
{
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i)
    {
        const struct sp_package *p = sp_package_at(i);
        if (sp_package_core_layer(p->layer) && contains_system(p->systems, system))
            return sp_package_enabled(s, p->id);
    }
    return true; /* Unsupported folders remain browseable, never launch-authorized. */
}
bool sc_tools_system_expert(const char *system)
{
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        const struct sp_package *p = sp_package_at(i);
        if (p->layer == SP_PACKAGE_EXPERT && contains_system(p->systems, system))
            return true;
    }
    return false;
}
bool sc_tools_game_enabled(struct sc_tools_backend *b, const struct cs_store *store,
                           const struct sc_game *game)
{
    struct sp_package_selection s;
    if (!sc_tools_packages(b, store, &s))
        return false;
    /* TGB Dual is the pinned normal-play variant of the existing GB/GBC
     * package. Keep that package's enable/disable policy authoritative. */
    if (strcmp(game->core,"/system/lib/libretro/onion-4.3.1-1/tgbdual_libretro.so")==0 &&
        strcmp(game->config,"/etc/console/core-configs/tgbdual-single.cfg")==0 &&
        (strcmp(game->profile.identity.core,"e2495af9a5be23045fc74981173f06b9856c3182d696651fa026999348134f45")==0 ||
         strcmp(game->profile.identity.core,"b83a869c3b25f1f02e2d06687abd0e1ff992d379ae815b5f7e08d53bcd457301")==0) &&
        (strcmp(game->entry.system,"GB")==0 || strcmp(game->entry.system,"GBC")==0))
        return sp_package_system_enabled(&s,game->entry.system);
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i)
    {
        const struct sp_package *p = sp_package_at(i);
        if (sp_package_core_layer(p->layer) && game->core[0] == '/' &&
            strcmp(game->core + 1, p->artifact) == 0)
            return sp_package_enabled(&s, p->id);
    }
    return false;
}
static void fail(struct cl_tool_snapshot *s, const char *message)
{
    s->ready = false;
    s->error = true;
    text_copy(s->status, sizeof(s->status), message);
}
static bool package_has_roms(const struct sc_config *c, const char *systems)
{
    for (size_t i = 0U; i < c->game_count; ++i)
        if (contains_system(systems, c->games[i].entry.system))
            return true;
    return false;
}
unsigned int sc_tools_app_features(const struct sp_package_selection *selection)
{
    static const struct { const char *id; unsigned int flag; } apps[] = {
        {"app.gallery", CL_APP_GALLERY}, {"app.search", CL_APP_SEARCH}, {"app.expert", CL_APP_EXPERT},
        {"app.list-shortcuts", CL_APP_LIST_SHORTCUTS}, {"app.battery", CL_APP_BATTERY},
        {"collection.ports", CL_APP_PORTS}
    };
    unsigned int flags = 0U;
    for (size_t i = 0U; i < sizeof(apps) / sizeof(apps[0]); ++i)
        if (sp_package_enabled(selection, apps[i].id)) flags |= apps[i].flag;
    return flags;
}
static void packages(struct sc_tools_backend *b, const struct sc_config *c,
                     const struct cs_store *store, const struct cl_tool_request *r,
                     struct cl_tool_snapshot *s)
{
    struct sp_package_selection selection;
    enum sp_package_status status;
    s->kind = CL_TOOL_PACKAGES;
    if (!registry(b))
    {
        fail(s, "Protected package inventory failed verification; no launch change authorized.");
        return;
    }
    status = sp_package_load(store, &b->registry, &selection);
    if (!selection_ok(status))
    {
        fail(s, sp_package_status_name(status));
        return;
    }
    if (r->operation == CL_TOOL_APPLY_PACKAGES)
    {
        struct sp_package_plan plan;
        sp_package_plan_begin(&plan, &selection);
        plan.revision = r->revision;
        plan.desired = r->enabled_mask;
        status = sp_package_apply(store, &b->registry, &plan, &selection);
    }
    else if (r->operation == CL_TOOL_ROLLBACK_PACKAGES)
    {
        status = sp_package_rollback(store, &b->registry, r->revision, &selection);
    }
    if (!selection_ok(status))
    {
        fail(s, sp_package_status_name(status));
        return;
    }
    s->revision = selection.revision;
    s->enabled_mask = selection.enabled;
    s->available_mask = b->registry.available;
    s->app_features = sc_tools_app_features(&selection);
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i)
    {
        const struct sp_package *p = sp_package_at(i);
        struct cl_tool_row *row;
        if (p->layer == SP_PACKAGE_RUNTIME)
            continue;
        row = &s->rows[s->count++];
        row->index = (unsigned int)i;
        row->group = p->layer == SP_PACKAGE_EXPERT ? CL_TOOL_EXPERT :
                     (p->layer == SP_PACKAGE_VERIFIED ? CL_TOOL_VERIFIED : CL_TOOL_APPS);
        text_copy(row->id, sizeof(row->id), p->id);
        text_copy(row->label, sizeof(row->label), p->title);
        text_copy(row->detail, sizeof(row->detail),
                  p->systems[0] == '\0' ? "Native system application" : p->systems);
        row->enabled = sp_package_enabled(&selection, p->id);
        row->available = (b->registry.available & (UINT64_C(1) << i)) != 0U;
        row->required = p->required;
        row->has_roms = package_has_roms(c, p->systems);
    }
    s->ready = true;
    text_copy(s->status, sizeof(s->status),
              status == SP_PACKAGE_RECOVERED
                  ? "Recovered previous package selection. ROMs and saves are retained."
                  : "Verified native packages; pending changes require Apply.");
}
static void activity(const struct cs_store *store, const struct cl_tool_request *r,
                     struct cl_tool_snapshot *s)
{
    struct ca_snapshot *snapshot = calloc(1U, sizeof(*snapshot));
    enum ca_result result;
    char total[32];
    s->kind = CL_TOOL_ACTIVITY;
    if (snapshot == NULL)
    {
        fail(s, "Activity snapshot allocation failed.");
        return;
    }
    result = ca_snapshot_read(store, snapshot);
    if (result != CA_OK && result != CA_EMPTY)
    {
        fail(s, ca_result_name(result));
        free(snapshot);
        return;
    }
    for (size_t i = 0U; i < snapshot->count; ++i)
    {
        const struct ca_entry *entry = &snapshot->entries[i];
        struct cl_tool_row *row = &s->rows[s->count++];
        struct ca_page page;
        char digest[65], time[32], average[32];
        cs_sha256_hex(entry->game.relative_path, strlen(entry->game.relative_path), digest);
        memcpy(row->id, digest, 32U);
        row->id[32] = '\0';
        ca_page_build(snapshot, i / CA_PAGE_ROWS, strcmp(r->id, "raw") == 0, &page);
        text_copy(row->label, sizeof(row->label), page.rows[i % CA_PAGE_ROWS].name);
        ca_format_duration(entry->total_seconds, time);
        ca_format_duration(entry->average_seconds, average);
        (void)snprintf(row->detail, sizeof(row->detail), "Total %s   Avg %s   Plays %" PRIu64, time,
                       average, entry->plays);
        row->group = CL_TOOL_SUMMARY;
        row->available = true;
        row->index = (unsigned int)i;
    }
    ca_format_duration(snapshot->total_seconds, total);
    (void)snprintf(s->status, sizeof(s->status), "Total %s | %" PRIu64 " games over 60 seconds%s",
                   total, snapshot->ranked_games,
                   snapshot->recovered_previous ? " | Recovered previous DB" : "");
    s->ready = true;
    free(snapshot);
}
#define BUNDLED_THEMES "/system/share/console/themes"
static const char *const bundled_themes[] = {
    "Analogue by Aemiii91", "PAC-MIYOO by tenlevels",
    "Super Onion Entertainment System by TheDewd", "System 9 by injekim",
    "NanoSwitch Modern Dark by Amdy + Cheetashock", "cherry os by pico cherry"
};

static bool theme_name(const char *name)
{
    size_t n = strnlen(name, 64U);
    if (n == 0U || n >= 64U || name[0] == '.' || strstr(name, "..") != NULL ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.+()&'") != n ||
        name[n-1U]==' ' || name[n-1U]=='.')
        return false;
    return true;
}
static bool theme_config(int dir)
{
    int fd = openat(dir, "config.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    struct cs_document *doc = NULL;
    char *text = NULL;
    size_t used = 0U;
    bool ok = false;
    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 2 ||
        st.st_size > 131072)
        goto done;
    text = malloc((size_t)st.st_size);
    doc = malloc(sizeof(*doc));
    if (text == NULL || doc == NULL)
        goto done;
    while (used < (size_t)st.st_size)
    {
        ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        used += (size_t)n;
    }
    ok = cs_doc_parse(doc, text, used) && doc->tokens[0].type == CS_DOC_OBJECT;
done:
    free(text);
    free(doc);
    if (close(fd) != 0)
        ok = false;
    return ok;
}
static bool theme_path(const struct sc_config *c, const struct cs_store *store, const char *name,
                       char path[512])
{
    int themes = -1, dir = -1, n;
    bool ok = false;
    if (strcmp(name, "@default") == 0)
    {
        dir = sc_open_absolute(c->ui_config.theme_path, true);
        if (dir < 0)
            return false;
        ok = theme_config(dir);
        if (ok)
            text_copy(path, 512U, c->ui_config.theme_path);
    }
    else if (theme_name(name) && cs_store_check_media(store) == CS_STORE_OK)
    {
        themes = openat(store->card_fd, "Themes", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (themes >= 0)
            dir = openat(themes, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dir >= 0 && theme_config(dir))
        {
            n = snprintf(path, 512U, "%s/Themes/%s", store->card_path, name);
            ok = n >= 0 && n < 512 && cs_store_check_media(store) == CS_STORE_OK;
        }
    }
    if (dir >= 0 && close(dir) != 0)
        ok = false;
    if (themes >= 0 && close(themes) != 0)
        ok = false;
    for (size_t i = 0U; !ok && i < sizeof(bundled_themes) / sizeof(bundled_themes[0]); ++i) {
        if (strcmp(name, bundled_themes[i]) != 0)
            continue;
        n = snprintf(path, 512U, "%s/%s", BUNDLED_THEMES, name);
        if (n < 0 || n >= 512)
            return false;
        dir = sc_open_absolute(path, true);
        if (dir < 0)
            return false;
        ok = theme_config(dir);
        if (close(dir) != 0)
            ok = false;
    }
    return ok;
}
static int selection_read(const struct cs_store *store, const char *filename, char name[192], uint64_t *revision)
{
    char text[512], body[320], checksum[65];
    struct cs_json json;
    struct stat st;
    int fd = openat(store->data_fd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC),
        n;
    size_t used = 0U;
    uint64_t schema;
    static const char *const keys[] = {"schema", "theme", "revision", "checksum"};
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 2 ||
        st.st_size >= (off_t)sizeof(text))
    {
        close(fd);
        return -1;
    }
    while (used < (size_t)st.st_size)
    {
        ssize_t count = read(fd, text + used, (size_t)st.st_size - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            close(fd);
            return -1;
        }
        used += (size_t)count;
    }
    if (close(fd) != 0 || !cs_json_parse(&json, text, used) || !cs_json_keys(&json, keys, 4U) ||
        !cs_json_u64(&json, "revision", revision) || *revision == 0U ||
        cs_json_string(&json, "theme") == NULL || strlen(cs_json_string(&json, "theme")) >= 192U ||
        cs_json_string(&json, "checksum") == NULL || !cs_json_u64(&json, "schema", &schema) ||
        schema != 1U)
        return -1;
    strcpy(name, cs_json_string(&json, "theme"));
    n = snprintf(body, sizeof(body), "{\"schema\":1,\"theme\":\"%s\",\"revision\":\"%" PRIu64 "\"}",
                 name, *revision);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;
    cs_sha256_hex(body, (size_t)n, checksum);
    return strcmp(checksum, cs_json_string(&json, "checksum")) == 0 ? 1 : -1;
}
static bool publish_presentation_asset(const struct cs_store *store, const char *filename,
                                       const void *data, size_t size)
{
    char temp[96];
    static uint64_t serial;
    struct stat before, live;
    int fd = -1;
    size_t used = 0U;
    bool ok = false;
    const unsigned char *bytes = data;
    if (cs_store_check_media(store) != CS_STORE_OK || fstat(store->data_fd, &before) != 0)
        return false;
    for (unsigned int tries = 0U; tries < 16U; ++tries)
    {
        (void)snprintf(temp, sizeof(temp), ".theme-%ld-%" PRIu64 ".tmp", (long)getpid(), ++serial);
        fd = openat(store->data_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        return false;
    while (used < size)
    {
        ssize_t count = write(fd, bytes + used, size - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto done;
        used += (size_t)count;
    }
    if (fsync(fd) != 0 || cs_store_check_media(store) != CS_STORE_OK ||
        fstatat(store->card_fd, ".console", &live, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(live.st_mode) || live.st_dev != before.st_dev || live.st_ino != before.st_ino)
        goto done;
    if (close(fd) != 0)
    {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0 ||
        renameat(store->data_fd, temp, store->data_fd, filename) != 0)
        goto done;
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0) goto done;
    ok = cs_store_check_media(store) == CS_STORE_OK;
done:
    if (fd >= 0)
        (void)close(fd);
    (void)unlinkat(store->data_fd, temp, 0);
    return ok;
}
struct presentation {
    char theme[64], pack[64], fallback[64];
    uint64_t revision;
};

static bool theme_key(const char *name)
{
    return strcmp(name, "@default") == 0 || theme_name(name);
}

static bool pack_key(const char *name)
{
    return theme_key(name) ||
           (strncmp(name, "theme:", 6U) == 0 && theme_key(name + 6U));
}

static bool read_presentation(const struct cs_store *store, struct presentation *state)
{
    char record[192];
    uint64_t revision = 0U;
    strcpy(state->theme, "@default");
    strcpy(state->pack, "@default");
    int loaded = selection_read(store, "presentation.json", record, &revision);
    if (loaded < 0) return false;
    if (loaded == 0) {
        int theme = selection_read(store, "theme.json", record, &revision);
        if (theme < 0 || (theme > 0 && !theme_key(record))) return false;
        if (theme > 0) strcpy(state->theme, record);
        state->revision = revision;
        int pack = selection_read(store, "icons.json", record, &revision);
        if (pack < 0 || (pack > 0 && !theme_key(record))) return false;
        if (pack > 0) strcpy(state->pack, record);
        if (revision > state->revision) state->revision = revision;
        return true;
    }
    char *separator = strchr(record, '|');
    if (separator == NULL) return false;
    *separator++ = '\0';
    char *fallback = strchr(separator, '|');
    if (fallback == NULL) return false;
    *fallback++ = '\0';
    if (strlen(separator) >= sizeof(state->pack) ||
        (*fallback != '\0' && !theme_key(fallback)) ||
        !theme_key(record) || !pack_key(separator))
        return false;
    strcpy(state->theme, record);
    strcpy(state->pack, separator);
    strcpy(state->fallback, fallback);
    state->revision = revision;
    return true;
}

static bool write_presentation(const struct cs_store *store, const struct presentation *state)
{
    char body[320], record[448], hash[65];
    int n = snprintf(body, sizeof(body), "{\"schema\":1,\"theme\":\"%s|%s|%s\",\"revision\":\"%" PRIu64 "\"}",
                     state->theme, state->pack, state->fallback, state->revision);
    if (n < 1 || (size_t)n >= sizeof(body)) return false;
    cs_sha256_hex(body, (size_t)n, hash);
    body[n - 1] = '\0';
    n = snprintf(record, sizeof(record), "%s,\"checksum\":\"%s\"}\n", body, hash);
    return n > 0 && (size_t)n < sizeof(record) &&
           publish_presentation_asset(store, "presentation.json", record, (size_t)n);
}

bool sc_tools_selected_theme(const struct sc_config *c, const struct cs_store *store,
                             char selected[512])
{
    struct presentation state = {0};
    return read_presentation(store, &state) && theme_path(c, store, state.theme, selected);
}

static bool icon_pack_path(const struct sc_config *c, const struct cs_store *store,
                           const char *name, char path[512]);

static void icon_fallback(const struct sc_config *c, const struct cs_store *store,
                           const struct presentation *state, char path[512])
{
    if (state->fallback[0] != '\0' && !theme_path(c, store, state->fallback, path))
        path[0] = '\0';
}

static int compare_rows(const void *a, const void *b)
{
    return strcmp(((const struct cl_tool_row *)a)->label, ((const struct cl_tool_row *)b)->label);
}
static void add_theme_row(struct cl_tool_snapshot *s, const char *id, const char *label,
                          const char *path)
{
    if (s->count >= CL_TOOL_ROWS)
        return;
    for (size_t i = 0U; i < s->count; ++i)
        if (strcmp(s->rows[i].id, id) == 0)
            return;
    struct cl_tool_row *row = &s->rows[s->count++];
    text_copy(row->id, sizeof(row->id), id);
    text_copy(row->label, sizeof(row->label), label);
    if (strlen(path) < sizeof(row->detail))
        text_copy(row->detail, sizeof(row->detail), path);
    row->available = true;
}

struct theme_archive {
    unsigned char *zip;
    size_t size, expanded;
    char config[512];
    int destination;
};

static unsigned char *read_asset_bytes(int folder,const char *name,size_t maximum,size_t *size)
{
    struct stat st;
    int fd=openat(folder,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) return NULL;
    unsigned char *bytes=NULL;
    if(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1U &&
       st.st_size>=22 && (uint64_t)st.st_size<=maximum) {
        bytes=malloc((size_t)st.st_size);
        size_t used=0U;
        while(bytes!=NULL && used<(size_t)st.st_size) {
            ssize_t n=read(fd,bytes+used,(size_t)st.st_size-used);
            if(n<0 && errno==EINTR) continue;
            if(n<=0){free(bytes);bytes=NULL;break;}
            used+=(size_t)n;
        }
        if(bytes!=NULL) *size=used;
    }
    (void)close(fd);
    return bytes;
}

static bool find_archive_config(void *context,const char *name,size_t expanded)
{
    struct theme_archive *archive=context;
    const char *slash=strchr(name,'/');
    if(slash!=NULL && strchr(slash+1,'/')!=NULL) return true;
    if(strcmp(slash!=NULL?slash+1:name,"config.json")!=0) return true;
    if(archive->config[0]!='\0' || expanded>131072U || expanded<2U) return false;
    strcpy(archive->config,name);
    return true;
}

static bool asset_name(const char *name)
{
    if(name[0]=='.' || name[0]=='\0' || strlen(name)>110U) return false;
    for(const unsigned char *p=(const unsigned char *)name;*p!='\0';++p)
        if(*p<32U || strchr("\\:\"<>|?*",*p)!=NULL) return false;
    size_t length=strlen(name);
    return name[length-1U]!='.' && name[length-1U]!=' ';
}
static bool write_asset(int directory,const char *name,const unsigned char *data,size_t size)
{
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if(fd<0) return false;
    size_t used=0U;
    while(used<size) {
        ssize_t n=write(fd,data+used,size-used);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) break;
        used+=(size_t)n;
    }
    bool ok=used==size && fsync(fd)==0;
    return close(fd)==0 && ok;
}

static bool extract_theme_asset(void *context,const char *name,size_t expanded)
{
    struct theme_archive *a=context;
    size_t prefix=strlen(a->config)-strlen("config.json");
    if(strncmp(name,a->config,prefix)!=0) return true;
    const char *relative=name+prefix;
    const char *ext=strrchr(relative,'.');
    if(ext==NULL || expanded==0U) return true;
    if(strcasecmp(ext,".png")!=0 && strcasecmp(ext,".jpg")!=0 &&
       strcasecmp(ext,".ttf")!=0 && strcasecmp(ext,".ttc")!=0 &&
       strcasecmp(ext,".otf")!=0 && strcasecmp(ext,".wav")!=0 &&
       strcasecmp(ext,".mp3")!=0 && strcasecmp(ext,".md")!=0 &&
       strcasecmp(ext,".txt")!=0 && strcasecmp(ext,".json")!=0) return true;
    if(expanded>8U*1024U*1024U || a->expanded+expanded>64U*1024U*1024U ||
       strlen(relative)>240U) return false;
    char path[256];
    strcpy(path,relative);
    char *part=path,*slash;
    int dir=dup(a->destination);
    if(dir<0) return false;
    while((slash=strchr(part,'/'))!=NULL) {
        *slash='\0';
        if(!asset_name(part) || (mkdirat(dir,part,0755)!=0 && errno!=EEXIST)) {
            (void)close(dir);return false;
        }
        int next=openat(dir,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        (void)close(dir);
        if(next<0) return false;
        dir=next;part=slash+1;
    }
    unsigned char *data=NULL;
    size_t size=0U;
    bool ok=asset_name(part) && sc_zip_read(a->zip,a->size,name,expanded,&data,&size) &&
            write_asset(dir,part,data,size);
    free(data);
    (void)close(dir);
    if(ok) a->expanded+=size;
    return ok;
}

static void clean_theme_stage(int root)
{
    DIR *dir=fdopendir(dup(root));
    if(dir==NULL) return;
    struct dirent *entry;
    while((entry=readdir(dir))!=NULL) {
        if(strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        int child=openat(root,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child>=0){
            clean_theme_stage(child);(void)close(child);
            (void)unlinkat(root,entry->d_name,AT_REMOVEDIR);
        } else (void)unlinkat(root,entry->d_name,0);
    }
    (void)closedir(dir);
}

static bool archived_theme(const struct cs_store *store,const char *stem,bool preview)
{
    static unsigned int sequence;
    struct theme_archive a={0};
    struct stat st;
    bool ok=false,created=false;
    char filename[72],temporary[72];
    if(!theme_name(stem) || cs_store_check_media(store)!=CS_STORE_OK) return false;
    int folder=openat(store->card_fd,"Themes",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(folder<0) return false;
    (void)snprintf(filename,sizeof(filename),"%s.zip",stem);
    a.zip=read_asset_bytes(folder,filename,64U*1024U*1024U,&a.size);
    if(a.zip==NULL || !sc_zip_walk(a.zip,a.size,find_archive_config,&a) ||
       a.config[0]=='\0') goto finish;
    (void)snprintf(temporary,sizeof(temporary),".theme-import-%ld-%u",(long)getpid(),++sequence);
    if(preview) {
        char member[512];
        size_t prefix=strlen(a.config)-strlen("config.json"), size=0U;
        unsigned char *data=NULL;
        int run=sc_open_absolute("/run/console",true);
        int n=snprintf(member,sizeof(member),"%.*spreview.png",(int)prefix,a.config);
        if(run>=0 && n>0 && (size_t)n<sizeof(member) &&
           sc_zip_read(a.zip,a.size,member,4U*1024U*1024U,&data,&size)) {
            ok=write_asset(run,temporary,data,size) &&
               renameat(run,temporary,run,"theme-preview.png")==0;
            if(!ok) (void)unlinkat(run,temporary,0);
        }
        if(run>=0) (void)close(run);
        free(data);
        goto finish;
    }
    if(fstatat(folder,stem,&st,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT ||
       mkdirat(folder,temporary,0755)!=0) goto finish;
    created=true;
    a.destination=openat(folder,temporary,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(a.destination>=0) {
        ok=sc_zip_walk(a.zip,a.size,extract_theme_asset,&a) && theme_config(a.destination) &&
           cs_flush_directory(a.destination,store->fat,store->qualified_fat_syncfs)==0 &&
           cs_store_check_media(store)==CS_STORE_OK && renameat(folder,temporary,folder,stem)==0;
        if(!ok) clean_theme_stage(a.destination);
        else created=false;
        (void)close(a.destination);
        if(ok) ok=cs_flush_directory(folder,store->fat,store->qualified_fat_syncfs)==0;
    }
finish:
    if(created) (void)unlinkat(folder,temporary,AT_REMOVEDIR);
    (void)close(folder);
    free(a.zip);
    return ok;
}

static void themes(const struct sc_config *c, const struct cs_store *store,
                   const struct cl_tool_request *r, struct cl_tool_snapshot *s)
{
    struct presentation state = {0};
    char path[512];
    int root;
    DIR *dir;
    struct dirent *entry;
    size_t visited = 0U;
    s->kind = CL_TOOL_THEMES;
    if (!read_presentation(store, &state))
    {
        fail(s, "Theme selection is corrupt; retained without overwrite.");
        return;
    }
    if(r->operation==CL_TOOL_PREVIEW_THEME) {
        if(strncmp(r->id,"zip:",4U)!=0 || !archived_theme(store,r->id+4U,true))
            fail(s,"This ZIP has no readable theme preview.");
        else {s->ready=true;s->enabled_mask=CL_THEME_ARCHIVE_PREVIEW;}
        return;
    }
    if (r->operation == CL_TOOL_APPLY_THEME)
    {
        if (r->revision != state.revision || state.revision == UINT64_MAX)
        {
            fail(s, "Theme selection changed; reload before applying.");
            return;
        }
        const char *selection=r->id;
        if(strncmp(selection,"zip:",4U)==0){
            selection+=4U;
            if(!theme_path(c,store,selection,path) && !archived_theme(store,selection,false)){
                fail(s,"Theme ZIP could not be imported. Existing themes were retained.");
                return;
            }
        }
        if (!theme_path(c, store, selection, path))
        {
            fail(s, "Selected theme has no valid bounded config.json.");
            return;
        }
        if ((r->enabled_mask & CL_THEME_KEEP_ICONS) != 0U && state.fallback[0] == '\0') {
            strcpy(state.fallback, state.theme);
        } else if ((r->enabled_mask & CL_THEME_APPLY_ICONS) != 0U) {
            strcpy(state.pack, "@default");
            state.fallback[0] = '\0';
        }
        strcpy(state.theme, selection);
        ++state.revision;
        if (!write_presentation(store, &state)) {
            fail(s, "Theme write barrier failed; reload before retrying.");
            return;
        }
    }
    if (theme_path(c, store, "@default", path))
        add_theme_row(s, "@default", "System default", path);
    for (size_t i = 0U; i < sizeof(bundled_themes) / sizeof(bundled_themes[0]); ++i)
        if (theme_path(c, store, bundled_themes[i], path))
            add_theme_row(s, bundled_themes[i], bundled_themes[i], path);
    root = openat(store->card_fd, "Themes", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    dir = root < 0 ? NULL : fdopendir(root);
    if (dir != NULL)
    {
        while ((entry = readdir(dir)) != NULL && ++visited <= 512U && s->count < CL_TOOL_ROWS)
        {
            if(theme_name(entry->d_name) && theme_path(c,store,entry->d_name,path)){
                add_theme_row(s,entry->d_name,entry->d_name,path);
                continue;
            }
            size_t length=strlen(entry->d_name);
            if(length<5U || length>=64U || strcmp(entry->d_name+length-4U,".zip")!=0) continue;
            char stem[64],id[64];
            (void)snprintf(stem,sizeof(stem),"%.*s",(int)(length-4U),entry->d_name);
            if(!theme_name(stem) || theme_path(c,store,stem,path)) continue;
            struct stat archive;
            if(fstatat(root,entry->d_name,&archive,AT_SYMLINK_NOFOLLOW)!=0 ||
               !S_ISREG(archive.st_mode) || archive.st_size<22 || archive.st_size>64*1024*1024) continue;
            (void)snprintf(id,sizeof(id),"zip:%.*s",(int)(length-4U),stem);
            add_theme_row(s,id,stem,"ZIP theme");
        }
        (void)closedir(dir);
    }
    else if (root >= 0)
        (void)close(root);
    qsort(s->rows, s->count, sizeof(s->rows[0]), compare_rows);
    if (!theme_path(c, store, state.theme, s->selected_theme))
    {
        if (!theme_path(c, store, "@default", s->selected_theme))
        {
            fail(s, "Selected and default theme are unavailable.");
            return;
        }
        text_copy(s->status, sizeof(s->status),
                  "Selected theme is unavailable; using protected default.");
    }
    else
        text_copy(s->status, sizeof(s->status),
                  "A applies theme. Executable theme scripts are never run.");
    for (size_t i = 0U; i < s->count; ++i)
    {
        s->rows[i].enabled = strcmp(state.theme, s->rows[i].id) == 0;
        s->rows[i].index = (unsigned int)i;
    }
    s->revision = state.revision;
    (void)icon_pack_path(c, store, state.pack, s->icon_pack);
    icon_fallback(c, store, &state, s->icon_theme);
    s->ready = true;
}
static bool icon_pack_path(const struct sc_config *c, const struct cs_store *store,
                           const char *name, char path[512])
{
    char root[512];
    int n;
    path[0] = '\0';
    if (strcmp(name, "@default") == 0) return true;
    if (!pack_key(name)) return false;
    if (strncmp(name, "theme:", 6U) == 0) {
        if (!theme_path(c, store, name + 6U, root)) return false;
        n = snprintf(path, 512U, "%s/icons", root);
    } else {
        n = snprintf(path, 512U, "%s/Icons/%s", store->card_path, name);
    }
    int fd = n > 0 && n < 512 ? sc_open_absolute(path, true) : -1;
    if (fd >= 0) {
        (void)close(fd);
        return true;
    }
    path[0] = '\0';
    return false;
}

bool sc_tools_selected_icons(const struct sc_config *c, const struct cs_store *store,
                             char selected[512], char icon_theme[512])
{
    struct presentation state = {0};
    icon_theme[0] = '\0';
    if (!read_presentation(store, &state)) return false;
    icon_fallback(c, store, &state, icon_theme);
    return icon_pack_path(c, store, state.pack, selected);
}

typedef void (*icon_pack_visitor)(const char *id, const char *path, void *context);
static void scan_icon_packs(const struct sc_config *c, const struct cs_store *store,
                            icon_pack_visitor visit, void *context)
{
    char id[64], path[512];
    for (size_t i = 0U; i < sizeof(bundled_themes) / sizeof(bundled_themes[0]); ++i) {
        (void)snprintf(id, sizeof(id), "theme:%s", bundled_themes[i]);
        if (icon_pack_path(c, store, id, path)) visit(id, path, context);
    }
    for (unsigned int kind = 0U; kind < 2U; ++kind) {
        int root = openat(store->card_fd, kind == 0U ? "Icons" : "Themes",
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *dir = root < 0 ? NULL : fdopendir(root);
        if (dir == NULL) {
            if (root >= 0) (void)close(root);
            continue;
        }
        size_t visited = 0U;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && ++visited <= 512U) {
            if (!theme_name(entry->d_name)) continue;
            int n = snprintf(id, sizeof(id), "%s%s", kind == 0U ? "" : "theme:", entry->d_name);
            if (n > 0 && (size_t)n < sizeof(id) && icon_pack_path(c, store, id, path))
                visit(id, path, context);
        }
        (void)closedir(dir);
    }
}

static void list_pack(const char *id, const char *path, void *context)
{
    struct cl_tool_snapshot *s = context;
    add_theme_row(s, id, strncmp(id, "theme:", 6U) == 0 ? id + 6U : id, path);
}

static void icon_packs(const struct sc_config *c, const struct cs_store *store,
                       const struct cl_tool_request *r, struct cl_tool_snapshot *s)
{
    struct presentation state = {0};
    char path[512];
    s->kind = CL_TOOL_ICON_PACKS;
    if (!read_presentation(store, &state)) {
        fail(s, "Icon selection could not be read.");
        return;
    }
    if (r->operation == CL_TOOL_APPLY_ICON_PACK) {
        if (state.revision == UINT64_MAX || r->revision != state.revision ||
            !icon_pack_path(c, store, r->id, path)) {
            fail(s, "Icon pack is unavailable or the selection changed.");
            return;
        }
        strcpy(state.pack, r->id);
        state.fallback[0] = '\0';
        ++state.revision;
        if (!write_presentation(store, &state)) {
            fail(s, "Icon pack could not be saved.");
            return;
        }
    }
    add_theme_row(s, "@default", "Follow theme", "Use the icons provided by each theme.");
    scan_icon_packs(c, store, list_pack, s);
    bool previous = strcmp(state.pack, "@default") == 0 && state.fallback[0] != '\0';
    if (previous)
        add_theme_row(s, "@previous", "Previous theme icons", "Retained by Theme Switcher.");
    qsort(s->rows + 1U, s->count - 1U, sizeof(s->rows[0]), compare_rows);
    for (size_t i = 0U; i < s->count; ++i) {
        s->rows[i].enabled = strcmp(previous ? "@previous" : state.pack, s->rows[i].id) == 0;
        if (strcmp(s->rows[i].id, "@previous") == 0) s->rows[i].available = false;
        s->rows[i].index = (unsigned int)i;
    }
    s->revision = state.revision;
    (void)icon_pack_path(c, store, state.pack, s->icon_pack);
    icon_fallback(c, store, &state, s->icon_theme);
    s->ready = true;
    text_copy(s->status, sizeof(s->status), "Choose an icon pack from Icons or Themes.");
}

struct icon_choices {
    struct cl_tool_snapshot *snapshot;
    const char *stem;
    enum cl_icon_group group;
};

static void list_icon_folder(const char *pack, const char *root, struct icon_choices *choices)
{
    struct cl_tool_snapshot *s = choices->snapshot;
    int fd = sc_open_absolute(root, true);
    if (fd < 0) return;
    DIR *dir = fdopendir(fd);
    if (dir == NULL) { (void)close(fd); return; }
    size_t stem = strlen(choices->stem), visited = 0U;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && ++visited <= 512U && s->count < CL_TOOL_ROWS) {
        const char *file = entry->d_name;
        size_t size = strlen(file);
        struct stat st;
        if (!theme_name(file) || size <= 4U ||
            strcasecmp(file + size - 4U, ".png") != 0 ||
            (stem != 0U && (size < stem + 4U || strncasecmp(file, choices->stem, stem) != 0 ||
                            (file[stem] != '-' && size != stem + 4U))) ||
            fstatat(fd, file, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
            st.st_nlink != 1U || st.st_size < 24 || st.st_size > 8 * 1024 * 1024) continue;
        char path[256], id[65], label[96];
        int n = snprintf(path, sizeof(path), "%s/%s", root, file);
        if (n < 1 || (size_t)n >= sizeof(path)) continue;
        cs_sha256_hex(path, (size_t)n, id);
        id[32] = '\0';
        if (stem == 0U) (void)snprintf(label, sizeof(label), "%s - %.*s", pack, (int)(size - 4U), file);
        else if (size == stem + 4U) (void)snprintf(label, sizeof(label), "%s", pack);
        else (void)snprintf(label, sizeof(label), "%s - %.*s", pack,
                            (int)(size - stem - 5U), file + stem + 1U);
        add_theme_row(s, id, label, path);
    }
    (void)closedir(dir);
}

static void list_icon_choices(const char *id, const char *path, void *context)
{
    struct icon_choices *choices = context;
    const char *label = strncmp(id, "theme:", 6U) == 0 ? id + 6U : id;
    char root[512];
    const char *folder = choices->group == CL_ICON_APP ? "/app" :
                         choices->group == CL_ICON_EXPERT ? "/rapp" : "";
    int n = snprintf(root, sizeof(root), "%s%s", path, folder);
    if (n > 0 && (size_t)n < sizeof(root)) list_icon_folder(label, root, choices);
    if (choices->group == CL_ICON_EXPERT) {
        n = snprintf(root, sizeof(root), "%s/expert", path);
        if (n > 0 && (size_t)n < sizeof(root)) list_icon_folder(label, root, choices);
        list_icon_folder(label, path, choices);
    }
}

static bool save_icon(const struct cs_store *store, const char *path, const char *target)
{
    char parent[256];
    const char *slash = strrchr(path, '/');
    if (slash == NULL || (size_t)(slash - path) >= sizeof(parent)) return false;
    (void)snprintf(parent, sizeof(parent), "%.*s", (int)(slash - path), path);
    int fd = sc_open_absolute(parent, true);
    if (fd < 0) return false;
    size_t size = 0U;
    unsigned char *bytes = read_asset_bytes(fd, slash + 1U, 8U*1024U*1024U, &size);
    (void)close(fd);
    if (bytes == NULL) return false;
    unsigned int width, height;
    unsigned char *rgba = NULL;
    LodePNGState png;
    lodepng_state_init(&png);
    png.decoder.zlibsettings.max_output_size = 8U * 1024U * 1024U;
    png.decoder.max_text_size = 65536U;
    png.decoder.max_icc_size = 65536U;
    bool valid = size <= 8U * 1024U * 1024U &&
        lodepng_inspect(&width, &height, &png, bytes, size) == 0U &&
        width > 0U && height > 0U && width <= 2048U && height <= 2048U &&
        (uint64_t)width * height <= 1048576U &&
        lodepng_decode(&rgba, &width, &height, &png, bytes, size) == 0U;
    lodepng_state_cleanup(&png);
    free(rgba);
    bool saved = valid && publish_presentation_asset(store, target, bytes, size);
    free(bytes);
    return saved;
}

static void icon_editor(const struct sc_config *c, const struct cs_store *store,
                        const struct cl_tool_request *r, struct cl_tool_snapshot *s)
{
    s->kind = CL_TOOL_ICON_EDITOR;
    if (r->enabled_mask >= CL_ICON_GROUP_COUNT) { fail(s, "Invalid icon category."); return; }
    enum cl_icon_group group = (enum cl_icon_group)r->enabled_mask;
    size_t count;
    const struct cl_icon_slot *slots = cl_icon_slots(group, &count);
    s->available_mask = r->enabled_mask;
    if (r->operation == CL_TOOL_LOAD_ICON_SLOTS) {
        for (size_t i = 0U; i < count; ++i)
            add_theme_row(s, slots[i].id, slots[i].label, "");
        s->ready = true;
        return;
    }
    size_t slot = 0U;
    while (slot < count && strcmp(slots[slot].id, r->target) != 0) ++slot;
    if (slot == count) { fail(s, "Invalid icon slot."); return; }
    s->enabled_mask = 1U;
    text_copy(s->status, sizeof(s->status), slots[slot].label);
    add_theme_row(s, "@default", "Use pack icon", "");
    struct icon_choices choices = {s, slots[slot].stem, group};
    scan_icon_packs(c, store, list_icon_choices, &choices);
    /* Zero 28-only apps have no corresponding filename in stock Onion
     * packs. Offer their existing app artwork if no dedicated icon exists. */
    if (group == CL_ICON_APP && s->count == 1U) {
        choices.stem = "";
        scan_icon_packs(c, store, list_icon_choices, &choices);
    }
    qsort(s->rows + 1U, s->count - 1U, sizeof(s->rows[0]), compare_rows);
    if (r->operation == CL_TOOL_APPLY_ICON) {
        char target[64];
        (void)snprintf(target, sizeof(target), "icon-%s-%s.png", cl_icon_group_prefix(group), slots[slot].id);
        bool saved = false;
        if (strcmp(r->id, "@default") == 0) {
            saved = cs_store_check_media(store) == CS_STORE_OK &&
                (unlinkat(store->data_fd, target, 0) == 0 || errno == ENOENT) &&
                cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) == 0;
        } else {
            for (size_t i = 1U; i < s->count; ++i)
                if (strcmp(r->id, s->rows[i].id) == 0)
                    saved = save_icon(store, s->rows[i].detail, target);
        }
        if (!saved) { fail(s, "Icon could not be saved. Choose a readable PNG."); return; }
        s->enabled_mask = 2U;
    }
    s->ready = true;
}

static bool clear_setting(const struct cs_store *store, int parent, const char *name,
                          unsigned int directory_depth)
{
    struct stat st;
    if (cs_store_check_media(store) != CS_STORE_OK) return false;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    if (S_ISREG(st.st_mode)) return unlinkat(parent, name, 0) == 0;
    if (!S_ISDIR(st.st_mode) || directory_depth == 0U) return false;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *dir = fdopendir(fd);
    if (dir == NULL) {
        close(fd);
        return false;
    }
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            ok = errno == 0;
            break;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            ok = clear_setting(store, fd, entry->d_name, directory_depth - 1U);
    }
    if (ok) ok = cs_flush_directory(fd, store->fat, store->qualified_fat_syncfs) == 0;
    closedir(dir);
    return ok && cs_store_check_media(store) == CS_STORE_OK &&
           unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

static bool reset_profile(const struct cs_store *store, const char *name,
                          const char *const *settings, size_t count, unsigned int depth)
{
    int fd = openat(store->data_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    bool ok = true;
    for (size_t i = 0; i < count; ++i)
        if (!clear_setting(store, fd, settings[i], depth)) ok = false;
    if (cs_flush_directory(fd, store->fat, store->qualified_fat_syncfs) != 0) ok = false;
    close(fd);
    return ok;
}

static bool reset_native_profiles(const struct cs_store *store, uint64_t scope)
{
    int fd=openat(store->data_fd,"retroarch-profiles",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT;
    DIR *games=fdopendir(fd);if(games==NULL){close(fd);return false;}
    bool ok=true;size_t visited=0U;
    for(;;){
        errno=0;struct dirent *game=readdir(games);
        if(game==NULL){if(errno!=0)ok=false;break;}
        if(strcmp(game->d_name,".")==0||strcmp(game->d_name,"..")==0)continue;
        if(!cs_hex_valid(game->d_name,64U)){ok=false;break;}
        int gamefd=openat(fd,game->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(gamefd<0){ok=false;break;}
        DIR *cores=fdopendir(gamefd);if(cores==NULL){close(gamefd);ok=false;break;}
        for(;;){
            errno=0;struct dirent *core=readdir(cores);
            if(core==NULL){if(errno!=0)ok=false;break;}
            if(strcmp(core->d_name,".")==0||strcmp(core->d_name,"..")==0)continue;
            if(!cs_hex_valid(core->d_name,64U)||++visited>16384U){ok=false;break;}
            int profile=openat(gamefd,core->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(profile<0){ok=false;break;}
            if(scope==CL_RESET_RETROARCH||scope==CL_RESET_ALL)
                ok=clear_setting(store,profile,"retroarch.cfg",0U)&&ok;
            if(scope==CL_RESET_CORE_OVERRIDES||scope==CL_RESET_ALL){
                const char *const names[]={"core-options.cfg","config","remaps"};
                for(size_t i=0U;i<sizeof(names)/sizeof(names[0]);++i)
                    ok=clear_setting(store,profile,names[i],4U)&&ok;
            }
            if(cs_flush_directory(profile,store->fat,store->qualified_fat_syncfs)!=0)ok=false;
            close(profile);if(!ok)break;
        }
        closedir(cores);if(!ok)break;
    }
    closedir(games);
    return ok&&cs_store_check_media(store)==CS_STORE_OK;
}

static void reset_config(const struct sc_config *c, const struct cs_store *store,
                         uint64_t scope, struct cl_tool_snapshot *s)
{
    s->kind = CL_TOOL_MAINTENANCE;
    if (scope < CL_RESET_RETROARCH || scope > CL_RESET_ALL ||
        cs_store_check_media(store) != CS_STORE_OK) {
        fail(s, "Reset needs a writable, qualified SD card.");
        return;
    }
    const char *const retroarch[] = {"retroarch.cfg"};
    const char *const cores[] = {"core-options.cfg", "config", "remaps"};
    const char *const advance[] = {"advmenu.rc", "advmame.rc", "advmess.rc"};
    const char *const mainui[] = {"presentation.json", "theme.json", "icons.json"};
    bool ok = true;
    if (scope == CL_RESET_RETROARCH || scope == CL_RESET_ALL)
        ok = reset_profile(store, "standalone-retroarch", retroarch, 1U, 0U);
    if (scope == CL_RESET_CORE_OVERRIDES || scope == CL_RESET_ALL)
        ok = reset_profile(store, "standalone-retroarch", cores, 3U, 4U) && ok;
    if (scope == CL_RESET_RETROARCH || scope == CL_RESET_CORE_OVERRIDES || scope == CL_RESET_ALL)
        ok = reset_native_profiles(store,scope) && ok;
    if (scope == CL_RESET_ADVANCE || scope == CL_RESET_ALL)
        ok = reset_profile(store, "standalone-advance", advance, 3U, 0U) && ok;
    if (scope == CL_RESET_ALL) {
        ok = sc_network_reset(store) && ok;
        for (size_t i = 0; i < sizeof(mainui) / sizeof(*mainui); ++i)
            if (!clear_setting(store, store->data_fd, mainui[i], 0U)) ok = false;
        for (unsigned int group = 0; group < CL_ICON_GROUP_COUNT; ++group) {
            size_t count = 0U;
            const struct cl_icon_slot *slots = cl_icon_slots(group, &count);
            for (size_t i = 0; i < count; ++i) {
                char icon[96];
                snprintf(icon, sizeof(icon), "icon-%s-%s.png", cl_icon_group_prefix(group), slots[i].id);
                if (!clear_setting(store, store->data_fd, icon, 0U)) ok = false;
            }
        }
        (void)theme_path(c, store, "@default", s->selected_theme);
    }
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0 ||
        cs_store_check_media(store) != CS_STORE_OK || !ok) {
        fail(s, "Some settings could not be reset. Check the SD card and try again.");
        return;
    }
    s->ready = true;
    s->enabled_mask = scope;
    strcpy(s->status, "Configuration reset. Games and saves were kept.");
}

static void import_recent_game(const struct sc_config *config, struct cl_history *history,
                               const char *path)
{
    if (history->count == CL_HISTORY_MAX) return;
    const char *roots[] = {config->base.card_path, config->library_secondary};
    const struct sc_game *found = NULL;
    for (size_t i = 0; i < config->game_count; ++i) {
        const struct sc_game *game = &config->games[i];
        bool same = strcmp(game->content, path) == 0;
        for (size_t j = 0U; !same && j < sizeof(roots) / sizeof(*roots); ++j) {
            size_t root = strlen(roots[j]);
            if (root != 0U && strncmp(game->content, roots[j], root) == 0 &&
                strncmp(game->content + root, "/Roms/", 6U) == 0)
                same = strcmp(game->content + root + 6U, path) == 0 ||
                       strcmp(game->content + root + 1U, path) == 0;
        }
        if (!same) continue;
        if (found != NULL) return; /* A relative path can exist on both cards. */
        found = game;
    }
    if (found == NULL) return;
    for (size_t j = 0; j < history->count; ++j)
        if (strcmp(history->entries[j].identity.game, found->entry.identity.game) == 0) return;
    struct sc_game resolved;
    if (sc_core_resolve(config, found->entry.identity.game, &resolved))
        history->entries[history->count++] = resolved.entry;
}

static void import_retroarch_history(const struct sc_config *c, struct cl_history *history,
                                     const char *relative)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", c->base.card_path, relative);
    if (length <= 0 || (size_t)length >= sizeof(path)) return;
    int fd = sc_open_absolute(path, false);
    if (fd < 0) return;
    struct stat st;
    char *data = NULL;
    struct cs_document *doc = NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > CS_DOC_BYTES)
        goto done;
    data = malloc((size_t)st.st_size);
    doc = malloc(sizeof(*doc));
    if (data == NULL || doc == NULL) goto done;
    size_t used = 0U;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (!cs_doc_parse(doc, data, used)) goto done;
    size_t items = cs_doc_get(doc, 0U, "items");
    if (items >= doc->count || doc->tokens[items].type != CS_DOC_ARRAY) goto done;
    for (size_t i = items + 1U; i < doc->tokens[items].next && history->count < CL_HISTORY_MAX;
         i = doc->tokens[i].next) {
        char rom[512];
        if (cs_doc_text(doc, cs_doc_get(doc, i, "path"), rom, sizeof(rom)))
            import_recent_game(c, history, rom);
    }
done:
    close(fd);
    free(data);
    free(doc);
}

static void regenerate_history(const struct sc_config *c, const struct cs_store *store,
                                struct cl_tool_snapshot *s)
{
    s->kind = CL_TOOL_MAINTENANCE;
    struct cl_history *history = calloc(1U, sizeof(*history));
    struct ca_snapshot *recent = malloc(sizeof(*recent));
    if (history == NULL || recent == NULL) {
        fail(s, "GameSwitcher history could not be read.");
        goto done;
    }
    if (ca_recent_read(store, recent) == CA_OK)
        for (size_t i = 0; i < recent->count; ++i)
            import_recent_game(c, history, recent->entries[i].game.relative_path);
    import_retroarch_history(c, history, ".console/standalone-retroarch/content_history.lpl");
    import_retroarch_history(c, history, "RetroArch/.retroarch/content_history.lpl");
    if (history->count == 0U) {
        fail(s, "No matching games in Activity Tracker or RetroArch history. Current list retained.");
        goto done;
    }
    struct cs_store qualified = *store;
    if (cl_history_regenerate(&qualified, history->entries, history->count) != CL_HISTORY_OK) {
        fail(s, "GameSwitcher history could not be regenerated.");
        goto done;
    }
    s->ready = true;
    s->enabled_mask = CL_HISTORY_REGENERATED;
    (void)snprintf(s->status, sizeof(s->status), CL_TR("GameSwitcher list regenerated from %zu played games."), history->count);
done:
    free(recent);
    free(history);
}

static int browser_row_compare(const void *left,const void *right)
{
    const struct cl_tool_row *a=left,*b=right;
    if(a->required!=b->required) return a->required?-1:1;
    int order=strcasecmp(a->label,b->label);
    return order!=0?order:strcmp(a->label,b->label);
}

static int gallery_row_compare(const void *left, const void *right)
{
    const struct cl_tool_row *a = left, *b = right;
    return strcmp(a->detail, b->detail);
}

static int browser_directory(const struct cs_store *store,const char *relative)
{
    char absolute[512];
    struct stat st;
    if(cs_store_check_media(store)!=CS_STORE_OK) return -1;
    if(relative[0]=='\0') return openat(store->card_fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int n=snprintf(absolute,sizeof(absolute),"%s/%s",store->card_path,relative);
    if(n<0 || (size_t)n>=sizeof(absolute)) return -1;
    int fd=sc_open_absolute(absolute,true);
    if(fd>=0 && (fstat(fd,&st)!=0 || st.st_dev!=store->device)){
        (void)close(fd);
        return -1;
    }
    return fd;
}

static void browser_id(const char *name,char id[64])
{
    char hash[65];
    cs_sha256_hex(name,strlen(name),hash);
    memcpy(id,hash,63U);
    id[63]='\0';
}

static bool browser_scan(const struct cs_store *store,const char *relative,bool gallery,
                         const char *enter,char destination[512],struct cl_tool_snapshot *s,
                         struct cl_tool_row *rows, size_t capacity)
{
    int fd=browser_directory(store,relative);
    if(fd<0) return false;
    DIR *dir=fdopendir(fd);
    if(dir==NULL){(void)close(fd);return false;}
    struct dirent *entry;
    size_t visited=0U;
    bool found=false;
    while((entry=readdir(dir))!=NULL && ++visited<=4096U){
        struct stat st;
        const char *name=entry->d_name;
        if(name[0]=='.' || !cs_utf8_valid(name,strlen(name)) ||
           fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)!=0 || st.st_dev!=store->device ||
           (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) continue;
        char id[64];
        browser_id(name,id);
        if(enter!=NULL){
            if(!S_ISDIR(st.st_mode) || strcmp(enter,id)!=0) continue;
            int n=snprintf(destination,512U,"%s%s%s",relative,relative[0]=='\0'?"":"/",name);
            found=n>0 && n<512;
            break;
        }
        const char *ext=strrchr(name,'.');
        if(gallery && (!S_ISREG(st.st_mode) || ext==NULL ||
           (strcasecmp(ext,".png")!=0 && strcasecmp(ext,".jpg")!=0 &&
            strcasecmp(ext,".jpeg")!=0 && strcasecmp(ext,".bmp")!=0))) continue;
        if(s->count==capacity) continue;
        struct cl_tool_row *row=&rows[s->count];
        if(gallery){
            int n=snprintf(row->detail,sizeof(row->detail),"%s/Screenshots/%s",store->card_path,name);
            if(n<0 || (size_t)n>=sizeof(row->detail)) continue;
        } else if(S_ISDIR(st.st_mode)) {
            strcpy(row->detail,"Folder");
        } else {
            (void)snprintf(row->detail,sizeof(row->detail),"%" PRIuMAX " bytes",(uintmax_t)st.st_size);
        }
        size_t length=strlen(name);
        if(length>=sizeof(row->label)) length=sizeof(row->label)-1U;
        while(length>0U && ((unsigned char)name[length]&0xc0U)==0x80U) --length;
        memcpy(row->label,name,length);row->label[length]='\0';
        strcpy(row->id,id);
        row->required=S_ISDIR(st.st_mode);
        row->available=true;
        ++s->count;
    }
    if(closedir(dir)!=0 || cs_store_check_media(store)!=CS_STORE_OK) return false;
    if(enter!=NULL) return found;
    qsort(rows,s->count,sizeof(*rows),gallery ? gallery_row_compare : browser_row_compare);
    return true;
}

static void browser_request(struct sc_tools_backend *b,const struct cs_store *store,
                            const struct cl_tool_request *r,struct cl_tool_snapshot *s)
{
    const bool gallery=r->operation==CL_TOOL_LOAD_GALLERY;
    s->kind=gallery?CL_TOOL_GALLERY:CL_TOOL_FILES;
    struct sp_package_selection selection;
    if(!sc_tools_packages(b,store,&selection) ||
       !sp_package_enabled(&selection,gallery?"app.gallery":"app.files")){
        fail(s,"Enable this app in Package Manager.");
        return;
    }
    if(gallery){
        struct cl_tool_row *rows = calloc(CL_GALLERY_MAX, sizeof(*rows));
        if (rows == NULL) { fail(s, "Could not allocate the screenshot list."); return; }
        bool ok = browser_scan(store,"Screenshots",true,NULL,NULL,s,rows,CL_GALLERY_MAX);
        int scan_error = errno;
        if (ok && s->count != 0U) {
            size_t offset = r->enabled_mask < s->count ? (size_t)r->enabled_mask : s->count - 1U;
            offset -= offset % CL_TOOL_ROWS;
            s->available_mask = s->count;
            s->enabled_mask = offset;
            s->count -= offset;
            if (s->count > CL_TOOL_ROWS) s->count = CL_TOOL_ROWS;
            memcpy(s->rows, rows + offset, s->count * sizeof(*rows));
        }
        free(rows);
        if (!ok) {
            s->count = 0U;
            if (scan_error != ENOENT) { fail(s,"Could not read screenshots."); return; }
        }
    } else {
        char directory[sizeof(b->files_directory)];
        strcpy(directory,b->files_directory);
        if(r->operation==CL_TOOL_LOAD_FILES) directory[0]='\0';
        else if(r->operation==CL_TOOL_PARENT_DIRECTORY){
            char *slash=strrchr(directory,'/');
            if(slash!=NULL) *slash='\0';
            else directory[0]='\0';
        } else if(!browser_scan(store,b->files_directory,false,r->id,directory,s,s->rows,CL_TOOL_ROWS)){
            fail(s,"Folder is unavailable. Press A to return to the card root.");
            return;
        }
        if(!browser_scan(store,directory,false,NULL,NULL,s,s->rows,CL_TOOL_ROWS)){fail(s,"Could not read the folder.");return;}
        strcpy(b->files_directory,directory);
        s->enabled_mask=directory[0]!='\0'?1U:0U;
        (void)snprintf(s->status,sizeof(s->status),"/%.159s",directory);
    }
    s->ready=true;
}

void sc_tools_request(struct sc_tools_backend *b, const struct sc_config *c,
                      const struct cs_store *store, const struct cl_tool_request *r,
                      struct cl_tool_snapshot *s)
{
    memset(s, 0, sizeof(*s));
    if ((unsigned int)r->operation > CL_TOOL_LOAD_BATTERY ||
        strnlen(r->id, sizeof(r->id)) == sizeof(r->id) || strnlen(r->target, sizeof(r->target)) == sizeof(r->target))
    {
        s->kind = cl_tool_request_kind(r);
        fail(s, "Tool request contains an unterminated identifier.");
        return;
    }
    if (r->operation == CL_TOOL_LOAD_BATTERY) {
        struct sp_package_selection selection;
        s->kind = CL_TOOL_BATTERY;
        if (!sc_tools_packages(b,store,&selection) || !sp_package_enabled(&selection,"app.battery"))
            fail(s,"Enable Battery Monitor in Package Manager.");
        else if (!sc_battery_history_read(store, &s->battery))
            fail(s,"Battery history could not be read.");
        else s->ready = true;
    }
    else if (r->operation >= CL_TOOL_LOAD_FILES && r->operation <= CL_TOOL_LOAD_GALLERY)
        browser_request(b,store,r,s);
    else if (r->operation == CL_TOOL_RESET_CONFIG)
        reset_config(c, store, r->enabled_mask, s);
    else if (r->operation == CL_TOOL_REGENERATE_HISTORY)
        regenerate_history(c, store, s);
    else if (r->operation == CL_TOOL_LOAD_GLO_SCRIPTS)
        sc_glo_scripts(c, r, s);
    else if (r->operation == CL_TOOL_LOAD_ICON_PACKS || r->operation == CL_TOOL_APPLY_ICON_PACK)
        icon_packs(c, store, r, s);
    else if (r->operation >= CL_TOOL_LOAD_ICON_SLOTS && r->operation <= CL_TOOL_APPLY_ICON)
        icon_editor(c, store, r, s);
    else if(cl_tool_maintenance_file_job(r->operation))
        sc_maintenance_request(c,store,r->operation,s);
    else if(r->operation>=CL_TOOL_LOAD_CLOCK && r->operation<=CL_TOOL_SET_TIMEZONE)
        sc_clock_request(store,r,s);
    else if (r->operation == CL_TOOL_LOAD_CORES || r->operation == CL_TOOL_SET_CORE)
        sc_core_options(c,store,r,s);
    else if (r->operation >= CL_TOOL_LOAD_SCRAPER && r->operation <= CL_TOOL_DOWNLOAD_PACKAGE)
        sc_online_request(c, store, r, s, &b->cancel);
    else if (r->operation == CL_TOOL_LOAD_DEVICE)
        sc_device_snapshot(store, s);
    else if (r->operation >= CL_TOOL_LOAD_NETWORK && r->operation <= CL_TOOL_FORGET_NETWORK)
        sc_network_request(store, r, s);
    else if (r->operation <= CL_TOOL_ROLLBACK_PACKAGES)
        packages(b, c, store, r, s);
    else if (r->operation == CL_TOOL_LOAD_ACTIVITY)
    {
        struct sp_package_selection selection;
        s->kind = CL_TOOL_ACTIVITY;
        if (!sc_tools_packages(b, store, &selection) ||
            !sp_package_enabled(&selection, "app.activity"))
            fail(s, "Activity Tracker is disabled or package selection is unavailable.");
        else
            activity(store, r, s);
    }
    else if (r->operation == CL_TOOL_LOAD_THEMES || r->operation == CL_TOOL_APPLY_THEME ||
             r->operation == CL_TOOL_PREVIEW_THEME)
    {
        struct sp_package_selection selection;
        s->kind = CL_TOOL_THEMES;
        if (!sc_tools_packages(b, store, &selection) ||
            !sp_package_enabled(&selection, "app.themes"))
            fail(s, "Theme Switcher is disabled or package selection is unavailable.");
        else
            themes(c, store, r, s);
    }
    else
        fail(s, "Unknown tool request.");
}
