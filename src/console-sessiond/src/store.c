#include "sessiond/durability.h"
#include "sessiond/store.h"
#include "sessiond/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static const char zero_hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
static const char *const artifacts[CS_STORE_ARTIFACTS] = {"state.bin", "sram.bin", "rtc.bin", "screenshot.png"};
static const uint64_t aux_limits[CS_STORE_ARTIFACTS] = {0U, 8U * 1024U * 1024U, 64U * 1024U, CS_SCREENSHOT_MAX};
static const char *const legacy_record_keys[] = {
    "schema",   "format",     "card",       "game",      "content",   "core",
    "config",   "bios",       "retroarch",  "disc",      "policy",    "generation",
    "frame",    "state_size", "state_hash", "sram_size", "sram_hash", "rtc_size",
    "rtc_hash", "menu",       "manifest",   "checksum"};

const char *cs_store_status_name(enum cs_store_status status)
{
    static const char *const names[] = {"ok",
                                        "empty",
                                        "io",
                                        "corrupt",
                                        "media-missing-or-changed",
                                        "unsupported-durability",
                                        "conflict",
                                        "incompatible",
                                        "resume-attempt-blocked"};
    return (unsigned int)status < sizeof(names) / sizeof(names[0]) ? names[status] : "invalid";
}
bool cs_identity_valid(const struct cs_identity *id)
{
    return cs_hex_valid(id->game, 64U) && cs_hex_valid(id->content, 64U) &&
           cs_hex_valid(id->core, 64U) && cs_hex_valid(id->config, 64U) &&
           cs_hex_valid(id->bios, 64U) && cs_hex_valid(id->retroarch, 64U) &&
           cs_hex_valid(id->disc, 64U) && cs_hex_valid(id->policy, 64U);
}
bool cs_identity_equal(const struct cs_identity *a, const struct cs_identity *b)
{
    return strcmp(a->game, b->game) == 0 && strcmp(a->content, b->content) == 0 &&
           strcmp(a->core, b->core) == 0 && strcmp(a->config, b->config) == 0 &&
           strcmp(a->bios, b->bios) == 0 && strcmp(a->retroarch, b->retroarch) == 0 &&
           strcmp(a->disc, b->disc) == 0 && strcmp(a->policy, b->policy) == 0;
}
void cs_store_init(struct cs_store *store)
{
    memset(store, 0, sizeof(*store));
    store->card_fd = -1;
    store->data_fd = -1;
    store->game_fd = -1;
    store->lock_fd = -1;
}
void cs_store_close(struct cs_store *store)
{
    if (store->game_fd >= 0) {
        (void)close(store->game_fd);
    }
    if (store->lock_fd >= 0) {
        (void)close(store->lock_fd);
    }
    if (store->data_fd >= 0) {
        (void)close(store->data_fd);
    }
    if (store->card_fd >= 0) {
        (void)close(store->card_fd);
    }
    cs_store_init(store);
}
static int sync_fd(int fd)
{
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result;
}
static int flush_dir(struct cs_store *store, int fd)
{
    return cs_flush_directory(fd, store->fat, store->qualified_fat_syncfs);
}
static bool uuid_valid(const char *uuid)
{
    size_t i, n = uuid == NULL ? 0U : strlen(uuid);
    if (n == 0U || n > 64U) {
        return false;
    }
    for (i = 0U; i < n; ++i) {
        if (!((uuid[i] >= 'a' && uuid[i] <= 'z') || (uuid[i] >= 'A' && uuid[i] <= 'Z') ||
              (uuid[i] >= '0' && uuid[i] <= '9') || uuid[i] == '-')) {
            return false;
        }
    }
    return true;
}
/* Walk every component without following symlinks; never create the mountpoint. */
static int open_absolute_dir(const char *path)
{
    int fd;
    const char *p;
    size_t length;
    if (path == NULL || path[0] != '/' || (length = strlen(path)) < 2U || length >= CS_PATH_MAX ||
        path[length - 1U] == '/') {
        return -1;
    }
    fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    p = path + 1;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        char part[256];
        int next;
        if (n == 0U || n >= sizeof(part) || (n == 1U && p[0] == '.') ||
            (n == 2U && p[0] == '.' && p[1] == '.')) {
            (void)close(fd);
            return -1;
        }
        memcpy(part, p, n);
        part[n] = '\0';
        next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        (void)close(fd);
        if (next < 0) {
            return -1;
        }
        fd = next;
        if (slash == NULL) {
            break;
        }
        p = slash + 1;
    }
    return fd;
}
static bool fd_mount_id(int fd, uint64_t *id)
{
    char path[64], line[256];
    FILE *file;
    bool found = false;
    if (snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd) < 0) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        if (strncmp(line, "mnt_id:", 7U) == 0) {
            char *p = line + 7;
            size_t n;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            n = strcspn(p, "\r\n");
            p[n] = '\0';
            found = cs_decimal(p, id);
            break;
        }
    }
    if (fclose(file) != 0) {
        found = false;
    }
    return found;
}
static bool mount_path_matches(uint64_t id, const char *path, bool fat)
{
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char line[8192];
    bool found = false;
    if (file == NULL) {
        return false;
    }
    while (fgets(line, (int)sizeof(line), file) != NULL) {
        char *save = NULL, *token = strtok_r(line, " ", &save);
        uint64_t line_id;
        unsigned int field = 0U;
        if (token == NULL || !cs_decimal(token, &line_id) || line_id != id) {
            continue;
        }
        while (token != NULL) {
            if (field == 4U) {
                char decoded[CS_PATH_MAX];
                size_t i = 0U, j = 0U;
                bool valid = true;
                while (token[i] != '\0' && j + 1U < sizeof(decoded)) {
                    if (token[i] == '\\') {
                        unsigned int value;
                        if (strlen(token + i) < 4U || token[i + 1U] < '0' || token[i + 1U] > '7' ||
                            token[i + 2U] < '0' || token[i + 2U] > '7' || token[i + 3U] < '0' ||
                            token[i + 3U] > '7') {
                            valid = false;
                            break;
                        }
                        value = (unsigned int)(token[i + 1U] - '0') * 64U +
                                (unsigned int)(token[i + 2U] - '0') * 8U +
                                (unsigned int)(token[i + 3U] - '0');
                        if (value == 0U || value > 255U) {
                            valid = false;
                            break;
                        }
                        decoded[j++] = (char)value;
                        i += 4U;
                    } else {
                        decoded[j++] = token[i++];
                    }
                }
                decoded[j] = '\0';
                found = valid && token[i] == '\0' && strcmp(decoded, path) == 0;
                /* ext2/ext3 share the ext4 magic; require the actual driver. */
                do {
                    token = strtok_r(NULL, " ", &save);
                } while (token != NULL && strcmp(token, "-") != 0);
                token = token != NULL ? strtok_r(NULL, " ", &save) : NULL;
                found = found && token != NULL && strcmp(token, fat ? "vfat" : "ext4") == 0;
                break;
            }
            ++field;
            token = strtok_r(NULL, " ", &save);
        }
        break;
    }
    if (fclose(file) != 0) {
        found = false;
    }
    return found;
}
enum cs_store_status cs_store_check_media(const struct cs_store *store)
{
    int live;
    struct stat st, block;
    struct statvfs vfs;
    uint64_t id;
    char device[128];
    if (store->card_fd < 0 || store->poisoned) {
        return CS_STORE_MEDIA;
    }
    live = open_absolute_dir(store->card_path);
    if (live < 0) {
        return CS_STORE_MEDIA;
    }
    if (fstat(live, &st) != 0 || st.st_dev != store->device || st.st_ino != store->inode ||
        fstatvfs(live, &vfs) != 0 || (vfs.f_flag & ST_RDONLY) != 0U) {
        (void)close(live);
        return CS_STORE_MEDIA;
    }
    if (!store->test_root && (!fd_mount_id(live, &id) || id != store->mount_id ||
                              !mount_path_matches(id, store->card_path, store->fat))) {
        (void)close(live);
        return CS_STORE_MEDIA;
    }
    (void)close(live);
    if (!store->test_root) {
        const int n = snprintf(device, sizeof(device), "/dev/disk/by-uuid/%s", store->uuid);
        if (n < 0 || (size_t)n >= sizeof(device) || stat(device, &block) != 0 ||
            !S_ISBLK(block.st_mode) || block.st_rdev != store->device) {
            return CS_STORE_MEDIA;
        }
    }
    return CS_STORE_OK;
}
static int child_dir(struct cs_store *store, int parent, const char *name, bool create)
{
    int fd;
    struct stat st;
    if (create && store->read_only) {
        errno = EROFS;
        return -1;
    }
    if (create && mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_dev != store->device || st.st_uid != geteuid() ||
        (st.st_mode & 0022U) != 0U) {
        (void)close(fd);
        errno = EPERM;
        return -1;
    }
    if (create && flush_dir(store, parent) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
enum cs_store_status cs_store_sync(struct cs_store *store)
{
    int result;
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return CS_STORE_MEDIA;
    }
    do {
        result = syncfs(store->card_fd);
    } while (result != 0 && errno == EINTR);
    if (result != 0 || (store->data_fd >= 0 && flush_dir(store, store->data_fd) != 0)) {
        store->poisoned = true;
        return CS_STORE_IO;
    }
    return cs_store_check_media(store);
}
static bool read_small(int dir, const char *name, char buffer[CS_JSON_MAX + 1U], size_t *length)
{
    int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    size_t used = 0U;
    bool ok = true;
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
        st.st_size > (off_t)CS_JSON_MAX) {
        ok = false;
    }
    while (ok && used < CS_JSON_MAX + 1U) {
        ssize_t n = read(fd, buffer + used, CS_JSON_MAX + 1U - used);
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
    if (used > CS_JSON_MAX) {
        ok = false;
    }
    if (ok) {
        buffer[used] = '\0';
        *length = used;
    }
    return ok;
}
static bool write_all(int fd, const char *data, size_t length)
{
    size_t used = 0U;
    while (used < length) {
        ssize_t n = write(fd, data + used, length - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        used += (size_t)n;
    }
    return true;
}
static enum cs_store_status atomic_write(struct cs_store *store, int dir, const char *name,
                                         const char *data, size_t length)
{
    char temp[96];
    int fd;
    bool ok;
    enum cs_store_status media;
    if (store->read_only) {
        return CS_STORE_CONFLICT;
    }
    media = cs_store_check_media(store);
    if (media != CS_STORE_OK) {
        return media;
    }
    if (store->temp_counter == UINT64_MAX) {
        return CS_STORE_CONFLICT;
    }
    if (snprintf(temp, sizeof(temp), ".tmp-%s-%" PRIu64, store->instance, ++store->temp_counter) <
        0) {
        return CS_STORE_IO;
    }
    fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return CS_STORE_IO;
    }
    ok = write_all(fd, data, length) && sync_fd(fd) == 0;
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        return CS_STORE_IO; /* Retain orphan; never delete a possibly-live writer's artifact. */
    }
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return CS_STORE_MEDIA;
    }
    if (flush_dir(store, dir) != 0 || renameat(dir, temp, dir, name) != 0) {
        return CS_STORE_IO;
    }
    if (flush_dir(store, dir) != 0) {
        store->poisoned = true;
        return CS_STORE_IO;
    }
    return cs_store_check_media(store);
}
static int record_body(const struct cs_store *store, const struct cs_record *r, char *out,
                       size_t capacity)
{
    int n, tail;
    unsigned int schema = r->schema == 1U ? 1U : 2U;
    n = snprintf(out, capacity,
        "{\"schema\":%u,\"format\":\"retroarch-v1\",\"card\":\"%s\",\"game\":\"%s\",\"content\":\"%s"
        "\",\"core\":\"%s\",\"config\":\"%s\",\"bios\":\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s"
        "\",\"policy\":\"%s\",\"generation\":\"%" PRIu64 "\",\"frame\":\"%" PRIu64
        "\",\"state_size\":\"%" PRIu64 "\",\"state_hash\":\"%s\",\"sram_size\":\"%" PRIu64
        "\",\"sram_hash\":\"%s\",\"rtc_size\":\"%" PRIu64 "\",\"rtc_hash\":\"%s\"",
        schema, store->uuid, r->identity.game, r->identity.content, r->identity.core,
        r->identity.config, r->identity.bios, r->identity.retroarch, r->identity.disc,
        r->identity.policy, r->generation, r->frame, r->size[0], r->hash[0],
        r->size[1], r->hash[1], r->size[2], r->hash[2]);
    if (n < 0 || (size_t)n >= capacity) return -1;
    if (schema == 2U) {
        tail = snprintf(out + (size_t)n, capacity - (size_t)n,
                        ",\"screenshot_size\":\"%" PRIu64 "\",\"screenshot_hash\":\"%s\"",
                        r->size[3], r->hash[3]);
        if (tail < 0 || (size_t)tail >= capacity - (size_t)n) return -1;
        n += tail;
    }
    tail = snprintf(out + (size_t)n, capacity - (size_t)n,
                    ",\"menu\":%s,\"manifest\":\"%s\"}", r->menu ? "true" : "false", r->manifest);
    if (tail < 0 || (size_t)tail >= capacity - (size_t)n) return -1;
    return n + tail;
}
static bool encode_record(const struct cs_store *store, const struct cs_record *record,
                          char out[CS_JSON_MAX + 1U], size_t *length)
{
    char hash[65];
    int n = record_body(store, record, out, CS_JSON_MAX + 1U);
    int tail;
    if (n <= 0 || (size_t)n > CS_JSON_MAX) {
        return false;
    }
    cs_sha256_hex(out, (size_t)n, hash);
    tail = snprintf(out + (size_t)n - 1U, CS_JSON_MAX + 2U - (size_t)n, ",\"checksum\":\"%s\"}\n",
                    hash);
    if (tail < 0 || (size_t)tail >= CS_JSON_MAX + 2U - (size_t)n) {
        return false;
    }
    *length = (size_t)n - 1U + (size_t)tail;
    return true;
}
static bool copy_hash(const struct cs_json *json, const char *key, char out[65])
{
    const char *value = cs_json_string(json, key);
    if (!cs_hex_valid(value, 64U)) {
        return false;
    }
    memcpy(out, value, 65U);
    return true;
}
static bool decode_record(const struct cs_store *store, const char *data, size_t length,
                          struct cs_record *r)
{
    struct cs_json json;
    char canonical[CS_JSON_MAX + 1U], hash[65];
    uint64_t schema;
    const char *card, *format, *checksum;
    int n;
    memset(r, 0, sizeof(*r));
    if (!cs_json_parse(&json, data, length) ||
        !cs_json_u64(&json, "schema", &schema) || (schema != 1U && schema != 2U)) {
        return false;
    }
    if (schema == 1U) {
        if (!cs_json_keys(&json, legacy_record_keys,
                          sizeof(legacy_record_keys) / sizeof(legacy_record_keys[0]))) return false;
    } else {
        static const char *const keys[] = {"schema", "format", "card", "game", "content", "core",
            "config", "bios", "retroarch", "disc", "policy", "generation", "frame", "state_size",
            "state_hash", "sram_size", "sram_hash", "rtc_size", "rtc_hash", "screenshot_size",
            "screenshot_hash", "menu", "manifest", "checksum"};
        if (!cs_json_keys(&json, keys, sizeof(keys) / sizeof(keys[0]))) return false;
    }
    r->schema = (unsigned int)schema;
    card = cs_json_string(&json, "card");
    format = cs_json_string(&json, "format");
    checksum = cs_json_string(&json, "checksum");
    if (card == NULL || strcmp(card, store->uuid) != 0 || format == NULL ||
        strcmp(format, "retroarch-v1") != 0 || !cs_hex_valid(checksum, 64U)) {
        return false;
    }
    if (!copy_hash(&json, "game", r->identity.game) ||
        !copy_hash(&json, "content", r->identity.content) ||
        !copy_hash(&json, "core", r->identity.core) ||
        !copy_hash(&json, "config", r->identity.config) ||
        !copy_hash(&json, "bios", r->identity.bios) ||
        !copy_hash(&json, "retroarch", r->identity.retroarch) ||
        !copy_hash(&json, "disc", r->identity.disc) ||
        !copy_hash(&json, "policy", r->identity.policy) ||
        !copy_hash(&json, "manifest", r->manifest)) {
        return false;
    }
    if (!cs_json_u64(&json, "generation", &r->generation) || r->generation == 0U ||
        !cs_json_u64(&json, "frame", &r->frame) || !cs_json_bool(&json, "menu", &r->menu) ||
        !cs_json_u64(&json, "state_size", &r->size[0]) ||
        !cs_json_u64(&json, "sram_size", &r->size[1]) ||
        !cs_json_u64(&json, "rtc_size", &r->size[2]) ||
        !copy_hash(&json, "state_hash", r->hash[0]) || !copy_hash(&json, "sram_hash", r->hash[1]) ||
        !copy_hash(&json, "rtc_hash", r->hash[2])) {
        return false;
    }
    if (schema == 2U) {
        if (!cs_json_u64(&json, "screenshot_size", &r->size[3]) || r->size[3] == 0U ||
            r->size[3] > CS_SCREENSHOT_MAX || !copy_hash(&json, "screenshot_hash", r->hash[3]))
            return false;
    } else {
        cs_sha256_hex("", 0U, r->hash[3]);
    }
    n = record_body(store, r, canonical, sizeof(canonical));
    if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        return false;
    }
    cs_sha256_hex(canonical, (size_t)n, hash);
    return strcmp(hash, checksum) == 0;
}
static bool hash_artifact(struct cs_store *store, int dir, size_t index, bool flush, uint64_t *size,
                          char digest[65])
{
    int fd = openat(dir, artifacts[index], O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat before, after;
    struct cs_sha256 hash;
    unsigned char buffer[16384];
    uint64_t total = 0U;
    bool ok = true;
    const uint64_t limit = index == 0U ? store->max_state_bytes : aux_limits[index];
    if (fd < 0) {
        return false;
    }
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_dev != store->device || before.st_size < 0 || (uint64_t)before.st_size > limit ||
        ((index == 0U || index == 3U) && before.st_size == 0)) {
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
        if (index == 3U && total == (uint64_t)n) {
            static const unsigned char signature[8] = {137U,80U,78U,71U,13U,10U,26U,10U};
            uint32_t width, height;
            if (n < 33 || memcmp(buffer, signature, 8U) != 0 ||
                memcmp(buffer + 8U, "\0\0\0\15IHDR", 8U) != 0) { ok = false; break; }
            width = ((uint32_t)buffer[16] << 24U) | ((uint32_t)buffer[17] << 16U) |
                    ((uint32_t)buffer[18] << 8U) | buffer[19];
            height = ((uint32_t)buffer[20] << 24U) | ((uint32_t)buffer[21] << 16U) |
                     ((uint32_t)buffer[22] << 8U) | buffer[23];
            if (width == 0U || height == 0U || width > 2048U || height > 2048U) {
                ok = false; break;
            }
        }
        cs_sha256_update(&hash, buffer, (size_t)n);
    }
    if (ok &&
        (fstat(fd, &after) != 0 || before.st_size != after.st_size ||
         before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
         before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || total != (uint64_t)before.st_size)) {
        ok = false;
    }
    if (ok && flush && sync_fd(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (ok) {
        *size = total;
        cs_sha256_final(&hash, digest);
    }
    return ok;
}
static bool artifact_matches(struct cs_store *store, int dir, size_t index,
                             uint64_t expected_size, const char expected_hash[65])
{
    if (!store->read_only || !store->presentation_only) {
        uint64_t size; char digest[65];
        return hash_artifact(store, dir, index, false, &size, digest) &&
               size == expected_size && strcmp(digest, expected_hash) == 0;
    }
    int fd = openat(dir, artifacts[index], O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    uint64_t limit = index == 0U ? store->max_state_bytes : aux_limits[index];
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
              st.st_dev == store->device && st.st_size >= 0 &&
              (uint64_t)st.st_size == expected_size && expected_size <= limit &&
              ((index != 0U && index != 3U) || expected_size != 0U);
    if (ok && index == 3U) {
        unsigned char header[33];
        static const unsigned char signature[8] = {137U,80U,78U,71U,13U,10U,26U,10U};
        ssize_t got;
        do { got = pread(fd, header, sizeof(header), 0); } while (got < 0 && errno == EINTR);
        ok = got == (ssize_t)sizeof(header) && memcmp(header, signature, 8U) == 0 &&
             memcmp(header + 8U, "\0\0\0\15IHDR", 8U) == 0;
        if (ok) {
            uint32_t width = ((uint32_t)header[16] << 24U) | ((uint32_t)header[17] << 16U) |
                             ((uint32_t)header[18] << 8U) | header[19];
            uint32_t height = ((uint32_t)header[20] << 24U) | ((uint32_t)header[21] << 16U) |
                              ((uint32_t)header[22] << 8U) | header[23];
            ok = width != 0U && height != 0U && width <= 2048U && height <= 2048U;
        }
    }
    if (fd >= 0 && close(fd) != 0) ok = false;
    return ok;
}
static enum cs_store_status validate_pointer_at(struct cs_store *store, int pointer_dir,
                                                const char *name, struct cs_record *record)
{
    char buffer[CS_JSON_MAX + 1U], digest[65], dirname[40];
    size_t length, i;
    struct cs_record manifest;
    int dir;
    struct stat st;
    if (fstatat(pointer_dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    }
    if (!read_small(pointer_dir, name, buffer, &length) ||
        !decode_record(store, buffer, length, record)) {
        return CS_STORE_CORRUPT;
    }
    if (!cs_identity_equal(&record->identity, &store->identity)) {
        return CS_STORE_INCOMPATIBLE;
    }
    if (snprintf(dirname, sizeof(dirname), "g-%020" PRIu64, record->generation) < 0) {
        return CS_STORE_CORRUPT;
    }
    dir = child_dir(store, store->game_fd, dirname, false);
    if (dir < 0) {
        return CS_STORE_CORRUPT;
    }
    if (!read_small(dir, "manifest.json", buffer, &length)) {
        (void)close(dir);
        return CS_STORE_CORRUPT;
    }
    cs_sha256_hex(buffer, length, digest);
    if (strcmp(digest, record->manifest) != 0 || !decode_record(store, buffer, length, &manifest) ||
        manifest.menu || strcmp(manifest.manifest, zero_hash) != 0 ||
        manifest.schema != record->schema || manifest.generation != record->generation || manifest.frame != record->frame ||
        !cs_identity_equal(&manifest.identity, &record->identity)) {
        (void)close(dir);
        return CS_STORE_CORRUPT;
    }
    for (i = 0U; i < (record->schema == 1U ? 3U : CS_STORE_ARTIFACTS); ++i) {
        if (manifest.size[i] != record->size[i] || strcmp(manifest.hash[i], record->hash[i]) != 0 ||
            !artifact_matches(store, dir, i, record->size[i], record->hash[i])) {
            (void)close(dir);
            return CS_STORE_CORRUPT;
        }
    }
    if (close(dir) != 0) {
        return CS_STORE_IO;
    }
    return CS_STORE_OK;
}
static enum cs_store_status validate_pointer(struct cs_store *store, const char *name,
                                             struct cs_record *record)
{
    return validate_pointer_at(store, store->data_fd, name, record);
}

static enum cs_store_status load_counter(struct cs_store *store)
{
    char buffer[CS_JSON_MAX + 1U], canonical[96], hash[65];
    size_t length;
    struct cs_json json;
    const char *checksum;
    uint64_t schema;
    int n;
    struct stat st;
    static const char *const keys[] = {"schema", "highwater", "checksum"};
    if (fstatat(store->data_fd, "counter.json", &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    }
    if (!read_small(store->data_fd, "counter.json", buffer, &length) ||
        !cs_json_parse(&json, buffer, length) || !cs_json_keys(&json, keys, 3U) ||
        !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "highwater", &store->highwater)) {
        return CS_STORE_CORRUPT;
    }
    checksum = cs_json_string(&json, "checksum");
    n = snprintf(canonical, sizeof(canonical), "{\"schema\":1,\"highwater\":\"%" PRIu64 "\"}",
                 store->highwater);
    if (n < 0 || (size_t)n >= sizeof(canonical)) {
        return CS_STORE_CORRUPT;
    }
    cs_sha256_hex(canonical, (size_t)n, hash);
    return checksum != NULL && strcmp(hash, checksum) == 0 ? CS_STORE_OK : CS_STORE_CORRUPT;
}
static enum cs_store_status reserve_counter(struct cs_store *store, uint64_t generation)
{
    char body[96], data[192], hash[65];
    int n, m;
    enum cs_store_status result;
    if (generation <= store->highwater) {
        return CS_STORE_CONFLICT;
    }
    n = snprintf(body, sizeof(body), "{\"schema\":1,\"highwater\":\"%" PRIu64 "\"}", generation);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return CS_STORE_IO;
    }
    cs_sha256_hex(body, (size_t)n, hash);
    m = snprintf(data, sizeof(data),
                 "{\"schema\":1,\"highwater\":\"%" PRIu64 "\",\"checksum\":\"%s\"}\n", generation,
                 hash);
    if (m < 0 || (size_t)m >= sizeof(data)) {
        return CS_STORE_IO;
    }
    result = atomic_write(store, store->data_fd, "counter.json", data, (size_t)m);
    if (result == CS_STORE_OK) {
        store->highwater = generation;
    }
    return result;
}
enum cs_store_status cs_store_open(struct cs_store *store, const struct cs_store_config *config)
{
    struct stat st;
    struct statfs fs;
    int sessions;
    enum cs_store_status result;
    cs_store_init(store);
    if (config == NULL || !cs_identity_valid(&config->identity) || !uuid_valid(config->card_uuid) ||
        !cs_hex_valid(config->instance, 32U) || config->max_state_bytes == 0U ||
        config->max_state_bytes > 128U * 1024U * 1024U || config->card_path == NULL ||
        strlen(config->card_path) >= sizeof(store->card_path)) {
        return CS_STORE_INCOMPATIBLE;
    }
    strcpy(store->card_path, config->card_path);
    strcpy(store->uuid, config->card_uuid);
    strcpy(store->instance, config->instance);
    store->identity = config->identity;
    store->max_state_bytes = config->max_state_bytes;
    store->qualified_fat_syncfs = config->qualified_fat_syncfs;
    store->test_root = config->test_root;
    store->card_fd = open_absolute_dir(store->card_path);
    if (store->card_fd < 0 || fstat(store->card_fd, &st) != 0 ||
        fstatfs(store->card_fd, &fs) != 0) {
        return CS_STORE_MEDIA;
    }
    store->device = st.st_dev;
    store->inode = st.st_ino;
    store->fat = (unsigned long)fs.f_type == 0x4d44UL;
    if (!store->test_root && ((unsigned long)fs.f_type != 0xef53UL && !store->fat)) {
        return CS_STORE_UNSUPPORTED;
    }
    if (store->fat && !store->qualified_fat_syncfs) {
        return CS_STORE_UNSUPPORTED;
    }
    if (!store->test_root && !fd_mount_id(store->card_fd, &store->mount_id)) {
        return CS_STORE_MEDIA;
    }
    result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    store->data_fd = child_dir(store, store->card_fd, ".console", true);
    if (store->data_fd < 0) {
        return CS_STORE_IO;
    }
    store->lock_fd =
        openat(store->data_fd, ".lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (store->lock_fd < 0) {
        fprintf(stderr, "storage lock open failed: pid=%ld errno=%d path=%s/.console/.lock\n",
                (long)getpid(), errno, store->card_path);
        return CS_STORE_IO;
    }
    if (fstat(store->lock_fd, &st) != 0) {
        fprintf(stderr, "storage lock stat failed: pid=%ld errno=%d\n", (long)getpid(), errno);
        return CS_STORE_IO;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        fprintf(stderr, "storage lock invalid: pid=%ld mode=%o links=%lu\n",
                (long)getpid(), (unsigned int)st.st_mode, (unsigned long)st.st_nlink);
        return CS_STORE_CORRUPT;
    }
    if (flock(store->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        fprintf(stderr, "storage lock flock failed: pid=%ld fd=%d errno=%d dev=%lu ino=%lu path=%s/.console/.lock\n",
                (long)getpid(), store->lock_fd, error, (unsigned long)st.st_dev,
                (unsigned long)st.st_ino, store->card_path);
        if (error != EWOULDBLOCK && error != EAGAIN)
            return CS_STORE_IO;
        return CS_STORE_CONFLICT;
    }
    sessions = child_dir(store, store->data_fd, "sessions", true);
    if (sessions < 0) {
        return CS_STORE_IO;
    }
    store->game_fd = child_dir(store, sessions, store->identity.game, true);
    (void)close(sessions);
    if (store->game_fd < 0) {
        return CS_STORE_IO;
    }
    result = load_counter(store);
    if (result != CS_STORE_OK && result != CS_STORE_EMPTY) {
        return result;
    }
    if (result == CS_STORE_EMPTY) { /* A missing counter is safe only on a new, empty store. */
        struct stat current, previous;
        DIR *games;
        struct dirent *game;
        bool nonempty = false;
        int scan = child_dir(store, store->data_fd, "sessions", false);
        if (fstatat(store->data_fd, "resume.json", &current, AT_SYMLINK_NOFOLLOW) == 0 ||
            fstatat(store->data_fd, "resume.previous.json", &previous, AT_SYMLINK_NOFOLLOW) == 0) {
            return CS_STORE_CORRUPT;
        }
        if (scan < 0) {
            return CS_STORE_IO;
        }
        games = fdopendir(scan);
        if (games == NULL) {
            (void)close(scan);
            return CS_STORE_IO;
        }
        while ((game = readdir(games)) != NULL) {
            int child;
            DIR *entries;
            struct dirent *entry;
            if (strcmp(game->d_name, ".") == 0 || strcmp(game->d_name, "..") == 0) {
                continue;
            }
            if (!cs_hex_valid(game->d_name, 64U)) {
                nonempty = true;
                break;
            }
            child = child_dir(store, dirfd(games), game->d_name, false);
            if (child < 0) {
                nonempty = true;
                break;
            }
            entries = fdopendir(child);
            if (entries == NULL) {
                (void)close(child);
                nonempty = true;
                break;
            }
            while ((entry = readdir(entries)) != NULL) {
                if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                    nonempty = true;
                    break;
                }
            }
            (void)closedir(entries);
            if (nonempty) {
                break;
            }
        }
        (void)closedir(games);
        if (nonempty) {
            return CS_STORE_CORRUPT;
        }
    }
    return CS_STORE_OK;
}
static enum cs_store_status attempt_blocked(struct cs_store *store)
{
    struct stat st;
    char data[CS_JSON_MAX + 1U], body[192], hash[65];
    size_t length;
    struct cs_json json;
    uint64_t gen, schema;
    bool completed;
    const char *checksum;
    int n;
    static const char *const keys[] = {"schema", "generation", "completed", "checksum"};
    if (fstatat(store->data_fd, "resume-attempt.json", &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? CS_STORE_OK : CS_STORE_IO;
    }
    if (!read_small(store->data_fd, "resume-attempt.json", data, &length) ||
        !cs_json_parse(&json, data, length) || !cs_json_keys(&json, keys, 4U) ||
        !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "generation", &gen) || !cs_json_bool(&json, "completed", &completed)) {
        return CS_STORE_ATTEMPT_BLOCKED;
    }
    n = snprintf(body, sizeof(body),
                 "{\"schema\":1,\"generation\":\"%" PRIu64 "\",\"completed\":%s}", gen,
                 completed ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return CS_STORE_CORRUPT;
    }
    cs_sha256_hex(body, (size_t)n, hash);
    checksum = cs_json_string(&json, "checksum");
    if (checksum == NULL || strcmp(hash, checksum) != 0) {
        return CS_STORE_ATTEMPT_BLOCKED;
    }
    return gen == store->selected.generation && !completed ? CS_STORE_ATTEMPT_BLOCKED : CS_STORE_OK;
}
enum cs_store_status cs_store_load(struct cs_store *store)
{
    struct cs_record record;
    enum cs_store_status current, previous, media = cs_store_check_media(store);
    if (media != CS_STORE_OK) {
        return media;
    }
    store->has_selected = false;
    store->fallback = false;
    current = validate_pointer(store, "resume.json", &record);
    if (current != CS_STORE_OK) {
        previous = validate_pointer(store, "resume.previous.json", &record);
        if (previous != CS_STORE_OK) {
            return current == CS_STORE_EMPTY && previous == CS_STORE_EMPTY
                       ? CS_STORE_EMPTY
                       : (current == CS_STORE_INCOMPATIBLE || previous == CS_STORE_INCOMPATIBLE
                              ? CS_STORE_INCOMPATIBLE
                              : CS_STORE_CORRUPT);
        }
        store->fallback = true;
    }
    if (record.generation > store->highwater) {
        return CS_STORE_CORRUPT;
    }
    store->selected = record;
    store->has_selected = true;
    if (!record.menu) {
        enum cs_store_status attempt = attempt_blocked(store);
        if (attempt != CS_STORE_OK) {
            return attempt;
        }
    }
    return cs_store_check_media(store);
}
enum cs_store_status cs_store_stage(struct cs_store *store, uint64_t generation, char relative[256])
{
    struct statvfs vfs;
    enum cs_store_status result;
    int dir;
    size_t i;
    uint64_t needed = store->max_state_bytes + aux_limits[1] + aux_limits[2] + aux_limits[3] + 65536U;
    if (store->read_only) {
        return CS_STORE_CONFLICT;
    }
    result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (fstatvfs(store->card_fd, &vfs) != 0 || vfs.f_frsize == 0U ||
        vfs.f_bavail < needed / (uint64_t)vfs.f_frsize + 1U) {
        return CS_STORE_IO;
    }
    result = reserve_counter(store, generation);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (snprintf(store->staging, sizeof(store->staging), "s-%020" PRIu64 "-%s", generation,
                 store->instance) < 0) {
        return CS_STORE_IO;
    }
    if (mkdirat(store->game_fd, store->staging, 0700) != 0) {
        return CS_STORE_CONFLICT;
    }
    dir = child_dir(store, store->game_fd, store->staging, false);
    if (dir < 0) {
        return CS_STORE_IO;
    }
    for (i = 0U; i < CS_STORE_ARTIFACTS; ++i) {
        int fd =
            openat(dir, artifacts[i], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        bool ok = fd >= 0;
        const uint64_t limit = i == 0U ? store->max_state_bytes : aux_limits[i];
        if (ok && !store->fat && !store->test_root &&
            fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)limit) != 0) {
            ok = false;
        }
        if (ok && sync_fd(fd) != 0) {
            ok = false;
        }
        if (fd >= 0 && close(fd) != 0) {
            ok = false;
        }
        if (!ok) {
            (void)close(dir);
            return CS_STORE_IO;
        }
    }
    if (flush_dir(store, dir) != 0 || flush_dir(store, store->game_fd) != 0) {
        (void)close(dir);
        return CS_STORE_IO;
    }
    (void)close(dir);
    store->staging_generation = generation;
    if (snprintf(relative, 256U, "sessions/%s/%s", store->identity.game, store->staging) < 0) {
        return CS_STORE_IO;
    }
    return cs_store_check_media(store);
}
enum cs_store_status cs_store_commit(struct cs_store *store, uint64_t frame)
{
    struct cs_record record, old;
    char data[CS_JSON_MAX + 1U], final[40];
    size_t length, i;
    int dir;
    struct stat st;
    enum cs_store_status result;
    if (store->read_only) {
        return CS_STORE_CONFLICT;
    }
    result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (store->staging_generation == 0U) {
        return CS_STORE_CONFLICT;
    }
    memset(&record, 0, sizeof(record));
    record.identity = store->identity;
    record.schema = 2U;
    record.generation = store->staging_generation;
    record.frame = frame;
    strcpy(record.manifest, zero_hash);
    dir = child_dir(store, store->game_fd, store->staging, false);
    if (dir < 0) {
        return CS_STORE_IO;
    }
    for (i = 0U; i < CS_STORE_ARTIFACTS; ++i) {
        if (!hash_artifact(store, dir, i, true, &record.size[i], record.hash[i])) {
            (void)close(dir);
            return CS_STORE_CORRUPT;
        }
    }
    if (!encode_record(store, &record, data, &length)) {
        (void)close(dir);
        return CS_STORE_CORRUPT;
    }
    result = atomic_write(store, dir, "manifest.json", data, length);
    if (result != CS_STORE_OK) {
        (void)close(dir);
        return result;
    }
    cs_sha256_hex(data, length, record.manifest);
    if (close(dir) != 0) {
        return CS_STORE_IO;
    }
    if (snprintf(final, sizeof(final), "g-%020" PRIu64, record.generation) < 0) {
        return CS_STORE_IO;
    }
    if (fstatat(store->game_fd, final, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        return CS_STORE_CONFLICT;
    }
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return CS_STORE_MEDIA;
    }
    if (syscall(SYS_renameat2, store->game_fd, store->staging, store->game_fd, final,
                RENAME_NOREPLACE) != 0) {
        return CS_STORE_IO;
    }
    store->staging_generation = 0U;
    if (flush_dir(store, store->game_fd) != 0) {
        store->poisoned = true;
        return CS_STORE_IO;
    }
    /* Never overwrite a known-good previous record with corrupt current data. */
    if (validate_pointer(store, "resume.json", &old) == CS_STORE_OK) {
        if (!encode_record(store, &old, data, &length)) {
            return CS_STORE_CORRUPT;
        }
        result = atomic_write(store, store->data_fd, "resume.previous.json", data, length);
        if (result != CS_STORE_OK) {
            return result;
        }
    }
    if (!encode_record(store, &record, data, &length)) {
        return CS_STORE_CORRUPT;
    }
    result = atomic_write(store, store->data_fd, "resume.json", data, length);
    if (result != CS_STORE_OK) {
        return result;
    }
    store->selected = record;
    store->has_selected = true;
    store->fallback = false;
    return CS_STORE_OK;
}
enum cs_store_status cs_store_menu(struct cs_store *store)
{
    char data[CS_JSON_MAX + 1U];
    size_t length;
    struct cs_record record;
    enum cs_store_status result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (!store->has_selected) {
        return flush_dir(store, store->data_fd) == 0 ? CS_STORE_OK : CS_STORE_IO;
    }
    record = store->selected;
    record.menu = true;
    if (!encode_record(store, &record, data, &length)) {
        return CS_STORE_CORRUPT;
    }
    result = atomic_write(store, store->data_fd, "resume.json", data, length);
    if (result == CS_STORE_OK) {
        store->selected = record;
        store->fallback = false;
    }
    return result;
}
enum cs_store_status cs_store_verify_selected(struct cs_store *store, uint64_t generation,
                                              char relative[256])
{
    struct cs_record verified;
    enum cs_store_status result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    result = validate_pointer(store, store->fallback ? "resume.previous.json" : "resume.json",
                              &verified);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (verified.generation != generation) {
        return CS_STORE_INCOMPATIBLE;
    }
    if (snprintf(relative, 256U, "sessions/%s/g-%020" PRIu64, verified.identity.game, generation) <
        0) {
        return CS_STORE_IO;
    }
    store->selected = verified;
    store->has_selected = true;
    return CS_STORE_OK;
}
enum cs_store_status cs_store_attempt(struct cs_store *store, bool completed)
{
    char body[192], data[320], hash[65];
    int n, m;
    if (!store->has_selected) {
        return CS_STORE_EMPTY;
    }
    n = snprintf(body, sizeof(body),
                 "{\"schema\":1,\"generation\":\"%" PRIu64 "\",\"completed\":%s}",
                 store->selected.generation, completed ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return CS_STORE_IO;
    }
    cs_sha256_hex(body, (size_t)n, hash);
    m = snprintf(data, sizeof(data),
                 "{\"schema\":1,\"generation\":\"%" PRIu64
                 "\",\"completed\":%s,\"checksum\":\"%s\"}\n",
                 store->selected.generation, completed ? "true" : "false", hash);
    if (m < 0 || (size_t)m >= sizeof(data)) {
        return CS_STORE_IO;
    }
    return atomic_write(store, store->data_fd, "resume-attempt.json", data, (size_t)m);
}

/* Launcher readers never take the daemon's writer lock or create an empty store.
 * The immutable-generation path and existing decoder/hasher are shared so UI
 * markers cannot silently introduce a weaker definition of a valid checkpoint. */
enum cs_store_status cs_store_open_reader(struct cs_store *store,
                                          const struct cs_store_config *config)
{
    struct stat st;
    struct statfs fs;
    enum cs_store_status result = CS_STORE_MEDIA;
    cs_store_init(store);
    store->read_only = true;
    if (config == NULL || !uuid_valid(config->card_uuid) || config->card_path == NULL ||
        strlen(config->card_path) >= sizeof(store->card_path) || config->max_state_bytes == 0U ||
        config->max_state_bytes > 128U * 1024U * 1024U) {
        return CS_STORE_INCOMPATIBLE;
    }
    strcpy(store->card_path, config->card_path);
    strcpy(store->uuid, config->card_uuid);
    store->max_state_bytes = config->max_state_bytes;
    store->qualified_fat_syncfs = config->qualified_fat_syncfs;
    store->test_root = config->test_root;
    store->card_fd = open_absolute_dir(store->card_path);
    if (store->card_fd < 0 || fstat(store->card_fd, &st) != 0 ||
        fstatfs(store->card_fd, &fs) != 0) {
        goto fail;
    }
    store->device = st.st_dev;
    store->inode = st.st_ino;
    store->fat = (unsigned long)fs.f_type == 0x4d44UL;
    if ((!store->test_root && (unsigned long)fs.f_type != 0xef53UL && !store->fat) ||
        (store->fat && !store->qualified_fat_syncfs)) {
        result = CS_STORE_UNSUPPORTED;
        goto fail;
    }
    if (!store->test_root && !fd_mount_id(store->card_fd, &store->mount_id)) {
        goto fail;
    }
    result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        goto fail;
    }
    store->data_fd = child_dir(store, store->card_fd, ".console", false);
    if (store->data_fd < 0) {
        result = errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
        goto fail;
    }
    return CS_STORE_OK;
fail:
    cs_store_close(store);
    store->read_only = true;
    return result;
}

static int reader_game(struct cs_store *store, const struct cs_identity *identity)
{
    int sessions, game;
    sessions = child_dir(store, store->data_fd, "sessions", false);
    if (sessions < 0) {
        return -1;
    }
    game = child_dir(store, sessions, identity->game, false);
    if (close(sessions) != 0) {
        if (game >= 0) {
            (void)close(game);
        }
        return -1;
    }
    return game;
}

enum cs_store_status cs_store_read_checkpoint(struct cs_store *store,
                                              const struct cs_identity *identity,
                                              uint64_t generation, const char manifest_hash[65],
                                              struct cs_record *record)
{
    struct cs_store view = *store;
    struct cs_record candidate;
    char name[40], data[CS_JSON_MAX + 1U], digest[65];
    size_t length, i;
    int dir = -1;
    enum cs_store_status result = cs_store_check_media(store);
    if (result != CS_STORE_OK) {
        return result;
    }
    if (record == NULL || identity == NULL || !cs_identity_valid(identity) || generation == 0U ||
        !cs_hex_valid(manifest_hash, 64U)) {
        return CS_STORE_INCOMPATIBLE;
    }
    view.game_fd = reader_game(store, identity);
    if (view.game_fd < 0) {
        return CS_STORE_CORRUPT;
    }
    (void)snprintf(name, sizeof(name), "g-%020" PRIu64, generation);
    dir = child_dir(store, view.game_fd, name, false);
    result = CS_STORE_CORRUPT;
    if (dir < 0 || !read_small(dir, "manifest.json", data, &length)) {
        goto done;
    }
    cs_sha256_hex(data, length, digest);
    if (strcmp(digest, manifest_hash) != 0 || !decode_record(store, data, length, &candidate) ||
        candidate.generation != generation || candidate.menu ||
        strcmp(candidate.manifest, zero_hash) != 0) {
        goto done;
    }
    if (!cs_identity_equal(identity, &candidate.identity)) {
        result = CS_STORE_INCOMPATIBLE;
        goto done;
    }
    for (i = 0U; i < (candidate.schema == 1U ? 3U : CS_STORE_ARTIFACTS); ++i) {
        if (!artifact_matches(store, dir, i, candidate.size[i], candidate.hash[i])) {
            goto done;
        }
    }
    strcpy(candidate.manifest, manifest_hash);
    result = cs_store_check_media(store);
done:
    if (dir >= 0 && close(dir) != 0) {
        result = CS_STORE_IO;
    }
    if (close(view.game_fd) != 0) {
        result = CS_STORE_IO;
    }
    if (result == CS_STORE_OK) {
        *record = candidate;
    }
    return result;
}

enum cs_store_status cs_store_read_resume(struct cs_store *store, bool previous,
                                          struct cs_record *record)
{
    const char *name = previous ? "resume.previous.json" : "resume.json";
    char data[CS_JSON_MAX + 1U];
    struct cs_record candidate;
    struct cs_store view = *store;
    struct stat st;
    size_t length;
    enum cs_store_status result = cs_store_check_media(store);
    if (result != CS_STORE_OK || record == NULL) {
        return result == CS_STORE_OK ? CS_STORE_INCOMPATIBLE : result;
    }
    if (fstatat(store->data_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    }
    if (!read_small(store->data_fd, name, data, &length) ||
        !decode_record(store, data, length, &candidate)) {
        return CS_STORE_CORRUPT;
    }
    view.identity = candidate.identity;
    view.game_fd = reader_game(store, &view.identity);
    if (view.game_fd < 0) {
        return CS_STORE_CORRUPT;
    }
    /* Re-read atomically replaced pointer through the existing complete validator. */
    result = validate_pointer(&view, name, &candidate);
    if (close(view.game_fd) != 0) {
        result = CS_STORE_IO;
    }
    if (result == CS_STORE_OK) {
        result = cs_store_check_media(store);
    }
    if (result == CS_STORE_OK) {
        *record = candidate;
    }
    return result;
}

/* Slot pointers contain the complete checksummed generation reference. The
 * generation remains immutable; replacing a slot never overwrites state bytes. */
enum cs_store_status cs_store_export_screenshot(struct cs_store *store)
{
    struct cs_record record;
    enum cs_store_status status;
    char dirname[40], name[112], digest[65];
    unsigned char *data = NULL;
    size_t used = 0U;
    int dir = -1, fd = -1, destination = -1;
    if (store->read_only || !store->has_selected) return CS_STORE_CONFLICT;
    status = cs_store_read_checkpoint(store, &store->identity, store->selected.generation,
                                      store->selected.manifest, &record);
    if (status != CS_STORE_OK) return status;
    if (record.schema != 2U || record.size[3] == 0U || record.size[3] > CS_SCREENSHOT_MAX)
        return CS_STORE_INCOMPATIBLE;
    status = CS_STORE_IO;
    data = malloc((size_t)record.size[3]);
    if (data == NULL) goto done;
    (void)snprintf(dirname, sizeof(dirname), "g-%020" PRIu64, record.generation);
    dir = child_dir(store, store->game_fd, dirname, false);
    if (dir < 0) goto done;
    fd = openat(dir, "screenshot.png", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) goto done;
    while (used < record.size[3]) {
        ssize_t n = read(fd, data + used, (size_t)record.size[3] - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    cs_sha256_hex(data, used, digest);
    if (strcmp(digest, record.hash[3]) != 0) {
        status = CS_STORE_CORRUPT;
        goto done;
    }
    destination = child_dir(store, store->card_fd, "Screenshots", true);
    if (destination < 0) goto done;
    (void)snprintf(name, sizeof(name), "%s-%020" PRIu64 ".png", store->identity.game,
                   record.generation);
    status = atomic_write(store, destination, name, (const char *)data, used);
    if (status == CS_STORE_OK && flush_dir(store, store->card_fd) != 0) status = CS_STORE_IO;
done:
    if (fd >= 0 && close(fd) != 0) status = CS_STORE_IO;
    if (dir >= 0 && close(dir) != 0) status = CS_STORE_IO;
    if (destination >= 0 && close(destination) != 0) status = CS_STORE_IO;
    free(data);
    return status;
}

enum cs_store_status cs_store_delete_states(struct cs_store *store, const char game[65])
{
    if (store->read_only || store->lock_fd < 0 || !cs_hex_valid(game,64U) ||
        cs_store_check_media(store) != CS_STORE_OK) return CS_STORE_CONFLICT;
    struct cs_identity identity = store->identity;
    memcpy(identity.game,game,65U);
    int game_fd = reader_game(store,&identity);
    if (game_fd < 0) return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    bool ok = true;
    /* Retire only this game's global resume pointers before deleting bytes.
     * They are independently checksummed, so a broken state cannot trap Reset. */
    for (unsigned int i = 0U; i < 2U && ok; ++i) {
        char bytes[CS_JSON_MAX+1U]; size_t used = 0U; struct cs_record record;
        const char *name = i == 0U ? "resume.json" : "resume.previous.json";
        if (read_small(store->data_fd,name,bytes,&used) && decode_record(store,bytes,used,&record) &&
            strcmp(record.identity.game,game) == 0)
            ok = unlinkat(store->data_fd,name,0) == 0;
    }
    if (ok) ok = flush_dir(store,store->data_fd) == 0;
    for (unsigned int slot = 0U; slot < CS_MANUAL_SLOTS && ok; ++slot) {
        char name[32]; (void)snprintf(name,sizeof(name),"slot-%02u.json",slot);
        if (unlinkat(game_fd,name,0) != 0 && errno != ENOENT) ok = false;
    }
    int duplicate = fcntl(game_fd,F_DUPFD_CLOEXEC,3);
    DIR *directory = duplicate >= 0 ? fdopendir(duplicate) : NULL;
    if (directory == NULL) { if (duplicate >= 0) (void)close(duplicate); ok = false; }
    if (directory != NULL) {
        struct dirent *entry;
        while (ok) {
            errno = 0; entry = readdir(directory);
            if (entry == NULL) { if (errno != 0) ok = false; break; }
            if (strlen(entry->d_name) != 22U || strncmp(entry->d_name,"g-",2U) != 0 ||
                strspn(entry->d_name+2U,"0123456789") != 20U) continue;
            int generation = openat(game_fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            struct stat st;
            if (generation < 0 || fstat(generation,&st) != 0 || st.st_dev != store->device) {
                if (generation >= 0) (void)close(generation);
                ok = false;
                break;
            }
            const char *const remove[] = {"state.bin","screenshot.png"};
            for (size_t i = 0U; i < 2U; ++i)
                if (unlinkat(generation,remove[i],0) != 0 && errno != ENOENT) ok = false;
            if (flush_dir(store,generation) != 0) ok = false;
            if (close(generation) != 0) ok = false;
        }
        if (closedir(directory) != 0) ok = false;
    }
    if (flush_dir(store,game_fd) != 0) ok = false;
    if (close(game_fd) != 0) ok = false;
    if (!ok) { store->poisoned = true; return CS_STORE_IO; }
    if (strcmp(store->identity.game,game) == 0) store->has_selected = false;
    return cs_store_sync(store);
}
enum cs_store_status cs_store_slot_save(struct cs_store *store, unsigned int slot)
{
    struct cs_record verified;
    char name[32], data[CS_JSON_MAX + 1U];
    size_t length;
    enum cs_store_status status;
    if (slot >= CS_MANUAL_SLOTS || store->read_only || !store->has_selected)
        return CS_STORE_CONFLICT;
    status = cs_store_read_checkpoint(store, &store->identity, store->selected.generation,
                                      store->selected.manifest, &verified);
    if (status != CS_STORE_OK) return status;
    verified.menu = false;
    if (!encode_record(store, &verified, data, &length)) return CS_STORE_CORRUPT;
    (void)snprintf(name, sizeof(name), "slot-%02u.json", slot);
    return atomic_write(store, store->game_fd, name, data, length);
}
enum cs_store_status cs_store_slot_read(struct cs_store *store, const struct cs_identity *identity,
                                        unsigned int slot, struct cs_record *record)
{
    struct cs_store view;
    struct cs_record candidate;
    enum cs_store_status status;
    char name[32];
    if (slot >= CS_MANUAL_SLOTS || identity == NULL || record == NULL || !cs_identity_valid(identity))
        return CS_STORE_INCOMPATIBLE;
    status = cs_store_check_media(store);
    if (status != CS_STORE_OK) return status;
    view = *store;
    view.identity = *identity;
    view.game_fd = reader_game(store, identity);
    if (view.game_fd < 0) return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    (void)snprintf(name, sizeof(name), "slot-%02u.json", slot);
    status = validate_pointer_at(&view, view.game_fd, name, &candidate);
    if (close(view.game_fd) != 0 && status == CS_STORE_OK) status = CS_STORE_IO;
    if (status == CS_STORE_OK) {
        status = cs_store_check_media(store);
        if (status == CS_STORE_OK) *record = candidate;
    }
    return status;
}
