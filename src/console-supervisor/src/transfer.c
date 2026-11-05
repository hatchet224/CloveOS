#include "supervisor/transfer.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct sink {
    struct sc_transfer *result;
    const struct sc_transfer_cancel *cancel;
    size_t limit;
    size_t capacity;
};
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static bool curl_ready;
static void initialize(void) { curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }
static bool cancelled(const struct sc_transfer_cancel *c)
{ return c != NULL && c->cancelled != NULL && c->cancelled(c->context); }
static size_t receive_bytes(char *data, size_t size, size_t count, void *context)
{
    struct sink *s = context; unsigned char *grown; size_t n;
    if (cancelled(s->cancel) || (count != 0U && size > SIZE_MAX / count)) return 0U;
    n = size * count;
    if (s->result->size > s->limit || n > s->limit - s->result->size) return 0U;
    if (s->result->size + n + 1U > s->capacity) {
        size_t capacity = s->capacity != 0U ? s->capacity : 4096U;
        while (capacity < s->result->size + n + 1U) {
            if (capacity > (s->limit + 1U) / 2U) { capacity = s->limit + 1U; break; }
            capacity *= 2U;
        }
        if (capacity > s->limit + 1U) capacity = s->limit + 1U;
        grown = realloc(s->result->bytes, capacity);
        if (grown == NULL) return 0U;
        s->result->bytes = grown; s->capacity = capacity;
    }
    grown = s->result->bytes;
    memcpy(grown + s->result->size, data, n); s->result->size += n;
    grown[s->result->size] = '\0'; return n;
}
static int progress(void *context, curl_off_t total, curl_off_t received,
                      curl_off_t upload_total, curl_off_t uploaded)
{
    struct sink *s = context;
    (void)upload_total; (void)uploaded;
    return cancelled(s->cancel) || total > (curl_off_t)s->limit || received > (curl_off_t)s->limit;
}
bool sc_transfer_get_ca(const char *url, const char *ca, size_t limit,
                          const struct sc_transfer_cancel *cancel, struct sc_transfer *r)
{
    CURL *curl; CURLcode result; struct sink sink = {r, cancel, limit, 0U};
    if (r == NULL) return false;
    memset(r, 0, sizeof(*r));
    if (url == NULL || strncmp(url, "https://", 8U) != 0 || strlen(url) > 8192U ||
        ca == NULL || limit == 0U || limit > 64U * 1024U * 1024U || cancelled(cancel)) return false;
    (void)pthread_once(&curl_once, initialize);
    if (!curl_ready || (curl = curl_easy_init()) == NULL) return false;
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) goto fail; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, "https"); SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SET(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO, ca); SET(CURLOPT_CAPATH, (const char *)NULL);
    SET(CURLOPT_PROXY, ""); SET(CURLOPT_NOPROXY, "*");
    SET(CURLOPT_FOLLOWLOCATION, 1L); SET(CURLOPT_MAXREDIRS, 3L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    /* The game lookup endpoint sometimes queues a request before sending its
     * first byte. Handheld logs confirmed CURLE_OPERATION_TIMEDOUT with HTTP 0.
     * Give that endpoint a bounded wait without relaxing TLS or other downloads. */
    bool scraper_api=strncmp(url,"https://api.screenscraper.fr/api2/jeuInfos.php?",
                             strlen("https://api.screenscraper.fr/api2/jeuInfos.php?"))==0 ||
        strncmp(url,"https://api.screenscraper.fr/api2/jeuRecherche.php?",
                strlen("https://api.screenscraper.fr/api2/jeuRecherche.php?"))==0;
    SET(CURLOPT_TIMEOUT_MS, limit >= 32U * 1024U * 1024U ? 120000L : scraper_api ? 45000L : 20000L);
    SET(CURLOPT_LOW_SPEED_LIMIT, 128L); SET(CURLOPT_LOW_SPEED_TIME, scraper_api ? 30L : 8L);
    SET(CURLOPT_NOSIGNAL, 1L); SET(CURLOPT_FAILONERROR, 1L);
    SET(CURLOPT_USERAGENT, "CloveOS/1.0");
    SET(CURLOPT_WRITEFUNCTION, receive_bytes); SET(CURLOPT_WRITEDATA, &sink);
    SET(CURLOPT_XFERINFOFUNCTION, progress); SET(CURLOPT_XFERINFODATA, &sink);
    SET(CURLOPT_NOPROGRESS, 0L); SET(CURLOPT_VERBOSE, 0L);
    result = curl_easy_perform(curl);
    r->transport_error = (int)result;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->http_status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && r->http_status >= 200L && r->http_status < 300L &&
           r->size > 0U && !cancelled(cancel);
fail:
    curl_easy_cleanup(curl); return false;
#undef SET
}
bool sc_transfer_get(const char *url, size_t limit, const struct sc_transfer_cancel *cancel,
                       struct sc_transfer *r)
{ return sc_transfer_get_ca(url, "/etc/console/ca-certificates.crt", limit, cancel, r); }
void sc_transfer_close(struct sc_transfer *r)
{ if (r != NULL) { free(r->bytes); memset(r, 0, sizeof(*r)); } }
static bool component(const char *p)
{
    size_t n = strlen(p);
    return n > 0U && n < 256U && strcmp(p, ".") != 0 && strcmp(p, "..") != 0 &&
           strchr(p, '/') == NULL && strchr(p, '\\') == NULL;
}
static bool flush_directory(const struct cs_store *s, int fd)
{
    return fsync(fd) == 0 || ((errno == EINVAL || errno == EOPNOTSUPP) && s->fat &&
                             s->qualified_fat_syncfs && syncfs(fd) == 0);
}
static bool parent_still_named(const struct cs_store *s, const char *directory,
                               const struct stat *expected)
{
    char path[512], *p, *save = NULL;
    struct stat live; int fd = fcntl(s->card_fd, F_DUPFD_CLOEXEC, 3);
    bool result = false;
    if (fd < 0) return false;
    strcpy(path, directory);
    for (p = strtok_r(path, "/", &save); p != NULL; p = strtok_r(NULL, "/", &save)) {
        int child = openat(fd, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(fd); fd = child;
        if (fd < 0) return false;
    }
    result = fstat(fd, &live) == 0 && live.st_dev == expected->st_dev &&
        live.st_ino == expected->st_ino && live.st_nlink != 0U;
    if (close(fd) != 0) result = false;
    return result;
}
bool sc_transfer_commit(const struct cs_store *s, const char *directory, const char *name,
                          const unsigned char *bytes, size_t size)
{
    char path[512], temp[96], *p, *save = NULL;
    struct stat root, entry, live; int parent = -1, fd = -1; bool result = false;
    size_t used = 0U; static unsigned int serial;
    temp[0] = '\0';
    if (s == NULL || directory == NULL || strlen(directory) >= sizeof(path) ||
        directory[0] == '/' || strstr(directory, "//") != NULL ||
        !component(name) || bytes == NULL || size == 0U ||
        cs_store_check_media(s) != CS_STORE_OK || fstat(s->card_fd, &root) != 0) return false;
    parent = fcntl(s->card_fd, F_DUPFD_CLOEXEC, 3);
    if (parent < 0) return false;
    strcpy(path, directory); p = strtok_r(path, "/", &save);
    while (p != NULL) {
        int child;
        if (!component(p) || (mkdirat(parent, p, 0700) != 0 && errno != EEXIST)) goto done;
        child = openat(parent, p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) goto done;
        if (fstat(child, &entry) != 0 || entry.st_dev != root.st_dev || !flush_directory(s, parent)) {
            close(child); goto done;
        }
        close(parent); parent = child; p = strtok_r(NULL, "/", &save);
    }
    if (fstat(parent, &entry) != 0) goto done;
    for (unsigned int tries = 0U; tries < 16U; ++tries) {
        (void)snprintf(temp, sizeof(temp), ".download-%ld-%u.tmp", (long)getpid(), ++serial);
        fd = openat(parent, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) goto done;
    while (used < size) {
        ssize_t n = write(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    if (fsync(fd) != 0 || cs_store_check_media(s) != CS_STORE_OK || fstat(parent, &live) != 0 ||
        live.st_dev != entry.st_dev || live.st_ino != entry.st_ino || live.st_nlink == 0U ||
        !parent_still_named(s, directory, &entry)) goto done;
    if (close(fd) != 0) { fd = -1; goto done; } fd = -1;
    if (renameat(parent, temp, parent, name) != 0 || !flush_directory(s, parent)) goto done;
    result = cs_store_check_media(s) == CS_STORE_OK && parent_still_named(s, directory, &entry);
done:
    if (fd >= 0) (void)close(fd);
    if (temp[0] != '\0') (void)unlinkat(parent, temp, 0);
    if (parent >= 0 && close(parent) != 0) result = false;
    return result;
}
