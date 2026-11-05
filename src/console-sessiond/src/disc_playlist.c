#include "sessiond/disc_playlist.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DESCRIPTION_LIMIT 65536U
#define IMAGE_LIMIT UINT64_C(0xffffffff)
#define PLAYLIST_LIMIT (UINT64_C(32) * 1024U * 1024U * 1024U)

static bool has_extension(const char *name, const char *const *allowed, size_t count)
{
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot != NULL && i < count; ++i)
        if (strcasecmp(dot + 1, allowed[i]) == 0) return true;
    return false;
}

static bool leaf(const char *name)
{
    size_t size = strlen(name);
    if (size == 0U || size > 255U || name[0] == '.' || name[size - 1U] == ' ' ||
        name[size - 1U] == '.' || strpbrk(name, "/\\:\"<>|?*#") != NULL) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p != 0U; ++p)
        if (*p < 32U || *p == 127U) return false;
    return true;
}

static bool unchanged(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int open_file(int parent, const char *name, uint64_t maximum, struct stat *info)
{
    struct stat directory;
    int fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0 && (fstat(parent, &directory) != 0 || fstat(fd, info) != 0 ||
        !S_ISREG(info->st_mode) || info->st_dev != directory.st_dev ||
        info->st_size <= 0 || (uint64_t)info->st_size > maximum)) {
        (void)close(fd);
        fd = -1;
    }
    return fd;
}

static char *read_description(int parent, const char *name, size_t *size)
{
    struct stat before, after;
    int fd = open_file(parent, name, DESCRIPTION_LIMIT, &before);
    if (fd < 0) return NULL;
    *size = (size_t)before.st_size;
    char *text = malloc(*size + 1U);
    size_t used = 0;
    while (text != NULL && used < *size) {
        ssize_t got = read(fd, text + used, *size - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    bool ok = text != NULL && used == *size && memchr(text, '\0', used) == NULL &&
              fstat(fd, &after) == 0 && unchanged(&before, &after);
    if (close(fd) != 0) ok = false;
    if (!ok) {
        free(text);
        return NULL;
    }
    text[used] = '\0';
    return text;
}

static bool parse_playlist(const char *name, char *text, size_t bytes,
                           char rows[CS_M3U_DISCS][512], size_t *count)
{
    static const char *const extensions[] = {"chd", "iso", "pbp", "dsk", "cue", "gdi"};
    const size_t header = sizeof(CS_M3U_HEADER) - 1U;
    const char *dot = strrchr(name, '.');
    const size_t stem = (size_t)(dot - name);
    char folder[256];
    *count = 0U;
    if (stem >= sizeof(folder) - 1U || bytes <= header || text[bytes - 1U] != '\n' ||
        memcmp(text, CS_M3U_HEADER, header) != 0) return false;
    (void)snprintf(folder, sizeof(folder), ".%.*s", (int)stem, name);
    for (char *line = text + header; *line != '\0';) {
        char *end = strchr(line, '\n');
        *end = '\0';
        const char *slash = strchr(line, '/');
        size_t prefix = slash == NULL ? 0U : (size_t)(slash - line);
        if (slash == NULL || *count == CS_M3U_DISCS || strlen(line) >= sizeof(rows[0]) ||
            !((prefix == 11U && memcmp(line, ".multi-disc", 11U) == 0) ||
              (prefix == stem + 1U && memcmp(line, folder, prefix) == 0)) ||
            !leaf(slash + 1U) || !has_extension(slash + 1U, extensions,
                                               sizeof(extensions) / sizeof(extensions[0]))) return false;
        for (size_t i = 0; i < *count; ++i)
            if (strcasecmp(line, rows[i]) == 0) return false;
        strcpy(rows[(*count)++], line);
        line = end + 1U;
    }
    return *count >= 2U;
}

bool cs_m3u_read(int directory, const char *name, char rows[CS_M3U_DISCS][512], size_t *count)
{
    static const char *const extension[] = {"m3u"};
    *count = 0;
    if (!leaf(name) || !has_extension(name, extension, 1U)) return false;
    size_t bytes;
    char *text = read_description(directory, name, &bytes);
    bool ok = text != NULL && parse_playlist(name, text, bytes, rows, count);
    free(text);
    return ok;
}

static bool hash_file(int directory, const char *name, uint64_t *budget, struct cs_sha256 *set)
{
    struct stat before, after;
    int fd = open_file(directory, name, IMAGE_LIMIT, &before);
    if (fd < 0) return false;
    uint64_t remaining = (uint64_t)before.st_size;
    bool ok = remaining <= *budget;
    unsigned char buffer[65536];
    struct cs_sha256 hash;
    cs_sha256_init(&hash);
    while (ok && remaining != 0U) {
        size_t want = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t got = read(fd, buffer, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = false; break; }
        cs_sha256_update(&hash, buffer, (size_t)got);
        remaining -= (uint64_t)got;
    }
    ok = ok && fstat(fd, &after) == 0 && unchanged(&before, &after);
    if (close(fd) != 0) ok = false;
    if (ok) {
        char digest[65];
        cs_sha256_final(&hash, digest);
        cs_sha256_update(set, name, strlen(name) + 1U);
        cs_sha256_update(set, digest, sizeof(digest));
        *budget -= (uint64_t)before.st_size;
    }
    return ok;
}

static char *number(char *line)
{
    line += strspn(line, " \t");
    size_t digits = strspn(line, "0123456789");
    return digits != 0U && strchr(" \t", line[digits]) != NULL ? line + digits : NULL;
}

static bool hash_descriptor(int directory, const char *name, bool gdi, uint64_t *budget,
                            struct cs_sha256 *set)
{
    static const char *const extensions[] = {"bin", "raw", "wav", "iso", "img", "aiff", "aif", "mp3"};
    size_t bytes, expected = 0U, tracks = 0U;
    char *text = read_description(directory, name, &bytes);
    if (text == NULL) return false;
    char descriptor[65];
    cs_sha256_hex(text, bytes, descriptor);
    cs_sha256_update(set, name, strlen(name) + 1U);
    cs_sha256_update(set, descriptor, sizeof(descriptor));
    bool ok = true;
    char *remaining = text;
    while (ok && remaining != NULL) {
        char *line = strsep(&remaining, "\n");
        size_t length = strlen(line);
        if (length > 0U && line[length - 1U] == '\r') line[--length] = '\0';
        char *cursor = line + strspn(line, " \t");
        if (*cursor == '\0') continue;
        if (length >= 1024U || strchr(line, '\r') != NULL) { ok = false; break; }
        if (gdi && expected == 0U) {
            char *end;
            unsigned long value = strtoul(cursor, &end, 10);
            ok = end != cursor && value >= 1U && value <= 99U && end[strspn(end, " \t")] == '\0';
            if (ok) expected = (size_t)value;
            continue;
        }
        if (gdi) {
            for (unsigned int i = 0; i < 4U && cursor != NULL; ++i) cursor = number(cursor);
        } else {
            if (strncasecmp(cursor, "FILE", 4U) != 0 ||
                (cursor[4] != ' ' && cursor[4] != '\t')) continue;
            cursor += 4;
        }
        if (cursor == NULL || ++tracks > 99U) { ok = false; break; }
        cursor += strspn(cursor, " \t");
        bool quoted = *cursor == '"';
        if (quoted) ++cursor;
        size_t token_size = strcspn(cursor, quoted ? "\"" : " \t");
        char *end = cursor + token_size;
        if ((quoted && *end != '"') || (!quoted && *end == '\0')) { ok = false; break; }
        *end = '\0';
        char *suffix = end + 1U;
        ok = leaf(cursor) && has_extension(cursor, extensions, sizeof(extensions) / sizeof(extensions[0]));
        if (gdi) {
            suffix = number(suffix);
            ok = ok && suffix != NULL && suffix[strspn(suffix, " \t")] == '\0';
        } else {
            suffix += strspn(suffix, " \t");
            ok = ok && *suffix != '\0';
        }
        if (ok) ok = hash_file(directory, cursor, budget, set);
    }
    free(text);
    return ok && tracks != 0U && (!gdi || tracks == expected);
}

static bool playlist_digest(const char *path, char digest[65], unsigned int *discs, bool playstation)
{
    static const char tag[] = "clove-managed-playlist-v1";
    static const char *const extension[] = {"m3u"};
    static const char *const ps_images[] = {"chd", "iso", "cue"};
    *discs = 0U;
    if (path == NULL || path[0] != '/' || strlen(path) >= 512U) return false;
    char relative[512];
    strcpy(relative, path + 1U);
    int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *file = relative, *slash;
    while (directory >= 0 && (slash = strchr(file, '/')) != NULL) {
        *slash = '\0';
        int next = leaf(file) ? openat(directory, file, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        (void)close(directory);
        directory = next;
        file = slash + 1U;
    }
    if (directory < 0) return false;
    char (*rows)[512] = calloc(CS_M3U_DISCS, sizeof(*rows));
    size_t count = 0U, bytes = 0U;
    char *text = leaf(file) && has_extension(file, extension, 1U) ?
                 read_description(directory, file, &bytes) : NULL;
    struct cs_sha256 set;
    cs_sha256_init(&set);
    cs_sha256_update(&set, tag, sizeof(tag));
    if (text != NULL) cs_sha256_update(&set, text, bytes + 1U);
    bool ok = rows != NULL && text != NULL && parse_playlist(file, text, bytes, rows, &count);
    if (playstation && count > CS_PS_PLAYLIST_DISCS) ok = false;
    uint64_t budget = PLAYLIST_LIMIT;
    for (size_t i = 0; ok && i < count; ++i) {
        slash = strchr(rows[i], '/');
        *slash = '\0';
        struct stat parent, child;
        int media = openat(directory, rows[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        ok = media >= 0 && fstat(directory, &parent) == 0 && fstat(media, &child) == 0 &&
             parent.st_dev == child.st_dev;
        if (ok) {
            const char *name = slash + 1U;
            const char *dot = strrchr(name, '.');
            bool gdi = strcasecmp(dot, ".gdi") == 0;
            if (playstation && !has_extension(name, ps_images, sizeof(ps_images) / sizeof(ps_images[0])))
                ok = false;
            else if (gdi || strcasecmp(dot, ".cue") == 0)
                ok = hash_descriptor(media, name, gdi, &budget, &set);
            else
                ok = hash_file(media, name, &budget, &set);
        }
        if (media >= 0 && close(media) != 0) ok = false;
    }
    free(text);
    free(rows);
    if (close(directory) != 0) ok = false;
    if (!ok) return false;
    cs_sha256_final(&set, digest);
    *discs = (unsigned int)count;
    return true;
}

bool cs_disc_playlist_digest(const char *path, char digest[65], unsigned int *discs)
{
    return playlist_digest(path, digest, discs, false);
}

bool cs_disc_ps_playlist_digest(const char *path, char digest[65], unsigned int *discs)
{
    return playlist_digest(path, digest, discs, true);
}

bool cs_disc_playlist_path(const char *path)
{
    static const char *const extension[] = {"m3u"};
    const char *name = path == NULL ? NULL : strrchr(path, '/');
    return name != NULL && leaf(name + 1U) && has_extension(name + 1U, extension, 1U);
}

void cs_disc_frozen_config(const char config[65], const char options[65], char digest[65])
{
    static const char tag[] = "magicx-frozen-default-config-v2";
    struct cs_sha256 hash;
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, tag, sizeof(tag));
    cs_sha256_update(&hash, config, 65U);
    cs_sha256_update(&hash, options, 65U);
    cs_sha256_final(&hash, digest);
}

bool cs_disc_state_encode(unsigned char trailer[CS_DISC_STATE_TRAILER], unsigned int discs,
                          unsigned int active)
{
    if (discs < 2U || discs > CS_M3U_DISCS || active >= discs) return false;
    memset(trailer, 0, CS_DISC_STATE_TRAILER);
    memcpy(trailer, "MXDISC1", 7U);
    trailer[8] = (unsigned char)discs;
    trailer[9] = (unsigned char)active;
    return true;
}

bool cs_disc_state_decode(const unsigned char trailer[CS_DISC_STATE_TRAILER], unsigned int discs,
                          unsigned int *active)
{
    unsigned char expected[CS_DISC_STATE_TRAILER];
    unsigned int index = trailer[9];
    if (!cs_disc_state_encode(expected, discs, index) || memcmp(expected, trailer, sizeof(expected)) != 0)
        return false;
    *active = index;
    return true;
}
