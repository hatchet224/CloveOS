#include "sessiond/durability.h"
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Task-handler-only disk I/O. No RetroArch or socket calls in this file. */
#include "magicx_bridge.h"
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct mx_volume {
    int card, root, game, directory;
    dev_t device;
    ino_t inode;
    uint64_t mount_id;
    bool fat;
};
static const char *const artifact_names[] = {"state.bin", "sram.bin", "rtc.bin", "screenshot.png"};
static const char zero_hash[] = "0000000000000000000000000000000000000000000000000000000000000000";

void magicx_blob_free(struct magicx_blob artifacts[MAGICX_ARTIFACT_COUNT])
{
    size_t i;
    for (i = 0U; i < MAGICX_ARTIFACT_COUNT; ++i) {
        free(artifacts[i].data);
        artifacts[i].data = NULL;
        artifacts[i].size = 0U;
    }
}
static bool sync_file(int fd)
{
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}
static bool sync_directory(int fd, bool fat, const struct magicx_config *config)
{
    return cs_flush_directory(fd, fat, config->qualified_fat_syncfs) == 0;
}
static int open_absolute_dir(const char *path)
{
    int fd;
    const char *p;
    size_t length;
    if (path == NULL || path[0] != '/' || (length = strlen(path)) < 2U ||
        length >= MAGICX_PATH_MAX || path[length - 1U] == '/') {
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
                char decoded[MAGICX_PATH_MAX];
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

static int child(int parent, const char *name, const struct mx_volume *volume, uid_t owner)
{
    struct stat st;
    uint64_t mount_id;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_dev != volume->device || st.st_uid != owner ||
        (st.st_mode & 0022U) != 0U || !fd_mount_id(fd, &mount_id) || mount_id != volume->mount_id) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
static bool same_volume(const struct magicx_io *job, const struct mx_volume *volume)
{
    int fd = open_absolute_dir(job->config.card_path);
    struct stat st, block;
    struct statvfs vfs;
    uint64_t mount_id;
    bool ok;
    char uuid_path[128];
    if (fd < 0) {
        return false;
    }
    ok = fstat(fd, &st) == 0 && st.st_dev == volume->device && st.st_ino == volume->inode &&
         fstatvfs(fd, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) == 0U && fd_mount_id(fd, &mount_id) &&
         mount_id == volume->mount_id;
    (void)close(fd);
#ifdef MAGICX_TESTING
    if (job->config.test_directory) {
        return ok;
    }
#endif
    if (snprintf(uuid_path, sizeof(uuid_path), "/dev/disk/by-uuid/%s", job->config.card_uuid) < 0) {
        return false;
    }
    return ok && mount_path_matches(volume->mount_id, job->config.card_path, volume->fat) &&
           stat(uuid_path, &block) == 0 && S_ISBLK(block.st_mode) &&
           block.st_rdev == volume->device;
}
static bool volume_open(const struct magicx_io *job, struct mx_volume *volume)
{
    struct stat st;
    struct statfs fs;
    char name[96], expected[256];
    int sessions;
    memset(volume, 0, sizeof(*volume));
    volume->card = volume->root = volume->game = volume->directory = -1;
    volume->card = open_absolute_dir(job->config.card_path);
    if (volume->card < 0 || fstat(volume->card, &st) != 0 || fstatfs(volume->card, &fs) != 0 ||
        !fd_mount_id(volume->card, &volume->mount_id)) {
        return false;
    }
    volume->device = st.st_dev;
    volume->inode = st.st_ino;
    volume->fat = (unsigned long)fs.f_type == 0x4d44UL;
#ifdef MAGICX_TESTING
    if (!job->config.test_directory)
#endif
    {
        if (((unsigned long)fs.f_type != 0xef53UL && !volume->fat) ||
            (volume->fat && !job->config.qualified_fat_syncfs)) {
            return false;
        }
    }
    if (!same_volume(job, volume)) {
        return false;
    }
    volume->root = child(volume->card, ".console", volume, job->config.daemon_uid);
    if (volume->root < 0) {
        return false;
    }
    sessions = child(volume->root, "sessions", volume, job->config.daemon_uid);
    if (sessions < 0) {
        return false;
    }
    volume->game = child(sessions, job->identity.game, volume, job->config.daemon_uid);
    (void)close(sessions);
    if (volume->game < 0) {
        return false;
    }
    if (job->kind == MAGICX_IO_WRITE) {
        if (snprintf(name, sizeof(name), "s-%020" PRIu64 "-%s", job->generation, job->instance) <
            0) {
            return false;
        }
    } else if (snprintf(name, sizeof(name), "g-%020" PRIu64, job->generation) < 0) {
        return false;
    }
    if (snprintf(expected, sizeof(expected), "sessions/%s/%s", job->identity.game, name) < 0 ||
        strcmp(expected, job->directory) != 0) {
        return false;
    }
    volume->directory = child(volume->game, name, volume, job->config.daemon_uid);
    return volume->directory >= 0;
}
static bool volume_close(struct mx_volume *volume)
{
    int *fds[] = {&volume->directory, &volume->game, &volume->root, &volume->card};
    bool ok = true;
    size_t i;
    for (i = 0U; i < sizeof(fds) / sizeof(fds[0]); ++i) {
        if (*fds[i] >= 0) {
            if (close(*fds[i]) != 0) {
                ok = false;
            }
            *fds[i] = -1;
        }
    }
    return ok;
}
static uint64_t artifact_limit(const struct magicx_io *job, size_t index)
{
    return index == 0U ? job->config.max_state_bytes : (index == 1U ? 8U * 1024U * 1024U :
                                              (index == 2U ? 65536U : 4U * 1024U * 1024U));
}
static bool regular_file(int fd, const struct mx_volume *volume, uid_t owner, struct stat *st)
{
    return fstat(fd, st) == 0 && S_ISREG(st->st_mode) && st->st_nlink == 1 &&
           st->st_dev == volume->device && st->st_uid == owner && (st->st_mode & 0022U) == 0U &&
           st->st_size >= 0;
}
static bool write_artifact(const struct magicx_io *job, const struct mx_volume *volume,
                           size_t index)
{
    int fd = openat(volume->directory, artifact_names[index],
                    O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat before, after;
    size_t used = 0U;
    bool ok;
    const struct magicx_blob *blob = &job->artifacts[index];
    if (fd < 0) {
        return false;
    }
    /* Files were precreated/reserved by sessiond. Never O_TRUNC or recreate. */
    ok = regular_file(fd, volume, job->config.daemon_uid, &before) && before.st_size == 0 &&
         ((index != 0U && index != 3U) || blob->size != 0U) && blob->size <= artifact_limit(job, index) &&
         (blob->size == 0U || blob->data != NULL);
    while (ok && used < blob->size) {
        ssize_t n = write(fd, (const unsigned char *)blob->data + used, blob->size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        used += (size_t)n;
    }
    if (ok) {
        ok = ftruncate(fd, (off_t)blob->size) == 0 && sync_file(fd) &&
             regular_file(fd, volume, job->config.daemon_uid, &after) &&
             before.st_ino == after.st_ino && (uint64_t)after.st_size == blob->size;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}
static bool read_file(int directory, const char *name, const struct mx_volume *volume, uid_t owner,
                      uint64_t limit, struct magicx_blob *blob)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat before, after;
    size_t used = 0U;
    bool ok;
    if (fd < 0) {
        return false;
    }
    ok = regular_file(fd, volume, owner, &before) && (uint64_t)before.st_size <= limit;
    if (ok) {
        blob->size = (size_t)before.st_size;
        blob->data = calloc(blob->size + 1U, 1U);
        ok = blob->data != NULL;
    }
    while (ok && used < blob->size) {
        ssize_t n = read(fd, (unsigned char *)blob->data + used, blob->size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        used += (size_t)n;
    }
    if (ok) {
        unsigned char extra;
        ssize_t n;
        do {
            n = read(fd, &extra, 1U);
        } while (n < 0 && errno == EINTR);
        ok = n == 0 && fstat(fd, &after) == 0 && before.st_size == after.st_size &&
             before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
             before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    return ok;
}
static bool load_bundle(struct magicx_io *job, const struct mx_volume *volume)
{
    static const char *const keys[] = {
        "schema",   "format",     "card",       "game",      "content",   "core",
        "config",   "bios",       "retroarch",  "disc",      "policy",    "generation",
        "frame",    "state_size", "state_hash", "sram_size", "sram_hash", "rtc_size",
        "rtc_hash", "menu",       "manifest",   "checksum"};
    static const char *const keys_v2[] = {
        "schema", "format", "card", "game", "content", "core", "config", "bios", "retroarch",
        "disc", "policy", "generation", "frame", "state_size", "state_hash", "sram_size",
        "sram_hash", "rtc_size", "rtc_hash", "screenshot_size", "screenshot_hash", "menu",
        "manifest", "checksum"};
    static const char *const identity_keys[] = {"game", "content",   "core", "config",
                                                "bios", "retroarch", "disc", "policy"};
    const char *const identities[] = {
        job->identity.game, job->identity.content,   job->identity.core, job->identity.config,
        job->identity.bios, job->identity.retroarch, job->identity.disc, job->identity.policy};
    static const char *const size_keys[] = {"state_size", "sram_size", "rtc_size", "screenshot_size"};
    static const char *const hash_keys[] = {"state_hash", "sram_hash", "rtc_hash", "screenshot_hash"};
    struct magicx_blob manifest = {0};
    struct cs_json json;
    char body[CS_JSON_MAX + 1U], hash[65];
    uint64_t schema, generation, sizes[MAGICX_ARTIFACT_COUNT];
    bool menu, ok = false;
    const char *format, *card, *digest, *checksum, *hashes[MAGICX_ARTIFACT_COUNT];
    size_t i, count;
    int n, tail;
    if (!read_file(volume->directory, "manifest.json", volume, job->config.daemon_uid, CS_JSON_MAX,
                   &manifest) ||
        !cs_json_parse(&json, manifest.data, manifest.size) ||
        !cs_json_u64(&json, "schema", &schema) || (schema != 1U && schema != 2U) ||
        !(schema == 1U ? cs_json_keys(&json, keys, 22U) : cs_json_keys(&json, keys_v2, 24U)) ||
        !cs_json_u64(&json, "generation", &generation) || generation != job->generation ||
        !cs_json_u64(&json, "frame", &job->frame) || !cs_json_bool(&json, "menu", &menu) || menu) {
        goto done;
    }
    for (i = 0U; i < 8U; ++i) {
        const char *value = cs_json_string(&json, identity_keys[i]);
        if (value == NULL || strcmp(value, identities[i]) != 0) {
            goto done;
        }
    }
    format = cs_json_string(&json, "format");
    card = cs_json_string(&json, "card");
    digest = cs_json_string(&json, "manifest");
    checksum = cs_json_string(&json, "checksum");
    if (format == NULL || strcmp(format, "retroarch-v1") != 0 || card == NULL ||
        strcmp(card, job->config.card_uuid) != 0 || digest == NULL ||
        strcmp(digest, zero_hash) != 0 || !cs_hex_valid(checksum, 64U)) {
        goto done;
    }
    count = schema == 1U ? 3U : MAGICX_ARTIFACT_COUNT;
    for (i = 0U; i < count; ++i) {
        hashes[i] = cs_json_string(&json, hash_keys[i]);
        if (!cs_json_u64(&json, size_keys[i], &sizes[i]) || sizes[i] > artifact_limit(job, i) ||
            ((i == 0U || i == 3U) && sizes[i] == 0U) || !cs_hex_valid(hashes[i], 64U)) {
            goto done;
        }
    }
    n = snprintf(body, sizeof(body),
                 "{\"schema\":%u,\"format\":\"retroarch-v1\",\"card\":\"%s\",\"game\":\"%s\","
                 "\"content\":\"%s\",\"core\":\"%s\",\"config\":\"%s\",\"bios\":\"%s\","
                 "\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%s\",\"generation\":\"%" PRIu64
                 "\",\"frame\":\"%" PRIu64 "\",\"state_size\":\"%" PRIu64
                 "\",\"state_hash\":\"%s\",\"sram_size\":\"%" PRIu64
                 "\",\"sram_hash\":\"%s\",\"rtc_size\":\"%" PRIu64
                 "\",\"rtc_hash\":\"%s\"",
                 (unsigned int)schema, card, identities[0], identities[1], identities[2], identities[3], identities[4],
                 identities[5], identities[6], identities[7], job->generation, job->frame, sizes[0],
                 hashes[0], sizes[1], hashes[1], sizes[2], hashes[2]);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        goto done;
    }
    if (schema == 2U) {
        tail = snprintf(body + (size_t)n, sizeof(body) - (size_t)n,
                        ",\"screenshot_size\":\"%" PRIu64 "\",\"screenshot_hash\":\"%s\"",
                        sizes[3], hashes[3]);
        if (tail < 0 || (size_t)tail >= sizeof(body) - (size_t)n) goto done;
        n += tail;
    }
    tail = snprintf(body + (size_t)n, sizeof(body) - (size_t)n,
                    ",\"menu\":false,\"manifest\":\"%s\"}", zero_hash);
    if (tail < 0 || (size_t)tail >= sizeof(body) - (size_t)n) goto done;
    n += tail;
    cs_sha256_hex(body, (size_t)n, hash);
    if (strcmp(hash, checksum) != 0) {
        goto done;
    }
    for (i = 0U; i < count; ++i) {
        if (!read_file(volume->directory, artifact_names[i], volume, job->config.daemon_uid,
                       sizes[i], &job->artifacts[i]) ||
            job->artifacts[i].size != sizes[i]) {
            goto done;
        }
        cs_sha256_hex(job->artifacts[i].data, job->artifacts[i].size, hash);
        if (strcmp(hash, hashes[i]) != 0) {
            goto done;
        }
    }
    ok = true;
done:
    free(manifest.data);
    return ok;
}
void magicx_bridge_io_run(struct magicx_io *job)
{
    struct mx_volume volume;
    bool ok = volume_open(job, &volume);
    size_t i;
    if (ok && job->kind == MAGICX_IO_WRITE) {
        for (i = 0U; i < MAGICX_ARTIFACT_COUNT && ok; ++i) {
            ok = write_artifact(job, &volume, i);
        }
        if (ok) {
            ok = sync_directory(volume.directory, volume.fat, &job->config);
        }
    } else if (ok) {
        ok = load_bundle(job, &volume);
    }
    if (ok) {
        ok = same_volume(job, &volume);
    }
    if (!volume_close(&volume)) {
        ok = false;
    }
    job->success = ok;
    if (!ok) {
        (void)snprintf(job->error, sizeof(job->error),
                       "artifact IO, validation, or durable flush failed");
    }
}
