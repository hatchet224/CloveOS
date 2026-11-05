#include "supervisor/catalog_index.h"
#include "supervisor/config.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAMPLE_BYTES 65536U
static bool sample_digest(int fd, off_t offset, size_t size, char digest[65])
{
    unsigned char bytes[SAMPLE_BYTES];
    size_t used = 0U;
    while (used < size) {
        ssize_t n = pread(fd, bytes + used, size - used, offset + (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += (size_t)n;
    }
    cs_sha256_hex(bytes, used, digest);
    return true;
}
bool sc_catalog_index_lookup(int fd, const char *path, off_t size,
                             const char *root, char digest[65])
{
    static const char prefix[] = "magicx-index-path-v1";
    static const char *const keys[] = {"schema", "path", "bytes", "content", "head", "tail"};
    struct stat directory, before, after;
    struct cs_json json;
    struct cs_sha256 hash;
    char name[65], record[768], head[65], tail[65];
    const char *indexed_path, *content, *indexed_head, *indexed_tail;
    uint64_t schema, indexed_size;
    size_t sample;
    int dir, n;
    bool protected;
    if (fd < 0 || path == NULL || root == NULL || size <= 0 ||
        (uint64_t)size > UINT64_C(0xffffffff) || strlen(path) > 511U) return false;
    dir = sc_open_absolute(root, true);
    if (dir < 0) return false;
    protected = fstat(dir, &directory) == 0 && S_ISDIR(directory.st_mode) &&
        directory.st_uid == 0U && (directory.st_mode & 0022U) == 0U;
    if (close(dir) != 0 || !protected) return false;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, prefix, sizeof(prefix));
    cs_sha256_update(&hash, path, strlen(path) + 1U);
    cs_sha256_final(&hash, name);
    n = snprintf(record, sizeof(record), "%s/%s.json", root, name);
    if (n < 0 || (size_t)n >= sizeof(record) || !sc_read_json(record, &json) ||
        !cs_json_keys(&json, keys, 6U) || !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "bytes", &indexed_size) || indexed_size != (uint64_t)size) return false;
    indexed_path = cs_json_string(&json, "path");
    content = cs_json_string(&json, "content");
    indexed_head = cs_json_string(&json, "head");
    indexed_tail = cs_json_string(&json, "tail");
    if (indexed_path == NULL || strcmp(indexed_path, path) != 0 || content == NULL ||
        indexed_head == NULL || indexed_tail == NULL || !cs_hex_valid(content, 64U) ||
        !cs_hex_valid(indexed_head, 64U) || !cs_hex_valid(indexed_tail, 64U)) return false;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size != size) return false;
    sample = (uint64_t)size < SAMPLE_BYTES ? (size_t)size : SAMPLE_BYTES;
    if (!sample_digest(fd, 0, sample, head) ||
        !sample_digest(fd, size - (off_t)sample, sample, tail) ||
        strcmp(head, indexed_head) != 0 || strcmp(tail, indexed_tail) != 0 ||
        fstat(fd, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return false;
    strcpy(digest, content);
    return true;
}
