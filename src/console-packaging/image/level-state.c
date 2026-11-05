#include "level-state.h"
#include "sessiond/durability.h"
#include "sessiond/json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static bool digest(const struct cs_store *store, const struct console_levels *levels,
                   unsigned int schema, char hash[65])
{
    char canonical[192];
    int n = snprintf(canonical, sizeof(canonical), "console-levels-v%u\n%s\n%" PRIu64
                     "\n%u\n%u\n", schema, store->uuid, levels->revision,
                     levels->brightness, levels->volume);
    if (n < 0 || (size_t)n >= sizeof(canonical)) return false;
    cs_sha256_hex(canonical, (size_t)n, hash);
    return true;
}
static bool safe_file(int fd, const struct cs_store *store)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1U &&
           st.st_dev == store->device && st.st_uid == geteuid() && (st.st_mode & 0077U) == 0U;
}
static enum cs_store_status read_record(struct cs_store *store, const char *name,
                                       struct console_levels *levels)
{
    static const char *const keys[] = {"schema", "uuid", "revision", "brightness", "volume", "digest"};
    char bytes[512], hash[65];
    struct cs_json json;
    struct console_levels parsed = {0};
    uint64_t schema, brightness, volume;
    const char *uuid, *stored;
    size_t used = 0U;
    enum cs_store_status result = CS_STORE_CORRUPT;
    int fd = openat(store->data_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
    if (!safe_file(fd, store)) goto done;
    for (;;) {
        ssize_t n;
        if (used == sizeof(bytes)) goto done;
        n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { result = CS_STORE_IO; goto done; }
        if (n == 0) break;
        used += (size_t)n;
    }
    if (!cs_json_parse(&json, bytes, used) || !cs_json_keys(&json, keys, 6U) ||
        !cs_json_u64(&json, "schema", &schema) || (schema != 1U && schema != 2U) ||
        !cs_json_u64(&json, "revision", &parsed.revision) ||
        !cs_json_u64(&json, "brightness", &brightness) ||
        (brightness != 0U && (brightness < 8U || brightness > 255U)) ||
        !cs_json_u64(&json, "volume", &volume) || volume > 101U) goto done;
    uuid = cs_json_string(&json, "uuid");
    stored = cs_json_string(&json, "digest");
    parsed.brightness = (unsigned int)brightness;
    parsed.volume = (unsigned int)volume;
    if (uuid == NULL || strcmp(uuid, store->uuid) != 0 || stored == NULL ||
        !digest(store, &parsed, (unsigned int)schema, hash) || strcmp(hash, stored) != 0) goto done;
    /* r10 recorded ALSA full-range percentages, which must never be treated
     * as percentages of the new bounded board range. Keep brightness and
     * revision, discard that volume, and use the board's initial level. */
    if (schema == 1U) parsed.volume = 101U;
    *levels = parsed;
    result = CS_STORE_OK;
done:
    if (close(fd) != 0) result = CS_STORE_IO;
    return result;
}
enum cs_store_status console_levels_load(struct cs_store *store, struct console_levels *levels)
{
    enum cs_store_status current, previous;
    if (cs_store_check_media(store) != CS_STORE_OK) return CS_STORE_MEDIA;
    current = read_record(store, "levels.json", levels);
    if (current == CS_STORE_OK) return cs_store_check_media(store);
    if (current == CS_STORE_IO) return current;
    previous = read_record(store, "levels.previous.json", levels);
    if (previous == CS_STORE_OK) {
        fprintf(stderr, "console-level: restoring previous verified levels record\n");
        return cs_store_check_media(store);
    }
    return current == CS_STORE_EMPTY && previous == CS_STORE_EMPTY
               ? CS_STORE_EMPTY : CS_STORE_CORRUPT;
}
static bool replace(struct cs_store *store, const char *name, const struct console_levels *levels)
{
    char text[512], hash[65], temporary[96];
    struct timespec now;
    uint64_t ticket;
    size_t used = 0U;
    int fd = -1, n;
    bool ok = false, renamed = false;
    if (!digest(store, levels, 2U, hash) || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    n = snprintf(text, sizeof(text), "{\"schema\":2,\"uuid\":\"%s\",\"revision\":%" PRIu64
                 ",\"brightness\":%u,\"volume\":%u,\"digest\":\"%s\"}\n", store->uuid,
                 levels->revision, levels->brightness, levels->volume, hash);
    if (n < 0 || (size_t)n >= sizeof(text)) return false;
    ticket = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    for (unsigned int attempt = 0U; attempt < 8U; ++attempt) {
        n = snprintf(temporary, sizeof(temporary), ".levels.%ld.%016" PRIx64 ".tmp",
                     (long)getpid(), ticket + attempt);
        if (n < 0 || (size_t)n >= sizeof(temporary)) return false;
        fd = openat(store->data_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return false;
    while (used < strlen(text)) {
        ssize_t count = write(fd, text + used, strlen(text) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) goto done;
        used += (size_t)count;
    }
    if (fsync(fd) != 0) goto done;
    if (close(fd) != 0) { fd = -1; goto done; }
    fd = -1;
    if (cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) != 0 ||
        cs_store_check_media(store) != CS_STORE_OK ||
        renameat(store->data_fd, temporary, store->data_fd, name) != 0) goto done;
    renamed = true;
    ok = cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs) == 0 &&
         cs_store_check_media(store) == CS_STORE_OK;
done:
    if (fd >= 0) (void)close(fd);
    if (!renamed) (void)unlinkat(store->data_fd, temporary, 0);
    return ok;
}
bool console_levels_save(struct cs_store *store, int brightness, int volume)
{
    struct console_levels old = {0U, 0U, 101U}, desired;
    enum cs_store_status status;
    int lock;
    bool ok = false;
    if ((brightness != -1 && (brightness < 8 || brightness > 255)) ||
        volume < -1 || volume > 100 || cs_store_check_media(store) != CS_STORE_OK) return false;
    lock = openat(store->data_fd, ".levels.lock",
                  O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock < 0) return false;
    if (!safe_file(lock, store) || flock(lock, LOCK_EX | LOCK_NB) != 0) goto done;
    status = console_levels_load(store, &old);
    if ((status != CS_STORE_OK && status != CS_STORE_EMPTY) || old.revision == UINT64_MAX) goto done;
    desired = old;
    if (brightness >= 0) desired.brightness = (unsigned int)brightness;
    if (volume >= 0) desired.volume = (unsigned int)volume;
    if (desired.brightness == old.brightness && desired.volume == old.volume) { ok = true; goto done; }
    ++desired.revision;
    ok = replace(store, "levels.previous.json", &old) && replace(store, "levels.json", &desired);
done:
    if (close(lock) != 0) ok = false;
    return ok;
}
