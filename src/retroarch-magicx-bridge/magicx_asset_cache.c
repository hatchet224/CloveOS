/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "magicx_asset_cache.h"
#include "sessiond/sha256.h"
#include "sessiond/json.h"
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
static bool same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mode == b->st_mode && a->st_uid == b->st_uid &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
void magicx_asset_cache_close(struct magicx_asset_cache *cache)
{
    if (cache->initialized)
        for (size_t i = 0U; i < MAGICX_ASSET_SLOTS; ++i)
            if (cache->entries[i].used) (void)close(cache->entries[i].pin);
    memset(cache, 0, sizeof(*cache));
}
void magicx_asset_cache_init(struct magicx_asset_cache *cache, bool pin_fat_session)
{
    magicx_asset_cache_close(cache);
    cache->initialized = true;
    cache->pin_fat_session = pin_fat_session;
}
bool magicx_asset_digest(struct magicx_asset_cache *cache, int fd, const char *path,
                        uint64_t limit, char digest[65])
{
    struct stat before, after;
    struct statfs fs;
    struct statvfs vfs;
    struct magicx_asset_entry *slot = NULL;
    struct cs_sha256 hash;
    unsigned char bytes[65536];
    uint64_t used = 0U;
    bool cacheable;
    if (!cache->initialized || path == NULL || strlen(path) >= sizeof(cache->entries[0].path) ||
        fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > limit || (before.st_mode & 0022U) != 0U ||
        (before.st_uid != 0U && before.st_uid != geteuid()) || fstatfs(fd, &fs) != 0 ||
        fstatvfs(fd, &vfs) != 0) return false;
    cacheable = (vfs.f_flag & ST_RDONLY) != 0U ||
        ((unsigned long)fs.f_type == 0xef53UL && before.st_ctim.tv_nsec != 0) ||
        ((unsigned long)fs.f_type == 0x4d44UL && cache->pin_fat_session);
    if (cacheable) {
        for (size_t i = 0U; i < MAGICX_ASSET_SLOTS; ++i) {
            struct magicx_asset_entry *entry = &cache->entries[i];
            if (entry->used && strcmp(entry->path, path) == 0) {
                struct stat pinned;
                if (fstat(entry->pin, &pinned) != 0 || !same_file(&before, &entry->fingerprint) ||
                    !same_file(&pinned, &entry->fingerprint)) return false;
                strcpy(digest, entry->digest);
                return true;
            }
            if (!entry->used && slot == NULL) slot = entry;
        }
    }
    cs_sha256_init(&hash);
    while (used < (uint64_t)before.st_size) {
        size_t amount = (uint64_t)before.st_size - used < sizeof(bytes) ?
            (size_t)((uint64_t)before.st_size - used) : sizeof(bytes);
        ssize_t got = pread(fd, bytes, amount, (off_t)used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        cs_sha256_update(&hash, bytes, (size_t)got); used += (uint64_t)got;
        cache->bytes_hashed += (uint64_t)got;
    }
    if (fstat(fd, &after) != 0 || !same_file(&before, &after)) return false;
    cs_sha256_final(&hash, digest);
    if (slot != NULL) {
        int pin = fcntl(fd, F_DUPFD_CLOEXEC, 5);
        if (pin < 0) return false;
        slot->pin = pin; slot->fingerprint = before;
        strcpy(slot->path, path); strcpy(slot->digest, digest); slot->used = true;
    }
    return true;
}

static int index_open(const char *path)
{
    /* No symlink component may redirect the immutable firmware index. */
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const char *part = path + 1;
    while (fd >= 0 && *part != '\0') {
        const char *end = strchr(part, '/');
        size_t n = end == NULL ? strlen(part) : (size_t)(end - part);
        char name[256];
        if (n == 0U || n >= sizeof(name) || (n == 1U && part[0] == '.') ||
            (n == 2U && part[0] == '.' && part[1] == '.')) { close(fd); return -1; }
        memcpy(name, part, n); name[n] = '\0';
        int next = openat(fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                          (end == NULL ? 0 : O_DIRECTORY));
        close(fd); fd = next;
        if (end == NULL) break;
        part = end + 1;
    }
    return fd;
}
static bool indexed_content(int fd, const char *path, uint64_t limit, char digest[65])
{
    static const char prefix[] = "magicx-index-path-v1";
    static const char sample_prefix[] = "magicx-launch-samples-v1";
    static const char *const keys[] = {"schema", "path", "bytes", "content", "samples"};
    struct stat before, after, index;
    struct statvfs fs;
    struct cs_json json;
    struct cs_sha256 hash;
    char key[65], file[160], text[CS_JSON_MAX + 1U], sample[65];
    unsigned char bytes[65536];
    uint64_t schema, size;
    size_t used = 0U;
    int input;
    bool ok = false;
    if (path == NULL || strlen(path) > 511U || fstat(fd, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_size < 16777216 ||
        (uint64_t)before.st_size > limit || (before.st_mode & 0022U) != 0U ||
        (before.st_uid != 0U && before.st_uid != geteuid())) return false;
    cs_sha256_init(&hash); cs_sha256_update(&hash, prefix, sizeof(prefix));
    cs_sha256_update(&hash, path, strlen(path) + 1U); cs_sha256_final(&hash, key);
    if (snprintf(file, sizeof(file), "/etc/console/launch-index/%s.json", key) >= (int)sizeof(file)) return false;
    input = index_open(file);
    if (input < 0) return false;
    if (fstat(input, &index) != 0 || !S_ISREG(index.st_mode) || index.st_uid != 0U ||
        (index.st_mode & 0022U) != 0U || index.st_nlink != 1U || index.st_size < 1 ||
        index.st_size > CS_JSON_MAX || fstatvfs(input, &fs) != 0 ||
        (fs.f_flag & ST_RDONLY) == 0U) goto done;
    while (used < (size_t)index.st_size) {
        ssize_t n = read(input, text + used, (size_t)index.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (!cs_json_parse(&json, text, used) || !cs_json_keys(&json, keys, 5U) ||
        !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "bytes", &size) || size != (uint64_t)before.st_size) goto done;
    const char *record_path = cs_json_string(&json, "path");
    const char *content = cs_json_string(&json, "content");
    const char *samples = cs_json_string(&json, "samples");
    if (record_path == NULL || strcmp(record_path, path) != 0 ||
        !cs_hex_valid(content, 64U) || !cs_hex_valid(samples, 64U)) goto done;
    cs_sha256_init(&hash); cs_sha256_update(&hash, sample_prefix, sizeof(sample_prefix));
    for (uint64_t i = 0U; i < 16U; ++i) {
        uint64_t offset = (size - sizeof(bytes)) * i / 15U;
        unsigned char number[8];
        for (unsigned int j = 0U; j < 8U; ++j) number[j] = (unsigned char)(offset >> (j * 8U));
        cs_sha256_update(&hash, number, sizeof(number));
        used = 0U;
        while (used < sizeof(bytes)) {
            ssize_t n = pread(fd, bytes + used, sizeof(bytes) - used, (off_t)offset + (off_t)used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto done;
            used += (size_t)n;
        }
        cs_sha256_update(&hash, bytes, sizeof(bytes));
    }
    cs_sha256_final(&hash, sample);
    if (strcmp(sample, samples) != 0 || fstat(fd, &after) != 0 || !same_file(&before, &after)) goto done;
    strcpy(digest, content); ok = true;
done:
    if (close(input) != 0) ok = false;
    return ok;
}
bool magicx_content_digest(struct magicx_asset_cache *cache, int fd, const char *path,
                          uint64_t limit, char digest[65])
{
    if (!cache->initialized || path == NULL) return false;
    for (size_t i = 0U; i < MAGICX_ASSET_SLOTS; ++i)
        if (cache->entries[i].used && strcmp(cache->entries[i].path, path) == 0)
            return magicx_asset_digest(cache, fd, path, limit, digest);
    struct stat before, after;
    if (fstat(fd, &before) != 0) return false;
    if (indexed_content(fd, path, limit, digest)) {
        if (fstat(fd, &after) != 0 || !same_file(&before, &after)) return false;
        for (size_t i = 0U; i < MAGICX_ASSET_SLOTS; ++i) {
            struct magicx_asset_entry *entry = &cache->entries[i];
            if (entry->used || strlen(path) >= sizeof(entry->path)) continue;
            int pin = fcntl(fd, F_DUPFD_CLOEXEC, 5);
            if (pin < 0) return false;
            entry->pin = pin; entry->fingerprint = after;
            strcpy(entry->path, path); strcpy(entry->digest, digest); entry->used = true;
            break;
        }
        fprintf(stderr, "MagicX content: provisioned fingerprint verified; read=1048576 bytes disc=%llu bytes\n",
                (unsigned long long)before.st_size);
        return true;
    }
    return magicx_asset_digest(cache, fd, path, limit, digest);
}
