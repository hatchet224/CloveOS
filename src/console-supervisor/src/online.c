#include "supervisor/online.h"
#include "scraper_client.h"
#include "supervisor/packages.h"
#include "supervisor/zip.h"
#include "supervisor/elf_dependency.h"
#include "sessiond/document.h"
#include "supervisor/api_document.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <unistd.h>

static const char *const regions[] = {"us", "eu", "jp", "wor", "fr", "de", "es", "it"};
static const char *const media_types[] = {"box-2D", "box-3D", "ss", "sstitle", "wheel", "marquee", "mixrbv1", "mixrbv2"};
static void hex_encode(const unsigned char *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0U; i < n; ++i) { out[i * 2U] = digits[bytes[i] >> 4U]; out[i * 2U + 1U] = digits[bytes[i] & 15U]; }
    out[n * 2U] = '\0';
}
static bool hex_decode(const char *text, char out[64])
{
    size_t n = strlen(text);
    if (n % 2U != 0U || n > 126U) return false;
    for (size_t i = 0U; i < n; i += 2U) {
        unsigned int a, b;
        if (text[i] >= '0' && text[i] <= '9') a = (unsigned int)(text[i] - '0');
        else if (text[i] >= 'a' && text[i] <= 'f') a = (unsigned int)(text[i] - 'a' + 10); else return false;
        if (text[i + 1U] >= '0' && text[i + 1U] <= '9') b = (unsigned int)(text[i + 1U] - '0');
        else if (text[i + 1U] >= 'a' && text[i + 1U] <= 'f') b = (unsigned int)(text[i + 1U] - 'a' + 10); else return false;
        if (a * 16U + b < 32U || a * 16U + b > 126U) return false;
        out[i / 2U] = (char)(a * 16U + b);
    }
    out[n / 2U] = '\0'; return true;
}
static int settings_body(const struct sc_online_settings *c, char body[2048])
{
    char hex[5][127];
    const char *values[] = {c->user, c->password, c->devid, c->devpassword, c->repository};
    if (c->region >= 8U || c->media >= 8U) return -1;
    for (size_t i = 0U; i < 5U; ++i) {
        size_t n = strnlen(values[i], 64U);
        if (n >= 64U) return -1;
        for (size_t j = 0U; j < n; ++j)
            if ((unsigned char)values[i][j] < 32U || (unsigned char)values[i][j] > 126U) return -1;
        hex_encode((const unsigned char *)values[i], n, hex[i]);
    }
    return snprintf(body, 2048U, "{\"schema\":1,\"user\":\"%s\",\"password\":\"%s\",\"devid\":\"%s\","
        "\"devpassword\":\"%s\",\"repository\":\"%s\",\"region\":%u,\"media\":%u}",
        hex[0], hex[1], hex[2], hex[3], hex[4], c->region, c->media);
}
static int settings_read(const struct cs_store *s, const char *name, struct sc_online_settings *c)
{
    char bytes[3072], body[2048], digest[65]; struct cs_json json; struct stat st;
    static const char *const keys[] = {"schema", "user", "password", "devid", "devpassword", "repository", "region", "media", "checksum"};
    char *values[] = {c->user, c->password, c->devid, c->devpassword, c->repository};
    uint64_t schema, region, media; size_t used = 0U; int fd, n;
    fd = openat(s->data_fd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 1 || st.st_size >= (off_t)sizeof(bytes)) { close(fd); return -1; }
    while (used < (size_t)st.st_size) {
        ssize_t count = read(fd, bytes + used, (size_t)st.st_size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return -1; } used += (size_t)count;
    }
    if (close(fd) != 0 || !cs_json_parse(&json, bytes, used) || !cs_json_keys(&json, keys, 9U) ||
        !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "region", &region) || region >= 8U ||
        !cs_json_u64(&json, "media", &media) || media >= 8U) return -1;
    for (size_t i = 0U; i < 5U; ++i) {
        const char *value = cs_json_string(&json, keys[i + 1U]);
        if (value == NULL || !hex_decode(value, values[i])) return -1;
    }
    c->region = (unsigned int)region; c->media = (unsigned int)media;
    n = settings_body(c, body);
    if (n < 1 || (size_t)n >= sizeof(body) || cs_json_string(&json, "checksum") == NULL) return -1;
    cs_sha256_hex(body, (size_t)n, digest);
    return strcmp(digest, cs_json_string(&json, "checksum")) == 0 ? 1 : -1;
}
bool sc_online_settings_load(const struct cs_store *s, struct sc_online_settings *c)
{
    int current, previous;
    memset(c, 0, sizeof(*c)); current = settings_read(s, "online.json", c);
    if (current == 1) return true;
    memset(c, 0, sizeof(*c)); previous = settings_read(s, "online.previous.json", c);
    if (previous == 1) return true;
    memset(c, 0, sizeof(*c)); return current == 0 && previous == 0;
}
bool sc_online_settings_save(const struct cs_store *s, const struct sc_online_settings *c)
{
    struct sc_online_settings old; char body[2048], record[2304], digest[65];
    int n = settings_body(c, body), count;
    if (n < 1 || (size_t)n >= sizeof(body)) return false;
    cs_sha256_hex(body, (size_t)n, digest); body[n - 1] = '\0';
    count = snprintf(record, sizeof(record), "%s,\"checksum\":\"%s\"}\n", body, digest);
    if (count < 1 || (size_t)count >= sizeof(record)) return false;
    if (settings_read(s, "online.json", &old) == 1) {
        char previous[2304]; int bytes = settings_body(&old, body);
        if (bytes < 1 || (size_t)bytes >= sizeof(body)) return false;
        cs_sha256_hex(body, (size_t)bytes, digest); body[bytes - 1] = '\0';
        bytes = snprintf(previous, sizeof(previous), "%s,\"checksum\":\"%s\"}\n", body, digest);
        if (bytes < 1 || (size_t)bytes >= sizeof(previous) ||
            !sc_transfer_commit(s, ".console", "online.previous.json", (const unsigned char *)previous, (size_t)bytes)) return false;
    }
    return sc_transfer_commit(s, ".console", "online.json", (const unsigned char *)record, (size_t)count);
}
static void add_row(struct cl_tool_snapshot *s, const char *id, const char *label, const char *detail, bool available)
{
    if (s->count < CL_TOOL_ROWS) {
        struct cl_tool_row *r = &s->rows[s->count++];
        (void)snprintf(r->id, sizeof(r->id), "%s", id); (void)snprintf(r->label, sizeof(r->label), "%s", label);
        (void)snprintf(r->detail, sizeof(r->detail), "%s", detail); r->available = available;
    }
}
static bool text_at(const struct cs_document *d, size_t object, const char *key, char *out, size_t cap)
{ return cs_doc_text(d, cs_doc_get(d, object, key), out, cap); }
static bool stop_requested(const struct sc_transfer_cancel *c)
{ return c != NULL && c->cancelled != NULL && c->cancelled(c->context); }
static bool escaped(const char *text, char *out, size_t cap)
{
    size_t used = 0U; static const char digits[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; ++p) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (used + (plain ? 1U : 3U) >= cap) return false;
        if (plain) out[used++] = (char)*p;
        else { out[used++] = '%'; out[used++] = digits[*p >> 4U]; out[used++] = digits[*p & 15U]; }
    }
    out[used] = '\0'; return true;
}
static unsigned int system_id(const char *system)
{
    static const char *const names[] = {"GB", "GBC", "GBA", "FC", "SFC", "MD", "MS", "PS", "PCE", "NGP", "WS", "ARCADE", "N64", "DC", "PSP", "SATURN","GG","FBNEO","NEOGEO","PICO","MAME2003PLUS","ATARI"};
    static const unsigned int ids[] = {9U, 10U, 12U, 3U, 4U, 1U, 2U, 57U, 31U, 25U, 45U, 75U, 14U, 23U, 61U, 22U,21U,75U,142U,234U,75U,26U};
    for (size_t i = 0U; i < sizeof(ids) / sizeof(ids[0]); ++i) if (strcmp(names[i], system) == 0) return ids[i];
    return 0U;
}
static bool rom_crc(const struct sc_game *game, const struct sc_transfer_cancel *cancel, char crc[9], unsigned int *system, uint64_t *size)
{
    unsigned char block[65536]; uint32_t value = UINT32_MAX; uint64_t bytes = 0U;
    struct cs_sha256 hash; char sha[65]; struct stat before, after; bool result = false;
    int fd = sc_open_absolute(game->content, false);
    if (fd < 0) return false;
    cs_sha256_init(&hash);
    if (fstat(fd, &before) != 0 || before.st_size < 1 || before.st_size > INT64_C(2147483648)) goto done;
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || stop_requested(cancel)) goto done;
        if (n == 0) break;
        /* PC collections often label monochrome GB ROMs .gbc. Use the actual
         * cartridge CGB flag when identifying GB/GBC games to ScreenScraper. */
        if (bytes == 0U && n > 0x143 &&
            (strcmp(game->entry.system, "GB") == 0 || strcmp(game->entry.system, "GBC") == 0))
            *system = (block[0x143] & 0x80U) != 0U ? 10U : 9U;
        bytes += (uint64_t)n; if (bytes > (uint64_t)before.st_size) goto done;
        cs_sha256_update(&hash, block, (size_t)n);
        /* Same IEEE CRC32, two nibble lookups instead of eight serial bit
         * iterations per byte. Disc images can exceed a gigabyte. */
        static const uint32_t table[16]={
            0x00000000U,0x1db71064U,0x3b6e20c8U,0x26d930acU,
            0x76dc4190U,0x6b6b51f4U,0x4db26158U,0x5005713cU,
            0xedb88320U,0xf00f9344U,0xd6d6a3e8U,0xcb61b38cU,
            0x9b64c2b0U,0x86d3d2d4U,0xa00ae278U,0xbdbdf21cU};
        for (size_t i = 0U; i < (size_t)n; ++i) {
            value ^= block[i];
            value=(value>>4U)^table[value&15U];
            value=(value>>4U)^table[value&15U];
        }
    }
    cs_sha256_final(&hash, sha);
    result = bytes == (uint64_t)before.st_size && strcmp(sha, game->profile.identity.content) == 0 &&
        fstat(fd, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
    if (result) { (void)snprintf(crc, 9U, "%08" PRIX32, value ^ UINT32_MAX); *size = bytes; }
done:
    if (close(fd) != 0) result = false; return result;
}
static bool image_destination(const struct sc_config *c, const struct sc_game *g, char directory[512], char name[256])
{
    size_t root = strlen(c->base.card_path); const char *relative, *slash, *dot; int n;
    if (strncmp(g->content, c->base.card_path, root) != 0 || g->content[root] != '/') return false;
    relative = g->content + root + 1U;
    if (strncmp(relative, "Roms/", 5U) != 0 || strstr(relative, "..") != NULL) return false;
    slash = strrchr(relative, '/'); if (slash == NULL) return false;
    dot = strrchr(slash + 1, '.'); if (dot == NULL) dot = slash + strlen(slash);
    n = snprintf(directory, 512U, "%.*s/Imgs", (int)(slash - relative), relative);
    if (n < 1 || n >= 512) return false;
    n = snprintf(name, 256U, "%.*s.png", (int)(dot - slash - 1), slash + 1);
    return n > 0 && n < 256;
}
static bool screenscraper_host(const char *url)
{
    const char *start; size_t n;
    if (strncmp(url, "https://", 8U) != 0) return false;
    start = url + 8U; n = strcspn(start, "/?#:");
    return n >= 16U && start[n] != ':' && memchr(start, '@', n) == NULL &&
        ((n == 16U && strncmp(start, "screenscraper.fr", n) == 0) ||
         (n > 16U && strncmp(start + n - 17U, ".screenscraper.fr", 17U) == 0));
}
enum scraper_match { SCRAPER_UNREADABLE, SCRAPER_NO_GAME, SCRAPER_NO_MEDIA, SCRAPER_MEDIA_UNAVAILABLE, SCRAPER_MATCHED };
static enum scraper_match select_media(const unsigned char *bytes, size_t size, const struct sc_online_settings *settings, char url[2048])
{
    struct sc_api_document *d = calloc(1U, sizeof(*d)); size_t response, game, media;
    int best = 100; enum scraper_match result = SCRAPER_UNREADABLE;
    if (d == NULL || !sc_api_doc_parse(d, (const char *)bytes, size)) goto done;
    response = sc_api_doc_get(d, 0U, "response");
    game = sc_api_doc_get(d, response == SIZE_MAX ? 0U : response, "jeu");
    result = SCRAPER_NO_GAME;
    if (game == SIZE_MAX || d->tokens[game].type != CS_DOC_OBJECT) goto done;
    result = SCRAPER_NO_MEDIA;
    media = sc_api_doc_get(d, game, "medias");
    if (media == SIZE_MAX || d->tokens[media].type != CS_DOC_ARRAY) goto done;
    result = SCRAPER_MEDIA_UNAVAILABLE;
    for (size_t i = media + 1U; i < d->tokens[media].next; i = d->tokens[i].next) {
        char type[64], region[32] = "", candidate[2048]; int score;
        if (!sc_api_doc_text(d, sc_api_doc_get(d, i, "type"), type, sizeof(type)) || strcmp(type, media_types[settings->media]) != 0 ||
            !sc_api_doc_text(d, sc_api_doc_get(d, i, "url"), candidate, sizeof(candidate)) || !screenscraper_host(candidate)) continue;
        (void)sc_api_doc_text(d, sc_api_doc_get(d, i, "region"), region, sizeof(region));
        score = strcmp(region, regions[settings->region]) == 0 ? 0 : (strcmp(region, "wor") == 0 ? 1 : 2);
        if (score < best) { best = score; strcpy(url, candidate); result = SCRAPER_MATCHED; }
    }
done:
    free(d); return result;
}
static bool scraper_request_url(const struct sc_online_settings *settings, const char *filename,
                                const char *crc, unsigned int system, uint64_t size, char *url, size_t capacity)
{
    char user[192], password[192], devid[192], devpassword[192], rom[768];
    int n;
    if (!escaped(settings->user, user, sizeof(user)) ||
        !escaped(settings->password, password, sizeof(password)) ||
        !escaped(settings->devid, devid, sizeof(devid)) ||
        !escaped(settings->devpassword, devpassword, sizeof(devpassword)) ||
        !escaped(filename, rom, sizeof(rom))) return false;
    n = snprintf(url, capacity,
        "https://api.screenscraper.fr/api2/jeuInfos.php?devid=%s&devpassword=%s&softname=%s&output=json&ssid=%s&sspassword=%s&crc=%s&systemeid=%u&romtype=rom&romnom=%s&romtaille=%" PRIu64,
        devid, devpassword, scraper_software, user, password, crc, system, rom, size);
    memset(password, 0, sizeof(password));
    memset(devpassword, 0, sizeof(devpassword));
    if (n < 1 || (size_t)n >= capacity) return false;
    /* A name fallback must not retain a container CRC/size that ruled it out. */
    if (crc[0] == '\0') {
        char *begin = strstr(url, "&crc="), *end;
        if (begin == NULL || (end = strstr(begin, "&systemeid=")) == NULL) return false;
        memmove(begin, end, strlen(end) + 1U);
        end = strstr(url, "&romtaille="); if (end != NULL) *end = '\0';
    }
    return true;
}
static bool scraper_clean_name(const char *filename, char out[256])
{
    const char *dot = strrchr(filename, '.');
    size_t end = dot == NULL ? strlen(filename) : (size_t)(dot - filename), used = 0U;
    unsigned int parentheses = 0U, brackets = 0U; bool space = false;
    /* Onion's title fallback removes release tags and packaging extensions.
     * Keep actual title/version words: do not identify a hack as its parent. */
    if (end > 5U && strncmp(filename + end - 5U, ".nkit", 5U) == 0) end -= 5U;
    for (size_t i = 0U; i < end; ++i) {
        unsigned char ch = (unsigned char)filename[i];
        if (ch == '(') { ++parentheses; continue; }
        if (ch == '[') { ++brackets; continue; }
        if (ch == ')' && parentheses != 0U) { --parentheses; space = true; continue; }
        if (ch == ']' && brackets != 0U) { --brackets; space = true; continue; }
        if (parentheses != 0U || brackets != 0U) continue;
        if (ch == '-' || ch == '_' || ch == '!' || ch == '&' || ch <= ' ') { space = true; continue; }
        if (used + (space && used != 0U ? 1U : 0U) + 1U + 5U > 256U) return false;
        if (space && used != 0U) out[used++] = ' ';
        out[used++] = (char)ch; space = false;
    }
    if (parentheses != 0U || brackets != 0U || used == 0U) return false;
    /* Disc labels identify media within a game, not a different title. Only
     * remove a complete trailing Disc/Disk/CD + number token. Keep sequels,
     * version numbers and words such as "Discworld" intact. */
    out[used] = '\0';
    char *number = strrchr(out, ' ');
    if (number != NULL && number[1] >= '0' && number[1] <= '9' &&
        (number[2] == '\0' || (number[2] >= '0' && number[2] <= '9' && number[3] == '\0')))
    {
        char *word = number;
        while (word > out && word[-1] != ' ') --word;
        size_t length = (size_t)(number - word);
        char label[5] = {0};
        if (length <= 4U)
        {
            for (size_t i = 0U; i < length; ++i)
                label[i] = word[i] >= 'A' && word[i] <= 'Z' ? (char)(word[i] + ('a' - 'A')) : word[i];
            if (word > out && (strcmp(label, "disc") == 0 || strcmp(label, "disk") == 0 || strcmp(label, "cd") == 0))
                used = (size_t)(word - out) - 1U;
        }
    }
    memcpy(out + used, ".zip", 5U); return true;
}
static const char *scraper_http_error(long status)
{
    switch (status) {
    case 401: return "ScreenScraper is busy and restricting account access. Try again later.";
    case 403: return "ScreenScraper rejected the bundled application. The scraper connector needs an update.";
    case 404: return "ScreenScraper could not match this game. Existing artwork was retained.";
    case 423: return "ScreenScraper is temporarily closed. Try again later.";
    case 426: return "ScreenScraper requires a newer scraper connector.";
    case 429: return "ScreenScraper's request limit is busy. Wait before trying again.";
    case 430: case 431: return "Your ScreenScraper daily quota is exhausted. Try again tomorrow.";
    default: return "ScreenScraper request failed. Check Wi-Fi, clock and account login; existing artwork was retained.";
    }
}
static bool scraper_gameid_valid(const char *id)
{
    size_t length = strnlen(id, 11U);
    if (length == 0U || length > 10U || id[0] < '1' || id[0] > '9') return false;
    for (size_t i = 1U; i < length; ++i) if (id[i] < '0' || id[i] > '9') return false;
    return true;
}
static bool scraper_gameid_url(const struct sc_online_settings *settings, const char *id,
                               unsigned int system, char *url, size_t capacity)
{
    if (!scraper_gameid_valid(id) || !scraper_request_url(settings, "", "", system, 0U, url, capacity)) return false;
    char *tail = strstr(url, "&romtype=");
    if (tail == NULL) return false;
    size_t left = capacity - (size_t)(tail - url);
    int n = snprintf(tail, left, "&gameid=%s", id);
    return n > 0 && (size_t)n < left;
}
static void scraper_title_key(const char *title, char out[256])
{
    size_t used = 0U;
    for (size_t i = 0U; title[i] != '\0' && used < 255U; ++i) {
        unsigned char ch = (unsigned char)title[i];
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + ('a' - 'A'));
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch >= 128U) out[used++] = (char)ch;
    }
    out[used] = '\0';
}
/* Search ranking is not identity: Shenmue II's live response lists Shenmue
 * first. Require an exact title/alias on the requested system and one ID. */
static int scraper_search_id(const unsigned char *bytes, size_t size, const char *title,
                             unsigned int system, char id[11])
{
    struct sc_api_document *d = calloc(1U, sizeof(*d)); int result = -1;
    char wanted[256], system_text[16];
    scraper_title_key(title, wanted); id[0] = '\0';
    (void)snprintf(system_text, sizeof(system_text), "%u", system);
    if (d == NULL || !sc_api_doc_parse(d, (const char *)bytes, size)) goto done;
    size_t response = sc_api_doc_get(d, 0U, "response");
    size_t games = sc_api_doc_get(d, response == SIZE_MAX ? 0U : response, "jeux");
    if (games == SIZE_MAX || d->tokens[games].type != CS_DOC_ARRAY) goto done;
    result = 0;
    for (size_t i = games + 1U; i < d->tokens[games].next; i = d->tokens[i].next) {
        char candidate_id[11], platform[16];
        size_t machine = sc_api_doc_get(d, i, "systeme"), names = sc_api_doc_get(d, i, "noms");
        if (!sc_api_doc_text(d, sc_api_doc_get(d, machine, "id"), platform, sizeof(platform)) || strcmp(platform, system_text) != 0 ||
            !sc_api_doc_text(d, sc_api_doc_get(d, i, "id"), candidate_id, sizeof(candidate_id)) || !scraper_gameid_valid(candidate_id) ||
            names == SIZE_MAX || d->tokens[names].type != CS_DOC_ARRAY) continue;
        for (size_t j = names + 1U; j < d->tokens[names].next; j = d->tokens[j].next) {
            char name[256], key[256];
            if (!sc_api_doc_text(d, sc_api_doc_get(d, j, "text"), name, sizeof(name))) continue;
            scraper_title_key(name, key);
            if (wanted[0] == '\0' || strcmp(wanted, key) != 0) continue;
            if (id[0] != '\0' && strcmp(id, candidate_id) != 0) { result = 2; goto done; }
            strcpy(id, candidate_id); result = 1;
        }
    }
done:
    free(d); return result;
}
static bool scraper_resolve_id(const struct sc_online_settings *settings, const char *filename,
                               unsigned int system, const struct sc_transfer_cancel *cancel,
                               char url[4096], char status[161], bool *unmatched)
{
    char base[4096], title[256], encoded[768], id[11]; struct sc_transfer search = {0}; bool found = false;
    if (!scraper_clean_name(filename, title)) goto unavailable;
    title[strlen(title) - 4U] = '\0';
    if (!escaped(title, encoded, sizeof(encoded)) || !scraper_request_url(settings, "", "", system, 0U, base, sizeof(base))) goto unavailable;
    char *query = strchr(base, '?'), *tail = strstr(base, "&romtype=");
    if (query == NULL || tail == NULL) goto unavailable;
    *tail = '\0';
    int n = snprintf(url, 4096U, "https://api.screenscraper.fr/api2/jeuRecherche.php%s&recherche=%s", query, encoded);
    memset(base, 0, sizeof(base));
    if (n < 1 || n >= 4096) goto unavailable;
    bool received = sc_transfer_get(url, CS_DOC_BYTES, cancel, &search);
    int selection = received ? scraper_search_id(search.bytes, search.size, title, system, id) : -1;
    fprintf(stderr, "scraper: title_search system=%u http=%ld transport=%d bytes=%zu selection=%d\n",
            system, search.http_status, search.transport_error, search.size, selection);
    if (received && selection == 1 && !stop_requested(cancel)) {
        found = scraper_gameid_url(settings, id, system, url, 4096U);
        if (found) fprintf(stderr, "scraper: automatic_gameid=%s system=%u\n", id, system);
    } else if (received && (selection == 0 || selection == 2)) {
        if (unmatched != NULL) *unmatched = true;
        strcpy(status, selection == 2 ? "Title matches multiple game IDs. Use Scrape selected game by ID. Existing artwork retained." :
            "No exact title match on this system. Use Scrape selected game by ID. Existing artwork retained.");
    } else {
        (void)snprintf(status, 161U, "%s", received ? "ScreenScraper returned an unreadable title-search response. Existing artwork retained." :
            (search.transport_error == 28 ? "ScreenScraper title search timed out. Existing artwork retained." : scraper_http_error(search.http_status)));
        if (!received && search.http_status == 404L && unmatched != NULL) *unmatched = true;
    }
    sc_transfer_close(&search); return found;
unavailable:
    memset(base, 0, sizeof(base));
    strcpy(status, "Cannot form a title search for this ROM. Existing artwork retained.");
    if (unmatched != NULL) *unmatched = true;
    return false;
}
static bool scrape_with_id(const struct sc_config *c, const struct cs_store *s, const struct sc_game *g,
                      const struct sc_online_settings *settings, const struct sc_transfer_cancel *cancel,
                      char status[161], bool *unmatched, const char *game_id)
{
    char url[4096], media_url[2048], crc[9], directory[512], name[256];
    struct sc_transfer reply = {0}, image = {0}; unsigned char *png = NULL; size_t png_size = 0U; bool result = false;
    const char *basename = strrchr(g->content, '/');
    unsigned int system = system_id(g->entry.system);
    uint64_t rom_size = 0U; enum scraper_match match = SCRAPER_UNREADABLE;
    if (unmatched != NULL) *unmatched = false;
    bool by_id = game_id[0] != '\0';
    if (by_id && !scraper_gameid_valid(game_id)) {
        strcpy(status, "Enter a numeric ScreenScraper game ID (1-10 digits). Existing artwork retained."); return false;
    }
    if (system_id(g->entry.system) == 0U || basename == NULL || !image_destination(c, g, directory, name) ||
        !rom_crc(g, cancel, crc, &system, &rom_size) ||
        !(by_id ? scraper_gameid_url(settings, game_id, system, url, sizeof(url)) :
          scraper_request_url(settings, basename + 1, crc, system, rom_size, url, sizeof(url)))) {
        strcpy(status, "ROM verification cancelled or failed; existing artwork was retained."); return false;
    }
    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        bool received = sc_transfer_get(url, CS_DOC_BYTES, cancel, &reply);
        if (received) match = select_media(reply.bytes, reply.size, settings, media_url);
        fprintf(stderr, "scraper: game=%.64s system=%u attempt=%u http=%ld transport=%d bytes=%zu match=%d media=%u\n",
                g->entry.identity.game, system, attempt, reply.http_status, reply.transport_error, reply.size, (int)match, settings->media);
        if (!by_id && attempt == 0U && !stop_requested(cancel) &&
            ((!received && reply.http_status == 404L) || (received && match == SCRAPER_NO_GAME))) {
            char clean[256];
            if (scraper_clean_name(basename + 1, clean) &&
                scraper_request_url(settings, clean, "", system, 0U, url, sizeof(url))) {
                sc_transfer_close(&reply); match = SCRAPER_UNREADABLE; continue;
            }
        }
        if (!by_id && attempt == 1U && !stop_requested(cancel) &&
            ((!received && reply.http_status == 404L) || (received && match == SCRAPER_NO_GAME))) {
            sc_transfer_close(&reply);
            if (!scraper_resolve_id(settings, basename + 1, system, cancel, url, status, unmatched)) goto done;
            match = SCRAPER_UNREADABLE; continue;
        }
        if (!received) {
            if (unmatched != NULL && reply.http_status == 404L && !stop_requested(cancel)) *unmatched = true;
            const char *error=reply.transport_error==28?"ScreenScraper timed out before completing its response. Try again; existing artwork retained.":scraper_http_error(reply.http_status);
            (void)snprintf(status, 161U, "%s", error); goto done;
        }
        break;
    }
    if (match != SCRAPER_MATCHED) {
        if (unmatched != NULL && match != SCRAPER_UNREADABLE) *unmatched = true;
        const char *message = match == SCRAPER_UNREADABLE ? "ScreenScraper returned an unreadable API response; existing artwork retained." :
            (match == SCRAPER_NO_GAME ? "ScreenScraper returned no game record; existing artwork retained." :
             (match == SCRAPER_NO_MEDIA ? "Game matched, but no artwork was returned; existing artwork retained." :
              "Game matched, but this artwork type is unavailable. Try Media: box-2D."));
        (void)snprintf(status, 161U, "%s", message); goto done;
    }
    if (!sc_transfer_get(media_url, 8U * 1024U * 1024U, cancel, &image) ||
        !sc_online_normalize_image(image.bytes, image.size, &png, &png_size) || stop_requested(cancel) ||
        !sc_transfer_commit(s, directory, name, png, png_size)) {
        strcpy(status, "Artwork download/validation/flush failed. No success was acknowledged."); goto done;
    }
    strcpy(status, "Artwork downloaded, converted to PNG and durably saved."); result = true;
done:
    free(png); sc_transfer_close(&reply); sc_transfer_close(&image);
    memset(url, 0, sizeof(url));
    return result;
}
static bool scrape(const struct sc_config *c, const struct cs_store *s, const struct sc_game *g,
                   const struct sc_online_settings *settings, const struct sc_transfer_cancel *cancel,
                   char status[161], bool *unmatched)
{
    return scrape_with_id(c, s, g, settings, cancel, status, unmatched, "");
}
static bool expected_package(const struct sp_package *p, char hash[65])
{
    struct cs_document *d = calloc(1U, sizeof(*d)); char *bytes = malloc(131073U); struct stat st;
    int fd = sc_open_absolute("/etc/console/BINARY_MANIFEST.json", false); bool result = false; size_t used = 0U, entries;
    if (fd < 0 || d == NULL || bytes == NULL || fstat(fd, &st) != 0 || st.st_size < 1 || st.st_size > 131072) goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue; if (n <= 0) goto done; used += (size_t)n;
    }
    if (!cs_doc_parse(d, bytes, used)) goto done;
    entries = cs_doc_get(d, 0U, "artifacts");
    if (entries == SIZE_MAX || d->tokens[entries].type != CS_DOC_ARRAY) goto done;
    for (size_t i = entries + 1U; i < d->tokens[entries].next; i = d->tokens[i].next) {
        char path[256];
        if (text_at(d, i, "path", path, sizeof(path)) && path[0] == '/' && strcmp(path + 1, p->artifact) == 0 &&
            text_at(d, i, "sha256", hash, 65U) && strlen(hash) == 64U) { result = true; break; }
    }
done:
    if (fd >= 0 && close(fd) != 0) result = false; free(bytes); free(d); return result;
}
static bool activate_package(const struct sp_package *p, const unsigned char *bytes, size_t size, const char expected[65])
{
    char target[256], source[256], temp[128], final[160]; struct stat st;
    struct statfs fs; int root = -1, dir = -1, fd = -1; size_t used = 0U; bool result = false;
    const char *file = strrchr(p->artifact, '/'); static unsigned int serial;
    temp[0] = '\0';
    if (file == NULL || size < 64U || size > 64U * 1024U * 1024U) return false;
    (void)snprintf(target, sizeof(target), "/%s", p->artifact);
    if (sc_hash_path(target, expected, 64U * 1024U * 1024U)) return true;
    /* Only fixed package placeholders in the immutable root may be overlaid. */
    fd = sc_open_absolute(target, false);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0U) goto done;
    close(fd); fd = -1;
    root = sc_open_absolute("/run/console", true);
    if (root < 0 || fstat(root, &st) != 0 || st.st_uid != 0U || (st.st_mode & 0077U) != 0U ||
        fstatfs(root, &fs) != 0 || fs.f_type != 0x01021994L ||
        (mkdirat(root, "packages", 0700) != 0 && errno != EEXIST)) goto done;
    dir = openat(root, "packages", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0 || fstat(dir, &st) != 0 || st.st_uid != 0U || (st.st_mode & 0077U) != 0U) goto done;
    (void)snprintf(final, sizeof(final), "%s-%s", file + 1, expected);
    for (unsigned int i = 0U; i < 16U; ++i) {
        (void)snprintf(temp, sizeof(temp), ".package-%ld-%u.tmp", (long)getpid(), ++serial);
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) goto done;
    while (used < size) {
        ssize_t n = write(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done; used += (size_t)n;
    }
    if (fchmod(fd, 0444) != 0 || fsync(fd) != 0) goto done;
    if (close(fd) != 0) { fd = -1; goto done; } fd = -1;
    if (renameat(dir, temp, dir, final) != 0 || fsync(dir) != 0) goto done;
    (void)snprintf(source, sizeof(source), "/run/console/packages/%s", final);
    if (mount(source, target, NULL, MS_BIND, NULL) != 0) goto done;
    if (mount(NULL, target, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) != 0 ||
        !sc_hash_path(target, expected, 64U * 1024U * 1024U)) {
        (void)umount(target); goto done;
    }
    result = true;
done:
    if (fd >= 0) (void)close(fd);
    if (dir >= 0) { if (temp[0] != '\0') (void)unlinkat(dir, temp, 0); (void)close(dir); }
    if (root >= 0) (void)close(root);
    return result;
}
struct public_source {
    char url[512], archive_hash[65], nested[128], member[512];
    char drop_needed[32];
};
static bool public_source_load(const struct sp_package *p, struct public_source *source)
{
    struct cs_document *d = calloc(1U, sizeof(*d)); unsigned char *bytes = NULL;
    struct stat st; size_t used = 0U, entries; bool result = false;
    int fd = sc_open_absolute("/etc/console/DOWNLOAD_SOURCES.json", false);
    memset(source, 0, sizeof(*source));
    if (fd < 0 || d == NULL || fstat(fd, &st) != 0 || st.st_size < 1 || st.st_size > 65536) goto done;
    bytes = malloc((size_t)st.st_size); if (bytes == NULL) goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue; if (n <= 0) goto done; used += (size_t)n;
    }
    if (!cs_doc_parse(d, (const char *)bytes, used)) goto done;
    entries = cs_doc_get(d, 0U, "packages");
    if (entries == SIZE_MAX || d->tokens[entries].type != CS_DOC_ARRAY) goto done;
    for (size_t i = entries + 1U; i < d->tokens[entries].next; i = d->tokens[i].next) {
        char id[64];
        if (!text_at(d, i, "id", id, sizeof(id)) || strcmp(id, p->id) != 0) continue;
        if (!text_at(d, i, "url", source->url, sizeof(source->url)) || strncmp(source->url, "https://", 8U) != 0 ||
            !text_at(d, i, "archive_sha256", source->archive_hash, sizeof(source->archive_hash)) || strlen(source->archive_hash) != 64U ||
            !text_at(d, i, "nested", source->nested, sizeof(source->nested)) ||
            !text_at(d, i, "member", source->member, sizeof(source->member))) goto done;
        if (cs_doc_get(d, i, "drop_needed") != SIZE_MAX &&
            (!text_at(d, i, "drop_needed", source->drop_needed, sizeof(source->drop_needed)) ||
             strcmp(source->drop_needed, "libtmenu.so") != 0)) goto done;
        if (source->member[0] == '\0' && source->nested[0] != '\0') goto done;
        for (size_t k = 0U; k < 64U; ++k)
            if (!((source->archive_hash[k] >= '0' && source->archive_hash[k] <= '9') ||
                  (source->archive_hash[k] >= 'a' && source->archive_hash[k] <= 'f'))) goto done;
        result = true; break;
    }
done:
    if (fd >= 0 && close(fd) != 0) result = false; free(bytes); free(d); return result;
}
static bool cached_source(const struct cs_store *s, const char *name, const char expected[65],
                          const struct sc_transfer_cancel *cancel, struct sc_transfer *reply)
{
    int directory = -1, fd = -1; struct stat st; char digest[65]; bool result = false;
    directory = openat(s->data_fd, "download-sources", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) goto done;
    fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 22 || st.st_size > 67108864) goto done;
    reply->bytes = malloc((size_t)st.st_size);
    if (reply->bytes == NULL) goto done;
    while (reply->size < (size_t)st.st_size) {
        size_t remaining = (size_t)st.st_size - reply->size;
        ssize_t n;
        if (stop_requested(cancel)) goto done;
        if (remaining > 65536U) remaining = 65536U;
        n = read(fd, reply->bytes + reply->size, remaining);
        if (n < 0 && errno == EINTR) continue; if (n <= 0) goto done; reply->size += (size_t)n;
    }
    cs_sha256_hex(reply->bytes, reply->size, digest);
    result = strcmp(digest, expected) == 0 && cs_store_check_media(s) == CS_STORE_OK;
done:
    if (fd >= 0 && close(fd) != 0) result = false;
    if (directory >= 0) (void)close(directory);
    if (!result) sc_transfer_close(reply);
    return result;
}
static bool public_source_fetch(const struct cs_store *s, const struct public_source *source,
                                const struct sc_transfer_cancel *cancel, struct sc_transfer *core)
{
    struct sc_transfer archive = {0}; unsigned char *nested = NULL;
    size_t nested_size = 0U; char hash[65], name[80]; bool result = false;
    (void)snprintf(name, sizeof(name), "%s.zip", source->archive_hash);
    if (!cached_source(s, name, source->archive_hash, cancel, &archive)) {
        if (!sc_transfer_get(source->url, 64U * 1024U * 1024U, cancel, &archive)) goto done;
        cs_sha256_hex(archive.bytes, archive.size, hash);
        if (strcmp(hash, source->archive_hash) != 0 || stop_requested(cancel) ||
            !sc_transfer_commit(s, ".console/download-sources", name, archive.bytes, archive.size)) goto done;
    }
    if (stop_requested(cancel)) goto done;
    if (source->member[0] == '\0') {
        core->bytes = archive.bytes; core->size = archive.size;
        archive.bytes = NULL; archive.size = 0U;
        result = source->drop_needed[0] == '\0' || sc_elf_remove_unused_tmenu(core->bytes, core->size);
    } else if (source->nested[0] == '\0')
        result = sc_zip_read(archive.bytes, archive.size, source->member, 32U * 1024U * 1024U, &core->bytes, &core->size);
    else if (sc_zip_read(archive.bytes, archive.size, source->nested, 48U * 1024U * 1024U, &nested, &nested_size))
        result = sc_zip_read(nested, nested_size, source->member, 32U * 1024U * 1024U, &core->bytes, &core->size);
    result = result && !stop_requested(cancel);
done:
    free(nested); sc_transfer_close(&archive); return result;
}
bool sc_online_restore_packages(const struct sc_config *c)
{
    struct cs_store s; struct cs_store_config cfg = {0}; int directory; bool result = true;
    cfg.card_path = c->base.card_path; cfg.card_uuid = c->base.card_uuid;
    cfg.max_state_bytes = c->base.max_state_bytes;
    cfg.qualified_fat_syncfs = c->base.qualified_fat_syncfs;
    cs_store_init(&s);
    if (cs_store_open_reader(&s, &cfg) != CS_STORE_OK) { cs_store_close(&s); return false; }
    directory = openat(s.data_fd, "downloads", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) { result = errno == ENOENT; cs_store_close(&s); return result; }
    for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i) {
        const struct sp_package *p = sp_package_at(i); const char *file = strrchr(p->artifact, '/');
        char expected[65], actual[65]; struct stat st; unsigned char *bytes = NULL; size_t used = 0U; int fd;
        if (!sp_package_core_layer(p->layer) || file == NULL) continue;
        fd = openat(directory, file + 1, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { if (errno != ENOENT) result = false; continue; }
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 64 ||
            st.st_size > 67108864 || !expected_package(p, expected)) { close(fd); result = false; continue; }
        /* The exact package is already in immutable firmware on this build.
         * Authenticate it first: reading/hashing the cached duplicate cannot
         * improve that installed core, and used to delay every cold boot.
         * Missing/mismatched placeholders retain the full cache validation and
         * protected activation path below. No persistent "verified" flag. */
        char installed[256];
        int path_size = snprintf(installed, sizeof(installed), "/%s", p->artifact);
        if (path_size > 0 && (size_t)path_size < sizeof(installed) &&
            sc_hash_path(installed, expected, 64U * 1024U * 1024U)) {
            fprintf(stderr, "packages: installed core verified; cached duplicate skipped: %s\n", p->id);
            if (close(fd) != 0) result = false;
            continue;
        }
        bytes = malloc((size_t)st.st_size);
        if (bytes != NULL) while (used < (size_t)st.st_size) {
            ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
            if (n < 0 && errno == EINTR) continue; if (n <= 0) break; used += (size_t)n;
        }
        if (bytes != NULL && used == (size_t)st.st_size) {
            cs_sha256_hex(bytes, used, actual);
            if (strcmp(actual, expected) != 0 || cs_store_check_media(&s) != CS_STORE_OK ||
                !activate_package(p, bytes, used, expected)) result = false;
        } else result = false;
        free(bytes); if (close(fd) != 0) result = false;
    }
    close(directory); cs_store_close(&s); return result;
}
static bool download_package(const struct cs_store *s, const struct sc_online_settings *settings,
                               const struct sp_package *p, const struct sc_transfer_cancel *cancel, char status[161])
{
    char expected[65], actual[65], url[512]; struct sc_transfer reply = {0}; bool result = false;
    struct public_source source;
    const char *file = strrchr(p->artifact, '/'); int n;
    if (!sp_package_core_layer(p->layer) || file == NULL || !expected_package(p, expected)) {
        strcpy(status, "Package has no protected AArch64 verification record."); return false;
    }
    if (stop_requested(cancel) || cs_store_check_media(s) != CS_STORE_OK) {
        strcpy(status, "Package request cancelled or game card unavailable. Existing package retained.");
        return false;
    }
    n = snprintf(url, sizeof(url), "/%s", p->artifact);
    if (n > 0 && (size_t)n < sizeof(url) && sc_hash_path(url, expected, 64U * 1024U * 1024U)) {
        strcpy(status, "Verified package is included in this firmware. Enable it in Package Manager.");
        return true;
    }
    if (settings->repository[0] == '\0') {
        if (!public_source_load(p, &source) || !public_source_fetch(s, &source, cancel, &reply)) {
            strcpy(status, "Public Zero28 package download/cache/archive validation failed. Existing core retained."); goto done;
        }
    } else {
        n = snprintf(url, sizeof(url), "%s%s%s", settings->repository,
            settings->repository[strlen(settings->repository) - 1U] == '/' ? "" : "/", file + 1);
        if (n < 1 || (size_t)n >= sizeof(url) || !sc_transfer_get(url, 64U * 1024U * 1024U, cancel, &reply)) {
            (void)snprintf(status, 161U, "Package download failed (HTTP %ld). Existing installed package retained.", reply.http_status); goto done;
        }
    }
    cs_sha256_hex(reply.bytes, reply.size, actual);
    if (strcmp(actual, expected) != 0 || reply.size < 64U || memcmp(reply.bytes, "\177ELF\002\001", 6U) != 0 ||
        reply.bytes[18] != 183U || reply.bytes[19] != 0U || reply.bytes[16] != 3U || reply.bytes[17] != 0U) {
        strcpy(status, "Package architecture/hash mismatch. Download was rejected."); goto done;
    }
    if (stop_requested(cancel) || !sc_transfer_commit(s, ".console/downloads", file + 1, reply.bytes, reply.size)) {
        strcpy(status, "Package could not be durably cached. Existing installation retained."); goto done;
    }
    if (!activate_package(p, reply.bytes, reply.size, expected)) {
        strcpy(status, "Package cached; protected activation failed. Existing installation retained."); goto done;
    }
    strcpy(status, "Verified native package downloaded and ready. Enable it in Package Manager."); result = true;
done:
    sc_transfer_close(&reply); return result;
}
void sc_online_request(const struct sc_config *c, const struct cs_store *s, const struct cl_tool_request *r,
                         struct cl_tool_snapshot *snapshot, const struct sc_transfer_cancel *cancel)
{
    struct sc_online_settings settings; bool downloads = r->operation == CL_TOOL_LOAD_DOWNLOADS || r->operation == CL_TOOL_DOWNLOAD_PACKAGE;
    char note[161] = ""; bool success = true, skip_unmatched = false;
    memset(snapshot, 0, sizeof(*snapshot)); snapshot->kind = downloads ? CL_TOOL_DOWNLOADS : CL_TOOL_SCRAPER;
    snapshot->ready = true;
    if (!sc_online_settings_load(s, &settings)) {
        snapshot->error = true; strcpy(snapshot->status, "Online settings are damaged/unreadable; previous data retained."); return;
    }
    if (r->operation == CL_TOOL_SAVE_ONLINE_SETTING) {
        char *field = r->enabled_mask == SC_ONLINE_USER ? settings.user : (r->enabled_mask == SC_ONLINE_PASSWORD ? settings.password :
            (r->enabled_mask == SC_ONLINE_DEVID ? settings.devid : (r->enabled_mask == SC_ONLINE_DEVPASSWORD ? settings.devpassword :
             (r->enabled_mask == SC_ONLINE_REPOSITORY ? settings.repository : NULL))));
        if (field != NULL) strcpy(field, r->id);
        else if (r->enabled_mask == SC_ONLINE_REGION) settings.region = (settings.region + 1U) % 8U;
        else if (r->enabled_mask == SC_ONLINE_MEDIA) settings.media = (settings.media + 1U) % 8U;
        else success = false;
        success = success && sc_online_settings_save(s, &settings);
        (void)snprintf(note, sizeof(note), "%s", success ? "Account/preferences durably saved." : "Online settings could not be saved.");
        downloads = r->enabled_mask == SC_ONLINE_REPOSITORY;
        snapshot->kind = downloads ? CL_TOOL_DOWNLOADS : CL_TOOL_SCRAPER;
    } else if (r->operation == CL_TOOL_SCRAPE_GAME || r->operation == CL_TOOL_SCRAPE_MISSING) {
        const struct sc_game *game = NULL;
        size_t next = 0U;
        if (r->operation == CL_TOOL_SCRAPE_GAME) game = sc_game_find(c, r->target);
        else for (size_t i = r->revision <= c->game_count ? (size_t)r->revision : c->game_count; i < c->game_count; ++i) {
            char directory[512], name[256], path[1024]; int fd;
            if (!image_destination(c, &c->games[i], directory, name)) continue;
            (void)snprintf(path, sizeof(path), "%s/%s/%s", c->base.card_path, directory, name);
            fd = sc_open_absolute(path, false);
            if (fd >= 0) { close(fd); continue; }
            if (errno == ENOENT) { game = &c->games[i]; next = i + 1U; break; }
        }
        if (settings.devid[0] == '\0' || settings.devpassword[0] == '\0') {
            success = false;
            strcpy(note, "Enter your registered ScreenScraper API client ID and password.");
        } else if (game == NULL) { strcpy(note, r->operation == CL_TOOL_SCRAPE_MISSING ? "No missing covers remain." : "No qualified selected ROM."); }
        else {
            success = r->operation == CL_TOOL_SCRAPE_GAME && r->id[0] != '\0' ?
                scrape_with_id(c, s, game, &settings, cancel, note, &skip_unmatched, r->id) :
                scrape(c, s, game, &settings, cancel, note, &skip_unmatched);
            snapshot->revision = next;
            snapshot->enabled_mask = (success || skip_unmatched) && next < c->game_count &&
                r->operation == CL_TOOL_SCRAPE_MISSING && !stop_requested(cancel) ? 1U : 0U;
        }
        if (r->operation == CL_TOOL_SCRAPE_MISSING) {
            uint64_t saved = r->enabled_mask >> 32U, skipped = r->enabled_mask & UINT32_MAX;
            char detail[161];
            if (game != NULL && success) ++saved;
            if (game != NULL && skip_unmatched) ++skipped;
            snapshot->available_mask = (saved << 32U) | skipped;
            (void)snprintf(detail, sizeof(detail), "%s", note);
            (void)snprintf(note, sizeof(note), "Saved %" PRIu64 ", skipped %" PRIu64 ". %.96s", saved, skipped,
                game == NULL && success ? "Missing-cover scan complete." : detail);
        }
    } else if (r->operation == CL_TOOL_DOWNLOAD_PACKAGE) {
        int index = sp_package_index(r->id);
        success = index >= 0 && download_package(s, &settings, sp_package_at((size_t)index), cancel, note);
    }
    if (downloads) {
        add_row(snapshot, "@repository", "Source: verified native Linux releases", settings.repository[0] == '\0' ? "Ready; A sets an optional direct-file mirror" : settings.repository, true);
        for (size_t i = 0U; i < SP_PACKAGE_COUNT; ++i) {
            const struct sp_package *p = sp_package_at(i);
            if (sp_package_core_layer(p->layer)) add_row(snapshot, p->id, p->title, "A downloads the exact protected AArch64 package", true);
        }
    } else {
        char label[96];
        add_row(snapshot, "@user", "ScreenScraper username", settings.user[0] == '\0' ? "A enters your account name" : settings.user, true);
        add_row(snapshot, "@password", "ScreenScraper password", settings.password[0] == '\0' ? "Not set" : "Saved (hidden)", true);
        add_row(snapshot, "@devid", "API client ID", settings.devid[0] == '\0' ? "Not set" : "Saved", true);
        add_row(snapshot, "@devpassword", "API client password", settings.devpassword[0] == '\0' ? "Not set" : "Saved (hidden)", true);
        (void)snprintf(label, sizeof(label), "Preferred region: %s", regions[settings.region]);
        add_row(snapshot, "@region", label, "A cycles region; world/other regions are fallbacks", true);
        (void)snprintf(label, sizeof(label), "Media: %s", media_types[settings.media]);
        add_row(snapshot, "@media", label, "A cycles cover / screenshot / wheel / marquee / mix", true);
        add_row(snapshot, "@selected", "Scrape selected game", "CRC and SHA verify the ROM; existing art changes only after validation", r->target[0] != '\0');
        add_row(snapshot, "@missing", "Scrape all missing covers", "Sequential requests; B cancels; covers already present are skipped", true);
        add_row(snapshot, "@gameid", "Scrape selected game by ID", "Enter the numeric ScreenScraper game ID; open Y > Scraper on a game", r->target[0] != '\0');
    }
    if (!success && note[0] == '\0') strcpy(note, "Online request failed. Existing data retained.");
    (void)snprintf(snapshot->status, sizeof(snapshot->status), "%s", note[0] != '\0' ? note :
        (downloads ? "Only packages matching this image's protected SHA256 are accepted." : "Set your ScreenScraper account and registered API client to download artwork."));
    /* Service failures remain on this screen so account/network setup is usable. */
    snapshot->error = false;
    if (!success && !skip_unmatched) snapshot->enabled_mask = 0U;
    memset(&settings, 0, sizeof(settings));
}
