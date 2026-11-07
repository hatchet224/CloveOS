#include "sessiond/bios_policy.h"
#include "sessiond/library.h"
/* Protected startup enrollment for explicitly qualified single-file cores. Only
 * bounded indexes and core resource directories are written to the game card;
 * no emulator downloads, shell commands, or writable-card core/profile overrides. */
#include "supervisor/config.h"
#include "supervisor/cpu_policy.h"
#include "supervisor/catalog_index.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#define ROOT "/run/console/config"
#define STOCK "/etc/console/catalog"
#define CORE "/system/lib/libretro/gambatte_libretro.so"
#define CFG "/etc/console/retroarch.cfg"
#define MAX_NAMES 4096U
#define SYSTEM_COUNT 20U
struct system_template {
    const char *system, *extensions, *core_path;
    uint64_t max_rom_bytes;
    struct cs_identity identity;
    char playlist_config[65];
    bool loaded, core_present;
};
static const struct system_template supported[SYSTEM_COUNT] = {
    {.system="GB", .extensions="gb|gbc", .core_path=CORE, .max_rom_bytes=8U * 1024U * 1024U},
    {.system="GBC", .extensions="gb|gbc", .core_path=CORE, .max_rom_bytes=8U * 1024U * 1024U},
    {.system="GBA", .extensions="gba", .core_path="/system/lib/libretro/mgba_libretro.so", .max_rom_bytes=32U * 1024U * 1024U},
    {.system="FC", .extensions="nes", .core_path="/system/lib/libretro/fceumm_libretro.so", .max_rom_bytes=32U * 1024U * 1024U},
    {.system="SFC", .extensions="sfc|smc", .core_path="/system/lib/libretro/snes9x2005_plus_libretro.so", .max_rom_bytes=16U * 1024U * 1024U},
    {.system="MD", .extensions="md|gen|bin", .core_path="/system/lib/libretro/picodrive_libretro.so", .max_rom_bytes=16U * 1024U * 1024U},
    {.system="MS", .extensions="sms", .core_path="/system/lib/libretro/picodrive_libretro.so", .max_rom_bytes=4U * 1024U * 1024U},
    {.system="N64", .extensions="z64|v64|n64", .core_path="/system/lib/libretro/mupen64plus_next_libretro.so", .max_rom_bytes=64U * 1024U * 1024U},
    {.system="PS", .extensions="iso|chd", .core_path="/system/lib/libretro/pcsx_rearmed_libretro.so", .max_rom_bytes=1024U * 1024U * 1024U},
    {.system="DC", .extensions="chd|cdi", .core_path="/system/lib/libretro/flycast_libretro.so", .max_rom_bytes=UINT64_C(0xffffffff)},
    {.system="PSP", .extensions="iso|cso|pbp|elf", .core_path="/system/lib/libretro/ppsspp_libretro.so", .max_rom_bytes=UINT64_C(0xffffffff)},
    {.system="SATURN", .extensions="chd|iso", .core_path="/system/lib/libretro/yabasanshiro_libretro.so", .max_rom_bytes=UINT64_C(0xffffffff)},
    {.system="GG", .extensions="gg", .core_path="/system/lib/libretro/picodrive_libretro.so", .max_rom_bytes=4U * 1024U * 1024U},
    {.system="ARCADE", .extensions="zip", .core_path="/system/lib/libretro/fbneo_libretro.so", .max_rom_bytes=512U * 1024U * 1024U},
    {.system="FBNEO", .extensions="zip", .core_path="/system/lib/libretro/fbneo_libretro.so", .max_rom_bytes=512U * 1024U * 1024U},
    {.system="NEOGEO", .extensions="zip", .core_path="/system/lib/libretro/fbneo_libretro.so", .max_rom_bytes=512U * 1024U * 1024U},
    {.system="PCE", .extensions="pce|chd", .core_path="/system/lib/libretro/mednafen_pce_fast_libretro.so", .max_rom_bytes=UINT64_C(0xffffffff)},
    {.system="PICO", .extensions="p8|png", .core_path="/system/lib/libretro/fake08_libretro.so", .max_rom_bytes=8U * 1024U * 1024U},
    {.system="MAME2003PLUS", .extensions="zip", .core_path="/system/lib/libretro/mame2003_plus_libretro.so", .max_rom_bytes=512U * 1024U * 1024U},
    {.system="ATARI", .extensions="a26|bin", .core_path="/system/lib/libretro/stella2014_libretro.so", .max_rom_bytes=1024U * 1024U}
};
static bool media_size_allowed(const struct system_template *system, off_t bytes)
{
    return bytes > 0 && (uint64_t)bytes <= system->max_rom_bytes;
}
struct seed {
    struct cs_json entry;
    struct cs_profile profile;
    char filename[70];
    bool enrollable;
};
struct output {
    char game[65], content[65], path[512];
};
struct state {
    struct cs_store volume;
    struct cs_profile base;
    struct cs_json base_json, supervisor;
    struct seed seeds[SC_CATALOG_MAX];
    size_t seed_count;
    struct system_template systems[SYSTEM_COUNT];
    struct output output[SC_CATALOG_MAX];
    size_t count;
    uint64_t hashed;
    uint64_t indexed;
    bool scan_failed;
    bool staging_created;
    char generation[512], catalog[512], profiles[512], bios[65];
};
static bool ascii(const char *s)
{
    size_t i;
    for (i = 0U; s[i] != '\0'; ++i) {
        if ((unsigned char)s[i] < 32U || (unsigned char)s[i] > 126U || s[i] == '"' ||
            s[i] == '\\') {
            return false;
        }
    }
    return true;
}
static bool protected_directory(const char *path)
{
    int fd = sc_open_absolute(path, true);
    struct stat st;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == 0U && (st.st_mode & 0022U) == 0U;
    if (fd >= 0 && close(fd) != 0) {
        ok = false;
    }
    return ok;
}
static bool set(struct cs_json *j, const char *key, const char *value, enum cs_json_type type)
{
    size_t i;
    if (!ascii(value) || strlen(value) >= sizeof(j->fields[0].value)) {
        return false;
    }
    for (i = 0U; i < j->count; ++i) {
        if (strcmp(j->fields[i].key, key) == 0) {
            break;
        }
    }
    if (i == j->count) {
        if (j->count == CS_JSON_FIELDS || strlen(key) >= sizeof(j->fields[0].key)) {
            return false;
        }
        ++j->count;
        strcpy(j->fields[i].key, key);
    }
    strcpy(j->fields[i].value, value);
    j->fields[i].type = type;
    return true;
}
static bool save(const char *path, const struct cs_json *j)
{
    char bytes[CS_JSON_MAX + 1U], temporary[600], parent[512];
    size_t used = 0U, i;
    int fd = -1, dir = -1, n;
    bool ok = false, created = false, renamed = false;
    char *slash;
    struct timespec now;
    if (strlen(path) >= sizeof(parent) || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (slash == NULL) {
        return false;
    }
    *slash = '\0';
    dir = sc_open_absolute(parent, true);
    if (dir < 0) {
        return false;
    }
    n = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%09ld", path, (long)getpid(),
                 now.tv_nsec);
    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        goto done;
    }
    bytes[used++] = '{';
    for (i = 0U; i < j->count; ++i) {
        const struct cs_json_field *f = &j->fields[i];
        if (!ascii(f->key) || !ascii(f->value)) {
            goto done;
        }
        n = snprintf(bytes + used, sizeof(bytes) - used, "%s\"%s\":%s%s%s", i == 0U ? "" : ",",
                     f->key, f->type == CS_JSON_STRING ? "\"" : "", f->value,
                     f->type == CS_JSON_STRING ? "\"" : "");
        if (n < 0 || (size_t)n >= sizeof(bytes) - used) {
            goto done;
        }
        used += (size_t)n;
    }
    if (used + 3U > sizeof(bytes)) {
        goto done;
    }
    bytes[used++] = '}';
    bytes[used++] = '\n';
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        goto done;
    }
    created = true;
    for (i = 0U; i < used;) {
        ssize_t wrote = write(fd, bytes + i, used - i);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            goto done;
        }
        i += (size_t)wrote;
    }
    if (fsync(fd) != 0) {
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0) {
        goto done;
    }
    renamed = true;
    ok = fsync(dir) == 0;
done:
    if (fd >= 0) {
        (void)close(fd);
    }
    if (created && !renamed) {
        (void)unlink(temporary);
    }
    if (close(dir) != 0) {
        ok = false;
    }
    return ok;
}
static bool pin(struct state *s)
{
    struct stat st;
    struct statfs fs;
    FILE *info;
    char path[80], line[128];
    bool found = false;
    int n;
    cs_store_init(&s->volume);
    strcpy(s->volume.card_path, s->base.card_path);
    strcpy(s->volume.uuid, s->base.card_uuid);
    s->volume.qualified_fat_syncfs = s->base.qualified_fat_syncfs;
    s->volume.card_fd = sc_open_absolute(s->base.card_path, true);
    if (s->volume.card_fd < 0 || fstat(s->volume.card_fd, &st) != 0 ||
        fstatfs(s->volume.card_fd, &fs) != 0) {
        return false;
    }
    s->volume.device = st.st_dev;
    s->volume.inode = st.st_ino;
    s->volume.fat = (unsigned long)fs.f_type == 0x4d44UL;
    if ((!s->volume.fat && (unsigned long)fs.f_type != 0xef53UL) ||
        (s->volume.fat && !s->base.qualified_fat_syncfs)) {
        return false;
    }
    n = snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", s->volume.card_fd);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return false;
    }
    info = fopen(path, "r");
    if (info == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), info) != NULL) {
        if (strncmp(line, "mnt_id:", 7U) == 0) {
            char *v = line + 7U;
            while (*v == ' ' || *v == '\t') {
                ++v;
            }
            v[strcspn(v, "\r\n")] = '\0';
            found = cs_decimal(v, &s->volume.mount_id);
            break;
        }
    }
    if (fclose(info) != 0) {
        return false;
    }
    return found && cs_store_check_media(&s->volume) == CS_STORE_OK;
}
static int name_compare(const void *a, const void *b) { return strcmp(a, b); }
static bool names(const char *path, char (*out)[256], size_t *count)
{
    int fd = sc_open_absolute(path, true);
    DIR *d;
    struct dirent *e;
    bool ok = true;
    size_t visited = 0U;
    *count = 0U;
    if (fd < 0) {
        return errno == ENOENT;
    }
    d = fdopendir(fd);
    if (d == NULL) {
        (void)close(fd);
        return false;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (++visited > MAX_NAMES) {
            fprintf(stderr, "catalog: directory exceeds %u visible entries; previous catalog retained\n", MAX_NAMES);
            ok = false;
            break;
        }
        if (strlen(e->d_name) >= 256U || !ascii(e->d_name)) {
            continue;
        }
        strcpy(out[(*count)++], e->d_name);
    }
    if (closedir(d) != 0) {
        ok = false;
    }
    qsort(out, *count, sizeof(out[0]), name_compare);
    return ok;
}
static const struct system_template *path_template(const struct state *s, const char *path)
{
    size_t root = strlen(s->base.card_path), i;
    const char *folder, *slash, *extension;
    if (strncmp(path, s->base.card_path, root) != 0 || strncmp(path + root, "/Roms/", 6U) != 0)
        return NULL;
    folder = path + root + 6U;
    slash = strchr(folder, '/');
    if (slash == NULL || slash[1] == '\0') return NULL;
    const char *file=slash+1,*hidden=strchr(file,'/');
    if (hidden!=NULL) {
        if (file[0]!='.' || hidden-file<2 || hidden[1]=='\0' || hidden[1]=='.' ||
            strchr(hidden+1,'/')!=NULL) return NULL;
        file=hidden+1;
    }
    extension = strrchr(file, '.');
    if (extension == NULL || extension[1] == '\0') return NULL;
    ++extension;
    for (i = 0U; i < SYSTEM_COUNT; ++i) {
        const struct system_template *t = &s->systems[i];
        const char *p = t->extensions;
        if (!t->loaded || strlen(t->system) != (size_t)(slash - folder) ||
            strncmp(folder, t->system, (size_t)(slash - folder)) != 0) continue;
        if (hidden == NULL && strcmp(t->system, "PS") == 0 && cs_disc_playlist_path(path)) return t;
        while (*p != '\0') {
            const char *end = strchr(p, '|');
            size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
            if (strlen(extension) == n && strncasecmp(extension, p, n) == 0) return t;
            if (end == NULL) break;
            p = end + 1;
        }
    }
    return NULL;
}
static bool content_class(int fd, const struct system_template *t)
{
    unsigned char header[16];
    ssize_t n;
    if (strcmp(t->system, "FC") == 0) {
        n = pread(fd, header, 16U, 0);
        /* FCEUmm dynamically appends options for VS/PlayChoice and mapper105.
         * The ordinary-iNES template must never claim those option identities. */
        return n == 16 && memcmp(header, "NES\x1a", 4U) == 0 && (header[7] & 15U) == 0U &&
               (((unsigned int)header[6] >> 4U) | ((unsigned int)header[7] & 240U)) != 105U;
    }
    if (strcmp(t->system, "MD") == 0) {
        return pread(fd, header, 4U, 256) == 4 && memcmp(header, "SEGA", 4U) == 0;
    }
    return true;
}
static bool hash_rom(struct state *s, const char *path, char out[65])
{
    int fd;
    struct stat before, after;
    unsigned char bytes[32768];
    struct cs_sha256 hash;
    uint64_t used = 0U;
    bool ok = true;
    const struct system_template *qualified = path_template(s, path);
    if (qualified == NULL || cs_store_check_media(&s->volume) != CS_STORE_OK) {
        return false;
    }
    fd = sc_open_absolute(path, false);
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_dev != s->volume.device ||
        !media_size_allowed(qualified, before.st_size) ||
        !content_class(fd, qualified)) {
        (void)close(fd);
        return false;
    }
    if (sc_catalog_index_lookup(fd, path, before.st_size, "/etc/console/content-index", out)) {
        bool stable = fstat(fd, &after) == 0 && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino && before.st_size == after.st_size &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
        if (close(fd) != 0) stable = false;
        if (stable && cs_store_check_media(&s->volume) == CS_STORE_OK) {
            s->indexed += (uint64_t)before.st_size;
            fprintf(stderr, "catalog: protected registration index hit: %s (%" PRIu64 " bytes; launch still verifies full content)\n",
                    path, (uint64_t)before.st_size);
            return true;
        }
        return false;
    }
    /* An explicit aggregate ceiling contains pathological duplicate directories. */
    if (s->hashed + (uint64_t)before.st_size > UINT64_C(32) * 1024U * 1024U * 1024U) {
        s->scan_failed = true;
        fprintf(stderr, "catalog: 32GiB ROM hashing budget reached; previous catalog retained\n");
        (void)close(fd);
        return false;
    }
    cs_sha256_init(&hash);
    for (;;) {
        ssize_t got = read(fd, bytes, sizeof(bytes));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        used += (uint64_t)got;
        if (used > qualified->max_rom_bytes) {
            ok = false;
            break;
        }
        cs_sha256_update(&hash, bytes, (size_t)got);
    }
    s->hashed += used;
    if (fstat(fd, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || used != (uint64_t)before.st_size) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (cs_store_check_media(&s->volume) != CS_STORE_OK) {
        ok = false;
    }
    if (ok) {
        cs_sha256_final(&hash, out);
    }
    return ok;
}
/* Exact magicx-bios-v2 byte stream used by the endpoint. Bounds are
 * stricter here: one pinned card device and a 512 MiB aggregate inventory. */
static dev_t bios_device;
static uint64_t bios_bytes;
static bool bios_hash_fd(int fd, uint64_t limit, char hex[65])
{
    struct stat before, after;
    struct cs_sha256 hash;
    unsigned char buffer[16384];
    uint64_t used = 0U;
    bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_dev == bios_device && before.st_size >= 0 &&
              (uint64_t)before.st_size <= limit && (before.st_mode & 0022U) == 0U &&
              (before.st_uid == 0U || before.st_uid == geteuid());
    if (ok && (uint64_t)before.st_size > UINT64_C(512) * 1024U * 1024U - bios_bytes) ok = false;
    if (ok) bios_bytes += (uint64_t)before.st_size;
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
        used += (uint64_t)n;
        if (used > limit) {
            ok = false;
            break;
        }
        cs_sha256_update(&hash, buffer, (size_t)n);
    }
    if (ok) {
        ok = fstat(fd, &after) == 0 && before.st_size == after.st_size &&
             used == (uint64_t)before.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
             before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
             before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
             before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    }
    if (ok) {
        cs_sha256_final(&hash, hex);
    }
    return ok;
}
static int bios_name_compare(const void *a, const void *b)
{
    const char *const *one = a, *const *two = b;
    return strcmp(*one, *two);
}
static bool bios_hash_dir(int fd, const char *prefix, struct cs_sha256 *hash, unsigned int depth,
                          unsigned int *files)
{
    DIR *directory;
    struct dirent *entry;
    struct stat before, after;
    char *names[256] = {0};
    size_t count = 0U, i;
    bool ok = depth <= 4U && fstat(fd, &before) == 0 && before.st_dev == bios_device && (before.st_mode & 0022U) == 0U &&
              (before.st_uid == 0U || before.st_uid == geteuid());
    int copy = dup(fd);
    if (copy < 0) {
        return false;
    }
    directory = fdopendir(copy);
    if (directory == NULL) {
        (void)close(copy);
        return false;
    }
    errno = 0;
    while (ok && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count == 256U || (names[count] = strdup(entry->d_name)) == NULL) {
            ok = false;
            break;
        }
        ++count;
    }
    {
        int read_error = errno;
        if (closedir(directory) != 0 || read_error != 0) {
            ok = false;
        }
    }
    qsort(names, count, sizeof(names[0]), bios_name_compare);
    for (i = 0U; i < count && ok; ++i) {
        struct stat st;
        char path[512U], digest[65];
        int child, n = snprintf(path, sizeof(path), "%s%s", prefix, names[i]);
        if (n < 0 || (size_t)n >= sizeof(path) ||
            fstatat(fd, names[i], &st, AT_SYMLINK_NOFOLLOW) != 0 || ++*files > 256U) {
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            size_t length = strlen(path);
            if (length + 1U >= sizeof(path)) {
                ok = false;
                break;
            }
            path[length] = '/';
            path[length + 1U] = '\0';
            child = openat(fd, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                ok = false;
                break;
            }
            if (!cs_bios_optional_container(path)) cs_sha256_update(hash, path, strlen(path) + 1U);
            ok = bios_hash_dir(child, path, hash, depth + 1U, files);
            if (close(child) != 0) {
                ok = false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (cs_bios_runtime_save(path)) continue;
            child = openat(fd, names[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (child < 0) {
                ok = false;
                break;
            }
            ok = bios_hash_fd(child, 64U * 1024U * 1024U, digest);
            if (close(child) != 0) {
                ok = false;
            }
            if (ok) {
                cs_sha256_update(hash, path, strlen(path) + 1U);
                cs_sha256_update(hash, digest, 65U);
            }
        } else {
            ok = false;
        }
    }
    for (i = 0U; i < count; ++i) {
        free(names[i]);
    }
    if (ok) {
        ok = fstat(fd, &after) == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
             before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    }
    return ok;
}
static bool prepare_resource_dirs(struct state *s)
{
    /* FBNeo creates these empty resource folders before its endpoint HELLO.
     * Directory names participate in the BIOS identity. Create the exact known
     * layout before enrollment so a first launch cannot change that identity.
     * Files in these folders remain fully hashed and validated. */
    static const char *const folders[] = {"cheats", "ips", "romdata", "samples", "snesmsu1"};
    char path[512]; struct stat st; int bios = -1, root = -1; bool ok = false, changed = false;
    if (snprintf(path, sizeof(path), "%s/BIOS", s->base.card_path) >= (int)sizeof(path) ||
        cs_store_check_media(&s->volume) != CS_STORE_OK) return false;
    bios = sc_open_absolute(path, true);
    if (bios < 0) return false;
    if (mkdirat(bios, "fbneo", 0700) == 0) changed = true;
    else if (errno != EEXIST) goto done;
    root = openat(bios, "fbneo", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0 || fstat(root, &st) != 0 || st.st_dev != s->volume.device) goto done;
    for (size_t i = 0U; i < sizeof(folders) / sizeof(folders[0]); ++i) {
        if (mkdirat(root, folders[i], 0700) == 0) changed = true;
        else if (errno != EEXIST) goto done;
        if (fstatat(root, folders[i], &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode) || st.st_dev != s->volume.device) goto done;
    }
    ok = (!changed || syncfs(bios) == 0) && cs_store_check_media(&s->volume) == CS_STORE_OK;
done:
    if (root >= 0 && close(root) != 0) ok = false;
    if (close(bios) != 0) ok = false;
    return ok;
}
static bool current_bios(struct state *s, char out[65])
{
    static const char prefix[] = CS_BIOS_HASH_PREFIX;
    char path[512];
    int fd;
    unsigned int count = 0U;
    struct cs_sha256 hash;
    bool ok;
    if (snprintf(path, sizeof(path), "%s/BIOS", s->base.card_path) >= (int)sizeof(path) ||
        cs_store_check_media(&s->volume) != CS_STORE_OK) return false;
    fd = sc_open_absolute(path, true);
    if (fd < 0) return false;
    bios_device = s->volume.device; bios_bytes = 0U;
    cs_sha256_init(&hash); cs_sha256_update(&hash, prefix, sizeof(prefix));
    ok = bios_hash_dir(fd, "", &hash, 0U, &count);
    if (close(fd) != 0) ok = false;
    if (cs_store_check_media(&s->volume) != CS_STORE_OK) ok = false;
    if (ok) cs_sha256_final(&hash, out);
    return ok;
}
static bool same_policy(const struct cs_identity *a, const struct cs_identity *b)
{
    return strcmp(a->core, b->core) == 0 && strcmp(a->config, b->config) == 0 &&
           strcmp(a->bios, b->bios) == 0 && strcmp(a->retroarch, b->retroarch) == 0 &&
           strcmp(a->disc, b->disc) == 0 && strcmp(a->policy, b->policy) == 0;
}
static bool load_templates(struct state *s)
{
    size_t i, j;
    bool any = false;
    for (i = 0U; i < SYSTEM_COUNT; ++i) {
        struct system_template *t = &s->systems[i];
        char path[128];
        struct cs_json json;
        struct stat st;
        uint64_t schema, limit;
        static const char *const keys[] = {"schema", "system", "extensions", "core_path", "config_path",
            "config_file_hash", "options_path", "options_file_hash", "core", "config", "bios",
            "retroarch", "disc", "policy", "max_rom_bytes"};
        static const char *const hashes[] = {"core", "config", "bios", "retroarch", "disc", "policy"};
        char *values[] = {t->identity.core, t->identity.config, t->identity.bios,
                          t->identity.retroarch, t->identity.disc, t->identity.policy};
        const char *core, *cfg, *options, *system, *extensions, *config_hash, *options_hash;
        if (snprintf(path, sizeof(path), "/etc/console/core-templates/%s.json", t->system) >= (int)sizeof(path))
            return false;
        if (lstat(path, &st) != 0) {
            if (errno != ENOENT) return false;
            any = any || t->loaded;
            continue;
        }
        if (!protected_directory("/etc/console/core-templates") || !sc_read_json(path, &json) ||
            !cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0])) ||
            !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
            !cs_json_u64(&json, "max_rom_bytes", &limit) || limit != t->max_rom_bytes) return false;
        core = cs_json_string(&json, "core_path"); cfg = cs_json_string(&json, "config_path");
        options = cs_json_string(&json, "options_path"); system = cs_json_string(&json, "system");
        extensions = cs_json_string(&json, "extensions"); config_hash = cs_json_string(&json, "config_file_hash");
        options_hash = cs_json_string(&json, "options_file_hash");
        if (core == NULL || strcmp(core, t->core_path) != 0 || cfg == NULL || strcmp(cfg, CFG) != 0 ||
            options == NULL || strcmp(options, "/etc/console/retroarch-core-options.cfg") != 0 ||
            system == NULL || strcmp(system, t->system) != 0 || extensions == NULL ||
            strcmp(extensions, t->extensions) != 0 || !cs_hex_valid(config_hash, 64U) ||
            !cs_hex_valid(options_hash, 64U)) return false;
        for (j = 0U; j < sizeof(hashes) / sizeof(hashes[0]); ++j) {
            const char *value = cs_json_string(&json, hashes[j]);
            if (!cs_hex_valid(value, 64U)) return false;
            strcpy(values[j], value);
        }
        if (!sc_hash_path(CFG, config_hash, UINT64_C(1) * 1024U * 1024U) ||
            !sc_hash_path(options, options_hash, UINT64_C(1) * 1024U * 1024U)) return false;
        cs_disc_frozen_config(config_hash, options_hash, t->playlist_config);
        t->loaded = true;
        any = true;
    }
    return any;
}
static bool load_seeds(struct state *s)
{
    char (*entries)[256] = calloc(MAX_NAMES, sizeof(*entries));
    size_t count, i, j;
    bool ok = false;
    static const char *const required[] = {"title", "system", "context", "content_path", "core_path", "config_path", "profile"};
    memcpy(s->systems, supported, sizeof(s->systems));
    if (entries == NULL || !protected_directory(STOCK) ||
        !protected_directory("/etc/console/profiles") || !names(STOCK, entries, &count) || count > SC_CATALOG_MAX) goto done;
    for (i = 0U; i < count; ++i) {
        struct seed *seed = &s->seeds[s->seed_count];
        char path[512], profile[512];
        const char *system, *core, *cfg;
        if (strlen(entries[i]) != 69U || strcmp(entries[i] + 64U, ".json") != 0) goto done;
        memcpy(seed->filename, entries[i], 70U);
        seed->filename[64] = '\0';
        if (!cs_hex_valid(seed->filename, 64U)) goto done;
        seed->filename[64] = '.';
        if (snprintf(path, sizeof(path), "%s/%s", STOCK, entries[i]) >= (int)sizeof(path) ||
            !sc_read_json(path, &seed->entry) || !cs_json_keys(&seed->entry, required, 7U) ||
            snprintf(profile, sizeof(profile), "/etc/console/profiles/%s", entries[i]) >= (int)sizeof(profile) ||
            !cs_profile_load(profile, &seed->profile) || strncmp(seed->filename, seed->profile.identity.game, 64U) != 0) goto done;
        for (j = 0U; j < 7U; ++j) {
            const char *value = cs_json_string(&seed->entry, required[j]);
            if (value == NULL || !ascii(value)) goto done;
        }
        system = cs_json_string(&seed->entry, "system"); core = cs_json_string(&seed->entry, "core_path");
        cfg = cs_json_string(&seed->entry, "config_path");
        /* Compatibility with an older protected image containing only the
         * seven GB seeds. New images additionally pin all core templates. */
        if ((strcmp(system, "GB") == 0 || strcmp(system, "GBC") == 0) &&
            strcmp(core, CORE) == 0 && strcmp(cfg, CFG) == 0) {
            for (j = 0U; j < 2U; ++j) {
                if (s->systems[j].loaded && !same_policy(&s->systems[j].identity, &seed->profile.identity)) goto done;
                s->systems[j].identity = seed->profile.identity; s->systems[j].loaded = true;
            }
        }
        ++s->seed_count;
    }
    if (!load_templates(s)) goto done;
    for (i = 0U; i < s->seed_count; ++i) {
        struct seed *seed = &s->seeds[i];
        for (j = 0U; j < SYSTEM_COUNT; ++j) {
            const struct system_template *t = &s->systems[j];
            if (t->loaded && strcmp(cs_json_string(&seed->entry, "system"), t->system) == 0 &&
                strcmp(cs_json_string(&seed->entry, "core_path"), t->core_path) == 0 &&
                strcmp(cs_json_string(&seed->entry, "config_path"), CFG) == 0) {
                if (!same_policy(&seed->profile.identity, &t->identity)) goto done;
                seed->enrollable = true;
            }
        }
    }
    {
        const char *retroarch = NULL;
        for (i = 0U; i < SYSTEM_COUNT; ++i) {
            const struct system_template *t = &s->systems[i];
            bool already_verified = false;
            if (!t->loaded) continue;
            if (retroarch == NULL) {
                if (!sc_retroarch_hash("/system/bin/retroarch", t->identity.retroarch)) goto done;
                retroarch = t->identity.retroarch;
            } else if (strcmp(retroarch, t->identity.retroarch) != 0) goto done;
            for (j = 0U; j < i; ++j) {
                if (s->systems[j].loaded && strcmp(t->core_path, s->systems[j].core_path) == 0) {
                    if (strcmp(t->identity.core, s->systems[j].identity.core) != 0) goto done;
                    already_verified = true;
                    break;
                }
            }
            (void)already_verified; /* Authenticate a core only when its content is enrolled. */
        }
    }
    ok = true;
done:
    free(entries);
    return ok;
}
static bool emit(struct state *s, const struct cs_json *entry, const struct cs_identity *identity)
{
    struct cs_json profile = s->base_json, game = *entry;
    char path[768], profile_path[512];
    size_t i;
    const char *keys[] = {"game", "content",   "core", "config",
                          "bios", "retroarch", "disc", "policy"};
    const char *values[] = {identity->game, identity->content,   identity->core, identity->config,
                            identity->bios, identity->retroarch, identity->disc, identity->policy};
    for (i = 0U; i < s->count; ++i) {
        if (strcmp(s->output[i].game, identity->game) == 0) {
            return true;
        }
    }
    if (s->count == SC_CATALOG_MAX) {
        fprintf(stderr, "catalog: more than %u unique qualified games; previous catalog retained\n", SC_CATALOG_MAX);
        return false;
    }
    for (i = 0U; i < 8U; ++i) {
        if (!set(&profile, keys[i], values[i], CS_JSON_STRING)) {
            return false;
        }
    }
    strcpy(s->output[s->count].game, identity->game);
    strcpy(s->output[s->count].content, identity->content);
    strcpy(s->output[s->count].path, cs_json_string(&game, "content_path"));
    ++s->count;
    if (!set(&profile, "attach_running", "true", CS_JSON_BOOL) ||
        snprintf(profile_path, sizeof(profile_path), "%s/%s.json", s->profiles, identity->game) >=
            (int)sizeof(profile_path) ||
        !set(&game, "profile", profile_path, CS_JSON_STRING) || !save(profile_path, &profile)) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s/%s.json", s->catalog, identity->game) >=
            (int)sizeof(path) ||
        !save(path, &game)) {
        return false;
    }
    return true;
}
static bool enroll(struct state *s, const char *path, const struct seed *preferred)
{
    const struct system_template *qualified = path_template(s, path);
    struct cs_identity id;
    if (qualified == NULL) return true;
    {
        struct system_template *t = &s->systems[(size_t)(qualified - s->systems)];
        if (!t->core_present) {
            /* Enrollment is a library operation, not permission to execute.
             * Do not decompress/hash every installed core before Home. START
             * authenticates the selected core and RetroArch in
             * sc_game_files_check(..., true), including automatic resume.
             * Still fail for missing, empty, oversized or redirected cores. */
            int core_fd = sc_open_absolute(t->core_path, false);
            struct stat core_stat;
            bool available = core_fd >= 0 && fstat(core_fd, &core_stat) == 0 &&
                S_ISREG(core_stat.st_mode) && core_stat.st_size > 0 &&
                (uint64_t)core_stat.st_size <= UINT64_C(128) * 1024U * 1024U;
            if (core_fd >= 0 && close(core_fd) != 0) available = false;
            if (!available) return false;
            for (size_t k = 0U; k < SYSTEM_COUNT; ++k)
                if (strcmp(s->systems[k].core_path, t->core_path) == 0) s->systems[k].core_present = true;
        }
    }
    id = qualified->identity;
    strcpy(id.bios, s->bios);
    struct cs_json entry = {0};
    char title[97];
    const char *filename;
    size_t len;
    struct cs_sha256 hash;
    static const char prefix[] = "magicx-game-v1";
    {
        size_t seen;
        /* The preference pass already validated/hash-pinned this exact path.
         * Keep one snapshot per path; the launch preflight revalidates content. */
        for (seen = 0U; seen < s->count; ++seen) {
            if (strcmp(s->output[seen].path, path) == 0) {
                return true;
            }
        }
    }
    bool playlist = cs_disc_playlist_path(path);
    if (playlist) {
        unsigned int discs;
        if (strcmp(qualified->system, "PS") != 0 ||
            !cs_disc_ps_playlist_digest(path, id.content, &discs))
            return true; /* Existing single-disc profiles still cover unsupported playlists. */
        strcpy(id.config, qualified->playlist_config);
        cs_sha256_hex(CS_DISC_PLAYLIST_POLICY, sizeof(CS_DISC_PLAYLIST_POLICY), id.disc);
    } else if (!hash_rom(s, path, id.content)) {
        return !s->scan_failed;
    }
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, prefix, sizeof(prefix));
    cs_sha256_update(&hash, id.content, sizeof(id.content));
    cs_sha256_final(&hash, id.game);
    if (preferred != NULL) {
        entry = preferred->entry;
    } else {
        filename = strrchr(path, '/');
        if (filename == NULL) {
            return false;
        }
        ++filename;
        len = strlen(filename);
        {
            const char *dot = strrchr(filename, '.');
            if (dot == NULL) return true;
            len = (size_t)(dot - filename);
        }
        if (len == 0U || len >= sizeof(title)) {
            fprintf(stderr, "catalog: title exceeds bounded UI field: %s\n", path);
            return true;
        }
        memcpy(title, filename, len);
        title[len] = '\0';
        if (!set(&entry, "title", title, CS_JSON_STRING) ||
            !set(&entry, "system", (strcmp(qualified->system, "GB") == 0 || strcmp(qualified->system, "GBC") == 0) ?
                 (strcasecmp(path + strlen(path) - 3U, ".gb") == 0 ? "GB" : "GBC") : qualified->system, CS_JSON_STRING) ||
            !set(&entry, "context", playlist ? "Multi-disc playlist" : "single file, no disc", CS_JSON_STRING) ||
            !set(&entry, "core_path", qualified->core_path, CS_JSON_STRING) ||
            !set(&entry, "config_path", CFG, CS_JSON_STRING)) {
            return false;
        }
    }
    return set(&entry, "content_path", path, CS_JSON_STRING) && emit(s, &entry, &id);
}
static void discard(struct state *s)
{
    size_t i;
    if (!s->staging_created) {
        return;
    }
    char path[768];
    for (i = 0U; i < s->count; ++i) {
        (void)snprintf(path, sizeof(path), "%s/%s.json", s->catalog, s->output[i].game);
        (void)unlink(path);
        (void)snprintf(path, sizeof(path), "%s/%s.json", s->profiles, s->output[i].game);
        (void)unlink(path);
    }
    if (s->generation[0] != '\0') {
        (void)snprintf(path, sizeof(path), "%s/supervisor.json", s->generation);
        (void)unlink(path);
    }
    if (s->catalog[0] != '\0') {
        (void)rmdir(s->catalog);
    }
    if (s->profiles[0] != '\0') {
        (void)rmdir(s->profiles);
    }
    if (s->generation[0] != '\0') {
        (void)rmdir(s->generation);
    }
}
static void boot_timing(const char *stage, uint64_t *previous)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return;
    uint64_t ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    fprintf(stderr, "catalog timing: stage=%s uptime_ms=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
            stage, ms, *previous != 0U && ms >= *previous ? ms - *previous : 0U);
    *previous = ms;
}
static int refresh_catalog(bool discover)
{
    struct sc_cpu_policy startup_cpu = {0};
    uint64_t timing = 0U;
    boot_timing("start", &timing);
    struct state *s = calloc(1U, sizeof(*s));
    char(*entries)[256] = NULL;
    char(*discs)[512] = NULL;
    size_t i, j, count;
    bool committed = false;
    int result = 1;
    char path[512];
    struct timespec now;
    if (s == NULL) {
        return 1;
    }
    /* Raise only to the kernel's supported maximum during CPU-heavy startup
     * validation/decompression. Restore the prior governor before exiting. */
    if (geteuid() == 0U &&
        !sc_cpu_policy_prepare(&startup_cpu, "/sys/devices/system/cpu/cpu0/cpufreq"))
        fprintf(stderr, "catalog: startup CPU policy unchanged/unavailable\n");
    cs_store_init(&s->volume);
    if (geteuid() != 0U || !protected_directory(ROOT) ||
        !sc_read_json(ROOT "/base-profile.json", &s->base_json) ||
        !cs_profile_load(ROOT "/base-profile.json", &s->base) ||
        !sc_read_json(ROOT "/supervisor.json", &s->supervisor) || !pin(s)) {
        fprintf(stderr, "catalog: runtime/volume setup failed\n");
        goto done;
    }
    boot_timing("runtime-mounted", &timing);
    if (!load_seeds(s)) {
        fprintf(stderr, "catalog: protected templates/seed policy failed\n");
        goto done;
    }
    boot_timing("templates-verified", &timing);
    if (!prepare_resource_dirs(s) || !current_bios(s, s->bios)) {
        fprintf(stderr, "catalog: BIOS inventory failed\n");
        goto done;
    }
    boot_timing("policy-and-bios", &timing);
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        snprintf(s->generation, sizeof(s->generation), ROOT "/catalog-generation.%ld.%09ld",
                 (long)getpid(), now.tv_nsec) >= (int)sizeof(s->generation) ||
        mkdir(s->generation, 0700) != 0) {
        goto done;
    }
    s->staging_created = true;
    if (snprintf(s->catalog, sizeof(s->catalog), "%s/catalog", s->generation) >=
            (int)sizeof(s->catalog) ||
        snprintf(s->profiles, sizeof(s->profiles), "%s/profiles", s->generation) >=
            (int)sizeof(s->profiles) ||
        mkdir(s->catalog, 0700) != 0 || mkdir(s->profiles, 0700) != 0) {
        goto done;
    }
    /* Exit 2 marks only discovery/staging failures after trusted runtime,
     * templates and mounted-volume checks. Boot may build a fresh, validated
     * empty runtime catalog; it never deletes or rewrites the user's ROMs. */
    result = discover ? 2 : 1;
    if (discover) {
    /* Existing registered paths win over identical copies. Non-GB registrations
     * retain their prequalified identities without admitting new core choices. */
    for (i = 0U; i < s->seed_count; ++i) {
        const struct seed *seed = &s->seeds[i];
        const char *content = cs_json_string(&seed->entry, "content_path");
        if (seed->enrollable) {
            if (!enroll(s, content, seed)) {
                goto done;
            }
        } else if (!emit(s, &seed->entry, &seed->profile.identity)) {
            goto done;
        }
    }
    boot_timing("seed-enrollment", &timing);
    entries = calloc(MAX_NAMES, sizeof(*entries));
    discs = calloc(CS_M3U_DISCS,sizeof(*discs));
    if (entries == NULL || discs == NULL) {
        goto done;
    }
    for (i = 0U; i < SYSTEM_COUNT; ++i) {
        if (!s->systems[i].loaded) continue;
        if (snprintf(path, sizeof(path), "%s/Roms/%s", s->base.card_path, s->systems[i].system) >=
                (int)sizeof(path) ||
            !names(path, entries, &count)) {
            goto done;
        }
        fprintf(stderr, "catalog: scanning %s (%zu candidates, %zu enrolled, %" PRIu64 " bytes hashed)\n",
                s->systems[i].system, count, s->count, s->hashed);
        for (j = 0U; j < count; ++j) {
            char rom[512];
            if (snprintf(rom, sizeof(rom), "%s/%s", path, entries[j]) >= (int)sizeof(rom)) {
                continue;
            }
            if (path_template(s, rom) != NULL && !enroll(s, rom, NULL)) {
                goto done;
            }
            const char *ext=strrchr(entries[j],'.');
            if(ext!=NULL && strcasecmp(ext,".m3u")==0) {
                size_t disc_count=0U;
                int dir=sc_open_absolute(path,true);
                bool managed=dir>=0 && cs_m3u_read(dir,entries[j],discs,&disc_count);
                if(dir>=0) (void)close(dir);
                for(size_t disc=0U;managed && disc<disc_count;++disc) {
                    int n=snprintf(rom,sizeof(rom),"%s/%s",path,discs[disc]);
                    if(n>0 && (size_t)n<sizeof(rom) && !enroll(s,rom,NULL)) goto done;
                }
            }
        }
    }
    }
    /* Volume/identity/publication failures are never downgraded to a library
     * warning. Even the empty recovery generation must pass these checks. */
    result = 1;
    boot_timing("rom-discovery", &timing);
    {
        char verified_bios[65];
        if (!current_bios(s, verified_bios) || strcmp(s->bios, verified_bios) != 0) goto done;
    }
    boot_timing("bios-recheck", &timing);
    if (cs_store_check_media(&s->volume) != CS_STORE_OK ||
        !set(&s->supervisor, "catalog", s->catalog, CS_JSON_STRING)) {
        goto done;
    }
    {
        struct sc_config *validated = calloc(1U, sizeof(*validated));
        char candidate[768];
        bool valid = validated != NULL &&
                     snprintf(candidate, sizeof(candidate), "%s/supervisor.json", s->generation) <
                         (int)sizeof(candidate) &&
                     save(candidate, &s->supervisor) && sc_config_load(candidate, validated);
        free(validated);
        if (!valid) {
            goto done;
        }
    }
    /* Once publication is attempted, retain a complete generation even if its
     * parent flush fails after rename; never remove a possibly referenced tree. */
    committed = true;
    if (!save(ROOT "/supervisor.json", &s->supervisor)) {
        goto done;
    }
    printf("catalog: %zu prequalified games; qualified single-file discovery enabled, bounded at %u; "
           "hashed=%" PRIu64 " bytes; indexed=%" PRIu64 " bytes (registration only)\n",
           s->count, SC_CATALOG_MAX, s->hashed, s->indexed);
    result = 0;
done:
    boot_timing("finish", &timing);
    if (!sc_cpu_policy_restore(&startup_cpu))
        fprintf(stderr, "catalog: startup CPU policy restore failed\n");
    if (result != 0) {
        fprintf(stderr,
                "catalog: enrollment failed; card untouched; possibly published runtime generation "
                "retained (%s)\n",
                strerror(errno));
        if (!committed) {
            discard(s);
        }
    }
    free(discs);
    free(entries);
    cs_store_close(&s->volume);
    free(s);
    return result;
}
int sc_catalog_refresh(void) { return refresh_catalog(true); }
#ifndef SC_CATALOG_LIBRARY
int main(int argc, char **argv)
{
    if (argc == 1) return sc_catalog_refresh();
    if (argc == 2 && strcmp(argv[1], "--empty-runtime") == 0)
        return refresh_catalog(false);
    fprintf(stderr, "usage: console-catalog [--empty-runtime]\n");
    return 1;
}
#endif
