#ifdef NDEBUG
#error "Controller persistence regression fixtures require active assertions (-UNDEBUG)"
#endif
#include "supervisor/diagnostics.h"
#include "supervisor/disk.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static void directory(const char *path) { assert(mkdir(path, 0700) == 0 || errno == EEXIST); }
static void file(const char *path, const char *bytes, size_t count)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, bytes, count) == (ssize_t)count);
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
}
static struct cl_input_profile baseline(void)
{
    struct cl_input_profile p = {0};
    size_t i;
    strcpy(p.guid, "1900385212b400006666000000010000");
    p.hat = -1;
    for (i = 0U; i < CL_CONTROL_COUNT; ++i) {
        p.button[i] = (int)i;
    }
    p.button[CL_MENU] = 21;
    p.button[CL_VOL_DOWN] = 19;
    p.button[CL_VOL_UP] = 20;
    return p;
}
static void swap(struct cl_input_profile *p)
{
    int b = p->button[CL_A];
    p->button[CL_A] = p->button[CL_B];
    p->button[CL_B] = b;
    b = p->button[CL_X];
    p->button[CL_X] = p->button[CL_Y];
    p->button[CL_Y] = b;
}
static void drain(struct sc_disk *disk, bool require_write, bool require_load)
{
    struct sc_disk_result *r = calloc(1U, sizeof(*r));
    bool write = false, loaded = false, final = false;
    unsigned int tries;
    assert(r != NULL);
    for (tries = 0U; tries < 1000U && !final; ++tries) {
        struct pollfd p = {disk->event_fd, POLLIN, 0};
        uint64_t value;
        assert(poll(&p, 1U, 20) >= 0);
        while (read(disk->event_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
        while (sc_disk_result(disk, r)) {
            if (r->verification_write) {
                assert(r->ok);
                write = true;
            }
            if (r->verification_loaded) {
                loaded = true;
            }
            if (r->message.token == 0U && !require_write && !r->final) {
                sc_disk_finish(disk);
            }
            if (r->final) {
                assert(r->ok);
                final = true;
            }
        }
    }
    assert(final);
    assert(!require_write || write);
    assert(!require_load || loaded);
    sc_disk_join(disk);
    free(r);
}
int main(int argc, char **argv)
{
    struct cs_store store;
    struct cs_store_config cfg = {0};
    struct cl_input_profile current = baseline(), wrong;
    struct cl_input_verification v = {0}, loaded, sentinel;
    char path[768], encoded[CL_INPUT_RECORD_MAX], bad[CL_INPUT_RECORD_MAX];
    struct statfs fs;
    size_t count;
    bool fat;
    if (argc != 2) {
        return 2;
    }
    directory(argv[1]);
    assert(statfs(argv[1], &fs) == 0);
    fat = (unsigned long)fs.f_type == 0x4d44UL;
    assert(snprintf(path, sizeof(path), "%s/.console", argv[1]) < (int)sizeof(path));
    directory(path);
    assert(snprintf(path, sizeof(path), "%s/Saves", argv[1]) < (int)sizeof(path));
    directory(path);
    assert(snprintf(path, sizeof(path), "%s/Saves/CurrentProfile", argv[1]) < (int)sizeof(path));
    directory(path);
    cfg.card_path = argv[1];
    cfg.card_uuid = "C028-0002";
    cfg.max_state_bytes = 16777216U;
    cfg.qualified_fat_syncfs = true;
    cfg.test_root = !fat;
    cs_store_init(&store);
    assert(cs_store_open_reader(&store, &cfg) == CS_STORE_OK);
    memset(&sentinel, 0x5a, sizeof(sentinel));
    loaded = sentinel;
    assert(cl_input_record_load(&store, &current, &loaded) == CS_STORE_EMPTY);
    assert(memcmp(&loaded, &sentinel, sizeof(loaded)) == 0);
    v.baseline = current;
    v.checked = current;
    swap(&v.checked);
    assert(cl_input_verification_valid(&current, &v));
    assert(cl_input_record_encode(cfg.card_uuid, &v, encoded, &count));
    assert(cl_input_record_decode(encoded, count, cfg.card_uuid, &current, &loaded));
    assert(memcmp(&loaded, &v, sizeof(v)) == 0);
    assert(!cl_input_record_decode(encoded, count, "C028-0003", &current, &loaded));
    wrong = current;
    wrong.guid[0] = '2';
    loaded = sentinel;
    assert(!cl_input_record_decode(encoded, count, cfg.card_uuid, &wrong, &loaded));
    assert(memcmp(&loaded, &sentinel, sizeof(loaded)) == 0);
    wrong = current;
    wrong.button[CL_MENU] = 22;
    assert(!cl_input_record_decode(encoded, count, cfg.card_uuid, &wrong, &loaded));
    memcpy(bad, encoded, count);
    bad[count - 4U] = bad[count - 4U] == 'a' ? 'b' : 'a';
    assert(!cl_input_record_decode(bad, count, cfg.card_uuid, &current, &loaded));
    assert(!cl_input_record_decode("{\"schema\":1,\"schema\":1}",
                                   strlen("{\"schema\":1,\"schema\":1}"), cfg.card_uuid, &current,
                                   &loaded));
    memcpy(bad, encoded, count);
    bad[count] = '\0';
    assert(strstr(bad, "schema") != NULL);
    memcpy(strstr(bad, "schema"), "foobar", 6U);
    assert(!cl_input_record_decode(bad, count, cfg.card_uuid, &current, &loaded));
    assert(!cl_input_record_decode(bad, CL_INPUT_RECORD_MAX, cfg.card_uuid, &current, &loaded));
    {
        struct cl_input_verification partial = v;
        int button = partial.checked.button[CL_A];
        partial.checked.button[CL_A] = partial.checked.button[CL_B];
        partial.checked.button[CL_B] = button;
        assert(!cl_input_verification_valid(&current, &partial));
    }
    memcpy(bad, encoded, count);
    bad[count - 4U] = bad[count - 4U] == 'a' ? 'b' : 'a';
    wrong = current;
    memset(wrong.guid, '0', 32U);
    assert(!cl_input_verification_valid(&wrong, &v));
    assert(cl_input_record_save(&store, &v) == CS_STORE_OK);
    cs_store_close(&store);
    assert(cs_store_open_reader(&store, &cfg) == CS_STORE_OK);
    assert(cl_input_record_load(&store, &current, &loaded) == CS_STORE_OK);
    assert(memcmp(&loaded, &v, sizeof(v)) == 0);
    directory("/run");
    directory("/run/console");
    directory("/run/console/config");
    assert(cl_input_runtime_publish(&loaded));
    {
        char runtime[1024] = {0};
        struct stat st;
        int fd = open("/run/console/config/verified-input.json", O_RDONLY | O_NOFOLLOW);
        ssize_t n;
        assert(fd >= 0);
        assert(fstat(fd, &st) == 0);
        assert(st.st_uid == 0U && (st.st_mode & 0777U) == 0600U);
        n = read(fd, runtime, sizeof(runtime) - 1U);
        assert(n > 0);
        assert(close(fd) == 0);
        assert(strstr(runtime, "\"guid\":\"1900385212b400006666000000010000\"") != NULL);
        assert(strstr(runtime, "\"hat\":65535") != NULL);
        assert(strstr(runtime, "\"a\":5") != NULL);
    }
    if (fat) {
        struct sc_config *c = calloc(1U, sizeof(*c));
        struct sc_disk disk;
        struct sc_message *query = calloc(1U, sizeof(*query));
        assert(c != NULL && query != NULL);
        strcpy(c->base.card_path, argv[1]);
        strcpy(c->base.card_uuid, cfg.card_uuid);
        c->base.max_state_bytes = cfg.max_state_bytes;
        c->base.qualified_fat_syncfs = true;
        c->ui_config.input = current;
        strcpy(c->system_path, argv[1]);
        /* A pending mutation survives finish discarding reads; reopen simulates reboot. */
        assert(sc_disk_start(&disk, c));
        assert(sc_disk_verify(&disk, &v, UINT64_C(0x100000001)));
        sc_disk_finish(&disk);
        drain(&disk, true, false);
        assert(sc_disk_start(&disk, c));
        sc_message_init(query, SC_QUERY, 0U);
        assert(sc_disk_submit(&disk, query));
        drain(&disk, false, true);
        free(query);
        free(c);
    }
    assert(snprintf(path, sizeof(path), "%s/Saves/CurrentProfile/controller.json", argv[1]) <
           (int)sizeof(path));
    file(path, bad, count);
    loaded = sentinel;
    assert(cl_input_record_load(&store, &current, &loaded) != CS_STORE_OK);
    assert(memcmp(&loaded, &sentinel, sizeof(loaded)) == 0);
    file(path, encoded, count);
    {
        char profile[800], absent[832];
        assert(snprintf(profile, sizeof(profile), "%s/Saves/CurrentProfile", argv[1]) <
               (int)sizeof(profile));
        assert(snprintf(absent, sizeof(absent), "%s.offline", profile) < (int)sizeof(absent));
        assert(rename(profile, absent) == 0);
        assert(cl_input_record_save(&store, &v) == CS_STORE_MEDIA);
        assert(rename(absent, profile) == 0);
        assert(cl_input_record_load(&store, &current, &loaded) == CS_STORE_OK);
        assert(memcmp(&loaded, &v, sizeof(v)) == 0);
    }
    {
        char renamed[800];
        assert(snprintf(renamed, sizeof(renamed), "%s.offline", argv[1]) < (int)sizeof(renamed));
        /* Moving a mountpoint is forbidden; simulate missing path only on tmpfs subdirectory. */
        if (!fat) {
            assert(rename(argv[1], renamed) == 0);
            assert(cl_input_record_save(&store, &v) == CS_STORE_MEDIA);
            assert(rename(renamed, argv[1]) == 0);
        }
    }
    assert(cl_input_record_load(&store, &current, &loaded) == CS_STORE_OK);
    file("/run/console/supervisor.log", "controller persistence test\n", 28U);
    file("/run/console/startup.log", "startup test\n", 13U);
    assert(sc_diagnostics_save(&store));
    cs_store_close(&store);
    assert(unlink(path) == 0);
    printf("CONTROLLER_PERSISTENCE_PASS fs_magic=%lx worker_shutdown=%s exact_guid_baseline=1 "
           "corrupt_rollback=1 runtime0600=1 diagnostics=1\n",
           (unsigned long)fs.f_type, fat ? "tested" : "fat-only");
    return 0;
}
