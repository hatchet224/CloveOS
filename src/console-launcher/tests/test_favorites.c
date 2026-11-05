#ifdef NDEBUG
#error "Regression fixtures require active assertions; compile with -UNDEBUG"
#endif
#define _GNU_SOURCE
#include "launcher/favorites.h"
#include "sessiond/library.h"
#include <SDL.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
/* A trusted fixture directory is supplied by the isolated target test runner. */
int cs_library_open_absolute(const char *path, bool directory)
{
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
}
static struct cl_favorite_snapshot wait_ready(struct cl_favorites *worker, bool settled)
{
    struct cl_favorite_snapshot snapshot = {0};
    unsigned int i;
    for (i = 0U; i < 1000U; ++i) {
        (void)cl_favorites_take(worker, &snapshot);
        if (snapshot.ready && (!settled || !snapshot.pending)) {
            return snapshot;
        }
        SDL_Delay(2U);
    }
    assert(!"favorites worker timed out");
    return snapshot;
}
int main(int argc, char **argv)
{
    char path[768], missing[768], game[65], corrupt[256];
    struct cl_favorites *worker;
    struct cl_favorite_snapshot snapshot;
    struct statfs fs;
    int fd, n;
    if (argc != 2) {
        return 2;
    }
    assert(SDL_Init(0U) == 0);
    assert(mkdir(argv[1], 0700) == 0 || errno == EEXIST);
    assert(snprintf(path, sizeof(path), "%s/favorites.json", argv[1]) < (int)sizeof(path));
    assert(lstat(path, &(struct stat){0}) != 0 && errno == ENOENT);
    memset(game, 'a', 64U);
    game[64] = '\0';
    worker = cl_favorites_start(path);
    assert(worker != NULL);
    snapshot = wait_ready(worker, false);
    assert(!snapshot.error && snapshot.count == 0U);
    assert(cl_favorites_toggle(worker, game));
    snapshot = wait_ready(worker, true);
    assert(!snapshot.error && snapshot.count == 1U && cl_favorites_contains(&snapshot, game));
    cl_favorites_stop(worker);
    worker = cl_favorites_start(path);
    snapshot = wait_ready(worker, false);
    assert(!snapshot.error && snapshot.count == 1U && cl_favorites_contains(&snapshot, game));
    assert(cl_favorites_toggle(worker, game));
    cl_favorites_stop(worker); /* stop drains the queued durable mutation */
    worker = cl_favorites_start(path);
    snapshot = wait_ready(worker, false);
    assert(!snapshot.error && snapshot.count == 0U);
    cl_favorites_stop(worker);
    n = snprintf(corrupt, sizeof(corrupt), "{\"version\":1,\"games\":[\"%s\",\"BAD\"]}", game);
    assert(n > 0 && (size_t)n < sizeof(corrupt));
    fd = open(path, O_WRONLY | O_TRUNC | O_NOFOLLOW);
    assert(fd >= 0);
    assert(write(fd, corrupt, (size_t)n) == n);
    assert(close(fd) == 0);
    worker = cl_favorites_start(path);
    snapshot = wait_ready(worker, false);
    assert(snapshot.error && snapshot.count == 0U);
    assert(!cl_favorites_toggle(worker, game));
    cl_favorites_stop(worker);
    {
        char bytes[256] = {0};
        fd = open(path, O_RDONLY | O_NOFOLLOW);
        assert(fd >= 0);
        assert(read(fd, bytes, sizeof(bytes)) == n);
        assert(memcmp(bytes, corrupt, (size_t)n) == 0);
        assert(close(fd) == 0);
    }
    assert(unlink(path) == 0);
    assert(snprintf(missing, sizeof(missing), "%s/missing/sub/favorites.json", argv[1]) <
           (int)sizeof(missing));
    worker = cl_favorites_start(missing);
    snapshot = wait_ready(worker, false);
    assert(snapshot.error && snapshot.count == 0U);
    assert(!cl_favorites_toggle(worker, game));
    cl_favorites_stop(worker);
    {
        char directory[768], file[800], second[65];
        assert(snprintf(directory, sizeof(directory), "%s/write-failure", argv[1]) <
               (int)sizeof(directory));
        assert(mkdir(directory, 0700) == 0);
        assert(snprintf(file, sizeof(file), "%s/favorites.json", directory) < (int)sizeof(file));
        worker = cl_favorites_start(file);
        snapshot = wait_ready(worker, false);
        assert(!snapshot.error);
        assert(cl_favorites_toggle(worker, game));
        snapshot = wait_ready(worker, true);
        assert(!snapshot.error && snapshot.count == 1U);
        assert(unlink(file) == 0);
        assert(rmdir(directory) == 0);
        memset(second, 'b', 64U);
        second[64] = '\0';
        assert(cl_favorites_toggle(worker, second));
        snapshot = wait_ready(worker, true);
        assert(snapshot.error && snapshot.count == 1U && cl_favorites_contains(&snapshot, game) &&
               !cl_favorites_contains(&snapshot, second));
        cl_favorites_stop(worker);
    }
    assert(statfs(argv[1], &fs) == 0);
    printf("FAVORITES_TEST_PASS fs_magic=%lx: add/reload/remove, shutdown drain, malformed "
           "rollback/no overwrite, missing card containment\n",
           (unsigned long)fs.f_type);
    SDL_Quit();
    return 0;
}
