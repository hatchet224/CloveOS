#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static size_t bytes_read;
static ssize_t counted_read(int fd, void *buffer, size_t size)
{
    ssize_t n = read(fd, buffer, size);
    if (n > 0) bytes_read += (size_t)n;
    return n;
}
#define read counted_read
#include "../src/store.c"
#undef read
int main(void)
{
    char root[] = "/tmp/presentation-XXXXXX", digest[65];
    const uint64_t size = 16U * 1024U * 1024U;
    uint64_t actual;
    struct cs_store store;
    struct stat st;
    assert(mkdtemp(root));
    int dir = open(root, O_RDONLY | O_DIRECTORY);
    assert(dir >= 0 && fstat(dir, &st) == 0);
    int fd = openat(dir, "state.bin", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
    cs_store_init(&store);
    assert(!store.presentation_only);
    store.device = st.st_dev; store.max_state_bytes = size;
    assert(hash_artifact(&store, dir, 0U, false, &actual, digest));
    assert(actual == size);
    bytes_read = 0U;
    store.read_only = true; store.presentation_only = true;
    assert(artifact_matches(&store, dir, 0U, size, digest));
    assert(bytes_read == 0U);
    assert(pwrite(fd, "X", 1U, (off_t)(size / 2U)) == 1);
    assert(artifact_matches(&store, dir, 0U, size, digest));
    assert(bytes_read == 0U);
    /* A normal reader and the writer still reject changed state contents. */
    store.presentation_only = false;
    assert(!artifact_matches(&store, dir, 0U, size, digest));
    assert(bytes_read == size);
    bytes_read = 0U;
    store.read_only = false; store.presentation_only = true;
    assert(!artifact_matches(&store, dir, 0U, size, digest));
    assert(bytes_read == size);
    store.read_only = true;
    assert(!artifact_matches(&store, dir, 0U, size - 1U, digest));
    assert(close(fd) == 0 && unlinkat(dir, "state.bin", 0) == 0);
    assert(!artifact_matches(&store, dir, 0U, size, digest));
    assert(symlinkat("/dev/zero", dir, "state.bin") == 0);
    assert(!artifact_matches(&store, dir, 0U, size, digest));
    assert(unlinkat(dir, "state.bin", 0) == 0);
    unsigned char png[33] = {137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R',0,0,0,1,0,0,0,1};
    fd = openat(dir, "screenshot.png", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0 && write(fd, png, sizeof(png)) == (ssize_t)sizeof(png));
    assert(artifact_matches(&store, dir, 3U, sizeof(png), digest));
    assert(pwrite(fd, "X", 1U, 0) == 1);
    assert(!artifact_matches(&store, dir, 3U, sizeof(png), digest));
    assert(close(fd) == 0 && unlinkat(dir, "screenshot.png", 0) == 0);
    assert(close(dir) == 0 && rmdir(root) == 0);
    puts("PRESENTATION_NO_STATE_READ_AUTHORITY_FULL_HASH_PASS");
    return 0;
}
