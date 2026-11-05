#include "sessiond/durability.h"
#include "launcher/favorites.h"
#include "sessiond/document.h"
#include "sessiond/library.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
struct cl_favorites {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *condition;
    char directory[512], name[64], game[65];
    struct cl_favorite_snapshot snapshot;
    bool stop, pending, changed;
};
static bool game_id(const char *id)
{
    size_t i;
    if (strnlen(id, 65U) != 64U) {
        return false;
    }
    for (i = 0U; i < 64U; ++i) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}
bool cl_favorites_contains(const struct cl_favorite_snapshot *s, const char game[65])
{
    size_t i;
    for (i = 0U; i < s->count; ++i) {
        if (strcmp(s->games[i], game) == 0) {
            return true;
        }
    }
    return false;
}
static bool read_snapshot(int directory, const char *name, struct cl_favorite_snapshot *snapshot)
{
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    char bytes[4097];
    size_t used = 0U, node, i;
    int version;
    struct cs_document *doc;
    bool ok = false;
    struct cl_favorite_snapshot parsed_snapshot = {0};
    if (fd < 0) {
        return errno == ENOENT;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 1 ||
        st.st_size > 4096) {
        goto done;
    }
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto done;
        }
        used += (size_t)n;
    }
    doc = calloc(1U, sizeof(*doc));
    if (doc == NULL) {
        goto done;
    }
    if (!cs_doc_parse(doc, bytes, used) || doc->tokens[0].type != CS_DOC_OBJECT) {
        goto parsed;
    }
    if (!cs_doc_int(doc, cs_doc_get(doc, 0U, "version"), &version) || version != 1) {
        goto parsed;
    }
    node = cs_doc_get(doc, 0U, "games");
    if (node == SIZE_MAX || doc->tokens[node].type != CS_DOC_ARRAY) {
        goto parsed;
    }
    for (i = node + 1U; i < doc->tokens[node].next; i = doc->tokens[i].next) {
        char id[65];
        if (parsed_snapshot.count >= CL_FAVORITE_MAX || !cs_doc_text(doc, i, id, sizeof(id)) ||
            !game_id(id) || cl_favorites_contains(&parsed_snapshot, id)) {
            goto parsed;
        }
        strcpy(parsed_snapshot.games[parsed_snapshot.count++], id);
    }
    ok = true;
parsed:
    free(doc);
done:
    if (close(fd) != 0) {
        ok = false;
    }
    if (ok) {
        *snapshot = parsed_snapshot;
    }
    return ok;
}
static bool flush_directory(int directory)
{
    struct statfs fs;
    return fstatfs(directory, &fs) == 0 &&
        cs_flush_directory(directory, (unsigned long)fs.f_type == 0x4d44UL, true) == 0;
}
static bool write_snapshot(int directory, const char *name,
                           const struct cl_favorite_snapshot *snapshot)
{
    char bytes[4096], temporary[96];
    size_t used = 0U, i;
    int fd = -1, n;
    bool ok = false, renamed = false;
    unsigned int attempt;
    uint64_t ticket = SDL_GetPerformanceCounter();
    n = snprintf(bytes, sizeof(bytes), "{\"version\":1,\"games\":[");
    if (n < 0) {
        return false;
    }
    used = (size_t)n;
    for (i = 0U; i < snapshot->count; ++i) {
        n = snprintf(bytes + used, sizeof(bytes) - used, "%s\"%s\"", i == 0U ? "" : ",",
                     snapshot->games[i]);
        if (n < 0 || (size_t)n >= sizeof(bytes) - used) {
            return false;
        }
        used += (size_t)n;
    }
    if (used + 4U > sizeof(bytes)) {
        return false;
    }
    memcpy(bytes + used, "]}\n", 3U);
    used += 3U;
    for (attempt = 0U; attempt < 8U; ++attempt) {
        n = snprintf(temporary, sizeof(temporary), ".favorites.%ld.%016" PRIx64 ".tmp",
                     (long)getpid(), ticket + attempt);
        if (n < 0 || n >= (int)sizeof(temporary)) {
            return false;
        }
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            return false;
        }
    }
    if (fd < 0) {
        return false;
    }
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
    if (!flush_directory(directory) || renameat(directory, temporary, directory, name) != 0) {
        goto done;
    }
    renamed = true;
    ok = flush_directory(directory);
done:
    if (fd >= 0) {
        (void)close(fd);
    }
    if (!renamed) {
        (void)unlinkat(directory, temporary, 0);
    }
    return ok;
}
static int worker_main(void *context)
{
    struct cl_favorites *worker = context;
    struct cl_favorite_snapshot current = {0};
    int directory = cs_library_open_absolute(worker->directory, true);
    current.error = directory < 0 || !read_snapshot(directory, worker->name, &current);
    current.ready = true;
    current.revision = 1U;
    (void)SDL_LockMutex(worker->mutex);
    worker->snapshot = current;
    worker->changed = true;
    SDL_UnlockMutex(worker->mutex);
    for (;;) {
        char game[65];
        size_t i;
        bool present = false;
        struct cl_favorite_snapshot desired;
        (void)SDL_LockMutex(worker->mutex);
        while (!worker->stop && !worker->pending) {
            (void)SDL_CondWait(worker->condition, worker->mutex);
        }
        if (worker->stop && !worker->pending) {
            SDL_UnlockMutex(worker->mutex);
            break;
        }
        strcpy(game, worker->game);
        worker->pending = false;
        SDL_UnlockMutex(worker->mutex);
        desired = current;
        for (i = 0U; i < desired.count; ++i) {
            if (strcmp(desired.games[i], game) == 0) {
                present = true;
                if (desired.count > i + 1U) {
                    memmove(desired.games[i], desired.games[i + 1U],
                            (desired.count - i - 1U) * sizeof(desired.games[0]));
                }
                --desired.count;
                break;
            }
        }
        if (!present && desired.count < CL_FAVORITE_MAX) {
            strcpy(desired.games[desired.count++], game);
        }
        if (write_snapshot(directory, worker->name, &desired)) {
            current = desired;
        } else {
            current.error = true;
        }
        current.pending = false;
        ++current.revision;
        (void)SDL_LockMutex(worker->mutex);
        worker->snapshot = current;
        worker->changed = true;
        SDL_UnlockMutex(worker->mutex);
    }
    if (directory >= 0) {
        (void)close(directory);
    }
    return 0;
}
struct cl_favorites *cl_favorites_start(const char *path)
{
    struct cl_favorites *worker;
    const char *slash;
    size_t length;
    if (path == NULL) {
        return NULL;
    }
    worker = calloc(1U, sizeof(*worker));
    slash = strrchr(path, '/');
    if (worker == NULL) {
        return NULL;
    }
    if (slash == NULL || slash == path ||
        (length = (size_t)(slash - path)) >= sizeof(worker->directory) ||
        strlen(slash + 1U) >= sizeof(worker->name)) {
        free(worker);
        return NULL;
    }
    memcpy(worker->directory, path, length);
    worker->directory[length] = '\0';
    strcpy(worker->name, slash + 1U);
    worker->mutex = SDL_CreateMutex();
    worker->condition = SDL_CreateCond();
    if (worker->mutex != NULL && worker->condition != NULL) {
        worker->thread = SDL_CreateThread(worker_main, "favorites", worker);
    }
    if (worker->thread == NULL) {
        cl_favorites_stop(worker);
        return NULL;
    }
    return worker;
}
bool cl_favorites_toggle(struct cl_favorites *worker, const char game[65])
{
    bool ok = false;
    if (worker == NULL || !game_id(game)) {
        return false;
    }
    (void)SDL_LockMutex(worker->mutex);
    if (worker->snapshot.ready && !worker->snapshot.error && !worker->snapshot.pending &&
        (worker->snapshot.count < CL_FAVORITE_MAX ||
         cl_favorites_contains(&worker->snapshot, game))) {
        strcpy(worker->game, game);
        worker->pending = true;
        worker->snapshot.pending = true;
        worker->changed = true;
        SDL_CondSignal(worker->condition);
        ok = true;
    }
    SDL_UnlockMutex(worker->mutex);
    return ok;
}
bool cl_favorites_take(struct cl_favorites *worker, struct cl_favorite_snapshot *snapshot)
{
    bool changed;
    if (worker == NULL) {
        return false;
    }
    (void)SDL_LockMutex(worker->mutex);
    changed = worker->changed;
    if (changed) {
        *snapshot = worker->snapshot;
        worker->changed = false;
    }
    SDL_UnlockMutex(worker->mutex);
    return changed;
}
void cl_favorites_stop(struct cl_favorites *worker)
{
    if (worker == NULL) {
        return;
    }
    if (worker->thread != NULL) {
        (void)SDL_LockMutex(worker->mutex);
        worker->stop = true;
        SDL_CondSignal(worker->condition);
        SDL_UnlockMutex(worker->mutex);
        SDL_WaitThread(worker->thread, NULL);
    }
    SDL_DestroyCond(worker->condition);
    SDL_DestroyMutex(worker->mutex);
    free(worker);
}
