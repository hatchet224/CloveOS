#include "../magicx_asset_cache.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    struct magicx_asset_cache cache = {0};
    char path[1024], replaced[1024], first[65], second[65];
    unsigned char bytes[16384]; memset(bytes, 0x5a, sizeof(bytes));
    assert(argc == 2);
    assert(snprintf(path, sizeof(path), "%s/asset.bin", argv[1]) > 0);
    assert(snprintf(replaced, sizeof(replaced), "%s/replacement.bin", argv[1]) > 0);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600); assert(fd >= 0);
    for (unsigned int i = 0U; i < 256U; ++i) assert(write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
    assert(fsync(fd) == 0);
    magicx_asset_cache_init(&cache, true);
    assert(magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, first));
    uint64_t hashed = cache.bytes_hashed; assert(hashed == 4U * 1024U * 1024U);
    for (unsigned int i = 0U; i < 10U; ++i) {
        assert(magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, second));
        assert(strcmp(first, second) == 0 && cache.bytes_hashed == hashed);
    }
    assert(!magicx_asset_digest(&cache, fd, path, 1024U, second));
    assert(close(fd) == 0); fd = open(path, O_RDWR | O_CLOEXEC); assert(fd >= 0);
    assert(magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, second));
    assert(lseek(fd, 0, SEEK_END) > 0 && write(fd, "x", 1U) == 1 && fsync(fd) == 0);
    assert(!magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, second));
    close(fd); magicx_asset_cache_close(&cache);
    magicx_asset_cache_init(&cache, true); fd = open(path, O_RDONLY | O_CLOEXEC); assert(fd >= 0);
    assert(magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, first)); close(fd);
    fd = open(replaced, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600); assert(fd >= 0);
    assert(write(fd, "replacement", 11U) == 11 && fsync(fd) == 0); close(fd);
    assert(rename(replaced, path) == 0); fd = open(path, O_RDONLY | O_CLOEXEC); assert(fd >= 0);
    assert(!magicx_asset_digest(&cache, fd, path, 8U * 1024U * 1024U, second)); close(fd);
    magicx_asset_cache_close(&cache); assert(!cache.initialized);
    unlink(path);
    puts("ASSET_CACHE_PASS full first-use hash; ten FAT lookups read zero content bytes; reopened FD retained; size/limit/rename changes rejected; pins released");
    return 0;
}
