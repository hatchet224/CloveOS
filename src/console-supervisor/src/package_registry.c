#include "supervisor/packages.h"
#include "sessiond/document.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define BIT(n) (UINT64_C(1) << (n))
#define ALL ((UINT64_C(1) << SP_PACKAGE_COUNT) - 1U)
static const struct sp_package packages[SP_PACKAGE_COUNT] = {
    {"runtime.sdl", "SDL2", "", "system/lib/libSDL2-2.0.so.0", SP_PACKAGE_RUNTIME, 0U, true, true},
    {"runtime.ttf", "SDL2 TTF", "", "system/lib/libSDL2_ttf-2.0.so.0", SP_PACKAGE_RUNTIME, BIT(0), true, true},
    {"runtime.frontend", "Console frontend", "", "system/bin/console-launcher", SP_PACKAGE_RUNTIME, BIT(0)|BIT(1), true, true},
    {"runtime.retroarch", "RetroArch", "", "system/bin/retroarch", SP_PACKAGE_RUNTIME, BIT(0), true, true},
    {"core.gambatte", "Nintendo - Game Boy / Color (Gambatte)", "GB|GBC", "system/lib/libretro/gambatte_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.mgba", "Nintendo - Game Boy Advance (mGBA)", "GBA", "system/lib/libretro/mgba_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.fceumm", "Nintendo - NES (FCEUmm)", "FC", "system/lib/libretro/fceumm_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.snes9x", "Nintendo - Super Nintendo (Snes9x 2005 Plus)", "SFC", "system/lib/libretro/snes9x2005_plus_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.genesis", "Sega - Mega Drive / Master System / Game Gear (PicoDrive)", "MD|MS|GG", "system/lib/libretro/picodrive_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.pcsx", "Sony - PlayStation (PCSX ReARMed)", "PS", "system/lib/libretro/pcsx_rearmed_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"app.package-manager", "Package Manager", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.activity", "Activity Tracker", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, true},
    {"app.themes", "Theme Switcher", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, true},
    {"core.mupen64plus", "Nintendo - Nintendo 64 (Mupen64Plus Next)", "N64", "system/lib/libretro/mupen64plus_next_libretro.so", SP_PACKAGE_EXPERT, BIT(3), false, true},
    {"core.flycast", "Sega - Dreamcast (Flycast)", "DC", "system/lib/libretro/flycast_libretro.so", SP_PACKAGE_EXPERT, BIT(3), false, true},
    {"core.ppsspp", "Sony - PSP (PPSSPP)", "PSP", "system/lib/libretro/ppsspp_libretro.so", SP_PACKAGE_EXPERT, BIT(3), false, true},
    {"core.fbneo", "Arcade / Neo Geo (FinalBurn Neo)", "ARCADE|FBNEO|NEOGEO", "system/lib/libretro/fbneo_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.pce-fast", "NEC - PC Engine / TurboGrafx-16", "PCE", "system/lib/libretro/mednafen_pce_fast_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.fake08", "PICO-8 (FAKE-08)", "PICO", "system/lib/libretro/fake08_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"app.gameswitcher", "GameSwitcher (Shortcut)", "", "system/bin/GameSwitcher", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.tweaks", "Tweaks", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.clock", "Clock", "", "system/bin/console-time-sync", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.advancemenu", "AdvanceMENU (Alternative frontend)", "", "system/bin/advmenu", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.retroarch", "RetroArch (Shortcut)", "", "system/bin/retroarch", SP_PACKAGE_APPS, BIT(2)|BIT(3), true, true},
    {"app.screenscraper", "ScreenScraper", "", "system/bin/console-supervisor", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.file-transfer", "File transfer", "", "system/bin/console-fileserver", SP_PACKAGE_APPS, BIT(2), true, true},
    {"app.files", "File Explorer", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, true},
    {"app.gallery", "Gallery", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, true},
    {"core.mame2003-plus", "Arcade (MAME 2003-Plus)", "MAME2003PLUS", "system/lib/libretro/mame2003_plus_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"core.stella2014", "Atari - 2600 (Stella 2014)", "ATARI", "system/lib/libretro/stella2014_libretro.so", SP_PACKAGE_VERIFIED, BIT(3), false, true},
    {"app.expert", "Expert (Shortcut)", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, false},
    {"app.list-shortcuts", "List shortcuts (Filter+Refresh)", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, false},
    {"app.search", "Search (Find your games)", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, true},
    {"app.battery", "Battery Monitor", "", "system/bin/console-launcher", SP_PACKAGE_APPS, BIT(2), false, false}
};
const struct sp_package *sp_package_at(size_t index)
{
    return index < SP_PACKAGE_COUNT ? &packages[index] : NULL;
}
int sp_package_index(const char *id)
{
    size_t i;
    if (id == NULL) return -1;
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        if (strcmp(id, packages[i].id) == 0) return (int)i;
    }
    return -1;
}
static int protected_open(int root, const char *path)
{
    char copy[SP_PACKAGE_PATH], *part, *save = NULL;
    struct stat st;
    int parent, result = -1;
    if (path == NULL || path[0] == '/' || strlen(path) >= sizeof(copy) ||
        strstr(path, "//") != NULL || fstat(root, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != 0U || (st.st_mode & 0022U) != 0U) return -1;
    strcpy(copy, path);
    parent = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (parent < 0) return -1;
    part = strtok_r(copy, "/", &save);
    while (part != NULL) {
        char *next = strtok_r(NULL, "/", &save);
        int fd;
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) break;
        fd = openat(parent, part, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|
                     (next == NULL ? 0 : O_DIRECTORY));
        if (fd < 0) break;
        if (fstat(fd, &st) != 0 || st.st_uid != 0U || (st.st_mode & 0022U) != 0U ||
            (next != NULL ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1U))) {
            (void)close(fd); break;
        }
        (void)close(parent); parent = fd;
        if (next == NULL) { result = parent; parent = -1; break; }
        part = next;
    }
    if (parent >= 0) (void)close(parent);
    return result;
}
static bool hash_native(int root, const char *path, const char expected[65])
{
    int fd = protected_open(root, path);
    struct stat before, after;
    struct cs_sha256 hash;
    char digest[65];
    unsigned char block[8192];
    uint64_t total = 0U;
    bool ok = false;
    if (fd < 0) return false;
    if (fstat(fd, &before) != 0 || before.st_size < 64 || before.st_size > 134217728) goto done;
    cs_sha256_init(&hash);
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || total + (uint64_t)(n > 0 ? n : 0) > 134217728U) goto done;
        if (n == 0) break;
        if (total == 0U && (n < 64 || memcmp(block, "\177ELF\002\001", 6U) != 0 ||
            block[18] != 183U || block[19] != 0U ||
            (block[16] != 2U && block[16] != 3U) || block[17] != 0U)) goto done;
        cs_sha256_update(&hash, block, (size_t)n); total += (uint64_t)n;
    }
    cs_sha256_final(&hash, digest);
    ok = strcmp(digest, expected) == 0 && total == (uint64_t)before.st_size &&
         fstat(fd, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
done:
    if (close(fd) != 0) ok = false;
    return ok;
}
static bool registry_read(int root, const char *manifest,
                           struct sp_package_registry *registry, bool authenticate)
{
    struct sp_package_registry parsed = {0};
    struct cs_document *doc = NULL;
    char *bytes = NULL;
    struct stat st;
    int fd = protected_open(root, manifest), schema;
    size_t used = 0U, node, item, i, j;
    uint64_t found = 0U;
    struct cs_sha256 hash;
    bool ok = false;
    if (registry == NULL || fd < 0) goto done;
    if (fstat(fd, &st) != 0 || st.st_size < 1 || st.st_size > 131072) goto done;
    bytes = malloc((size_t)st.st_size + 1U); doc = calloc(1U, sizeof(*doc));
    if (bytes == NULL || doc == NULL) goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (!cs_doc_parse(doc, bytes, used) || doc->tokens[0].type != CS_DOC_OBJECT ||
        !cs_doc_int(doc, cs_doc_get(doc, 0U, "schema"), &schema) || schema != 2) goto done;
    node = cs_doc_get(doc, 0U, "artifacts");
    if (node == SIZE_MAX || doc->tokens[node].type != CS_DOC_ARRAY) goto done;
    for (item = node + 1U; item < doc->tokens[node].next; item = doc->tokens[item].next) {
        char path[SP_PACKAGE_PATH], digest[65];
        if (doc->tokens[item].type != CS_DOC_OBJECT ||
            !cs_doc_text(doc, cs_doc_get(doc, item, "path"), path, sizeof(path)) ||
            !cs_doc_text(doc, cs_doc_get(doc, item, "sha256"), digest, sizeof(digest)) ||
            !cs_hex_valid(digest, 64U)) goto done;
        for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
            if (path[0] == '/' && strcmp(path + 1, packages[i].artifact) == 0) {
                if ((found & BIT(i)) != 0U) goto done;
                found |= BIT(i); strcpy(parsed.artifact_sha256[i], digest);
            }
        }
    }
    if (found != ALL) goto done;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, "native-package-registry-v1", 26U);
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        bool available;
        for (j = 0U; j < i; ++j) if (strcmp(packages[i].artifact, packages[j].artifact) == 0) break;
        if (j < i) available = (parsed.available & BIT(j)) != 0U;
        else if (authenticate) available = hash_native(root, packages[i].artifact, parsed.artifact_sha256[i]);
        else {
            unsigned char header[64];
            int artifact = protected_open(root, packages[i].artifact);
            struct stat info;
            available = artifact >= 0 && fstat(artifact, &info) == 0 && info.st_size >= 64 &&
                info.st_size <= 134217728 && pread(artifact, header, sizeof(header), 0) == (ssize_t)sizeof(header) &&
                memcmp(header, "\177ELF\002\001", 6U) == 0 && header[18] == 183U && header[19] == 0U &&
                (header[16] == 2U || header[16] == 3U) && header[17] == 0U;
            if (artifact >= 0 && close(artifact) != 0) available = false;
        }
        if (available) parsed.available |= BIT(i);
        char policy[80];
        int n = snprintf(policy, sizeof(policy), "%u:%016" PRIx64 ":%u:%u",
                         (unsigned int)packages[i].layer, packages[i].dependencies,
                         packages[i].required ? 1U : 0U, packages[i].default_enabled ? 1U : 0U);
        if (n < 0 || (size_t)n >= sizeof(policy)) goto done;
        cs_sha256_update(&hash, packages[i].id, strlen(packages[i].id) + 1U);
        cs_sha256_update(&hash, packages[i].systems, strlen(packages[i].systems) + 1U);
        cs_sha256_update(&hash, policy, (size_t)n + 1U);
        cs_sha256_update(&hash, parsed.artifact_sha256[i], 65U);
    }
    /* Dependencies are ordered before dependents in the fixed table. */
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        if ((packages[i].dependencies & parsed.available) != packages[i].dependencies)
            parsed.available &= ~BIT(i);
        if (packages[i].required && (parsed.available & BIT(i)) == 0U) goto done;
    }
    cs_sha256_final(&hash, parsed.fingerprint);
    ok = true;
done:
    if (fd >= 0 && close(fd) != 0) ok = false;
    free(bytes); free(doc);
    if (ok) *registry = parsed;
    return ok;
}
bool sp_package_registry_load(int root, const char *manifest, struct sp_package_registry *registry)
{ return registry_read(root, manifest, registry, true); }
bool sp_package_registry_describe(int root, const char *manifest, struct sp_package_registry *registry)
{ return registry_read(root, manifest, registry, false); }
uint64_t sp_package_defaults(const struct sp_package_registry *registry)
{
    uint64_t mask = 0U;
    size_t i;
    if (registry == NULL) return 0U;
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i)
        if (packages[i].default_enabled && (registry->available & BIT(i)) != 0U) mask |= BIT(i);
    return mask;
}
bool sp_package_selection_valid(const struct sp_package_registry *registry, uint64_t enabled)
{
    size_t i;
    if (registry == NULL || (enabled & ~ALL) != 0U || (enabled & ~registry->available) != 0U) return false;
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        if (packages[i].required && (enabled & BIT(i)) == 0U) return false;
        if ((enabled & BIT(i)) != 0U && (packages[i].dependencies & enabled) != packages[i].dependencies) return false;
    }
    return true;
}
void sp_package_plan_begin(struct sp_package_plan *plan, const struct sp_package_selection *selection)
{
    if (plan == NULL) return;
    memset(plan, 0, sizeof(*plan));
    if (selection == NULL) return;
    plan->revision = selection->revision;
    plan->original = plan->desired = selection->enabled;
}
enum sp_package_status sp_package_plan_set(const struct sp_package_registry *registry,
                                            struct sp_package_plan *plan, size_t index, bool enabled)
{
    uint64_t desired;
    size_t i;
    bool changed;
    if (registry == NULL || plan == NULL || index >= SP_PACKAGE_COUNT) return SP_PACKAGE_INCOMPATIBLE;
    if (packages[index].required && !enabled) return SP_PACKAGE_REQUIRED;
    desired = enabled ? plan->desired | BIT(index) : plan->desired & ~BIT(index);
    do {
        changed = false;
        for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
            uint64_t before = desired;
            if ((desired & BIT(i)) == 0U) continue;
            if (enabled) desired |= packages[i].dependencies;
            else if ((packages[i].dependencies & desired) != packages[i].dependencies) {
                if (packages[i].required) return SP_PACKAGE_REQUIRED;
                desired &= ~BIT(i);
            }
            changed = changed || before != desired;
        }
    } while (changed);
    if (!sp_package_selection_valid(registry, desired)) return SP_PACKAGE_UNAVAILABLE;
    plan->desired = desired; plan->installs = plan->removals = 0U;
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        if ((desired & BIT(i)) != 0U && (plan->original & BIT(i)) == 0U) ++plan->installs;
        if ((desired & BIT(i)) == 0U && (plan->original & BIT(i)) != 0U) ++plan->removals;
    }
    return SP_PACKAGE_OK;
}
bool sp_package_enabled(const struct sp_package_selection *selection, const char *id)
{
    int index = sp_package_index(id);
    return selection != NULL && index >= 0 && (selection->enabled & BIT((unsigned int)index)) != 0U;
}
bool sp_package_system_enabled(const struct sp_package_selection *selection, const char *folder)
{
    size_t i;
    if (selection == NULL || folder == NULL || folder[0] == '\0') return false;
    for (i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        const char *start = packages[i].systems;
        while (*start != '\0') {
            const char *end = strchr(start, '|');
            size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
            if (strlen(folder) == length && memcmp(start, folder, length) == 0)
                return (selection->enabled & BIT(i)) != 0U;
            if (end == NULL) break;
            start = end + 1;
        }
    }
    return false;
}
