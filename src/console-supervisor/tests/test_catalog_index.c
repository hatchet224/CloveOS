#include "supervisor/catalog_index.h"
#include "supervisor/config.h"
#include "sessiond/sha256.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void record(off_t size, const char *content, const char *sample)
{
    static const char prefix[] = "magicx-index-path-v1";
    struct cs_sha256 hash;
    char key[65], path[160];
    FILE *stream;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, prefix, sizeof(prefix));
    cs_sha256_update(&hash, "/fixture-rom", sizeof("/fixture-rom"));
    cs_sha256_final(&hash, key);
    assert(snprintf(path, sizeof(path), "/index/%s.json", key) > 0);
    stream = fopen(path, "w");
    assert(stream != NULL);
    assert(fprintf(stream, "{\"schema\":1,\"path\":\"/fixture-rom\",\"bytes\":%lld,"
                   "\"content\":\"%s\",\"head\":\"%s\",\"tail\":\"%s\"}\n",
                   (long long)size, content, sample, sample) > 0);
    assert(fclose(stream) == 0);
    assert(chmod(path, 0600) == 0);
}
int main(void)
{
    unsigned char zero[65536] = {0};
    char digest[65], expected[65], sample[65];
    int fd;
    struct cs_sha256 hash;
    assert(mkdir("/index", 0700) == 0);
    fd = open("/fixture-rom", O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, 196608) == 0);
    cs_sha256_hex(zero, sizeof(zero), sample);
    cs_sha256_init(&hash);
    for (unsigned int i = 0; i < 3U; ++i) cs_sha256_update(&hash, zero, sizeof(zero));
    cs_sha256_final(&hash, expected);
    record(196608, expected, sample);
    assert(sc_catalog_index_lookup(fd, "/fixture-rom", 196608, "/index", digest));
    assert(strcmp(digest, expected) == 0);
    assert(sc_hash_path("/fixture-rom", expected, 196608));
    assert(!sc_catalog_index_lookup(fd, "/wrong", 196608, "/index", digest));
    assert(!sc_catalog_index_lookup(fd, "/fixture-rom", 196607, "/index", digest));
    assert(chmod("/index", 0777) == 0);
    assert(!sc_catalog_index_lookup(fd, "/fixture-rom", 196608, "/index", digest));
    assert(chmod("/index", 0700) == 0);
    assert(symlink("/index", "/linked-index") == 0);
    assert(!sc_catalog_index_lookup(fd, "/fixture-rom", 196608, "/linked-index", digest));
    assert(pwrite(fd, "x", 1, 90000) == 1);
    /* Registration is deliberately not authentication: launch rejects middle damage. */
    assert(sc_catalog_index_lookup(fd, "/fixture-rom", 196608, "/index", digest));
    assert(!sc_hash_path("/fixture-rom", expected, 196608));
    assert(pwrite(fd, "x", 1, 0) == 1);
    assert(!sc_catalog_index_lookup(fd, "/fixture-rom", 196608, "/index", digest));
    assert(pwrite(fd, zero, sizeof(zero), 0) == (ssize_t)sizeof(zero));
    assert(ftruncate(fd, (off_t)3221225472LL) == 0);
    record((off_t)3221225472LL, expected, sample);
    assert(sc_catalog_index_lookup(fd, "/fixture-rom", (off_t)3221225472LL, "/index", digest));
    assert(close(fd) == 0);
    puts("R6_INDEX_PASS=1 sparse_3GiB_bounded_samples full_launch_rejects_middle_corruption");
    return 0;
}
