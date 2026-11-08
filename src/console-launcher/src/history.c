#include "sessiond/durability.h"
#include "launcher/history.h"
#include "sessiond/json.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_BYTES (256U * 1024U)
#define ENTRY_BYTES 2048U
bool cl_title_matches(const char *title,const char *keyword)
{
    if(keyword[0]=='\0')return true;
    for(size_t i=0U;title[i]!='\0';++i){size_t j=0U;
        while(keyword[j]!='\0' && title[i+j]!='\0' &&
              tolower((unsigned char)title[i+j])==tolower((unsigned char)keyword[j]))++j;
        if(keyword[j]=='\0')return true;
    }
    return false;
}
static const char zero_hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
static bool ref_valid(const struct cl_reference *ref)
{
    if (ref == NULL || strnlen(ref->manifest, sizeof(ref->manifest)) == sizeof(ref->manifest)) {
        return false;
    }
    return ref->generation == 0U
               ? (ref->manifest[0] == '\0' || strcmp(ref->manifest, zero_hash) == 0)
               : cs_hex_valid(ref->manifest, 64U) != 0;
}
static void hex_text(const char *text, char *out)
{
    static const char digits[] = "0123456789abcdef";
    while (*text != '\0') {
        unsigned char c = (unsigned char)*text++;
        *out++ = digits[c >> 4];
        *out++ = digits[c & 15U];
    }
    *out = '\0';
}
static unsigned int nibble(char c)
{
    return c <= '9' ? (unsigned int)(c - '0') : (unsigned int)(c - 'a') + 10U;
}
static bool unhex_text(const char *text, char *out, size_t capacity)
{
    size_t i, length;
    if (text == NULL || (length = strlen(text)) % 2U != 0U || length / 2U >= capacity ||
        (length != 0U && !cs_hex_valid(text, length))) {
        return false;
    }
    for (i = 0U; i < length / 2U; ++i) {
        unsigned int c = (nibble(text[2U * i]) << 4) | nibble(text[2U * i + 1U]);
        if (c < 32U || c == 127U) {
            return false;
        }
        out[i] = (char)c;
    }
    out[length / 2U] = '\0';
    return true;
}
static bool text_valid(const char *text, size_t capacity)
{
    size_t i, length = strnlen(text, capacity);
    if (length == capacity) {
        return false;
    }
    for (i = 0U; i < length; ++i) {
        if ((unsigned char)text[i] < 32U || (unsigned char)text[i] == 127U) {
            return false;
        }
    }
    return true;
}
static bool entry_valid(const struct cl_entry *entry)
{
    return cs_identity_valid(&entry->identity) && entry->play_sequence != 0U &&
           entry->title[0] != '\0' && text_valid(entry->title, sizeof(entry->title)) &&
           text_valid(entry->system, sizeof(entry->system)) &&
           text_valid(entry->context, sizeof(entry->context)) && ref_valid(&entry->latest) &&
           ref_valid(&entry->previous) &&
           (entry->previous.generation == 0U ||
            entry->latest.generation > entry->previous.generation);
}
static int entry_encode(const struct cl_entry *e, char out[ENTRY_BYTES])
{
    char title[193], system[65], context[193];
    hex_text(e->title, title);
    hex_text(e->system, system);
    hex_text(e->context, context);
    return snprintf(
        out, ENTRY_BYTES,
        "{\"game\":\"%s\",\"content\":\"%s\",\"core\":\"%s\",\"config\":\"%s\","
        "\"bios\":\"%s\",\"retroarch\":\"%s\",\"disc\":\"%s\",\"policy\":\"%s\","
        "\"title_hex\":\"%s\",\"system_hex\":\"%s\",\"context_hex\":\"%s\","
        "\"sequence\":\"%" PRIu64 "\",\"latest\":\"%" PRIu64 "\",\"latest_hash\":\"%s\","
        "\"previous\":\"%" PRIu64 "\",\"previous_hash\":\"%s\"}\n",
        e->identity.game, e->identity.content, e->identity.core, e->identity.config,
        e->identity.bios, e->identity.retroarch, e->identity.disc, e->identity.policy, title,
        system, context, e->play_sequence, e->latest.generation,
        e->latest.generation == 0U ? zero_hash : e->latest.manifest, e->previous.generation,
        e->previous.generation == 0U ? zero_hash : e->previous.manifest);
}
static bool get_hash(const struct cs_json *json, const char *key, char out[65])
{
    const char *s = cs_json_string(json, key);
    if (!cs_hex_valid(s, 64U)) {
        return false;
    }
    memcpy(out, s, 65U);
    return true;
}
static bool entry_decode(const char *data, size_t length, struct cl_entry *e)
{
    struct cs_json json;
    static const char *const keys[] = {"game",      "content",     "core",        "config",
                                       "bios",      "retroarch",   "disc",        "policy",
                                       "title_hex", "system_hex",  "context_hex", "sequence",
                                       "latest",    "latest_hash", "previous",    "previous_hash"};
    memset(e, 0, sizeof(*e));
    if (!cs_json_parse(&json, data, length) || !cs_json_keys(&json, keys, 16U)) {
        return false;
    }
    return get_hash(&json, "game", e->identity.game) &&
           get_hash(&json, "content", e->identity.content) &&
           get_hash(&json, "core", e->identity.core) &&
           get_hash(&json, "config", e->identity.config) &&
           get_hash(&json, "bios", e->identity.bios) &&
           get_hash(&json, "retroarch", e->identity.retroarch) &&
           get_hash(&json, "disc", e->identity.disc) &&
           get_hash(&json, "policy", e->identity.policy) &&
           unhex_text(cs_json_string(&json, "title_hex"), e->title, sizeof(e->title)) &&
           unhex_text(cs_json_string(&json, "system_hex"), e->system, sizeof(e->system)) &&
           unhex_text(cs_json_string(&json, "context_hex"), e->context, sizeof(e->context)) &&
           cs_json_u64(&json, "sequence", &e->play_sequence) &&
           cs_json_u64(&json, "latest", &e->latest.generation) &&
           get_hash(&json, "latest_hash", e->latest.manifest) &&
           cs_json_u64(&json, "previous", &e->previous.generation) &&
           get_hash(&json, "previous_hash", e->previous.manifest) && entry_valid(e);
}
static bool history_valid(const struct cl_history *h)
{
    size_t i, j;
    if (h->count > CL_HISTORY_MAX || h->revision == 0U || h->revision < h->highwater) {
        return false;
    }
    for (i = 0U; i < h->count; ++i) {
        if (!entry_valid(&h->entries[i]) || h->entries[i].play_sequence > h->highwater ||
            (i != 0U && h->entries[i - 1U].play_sequence <= h->entries[i].play_sequence)) {
            return false;
        }
        for (j = 0U; j < i; ++j) {
            if (strcmp(h->entries[i].identity.game, h->entries[j].identity.game) == 0) {
                return false;
            }
        }
    }
    return true;
}
static bool history_digest(const struct cl_history *h, char digest[65])
{
    char line[ENTRY_BYTES];
    struct cs_sha256 hash;
    size_t i;
    int n = snprintf(line, sizeof(line),
                     "{\"schema\":1,\"revision\":\"%" PRIu64 "\",\"highwater\":\"%" PRIu64
                     "\",\"count\":%zu}\n",
                     h->revision, h->highwater, h->count);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return false;
    }
    cs_sha256_init(&hash);
    cs_sha256_update(&hash, line, (size_t)n);
    for (i = 0U; i < h->count; ++i) {
        n = entry_encode(&h->entries[i], line);
        if (n < 0 || (size_t)n >= sizeof(line)) {
            return false;
        }
        cs_sha256_update(&hash, line, (size_t)n);
    }
    cs_sha256_final(&hash, digest);
    return true;
}
enum cl_history_result cl_history_read(struct cs_store *store, struct cl_history *out)
{
    int fd;
    struct stat before, after;
    struct cl_history *h;
    struct cs_json json;
    char *buffer, *p, *end, digest[65];
    char expected[65];
    size_t used = 0U, i;
    uint64_t schema, count;
    enum cl_history_result result = CL_HISTORY_CORRUPT;
    static const char *const keys[] = {"schema", "revision", "highwater", "count", "checksum"};
    if (out == NULL || cs_store_check_media(store) != CS_STORE_OK) {
        return CL_HISTORY_MEDIA;
    }
    fd = openat(store->data_fd, "history.jsonl", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? CL_HISTORY_EMPTY : CL_HISTORY_IO;
    }
    buffer = malloc(HISTORY_BYTES + 1U);
    h = calloc(1U, sizeof(*h));
    if (buffer == NULL || h == NULL) {
        result = CL_HISTORY_IO;
        goto done;
    }
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_uid != geteuid() || before.st_dev != store->device ||
        (before.st_mode & 0022U) != 0U || before.st_size < 1 ||
        (uint64_t)before.st_size > HISTORY_BYTES) {
        goto done;
    }
    while (used < HISTORY_BYTES + 1U) {
        ssize_t n = read(fd, buffer + used, HISTORY_BYTES + 1U - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            result = CL_HISTORY_IO;
            goto done;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    if (used > HISTORY_BYTES || used != (size_t)before.st_size || fstat(fd, &after) != 0 ||
        after.st_size != before.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || memchr(buffer, '\0', used) != NULL) {
        goto done;
    }
    end = buffer + used;
    p = memchr(buffer, '\n', used);
    if (p == NULL || !cs_json_parse(&json, buffer, (size_t)(p - buffer)) ||
        !cs_json_keys(&json, keys, 5U) || !cs_json_u64(&json, "schema", &schema) || schema != 1U ||
        !cs_json_u64(&json, "revision", &h->revision) ||
        !cs_json_u64(&json, "highwater", &h->highwater) || !cs_json_u64(&json, "count", &count) ||
        count > CL_HISTORY_MAX || !get_hash(&json, "checksum", expected)) {
        goto done;
    }
    h->count = (size_t)count;
    ++p;
    for (i = 0U; i < h->count; ++i) {
        char *next = memchr(p, '\n', (size_t)(end - p));
        if (next == NULL || (size_t)(next - p) >= ENTRY_BYTES ||
            !entry_decode(p, (size_t)(next - p), &h->entries[i])) {
            goto done;
        }
        p = next + 1;
    }
    if (p != end || !history_valid(h) || !history_digest(h, digest) ||
        strcmp(digest, expected) != 0) {
        goto done;
    }
    result = cs_store_check_media(store) == CS_STORE_OK ? CL_HISTORY_OK : CL_HISTORY_MEDIA;
done:
    if (close(fd) != 0) {
        result = CL_HISTORY_IO;
    }
    if (result == CL_HISTORY_OK) {
        *out = *h;
    }
    free(buffer);
    free(h);
    return result;
}
enum cl_history_result cl_history_read_catalog(struct cs_store *store, struct cl_history *out,
                                               enum cs_store_status resume_status[2])
{
    struct cl_history *h = calloc(1U, sizeof(*h));
    enum cl_history_result result;
    size_t pointer, i;
    if (h == NULL || out == NULL || resume_status == NULL) {
        free(h);
        return CL_HISTORY_IO;
    }
    resume_status[0] = CS_STORE_IO;
    resume_status[1] = CS_STORE_IO;
    result = cl_history_read(store, h);
    if (result != CL_HISTORY_OK && result != CL_HISTORY_EMPTY) {
        free(h);
        return result;
    }
    for (pointer = 0U; pointer < 2U; ++pointer) {
        struct cs_record record;
        resume_status[pointer] = cs_store_read_resume(store, pointer == 1U, &record);
        if (resume_status[pointer] != CS_STORE_OK) {
            continue;
        }
        for (i = 0U; i < h->count; ++i) {
            struct cl_entry *e = &h->entries[i];
            struct cl_reference reference;
            if (!cs_identity_equal(&e->identity, &record.identity)) {
                continue;
            }
            reference.generation = record.generation;
            strcpy(reference.manifest, record.manifest);
            if (reference.generation > e->latest.generation) {
                e->previous = e->latest;
                e->latest = reference;
            } else if (reference.generation < e->latest.generation &&
                       reference.generation > e->previous.generation) {
                e->previous = reference;
            }
            /* Disk integrity alone does not prove compatibility with today's
             * core/config/BIOS. Leave its marker unknown until a qualified probe. */
            break;
        }
    }
    if (cs_store_check_media(store) != CS_STORE_OK) {
        free(h);
        return CL_HISTORY_MEDIA;
    }
    *out = *h;
    free(h);
    return result;
}
static bool write_all(int fd, const char *data, size_t length)
{
    while (length != 0U) {
        ssize_t n = write(fd, data, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += (size_t)n;
        length -= (size_t)n;
    }
    return true;
}
static int sync_file(int fd)
{
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result;
}
static int sync_directory(struct cs_store *store)
{
    return cs_flush_directory(store->data_fd, store->fat, store->qualified_fat_syncfs);
}
static enum cl_history_result publish(struct cs_store *store, const struct cl_history *h)
{
    char temp[96], line[ENTRY_BYTES], digest[65];
    int fd, n;
    size_t i;
    bool ok;
    if (!history_valid(h) || !history_digest(h, digest) || store->temp_counter == UINT64_MAX ||
        !cs_hex_valid(store->instance, 32U)) {
        return CL_HISTORY_CORRUPT;
    }
    (void)snprintf(temp, sizeof(temp), ".history-%s-%" PRIu64, store->instance,
                   ++store->temp_counter);
    fd = openat(store->data_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return CL_HISTORY_IO;
    }
    n = snprintf(line, sizeof(line),
                 "{\"schema\":1,\"revision\":\"%" PRIu64 "\",\"highwater\":\"%" PRIu64
                 "\",\"count\":%zu,\"checksum\":\"%s\"}\n",
                 h->revision, h->highwater, h->count, digest);
    ok = n > 0 && (size_t)n < sizeof(line) && write_all(fd, line, (size_t)n);
    for (i = 0U; ok && i < h->count; ++i) {
        n = entry_encode(&h->entries[i], line);
        ok = n > 0 && (size_t)n < sizeof(line) && write_all(fd, line, (size_t)n);
    }
    if (ok && sync_file(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        return CL_HISTORY_IO; /* Failed temp retained; never interpreted as history. */
    }
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return CL_HISTORY_MEDIA;
    }
    if (sync_directory(store) != 0 || renameat(store->data_fd, temp, store->data_fd, "history.jsonl") != 0) {
        return CL_HISTORY_IO;
    }
    if (sync_directory(store) != 0) {
        store->poisoned = true; /* Publication happened, durability is uncertain. */
        return CL_HISTORY_IO;
    }
    return cs_store_check_media(store) == CS_STORE_OK ? CL_HISTORY_OK : CL_HISTORY_MEDIA;
}
static int lock_history(const struct cs_store *store)
{
    int lock = openat(store->data_fd, ".history.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return -1;
    struct stat st;
    if (fstat(lock, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        st.st_uid == geteuid() && st.st_dev == store->device && (st.st_mode & 0022U) == 0U &&
        flock(lock, LOCK_EX | LOCK_NB) == 0)
        return lock;
    close(lock);
    return -1;
}

enum mutation { RECORD_SUCCESS, REMOVE_HISTORY, UPDATE_CHECKPOINT };
static enum cl_history_result mutate(struct cs_store *store, enum mutation operation,
                                     const char *game, const struct cl_entry *entry,
                                     const struct cl_reference *latest,
                                     const struct cl_reference *previous, struct cl_history *out)
{
    struct cl_history *h;
    enum cl_history_result result;
    int lock;
    size_t i;
    if (store->read_only || !cs_hex_valid(game, 64U) ||
        cs_store_check_media(store) != CS_STORE_OK) {
        return CL_HISTORY_MEDIA;
    }
    lock = lock_history(store);
    if (lock < 0) return CL_HISTORY_BUSY;
    h = calloc(1U, sizeof(*h));
    result = h == NULL ? CL_HISTORY_IO : cl_history_read(store, h);
    if (result == CL_HISTORY_EMPTY) {
        result = CL_HISTORY_OK;
    }
    if (result != CL_HISTORY_OK) {
        goto done;
    }
    if (h->revision == UINT64_MAX || (operation == RECORD_SUCCESS && h->highwater == UINT64_MAX)) {
        result = CL_HISTORY_CORRUPT;
        goto done;
    }
    for (i = 0U; i < h->count; ++i) {
        if (strcmp(h->entries[i].identity.game, game) == 0) {
            break;
        }
    }
    if (operation == UPDATE_CHECKPOINT) {
        if (i == h->count) {
            result = CL_HISTORY_EMPTY;
            goto done;
        }
        h->entries[i].latest = *latest;
        h->entries[i].previous = *previous;
    } else {
        if (i < h->count) {
            memmove(&h->entries[i], &h->entries[i + 1U],
                    (h->count - i - 1U) * sizeof(h->entries[0]));
            --h->count;
        }
        if (operation == RECORD_SUCCESS) {
            if (h->count == CL_HISTORY_MAX) {
                --h->count; /* Eviction removes history only, never generations. */
            }
            memmove(&h->entries[1], &h->entries[0], h->count * sizeof(h->entries[0]));
            h->entries[0] = *entry;
            h->entries[0].play_sequence = ++h->highwater;
            ++h->count;
        }
    }
    ++h->revision;
    result = publish(store, h);
done:
    if (close(lock) != 0) {
        result = CL_HISTORY_IO;
    }
    if (result == CL_HISTORY_OK && out != NULL) {
        *out = *h;
    }
    free(h);
    return result;
}
enum cl_history_result cl_history_record_success(struct cs_store *store,
                                                 const struct cl_entry *entry,
                                                 struct cl_history *out)
{
    struct cl_entry checked;
    if (entry == NULL) {
        return CL_HISTORY_CORRUPT;
    }
    checked = *entry;
    checked.play_sequence = 1U;
    if (!entry_valid(&checked)) {
        return CL_HISTORY_CORRUPT;
    }
    return mutate(store, RECORD_SUCCESS, entry->identity.game, entry, NULL, NULL, out);
}
enum cl_history_result cl_history_remove(struct cs_store *store, const char game[65],
                                         struct cl_history *out)
{
    return mutate(store, REMOVE_HISTORY, game, NULL, NULL, NULL, out);
}
enum cl_history_result cl_history_regenerate(struct cs_store *store,
                                             const struct cl_entry *entries, size_t count)
{
    if (entries == NULL || count == 0U || count > CL_HISTORY_MAX) return CL_HISTORY_EMPTY;
    if (cs_store_check_media(store) != CS_STORE_OK) return CL_HISTORY_MEDIA;
    int lock = lock_history(store);
    if (lock < 0) return CL_HISTORY_BUSY;
    struct cl_history *old = calloc(1U, sizeof(*old));
    struct cl_history *next = calloc(1U, sizeof(*next));
    enum cl_history_result result = CL_HISTORY_IO;
    if (old == NULL || next == NULL) goto done;
    result = cl_history_read(store, old);
    if (result != CL_HISTORY_OK && result != CL_HISTORY_EMPTY && result != CL_HISTORY_CORRUPT)
        goto done;
    if (result == CL_HISTORY_OK) {
        next->revision = old->revision;
        next->highwater = old->highwater;
    }
    if (UINT64_MAX - next->revision < count || UINT64_MAX - next->highwater < count) {
        result = CL_HISTORY_CORRUPT;
        goto done;
    }
    next->revision += count;
    next->highwater += count;
    next->count = count;
    for (size_t i = 0; i < count; ++i) {
        struct cl_entry *entry = &next->entries[i];
        *entry = entries[i];
        entry->play_sequence = next->highwater - i;
        for (size_t j = 0; j < old->count; ++j) {
            const struct cs_identity *saved=&old->entries[j].identity;
            if(strcmp(entry->identity.game,saved->game)||strcmp(entry->identity.core,saved->core)||
               strcmp(entry->identity.content,saved->content))continue;
            /* Regeneration changes presentation/order, not the identity of
             * existing committed states. Native migration validates those
             * original records instead of relabeling their artifacts. */
            entry->identity=*saved;
            entry->latest = old->entries[j].latest;
            entry->previous = old->entries[j].previous;
            break;
        }
    }
    struct cs_store publication = *store;
    unsigned char nonce[16];
    if (getrandom(nonce, sizeof(nonce), 0) != (ssize_t)sizeof(nonce)) {
        result = CL_HISTORY_IO;
        goto done;
    }
    for (size_t i = 0; i < sizeof(nonce); ++i)
        (void)snprintf(publication.instance + i * 2U, 3U, "%02x", (unsigned int)nonce[i]);
    result = publish(&publication, next);
done:
    if (close(lock) != 0) result = CL_HISTORY_IO;
    free(next);
    free(old);
    return result;
}
enum cl_history_result cl_history_set_checkpoint(struct cs_store *store, const char game[65],
                                                 const struct cl_reference *latest,
                                                 const struct cl_reference *previous,
                                                 struct cl_history *out)
{
    if (!ref_valid(latest) || !ref_valid(previous)) {
        return CL_HISTORY_CORRUPT;
    }
    return mutate(store, UPDATE_CHECKPOINT, game, NULL, latest, previous, out);
}
void cl_history_restore_position(const struct cl_history *h, struct cl_position *p)
{
    size_t i;
    if (h->count == 0U) {
        memset(p, 0, sizeof(*p));
        return;
    }
    for (i = 0U; i < h->count; ++i) {
        if (strcmp(p->game, h->entries[i].identity.game) == 0) {
            p->selected = i;
            break;
        }
    }
    if (p->selected >= h->count) {
        p->selected = h->count - 1U;
    }
    if (p->first > p->selected) {
        p->first = p->selected;
    }
    if (p->selected - p->first >= CL_VISIBLE_ROWS) {
        p->first = p->selected - CL_VISIBLE_ROWS + 1U;
    }
    if (p->first > (h->count > CL_VISIBLE_ROWS ? h->count - CL_VISIBLE_ROWS : 0U)) {
        p->first = h->count > CL_VISIBLE_ROWS ? h->count - CL_VISIBLE_ROWS : 0U;
    }
    strcpy(p->game, h->entries[p->selected].identity.game);
}
void cl_history_move(const struct cl_history *h, struct cl_position *p, int delta)
{
    int selected;
    if (h->count == 0U || h->count > CL_HISTORY_MAX) {
        return;
    }
    cl_history_restore_position(h, p);
    if (delta > (int)CL_HISTORY_MAX) {
        delta = (int)CL_HISTORY_MAX;
    }
    if (delta < -(int)CL_HISTORY_MAX) {
        delta = -(int)CL_HISTORY_MAX;
    }
    if (delta >= (int)CL_VISIBLE_ROWS || delta <= -(int)CL_VISIBLE_ROWS) {
        int first = (int)p->first + delta;
        p->first = first < 0 ? 0U : (size_t)first;
    }
    selected = (int)p->selected + delta;
    if (selected < 0) {
        selected = 0;
    }
    if ((size_t)selected >= h->count) {
        selected = (int)h->count - 1;
    }
    p->selected = (size_t)selected;
    p->game[0] = '\0';
    cl_history_restore_position(h, p);
}
enum cl_availability cl_history_probe(struct cs_store *store, const struct cl_entry *entry,
                                      const struct cs_identity *qualified,
                                      const struct cl_reference *reference)
{
    struct cs_record record;
    enum cs_store_status status;
    if (entry == NULL || qualified == NULL || reference == NULL || !cs_identity_valid(qualified) ||
        !cs_identity_equal(&entry->identity, qualified)) {
        return CL_INCOMPATIBLE;
    }
    if (reference->generation == 0U) {
        return CL_MISSING;
    }
    status = cs_store_read_checkpoint(store, qualified, reference->generation, reference->manifest,
                                      &record);
    if (status == CS_STORE_OK) {
        return CL_READY;
    }
    if (status == CS_STORE_MEDIA || status == CS_STORE_EMPTY) {
        return CL_MISSING;
    }
    return status == CS_STORE_INCOMPATIBLE ? CL_INCOMPATIBLE : CL_DAMAGED;
}
