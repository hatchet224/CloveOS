#include "sessiond/durability.h"
#include "supervisor/packages.h"
#include "sessiond/json.h"
#include "sessiond/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#define CURRENT "packages.json"
#define PREVIOUS "packages.previous.json"
#define RECORD_MAX 512U
#define R34_PACKAGE_BITS ((UINT64_C(1) << 19U) - 1U)
#define R35_PACKAGE_BITS ((UINT64_C(1) << 26U) - 1U)
#define R38_PACKAGE_BITS ((UINT64_C(1) << 28U) - 1U)
#define R44_PACKAGE_BITS ((UINT64_C(1) << 29U) - 1U)
#define R46_PACKAGE_BITS ((UINT64_C(1) << 30U) - 1U)
static const char r34_registry[] = "83311cd29369da032bc5258803b8ed1598add524fb2b48e553a198bf053f4991";
static const char r35_registry[] = "059dd34ce8c728b8990743c235370a824d3f39da04f2a84077b92de64adc6905";
static const char r36_registry[] = "9c70b5fb9fc32bb053d3160f020d093c285fd1734f45efa90004cff9562765d7";
static const char r38_registry[] = "9df83a507459215a1a9d94ee2e62ef276ab690103c6d5560f87263018a573d5a";
static const char r45_registry[] = "448e3c14ac3ed7466da03682318ca5c49622997476f03f08ecd20a02cafcdb65";
static const char r46_registry[] = "a5b0e1fb23f75a6658f5da3a0d8a24ddd4bbc5725c7633926c8697fe554b09f0";
/* These releases kept the same package bit positions. Artifact hashes changed. */
static const char r39_to_r44_registries[][65] = {
    "1802eb979f1499d5360706e5b3ee2e5b7968fc263215bc783e903a14b3d3eff6",
    "6e366a247e6f89e2431e46a60c30d97ce5e7629c4f4cff1b9caf2bc3cf3ce838",
    "12fdb7df57b95f07f7483ac52c9d41ff03438dd64f5225a33980c46cba4ab89e",
    "fd1b8a34fd8e17acffc6b28170fbd252aff400d52bbf100f23e927d4b58425f5",
    "8482543e12611382aff08cc3e874fc10162f8cd26ee93a21f17284985c024af4",
    "05262b1a7effa570200f40601a9b6ea4025f40283539c1e9453535dddd341e36"
};
#ifdef SP_PACKAGE_TESTING
static unsigned int injected_fault;
void sp_package_test_fault(unsigned int stage) { injected_fault = stage; }
static bool fault(unsigned int stage) { return injected_fault == stage; }
#else
static bool fault(unsigned int stage) { (void)stage; return false; }
#endif
static enum sp_package_status media(const struct cs_store *v)
{
    return v != NULL && v->data_fd >= 0 && cs_store_check_media(v) == CS_STORE_OK ?
           SP_PACKAGE_OK : SP_PACKAGE_MEDIA;
}
static bool directory_sync(const struct cs_store *v)
{
    return cs_flush_directory(v->data_fd, v->fat, v->qualified_fat_syncfs) == 0;
}
static void record_digest(const char *fingerprint,
                          const struct sp_package_selection *s, char out[65])
{
    char canonical[192];
    int n = snprintf(canonical, sizeof(canonical), "native-package-selection-v1\n%" PRIu64 "\n%s\n%016" PRIx64 "\n",
                     s->revision, fingerprint, s->enabled);
    if (n < 0 || (size_t)n >= sizeof(canonical)) { out[0] = '\0'; return; }
    cs_sha256_hex(canonical, (size_t)n, out);
}
static enum sp_package_status read_record(const struct cs_store *v, const char *name,
                                          const struct sp_package_registry *r,
                                          struct sp_package_selection *out)
{
    static const char *const keys[] = {"schema", "revision", "registry", "enabled", "digest"};
    char bytes[RECORD_MAX + 1U], digest[65];
    const char *enabled, *registry, *checksum;
    struct cs_json json;
    struct sp_package_selection parsed = {0};
    struct stat st;
    uint64_t version;
    size_t used = 0U, i;
    enum sp_package_status status = SP_PACKAGE_CORRUPT;
    int fd = openat(v->data_fd, name, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? SP_PACKAGE_EMPTY :
                      (errno == ELOOP ? SP_PACKAGE_CORRUPT : SP_PACKAGE_IO);
    if (fstat(fd, &st) != 0) { status = SP_PACKAGE_IO; goto done; }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_dev != v->device ||
        st.st_uid != 0U || (st.st_mode & 0077U) != 0U || st.st_size < 1 ||
        st.st_size > (off_t)RECORD_MAX) goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { status = SP_PACKAGE_IO; goto done; }
        used += (size_t)n;
    }
    if (!cs_json_parse(&json, bytes, used) || !cs_json_keys(&json, keys, 5U) ||
        !cs_json_u64(&json, "schema", &version) || version != 1U ||
        !cs_json_u64(&json, "revision", &parsed.revision)) goto done;
    registry = cs_json_string(&json, "registry");
    enabled = cs_json_string(&json, "enabled"); checksum = cs_json_string(&json, "digest");
    if (registry == NULL || enabled == NULL || checksum == NULL ||
        !cs_hex_valid(registry, 64U) || !cs_hex_valid(enabled, 16U) ||
        !cs_hex_valid(checksum, 64U)) goto done;
    uint64_t previous_bits = 0U;
    if (strcmp(registry, r->fingerprint) != 0) {
        if (strcmp(registry, r45_registry) == 0 || strcmp(registry, r46_registry) == 0)
            previous_bits = R46_PACKAGE_BITS;
        else if (strcmp(registry, r38_registry) == 0) previous_bits = R38_PACKAGE_BITS;
        else if (strcmp(registry, r36_registry) == 0 || strcmp(registry, r35_registry) == 0)
            previous_bits = R35_PACKAGE_BITS;
        else if (strcmp(registry, r34_registry) == 0) previous_bits = R34_PACKAGE_BITS;
        else {
            for (i = 0U; i < sizeof(r39_to_r44_registries) / sizeof(r39_to_r44_registries[0]); ++i)
                if (strcmp(registry, r39_to_r44_registries[i]) == 0) { previous_bits = R44_PACKAGE_BITS; break; }
            if (previous_bits == 0U) {
                status = SP_PACKAGE_INCOMPATIBLE;
                goto done;
            }
        }
    }
    for (i = 0U; i < 16U; ++i) {
        unsigned int digit = enabled[i] >= '0' && enabled[i] <= '9' ?
                             (unsigned int)(enabled[i] - '0') : (unsigned int)(enabled[i] - 'a') + 10U;
        parsed.enabled = (parsed.enabled << 4U) | digit;
    }
    record_digest(registry, &parsed, digest);
    if (strcmp(digest, checksum) != 0) goto done;
    if (previous_bits != 0U) {
        if ((parsed.enabled & ~previous_bits) != 0U) goto done;
        parsed.enabled |= sp_package_defaults(r) & ~previous_bits;
    }
    if (!sp_package_selection_valid(r, parsed.enabled)) goto done;
    parsed.initialized = true; *out = parsed; status = SP_PACKAGE_OK;
done:
    if (close(fd) != 0) status = SP_PACKAGE_IO;
    return status;
}
static enum sp_package_status load_internal(const struct cs_store *v,
                                           const struct sp_package_registry *r,
                                           struct sp_package_selection *out)
{
    struct sp_package_selection current = {0}, previous = {0};
    enum sp_package_status a, b;
    a = read_record(v, CURRENT, r, &current);
    if (a == SP_PACKAGE_OK) { *out = current; return SP_PACKAGE_OK; }
    if (a == SP_PACKAGE_IO) return a;
    b = read_record(v, PREVIOUS, r, &previous);
    if (b == SP_PACKAGE_OK) { previous.recovered = true; *out = previous; return SP_PACKAGE_RECOVERED; }
    if (a == SP_PACKAGE_EMPTY && b == SP_PACKAGE_EMPTY) {
        current.enabled = sp_package_defaults(r);
        if (!sp_package_selection_valid(r, current.enabled)) return SP_PACKAGE_UNAVAILABLE;
        *out = current; return SP_PACKAGE_EMPTY;
    }
    if (a == SP_PACKAGE_INCOMPATIBLE || b == SP_PACKAGE_INCOMPATIBLE) return SP_PACKAGE_INCOMPATIBLE;
    return b == SP_PACKAGE_IO ? b : SP_PACKAGE_CORRUPT;
}
enum sp_package_status sp_package_load(const struct cs_store *v,
                                       const struct sp_package_registry *r,
                                       struct sp_package_selection *out)
{
    struct sp_package_selection parsed;
    enum sp_package_status status;
    if (out == NULL || r == NULL || !cs_hex_valid(r->fingerprint, 64U)) return SP_PACKAGE_INCOMPATIBLE;
    if (media(v) != SP_PACKAGE_OK) return SP_PACKAGE_MEDIA;
    status = load_internal(v, r, &parsed);
    if (media(v) != SP_PACKAGE_OK) return SP_PACKAGE_MEDIA;
    if (status == SP_PACKAGE_OK || status == SP_PACKAGE_EMPTY || status == SP_PACKAGE_RECOVERED) *out = parsed;
    return status;
}
static enum sp_package_status write_record(const struct cs_store *v, const char *name,
                                           const struct sp_package_registry *r,
                                           const struct sp_package_selection *s, bool current)
{
    char bytes[RECORD_MAX], temp[96], digest[65];
    struct timespec clock;
    size_t used;
    unsigned int attempt;
    bool renamed = false;
    int fd = -1, n;
    enum sp_package_status status = SP_PACKAGE_IO;
    if (clock_gettime(CLOCK_MONOTONIC, &clock) != 0) return status;
    record_digest(r->fingerprint, s, digest);
    n = snprintf(bytes, sizeof(bytes), "{\"schema\":1,\"revision\":%" PRIu64 ",\"registry\":\"%s\",\"enabled\":\"%016" PRIx64 "\",\"digest\":\"%s\"}\n",
                 s->revision, r->fingerprint, s->enabled, digest);
    if (n < 0 || (size_t)n >= sizeof(bytes)) return status;
    used = (size_t)n;
    for (attempt = 0U; attempt < 8U; ++attempt) {
        n = snprintf(temp, sizeof(temp), ".packages.%ld.%ld.%u.tmp", (long)getpid(), clock.tv_nsec, attempt);
        if (n < 0 || (size_t)n >= sizeof(temp)) return status;
        fd = openat(v->data_fd, temp, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return status;
    for (size_t i = 0U; i < used;) {
        ssize_t nwrite = write(fd, bytes + i, used - i);
        if (nwrite < 0 && errno == EINTR) continue;
        if (nwrite <= 0) goto done;
        i += (size_t)nwrite;
    }
    if (fsync(fd) != 0 || !directory_sync(v)) goto done;
    if (close(fd) != 0) { fd = -1; goto done; }
    fd = -1;
    if (media(v) != SP_PACKAGE_OK) { status = SP_PACKAGE_MEDIA; goto done; }
    if (current && fault(2U)) goto done;
    if (renameat(v->data_fd, temp, v->data_fd, name) != 0) goto done;
    renamed = true;
    if ((current && fault(3U)) || !directory_sync(v)) { status = SP_PACKAGE_UNCERTAIN; goto done; }
    status = media(v);
done:
    if (fd >= 0) (void)close(fd);
    if (!renamed) (void)unlinkat(v->data_fd, temp, 0);
    return status;
}
static int lock_volume(const struct cs_store *v)
{
    struct stat st;
    int fd = openat(v->data_fd, ".packages.lock", O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1U || st.st_dev != v->device ||
        st.st_uid != 0U || (st.st_mode & 0077U) != 0U || flock(fd, LOCK_EX|LOCK_NB) != 0) {
        int saved = errno; (void)close(fd); errno = saved; return -1;
    }
    return fd;
}
static enum sp_package_status commit(const struct cs_store *v,
                                     const struct sp_package_registry *r,
                                     uint64_t expected, uint64_t original, uint64_t desired, bool rollback,
                                     struct sp_package_selection *out)
{
    struct sp_package_selection current, prior, next;
    enum sp_package_status status;
    int lock;
    if (v == NULL || r == NULL || out == NULL) return SP_PACKAGE_INCOMPATIBLE;
    if (media(v) != SP_PACKAGE_OK) return SP_PACKAGE_MEDIA;
    lock = lock_volume(v);
    if (lock < 0) return errno == EWOULDBLOCK ? SP_PACKAGE_CONFLICT : SP_PACKAGE_IO;
    status = load_internal(v, r, &current);
    if (status != SP_PACKAGE_OK && status != SP_PACKAGE_EMPTY && status != SP_PACKAGE_RECOVERED) goto done;
    if (current.revision != expected || current.revision == UINT64_MAX ||
        (!rollback && current.enabled != original)) { status = SP_PACKAGE_CONFLICT; goto done; }
    if (rollback) {
        status = read_record(v, PREVIOUS, r, &prior);
        if (status != SP_PACKAGE_OK) goto done;
        desired = prior.enabled;
    }
    if (!sp_package_selection_valid(r, desired)) { status = SP_PACKAGE_UNAVAILABLE; goto done; }
    next = current; next.revision++; next.enabled = desired; next.initialized = true; next.recovered = false;
    if (fault(1U)) { status = SP_PACKAGE_IO; goto done; }
    /* The known-good old complete selection is durably retained first. A
     * failure before publishing current cannot turn a pending toggle live. */
    status = write_record(v, PREVIOUS, r, &current, false);
    if (status != SP_PACKAGE_OK) goto done;
    status = write_record(v, CURRENT, r, &next, true);
    if (status == SP_PACKAGE_OK) *out = next;
done:
    if (close(lock) != 0 && status == SP_PACKAGE_OK) status = SP_PACKAGE_UNCERTAIN;
    return status;
}
enum sp_package_status sp_package_apply(const struct cs_store *v,
                                        const struct sp_package_registry *r,
                                        const struct sp_package_plan *plan,
                                        struct sp_package_selection *out)
{
    if (plan == NULL) return SP_PACKAGE_INCOMPATIBLE;
    return commit(v, r, plan->revision, plan->original, plan->desired, false, out);
}
enum sp_package_status sp_package_rollback(const struct cs_store *v,
                                           const struct sp_package_registry *r,
                                           uint64_t expected_revision,
                                           struct sp_package_selection *out)
{
    return commit(v, r, expected_revision, 0U, 0U, true, out);
}
const char *sp_package_status_name(enum sp_package_status status)
{
    static const char *const names[] = {"ok", "empty", "recovered", "corrupt", "incompatible", "media", "io", "conflict", "unavailable", "required", "uncertain"};
    return (unsigned int)status < sizeof(names)/sizeof(names[0]) ? names[(unsigned int)status] : "invalid";
}
