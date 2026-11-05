#include "launcher/qualification.h"
#include "sessiond/document.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static const char *const controls[] = {"up",   "down",        "left",     "right", "a",
                                       "b",    "x",           "y",        "l1",    "r1",
                                       "menu", "volume_down", "volume_up", "start", "select"};
_Static_assert(sizeof(controls) / sizeof(controls[0]) == CL_CONTROL_COUNT,
               "qualification controls");
static bool same(const struct cl_input_profile *a, const struct cl_input_profile *b)
{
    size_t i;
    if (memcmp(a->guid, b->guid, sizeof(a->guid)) != 0 || a->hat != b->hat) {
        return false;
    }
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        if (a->button[i] != b->button[i]) {
            return false;
        }
    }
    return true;
}
static bool valid(const struct cl_input_profile *profile)
{
    size_t i, j;
    if (strnlen(profile->guid, sizeof(profile->guid)) != 32U || !cs_hex_valid(profile->guid, 32U) ||
        strspn(profile->guid, "0") == 32U || profile->hat < -1 || profile->hat > 255) {
        return false;
    }
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        if (profile->button[i] < -1 || profile->button[i] > 255 ||
            (i >= CL_A && i <= CL_Y && profile->button[i] < 0) ||
            (i < CL_A && profile->button[i] < 0 && profile->hat < 0)) {
            return false;
        }
        for (j = 0U; j < i; ++j) {
            if (profile->button[i] >= 0 && profile->button[i] == profile->button[j]) {
                return false;
            }
        }
    }
    return true;
}
bool cl_input_verification_valid(const struct cl_input_profile *current,
                                 const struct cl_input_verification *v)
{
    struct cl_input_profile swapped;
    int button;
    if (current == NULL || v == NULL || !valid(current) || !valid(&v->baseline) ||
        !valid(&v->checked) || !same(current, &v->baseline)) {
        return false;
    }
    if (same(current, &v->checked)) {
        return true;
    }
    swapped = *current;
    button = swapped.button[CL_A];
    swapped.button[CL_A] = swapped.button[CL_B];
    swapped.button[CL_B] = button;
    button = swapped.button[CL_X];
    swapped.button[CL_X] = swapped.button[CL_Y];
    swapped.button[CL_Y] = button;
    return same(&swapped, &v->checked);
}
static bool append_profile(char *out, size_t capacity, size_t *used,
                           const struct cl_input_profile *p, size_t count)
{
    size_t i;
    int n = snprintf(out + *used, capacity - *used, "{\"hat\":%d", p->hat);
    if (n < 0 || (size_t)n >= capacity - *used) {
        return false;
    }
    *used += (size_t)n;
    for (i = 0U; i < count; ++i) {
        n = snprintf(out + *used, capacity - *used, ",\"%s\":%d", controls[i], p->button[i]);
        if (n < 0 || (size_t)n >= capacity - *used) {
            return false;
        }
        *used += (size_t)n;
    }
    if (*used + 2U > capacity) {
        return false;
    }
    out[(*used)++] = '}';
    out[*used] = '\0';
    return true;
}
static bool body(const char *uuid, const struct cl_input_verification *v,
                 unsigned int schema, char out[CL_INPUT_RECORD_MAX], size_t *used)
{
    int n;
    size_t count = schema == 1U ? (size_t)CL_START : (size_t)CL_CONTROL_COUNT;
    if ((schema != 1U && schema != 2U) || uuid == NULL || (strlen(uuid) != 9U && strlen(uuid) != 36U) ||
        strspn(uuid, "0123456789abcdefABCDEF-") != strlen(uuid) ||
        !cl_input_verification_valid(&v->baseline, v)) {
        return false;
    }
    n = snprintf(out, CL_INPUT_RECORD_MAX,
                 "{\"schema\":%u,\"coverage\":\"navigation-8\",\"card_uuid\":\"%s\",\"guid\":\"%s\","
                 "\"baseline\":",
                 schema, uuid, v->baseline.guid);
    if (n < 0 || (size_t)n >= CL_INPUT_RECORD_MAX) {
        return false;
    }
    *used = (size_t)n;
    if (!append_profile(out, CL_INPUT_RECORD_MAX, used, &v->baseline, count) ||
        *used + 13U >= CL_INPUT_RECORD_MAX) {
        return false;
    }
    memcpy(out + *used, ",\"verified\":", 12U);
    *used += 12U;
    if (!append_profile(out, CL_INPUT_RECORD_MAX, used, &v->checked, count) ||
        *used + 2U >= CL_INPUT_RECORD_MAX) {
        return false;
    }
    out[(*used)++] = '}';
    out[*used] = '\0';
    return true;
}
bool cl_input_record_encode(const char *uuid, const struct cl_input_verification *v,
                            char out[CL_INPUT_RECORD_MAX], size_t *bytes)
{
    char canonical[CL_INPUT_RECORD_MAX], checksum[65];
    size_t used;
    int n;
    if (out == NULL || bytes == NULL || v == NULL || !body(uuid, v, 2U, canonical, &used)) {
        return false;
    }
    cs_sha256_hex(canonical, used, checksum);
    n = snprintf(out, CL_INPUT_RECORD_MAX, "%.*s,\"checksum\":\"%s\"}\n", (int)(used - 1U),
                 canonical, checksum);
    if (n < 0 || (size_t)n >= CL_INPUT_RECORD_MAX) {
        return false;
    }
    *bytes = (size_t)n;
    return true;
}
static bool keys(const struct cs_document *d, size_t object, const char *const *names, size_t count)
{
    uint32_t seen = 0U;
    size_t t, i;
    if (object >= d->count || d->tokens[object].type != CS_DOC_OBJECT || count > 31U) {
        return false;
    }
    for (t = object + 1U; t < d->tokens[object].next; t = d->tokens[t + 1U].next) {
        char key[40];
        bool found = false;
        if (!cs_doc_text(d, t, key, sizeof(key))) {
            return false;
        }
        for (i = 0U; i < count; ++i) {
            if (strcmp(key, names[i]) == 0) {
                uint32_t bit = UINT32_C(1) << i;
                if ((seen & bit) != 0U) {
                    return false;
                }
                seen |= bit;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return seen == ((UINT32_C(1) << count) - 1U);
}
static bool read_profile(const struct cs_document *d, size_t object,
                         struct cl_input_profile *profile, size_t count)
{
    const char *names[CL_CONTROL_COUNT + 1U];
    size_t i;
    names[0] = "hat";
    for (i = 0U; i < count; ++i) {
        names[i + 1U] = controls[i];
    }
    if (!keys(d, object, names, count + 1U) ||
        !cs_doc_int(d, cs_doc_get(d, object, "hat"), &profile->hat)) {
        return false;
    }
    for (i = 0U; i < count; ++i) {
        if (!cs_doc_int(d, cs_doc_get(d, object, controls[i]), &profile->button[i])) {
            return false;
        }
    }
    return true;
}
bool cl_input_record_decode(const char *input, size_t bytes, const char *uuid,
                            const struct cl_input_profile *current,
                            struct cl_input_verification *out)
{
    static const char *const names[] = {"schema",   "coverage", "card_uuid", "guid",
                                        "baseline", "verified", "checksum"};
    struct cs_document *d;
    struct cl_input_verification candidate;
    char card[65], guid[33], coverage[24], checksum[65], canonical[CL_INPUT_RECORD_MAX],
        expected[65];
    size_t used;
    int schema;
    bool ok = false;
    if (input == NULL || out == NULL || current == NULL || uuid == NULL || bytes == 0U ||
        bytes >= CL_INPUT_RECORD_MAX) {
        return false;
    }
    d = calloc(1U, sizeof(*d));
    if (d == NULL) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.baseline = candidate.checked = *current;
    if (!cs_doc_parse(d, input, bytes) || !keys(d, 0U, names, 7U) ||
        !cs_doc_int(d, cs_doc_get(d, 0U, "schema"), &schema) || (schema != 1 && schema != 2) ||
        !cs_doc_text(d, cs_doc_get(d, 0U, "coverage"), coverage, sizeof(coverage)) ||
        strcmp(coverage, "navigation-8") != 0 ||
        !cs_doc_text(d, cs_doc_get(d, 0U, "card_uuid"), card, sizeof(card)) ||
        strcasecmp(card, uuid) != 0 ||
        !cs_doc_text(d, cs_doc_get(d, 0U, "guid"), guid, sizeof(guid)) ||
        !cs_doc_text(d, cs_doc_get(d, 0U, "checksum"), checksum, sizeof(checksum)) ||
        !cs_hex_valid(checksum, 64U)) {
        goto done;
    }
    strcpy(candidate.baseline.guid, guid);
    strcpy(candidate.checked.guid, guid);
    if (!read_profile(d, cs_doc_get(d, 0U, "baseline"), &candidate.baseline, schema == 1 ? (size_t)CL_START : (size_t)CL_CONTROL_COUNT) ||
        !read_profile(d, cs_doc_get(d, 0U, "verified"), &candidate.checked, schema == 1 ? (size_t)CL_START : (size_t)CL_CONTROL_COUNT) ||
        !cl_input_verification_valid(current, &candidate) ||
        !body(card, &candidate, (unsigned int)schema, canonical, &used)) {
        goto done;
    }
    cs_sha256_hex(canonical, used, expected);
    ok = strcmp(expected, checksum) == 0;
    if (ok) {
        *out = candidate;
    }
done:
    free(d);
    return ok;
}
static int profile_directory(struct cs_store *store)
{
    int saves = -1, directory = -1;
    struct stat st;
    if (cs_store_check_media(store) != CS_STORE_OK) {
        return -1;
    }
    saves = openat(store->card_fd, "Saves", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (saves < 0 || fstat(saves, &st) != 0 || st.st_dev != store->device) {
        goto done;
    }
    directory = openat(saves, "CurrentProfile", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory >= 0 && (fstat(directory, &st) != 0 || st.st_dev != store->device)) {
        (void)close(directory);
        directory = -1;
    }
done:
    if (saves >= 0 && close(saves) != 0) {
        if (directory >= 0) {
            (void)close(directory);
        }
        directory = -1;
    }
    return directory;
}
enum cs_store_status cl_input_record_load(struct cs_store *store,
                                          const struct cl_input_profile *current,
                                          struct cl_input_verification *out)
{
    int directory = -1, fd = -1;
    struct stat st;
    struct cl_input_verification candidate;
    char bytes[CL_INPUT_RECORD_MAX];
    size_t used = 0U;
    enum cs_store_status result = CS_STORE_IO;
    if (store == NULL || current == NULL || out == NULL) {
        return CS_STORE_INCOMPATIBLE;
    }
    directory = profile_directory(store);
    if (directory < 0) {
        return CS_STORE_MEDIA;
    }
    fd = openat(directory, "controller.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        result = errno == ENOENT ? CS_STORE_EMPTY : CS_STORE_IO;
        goto done;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_size < 1 ||
        st.st_size >= (off_t)sizeof(bytes)) {
        result = CS_STORE_CORRUPT;
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
    result = cl_input_record_decode(bytes, used, store->uuid, current, &candidate)
                 ? cs_store_check_media(store)
                 : CS_STORE_INCOMPATIBLE;
done:
    if (fd >= 0 && close(fd) != 0) {
        result = CS_STORE_IO;
    }
    if (close(directory) != 0) {
        result = CS_STORE_IO;
    }
    if (result == CS_STORE_OK) {
        *out = candidate;
    }
    return result;
}
enum cs_store_status cl_input_record_save(struct cs_store *store,
                                          const struct cl_input_verification *v)
{
    char bytes[CL_INPUT_RECORD_MAX], temporary[96];
    size_t size, used = 0U;
    int directory = -1, fd = -1;
    unsigned int attempt;
    bool renamed = false, created = false;
    struct timespec now;
    enum cs_store_status result = CS_STORE_IO;
    uint64_t ticket;
    if (store == NULL || !cl_input_record_encode(store->uuid, v, bytes, &size)) {
        return CS_STORE_INCOMPATIBLE;
    }
    directory = profile_directory(store);
    if (directory < 0) {
        return CS_STORE_MEDIA;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        goto done;
    }
    ticket = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    for (attempt = 0U; attempt < 8U; ++attempt) {
        int n = snprintf(temporary, sizeof(temporary), ".controller.%ld.%016" PRIx64 ".tmp",
                         (long)getpid(), ticket + attempt);
        if (n < 0 || n >= (int)sizeof(temporary)) {
            goto done;
        }
        fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0) {
            created = true;
            break;
        }
        if (errno != EEXIST) {
            goto done;
        }
    }
    if (fd < 0) {
        goto done;
    }
    while (used < size) {
        ssize_t n = write(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto done;
        }
        used += (size_t)n;
    }
    if (fsync(fd) != 0) {
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (cs_store_check_media(store) != CS_STORE_OK) {
        result = CS_STORE_MEDIA;
        goto done;
    }
    if (renameat(directory, temporary, directory, "controller.json") != 0) {
        goto done;
    }
    renamed = true;
    if (fsync(directory) != 0 &&
        !(store->fat && store->qualified_fat_syncfs && (errno == EINVAL || errno == EOPNOTSUPP) &&
          syncfs(directory) == 0)) {
        goto done;
    }
    result = cs_store_check_media(store);
done:
    if (fd >= 0) {
        (void)close(fd);
    }
    if (directory >= 0) {
        if (!renamed && created) {
            (void)unlinkat(directory, temporary, 0);
        }
        if (close(directory) != 0) {
            result = CS_STORE_IO;
        }
    }
    return result;
}

bool cl_input_runtime_publish(const struct cl_input_verification *verification)
{
    char bytes[1024], name[96];
    size_t used, i;
    int directory = -1, fd = -1, n;
    struct stat st;
    struct timespec now;
    bool ok = false, created = false, renamed = false;
    unsigned int attempt;
    uint64_t ticket;
    if (verification == NULL ||
        !cl_input_verification_valid(&verification->baseline, verification)) {
        return false;
    }
    directory = open("/run/console/config", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0 || fstat(directory, &st) != 0 || st.st_uid != 0U ||
        (st.st_mode & 0022U) != 0U) {
        goto done;
    }
    n = snprintf(bytes, sizeof(bytes),
                 "{\"schema\":2,\"coverage\":\"navigation-8\",\"guid\":\"%s\",\"hat\":%u",
                 verification->checked.guid,
                 verification->checked.hat < 0 ? 65535U : (unsigned int)verification->checked.hat);
    if (n < 0 || (size_t)n >= sizeof(bytes)) {
        goto done;
    }
    used = (size_t)n;
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        n = snprintf(bytes + used, sizeof(bytes) - used, ",\"%s\":%u", controls[i],
                     verification->checked.button[i] < 0
                         ? 65535U
                         : (unsigned int)verification->checked.button[i]);
        if (n < 0 || (size_t)n >= sizeof(bytes) - used) {
            goto done;
        }
        used += (size_t)n;
    }
    if (used + 3U >= sizeof(bytes)) {
        goto done;
    }
    memcpy(bytes + used, "}\n", 2U);
    used += 2U;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        goto done;
    }
    ticket = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    for (attempt = 0U; attempt < 8U; ++attempt) {
        n = snprintf(name, sizeof(name), ".verified-input.%ld.%016" PRIx64 ".tmp", (long)getpid(),
                     ticket + attempt);
        if (n < 0 || n >= (int)sizeof(name)) {
            goto done;
        }
        fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            created = true;
            break;
        }
        if (errno != EEXIST) {
            goto done;
        }
    }
    if (fd < 0) {
        goto done;
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
    if (renameat(directory, name, directory, "verified-input.json") != 0) {
        goto done;
    }
    renamed = true;
    ok = fsync(directory) == 0;
done:
    if (fd >= 0) {
        (void)close(fd);
    }
    if (directory >= 0) {
        if (created && !renamed) {
            (void)unlinkat(directory, name, 0);
        }
        if (close(directory) != 0) {
            ok = false;
        }
    }
    return ok;
}
